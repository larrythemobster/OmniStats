#include <gtest/gtest.h>
#include "database/ReaderPool.hpp"
#include "database/sqlite/Connection.hpp"
#include "database/sqlite/SqliteError.hpp"
#include "database/sqlite/Statement.hpp"
#include "database/sqlite/Transaction.hpp"

#include <atomic>
#include <chrono>
#include <filesystem>
#include <future>
#include <stdexcept>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

class SqliteRaiiTest : public ::testing::Test {
  protected:
    void SetUp() override {
        tempDir = fs::temp_directory_path() / ("omni_raii_test_" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        fs::create_directories(tempDir);
        dbPath = (tempDir / "test.db").string();
    }

    void TearDown() override {
        std::error_code ec;
        fs::remove_all(tempDir, ec);
    }

    fs::path tempDir;
    std::string dbPath;
};

TEST_F(SqliteRaiiTest, StatementFinalizeOnEarlyReturn) {
    auto conn = database::sqlite::Connection::Open(dbPath);
    conn.Execute("CREATE TABLE Items (id INTEGER PRIMARY KEY, name TEXT);");
    conn.Execute("INSERT INTO Items (name) VALUES ('one'), ('two'), ('three');");

    auto earlyReturnFunc = [&]() {
        database::sqlite::Statement stmt(conn, "SELECT name FROM Items ORDER BY id;");
        if (stmt.Step()) {
            EXPECT_EQ(stmt.Get<std::string>(0), "one");
            return; // Early return before consuming all rows or explicit finalize
        }
    };

    earlyReturnFunc();

    // Verify statement was finalized so schema/drop operations or connection close succeed without SQLITE_BUSY
    EXPECT_NO_THROW({
        conn.Execute("DROP TABLE Items;");
    });
}

TEST_F(SqliteRaiiTest, StatementFinalizeOnException) {
    auto conn = database::sqlite::Connection::Open(dbPath);
    conn.Execute("CREATE TABLE Items (id INTEGER PRIMARY KEY, name TEXT);");
    conn.Execute("INSERT INTO Items (name) VALUES ('alpha'), ('beta');");

    try {
        database::sqlite::Statement stmt(conn, "SELECT name FROM Items;");
        if (stmt.Step()) {
            EXPECT_EQ(stmt.Get<std::string>(0), "alpha");
            throw std::runtime_error("Simulated failure mid-statement");
        }
    } catch (const std::runtime_error& e) {
        EXPECT_STREQ(e.what(), "Simulated failure mid-statement");
    }

    // Verify statement destructor finalized the handle and the table is unlocked
    EXPECT_NO_THROW({
        conn.Execute("INSERT INTO Items (name) VALUES ('gamma');");
    });
    conn.Close();
    EXPECT_FALSE(conn.IsOpen());
}

TEST_F(SqliteRaiiTest, TransactionRollbackOnThrow) {
    auto conn = database::sqlite::Connection::Open(dbPath);
    conn.Execute("CREATE TABLE Accounts (id INTEGER PRIMARY KEY, balance INTEGER);");
    conn.Execute("INSERT INTO Accounts (id, balance) VALUES (1, 100);");

    auto failOperation = [&]() {
        database::sqlite::Transaction tx(conn, database::sqlite::TransactionType::Immediate);
        conn.Execute("UPDATE Accounts SET balance = 500 WHERE id = 1;");
        conn.Execute("INSERT INTO Accounts (id, balance) VALUES (2, 200);");
        throw std::logic_error("Something went wrong before commit");
    };

    EXPECT_THROW(failOperation(), std::logic_error);

    // Verify transaction rolled back automatically on stack unwind
    database::sqlite::Statement check(conn, "SELECT balance FROM Accounts WHERE id = 1;");
    ASSERT_TRUE(check.Step());
    EXPECT_EQ(check.Get<int>(0), 100);

    database::sqlite::Statement countStmt(conn, "SELECT COUNT(*) FROM Accounts;");
    ASSERT_TRUE(countStmt.Step());
    EXPECT_EQ(countStmt.Get<int>(0), 1);
}

TEST_F(SqliteRaiiTest, TypedBindAndGet) {
    auto conn = database::sqlite::Connection::OpenInMemory();
    conn.Execute("CREATE TABLE Data (i INTEGER, i64 INTEGER, d REAL, s TEXT, n TEXT);");

    database::sqlite::Statement ins(conn, "INSERT INTO Data VALUES (?1, ?2, ?3, ?4, ?5);");
    ins.Bind(1, 42);
    ins.Bind(2, static_cast<int64_t>(9876543210LL));
    ins.Bind(3, 3.14159);
    ins.Bind(4, std::string("test string"));
    ins.BindNull(5);
    ins.Execute();

    database::sqlite::Statement sel(conn, "SELECT i, i64, d, s, n FROM Data;");
    ASSERT_TRUE(sel.Step());
    EXPECT_EQ(sel.Get<int>(0), 42);
    EXPECT_EQ(sel.Get<int64_t>(1), 9876543210LL);
    EXPECT_NEAR(sel.Get<double>(2), 3.14159, 0.0001);
    EXPECT_EQ(sel.Get<std::string>(3), "test string");
    EXPECT_TRUE(sel.IsNull(4));
}

TEST_F(SqliteRaiiTest, ConcurrentReadersOverlappingWrite) {
    auto writer = database::sqlite::Connection::Open(dbPath);
    writer.Execute("PRAGMA journal_mode = WAL;");
    writer.Execute("PRAGMA synchronous = NORMAL;");
    writer.Execute("CREATE TABLE Counters (id INTEGER PRIMARY KEY, value INTEGER);");
    writer.Execute("INSERT INTO Counters VALUES (1, 0);");

    database::ReaderPool pool;
    ASSERT_TRUE(pool.Initialize(dbPath, 3));

    std::atomic<bool> stop{false};
    std::atomic<int64_t> totalReads{0};
    std::atomic<int64_t> readErrors{0};

    // 3 reader threads continuously reading through the ReaderPool
    std::vector<std::jthread> readers;
    for (int i = 0; i < 3; ++i) {
        readers.emplace_back([&]() {
            while (!stop.load(std::memory_order_relaxed)) {
                try {
                    auto lease = pool.Acquire();
                    if (!lease.IsValid()) {
                        readErrors.fetch_add(1);
                        continue;
                    }
                    database::sqlite::Statement stmt(lease.Get(), "SELECT value FROM Counters WHERE id = 1;");
                    if (stmt.Step()) {
                        int val = stmt.Get<int>(0);
                        EXPECT_GE(val, 0);
                        totalReads.fetch_add(1, std::memory_order_relaxed);
                    }
                } catch (...) {
                    readErrors.fetch_add(1);
                }
            }
        });
    }

    // Writer performs 100 sequential updates in immediate transactions
    for (int step = 1; step <= 100; ++step) {
        database::sqlite::Transaction tx(writer, database::sqlite::TransactionType::Immediate);
        writer.Execute("UPDATE Counters SET value = value + 1 WHERE id = 1;");
        tx.Commit();
        std::this_thread::sleep_for(std::chrono::microseconds(500));
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& t : readers) {
        if (t.joinable()) t.join();
    }

    pool.Shutdown();

    database::sqlite::Statement verify(writer, "SELECT value FROM Counters WHERE id = 1;");
    ASSERT_TRUE(verify.Step());
    EXPECT_EQ(verify.Get<int>(0), 100);
    EXPECT_GT(totalReads.load(), 0);
    EXPECT_EQ(readErrors.load(), 0);
}

TEST_F(SqliteRaiiTest, ReaderPoolShutdownWaitsForLeaseAndWakesAcquirers) {
    auto writer = database::sqlite::Connection::Open(dbPath);
    writer.Execute("CREATE TABLE Items (id INTEGER);");

    database::ReaderPool pool;
    ASSERT_TRUE(pool.Initialize(dbPath, 1));
    auto lease = pool.Acquire();
    ASSERT_TRUE(lease.IsValid());

    auto shutdown = std::async(std::launch::async, [&pool] { pool.Shutdown(); });
    auto rejected = pool.Acquire();
    EXPECT_FALSE(rejected.IsValid());
    EXPECT_EQ(shutdown.wait_for(std::chrono::seconds(0)), std::future_status::timeout);
    EXPECT_NO_THROW(lease->Execute("SELECT * FROM Items;"));
    lease = {};
    shutdown.get();
    EXPECT_FALSE(pool.HasPool());
}

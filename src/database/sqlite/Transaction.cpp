#include "Transaction.hpp"
#include "Connection.hpp"

namespace database::sqlite {

    Transaction::Transaction(Connection& conn, TransactionType type)
        : Transaction(conn.Handle(), type) {}

    Transaction::Transaction(sqlite3* db, TransactionType type)
        : m_db(db) {
        if (!m_db) {
            throw SqliteError(SQLITE_MISUSE, "Cannot start transaction with null database handle");
        }

        const char* sql = "BEGIN;";
        if (type == TransactionType::Immediate) {
            sql = "BEGIN IMMEDIATE;";
        } else if (type == TransactionType::Exclusive) {
            sql = "BEGIN EXCLUSIVE;";
        }

        char* errMsg = nullptr;
        const int rc = sqlite3_exec(m_db, sql, nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::string msg = errMsg ? errMsg : "Failed to begin transaction";
            if (errMsg) sqlite3_free(errMsg);
            throw SqliteError(rc, msg, sql);
        }
        m_active = true;
        m_committed = false;
    }

    Transaction::~Transaction() {
        if (m_active && !m_committed && m_db) {
            sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            m_active = false;
        }
    }

    Transaction::Transaction(Transaction&& other) noexcept
        : m_db(other.m_db), m_committed(other.m_committed), m_active(other.m_active) {
        other.m_db = nullptr;
        other.m_committed = false;
        other.m_active = false;
    }

    Transaction& Transaction::operator=(Transaction&& other) noexcept {
        if (this != &other) {
            if (m_active && !m_committed && m_db) {
                sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, nullptr);
            }
            m_db = other.m_db;
            m_committed = other.m_committed;
            m_active = other.m_active;
            other.m_db = nullptr;
            other.m_committed = false;
            other.m_active = false;
        }
        return *this;
    }

    void Transaction::Commit() {
        if (!m_active) {
            throw SqliteError(SQLITE_MISUSE, "Cannot commit inactive transaction");
        }
        char* errMsg = nullptr;
        const int rc = sqlite3_exec(m_db, "COMMIT;", nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::string msg = errMsg ? errMsg : "Failed to commit transaction";
            if (errMsg) sqlite3_free(errMsg);
            throw SqliteError(rc, msg, "COMMIT;");
        }
        m_committed = true;
        m_active = false;
    }

    void Transaction::Rollback() {
        if (!m_active) {
            return;
        }
        char* errMsg = nullptr;
        const int rc = sqlite3_exec(m_db, "ROLLBACK;", nullptr, nullptr, &errMsg);
        m_committed = false;
        m_active = false;
        if (rc != SQLITE_OK) {
            std::string msg = errMsg ? errMsg : "Failed to rollback transaction";
            if (errMsg) sqlite3_free(errMsg);
            throw SqliteError(rc, msg, "ROLLBACK;");
        }
    }

} // namespace database::sqlite

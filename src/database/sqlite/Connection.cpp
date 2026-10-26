#include "Connection.hpp"
#include "Statement.hpp"

namespace database::sqlite {

    Connection Connection::Open(const std::string& path, int flags) {
        sqlite3* db = nullptr;
        const int rc = sqlite3_open_v2(path.c_str(), &db, flags, nullptr);
        if (rc != SQLITE_OK || !db) {
            std::string err = db ? sqlite3_errmsg(db) : "Failed to allocate SQLite handle";
            if (db) sqlite3_close(db);
            throw SqliteError(rc, err, "sqlite3_open_v2: " + path);
        }
        return Connection(db, true);
    }

    Connection Connection::OpenReadOnly(const std::string& path) {
        return Open(path, SQLITE_OPEN_READONLY);
    }

    Connection Connection::OpenInMemory() {
        return Open(":memory:", SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
    }

    void Connection::Close() {
        if (m_owns && m_db) {
            sqlite3_close(m_db);
            m_db = nullptr;
        }
    }

    void Connection::Execute(std::string_view sql) {
        if (!m_db) {
            throw SqliteError(SQLITE_MISUSE, "Cannot execute SQL on closed database", std::string(sql));
        }
        char* errMsg = nullptr;
        const int rc = sqlite3_exec(m_db, std::string(sql).c_str(), nullptr, nullptr, &errMsg);
        if (rc != SQLITE_OK) {
            std::string msg = errMsg ? errMsg : "Execution error";
            if (errMsg) sqlite3_free(errMsg);
            throw SqliteError(rc, msg, std::string(sql));
        }
    }

    int64_t Connection::LastInsertRowId() const noexcept {
        return m_db ? sqlite3_last_insert_rowid(m_db) : 0;
    }

    int Connection::Changes() const noexcept {
        return m_db ? sqlite3_changes(m_db) : 0;
    }

    int Connection::TotalChanges() const noexcept {
        return m_db ? sqlite3_total_changes(m_db) : 0;
    }

    void Connection::BusyTimeout(int ms) {
        if (m_db) {
            sqlite3_busy_timeout(m_db, ms);
        }
    }

    bool Connection::TableExists(std::string_view tableName) {
        Statement stmt(*this, "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ? LIMIT 1;");
        stmt.Bind(1, tableName);
        return stmt.Step();
    }

    bool Connection::TableHasColumn(std::string_view tableName, std::string_view columnName) {
        Statement stmt(*this, "PRAGMA table_info(" + std::string(tableName) + ");");
        while (stmt.Step()) {
            if (stmt.Get<std::string>(1) == columnName) {
                return true;
            }
        }
        return false;
    }

    int Connection::UserVersion() {
        Statement stmt(*this, "PRAGMA user_version;");
        if (stmt.Step()) {
            return stmt.Get<int>(0);
        }
        return 0;
    }

    void Connection::SetUserVersion(int version) {
        Execute("PRAGMA user_version = " + std::to_string(version) + ";");
    }

    Statement Connection::Prepare(std::string_view sql) {
        return Statement(*this, sql);
    }

} // namespace database::sqlite

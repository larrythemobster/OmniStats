#include "Statement.hpp"
#include "Connection.hpp"

namespace database::sqlite {

    Statement::Statement(sqlite3* db, std::string_view sql)
        : m_db(db), m_sql(sql) {
        if (!m_db) {
            throw SqliteError(SQLITE_MISUSE, "Cannot prepare statement with null database connection", m_sql);
        }
        const int rc = sqlite3_prepare_v2(m_db, m_sql.c_str(), static_cast<int>(m_sql.size()), &m_stmt, nullptr);
        if (rc != SQLITE_OK || !m_stmt) {
            throw SqliteError::FromDb(m_db, rc, m_sql);
        }
    }

    Statement::Statement(Connection& conn, std::string_view sql)
        : Statement(conn.Handle(), sql) {}

    Statement::~Statement() {
        if (m_stmt) {
            sqlite3_finalize(m_stmt);
            m_stmt = nullptr;
        }
    }

    Statement::Statement(Statement&& other) noexcept
        : m_db(other.m_db), m_stmt(other.m_stmt), m_sql(std::move(other.m_sql)) {
        other.m_db = nullptr;
        other.m_stmt = nullptr;
    }

    Statement& Statement::operator=(Statement&& other) noexcept {
        if (this != &other) {
            if (m_stmt) {
                sqlite3_finalize(m_stmt);
            }
            m_db = other.m_db;
            m_stmt = other.m_stmt;
            m_sql = std::move(other.m_sql);
            other.m_db = nullptr;
            other.m_stmt = nullptr;
        }
        return *this;
    }

    void Statement::Reset() {
        if (m_stmt) {
            sqlite3_reset(m_stmt);
            sqlite3_clear_bindings(m_stmt);
        }
    }

    void Statement::ClearBindings() {
        if (m_stmt) {
            sqlite3_clear_bindings(m_stmt);
        }
    }

    bool Statement::Step() {
        if (!m_stmt) {
            throw SqliteError(SQLITE_MISUSE, "Cannot step null statement", m_sql);
        }
        const int rc = sqlite3_step(m_stmt);
        if (rc == SQLITE_ROW) {
            return true;
        }
        if (rc == SQLITE_DONE) {
            return false;
        }
        throw SqliteError::FromDb(m_db, rc, m_sql);
    }

    void Statement::Execute() {
        (void)Step();
        Reset();
    }

    void Statement::Bind(int index, int val) {
        if (sqlite3_bind_int(m_stmt, index, val) != SQLITE_OK) {
            throw SqliteError::FromDb(m_db, sqlite3_errcode(m_db), m_sql);
        }
    }

    void Statement::Bind(int index, unsigned int val) {
        Bind(index, static_cast<int64_t>(val));
    }

    void Statement::Bind(int index, int64_t val) {
        if (sqlite3_bind_int64(m_stmt, index, val) != SQLITE_OK) {
            throw SqliteError::FromDb(m_db, sqlite3_errcode(m_db), m_sql);
        }
    }

    void Statement::Bind(int index, uint64_t val) {
        Bind(index, static_cast<int64_t>(val));
    }

    void Statement::Bind(int index, double val) {
        if (sqlite3_bind_double(m_stmt, index, val) != SQLITE_OK) {
            throw SqliteError::FromDb(m_db, sqlite3_errcode(m_db), m_sql);
        }
    }

    void Statement::Bind(int index, float val) {
        Bind(index, static_cast<double>(val));
    }

    void Statement::Bind(int index, bool val) {
        Bind(index, val ? 1 : 0);
    }

    void Statement::Bind(int index, const char* val) {
        if (!val) {
            BindNull(index);
            return;
        }
        if (sqlite3_bind_text(m_stmt, index, val, -1, SQLITE_TRANSIENT) != SQLITE_OK) {
            throw SqliteError::FromDb(m_db, sqlite3_errcode(m_db), m_sql);
        }
    }

    void Statement::Bind(int index, const std::string& val) {
        if (sqlite3_bind_text(m_stmt, index, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
            throw SqliteError::FromDb(m_db, sqlite3_errcode(m_db), m_sql);
        }
    }

    void Statement::Bind(int index, std::string_view val) {
        if (sqlite3_bind_text(m_stmt, index, val.data(), static_cast<int>(val.size()), SQLITE_TRANSIENT) != SQLITE_OK) {
            throw SqliteError::FromDb(m_db, sqlite3_errcode(m_db), m_sql);
        }
    }

    void Statement::Bind(int index, std::nullptr_t) {
        BindNull(index);
    }

    void Statement::Bind(int index, std::nullopt_t) {
        BindNull(index);
    }

    void Statement::BindNull(int index) {
        if (sqlite3_bind_null(m_stmt, index) != SQLITE_OK) {
            throw SqliteError::FromDb(m_db, sqlite3_errcode(m_db), m_sql);
        }
    }

    int Statement::ParameterIndex(std::string_view name) const {
        if (!m_stmt) {
            throw SqliteError(SQLITE_MISUSE, "Cannot find parameter index on null statement", m_sql);
        }
        std::string nameStr(name);
        int idx = sqlite3_bind_parameter_index(m_stmt, nameStr.c_str());
        if (idx == 0) {
            throw SqliteError(SQLITE_NOTFOUND, "Parameter not found: " + nameStr, m_sql);
        }
        return idx;
    }

    bool Statement::IsNull(int col) const {
        return sqlite3_column_type(m_stmt, col) == SQLITE_NULL;
    }

    int Statement::ColumnCount() const {
        return sqlite3_column_count(m_stmt);
    }

    int Statement::ColumnType(int col) const {
        return sqlite3_column_type(m_stmt, col);
    }

    std::string_view Statement::ColumnName(int col) const {
        const char* name = sqlite3_column_name(m_stmt, col);
        return name ? std::string_view(name) : std::string_view{};
    }

} // namespace database::sqlite

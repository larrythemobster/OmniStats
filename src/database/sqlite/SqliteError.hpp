#pragma once

#include <sqlite3.h>
#include <stdexcept>
#include <string>
#include <utility>

namespace database::sqlite {

    class SqliteError : public std::runtime_error {
      public:
        SqliteError(int errorCode, std::string message, std::string sql = "")
            : std::runtime_error(FormatMessage(errorCode, message, sql)), m_errorCode(errorCode), m_message(std::move(message)), m_sql(std::move(sql)) {}

        int ErrorCode() const noexcept {
            return m_errorCode;
        }
        const std::string& Message() const noexcept {
            return m_message;
        }
        const std::string& Sql() const noexcept {
            return m_sql;
        }

        static SqliteError FromDb(sqlite3* db, int errorCode, std::string sql = "") {
            const char* msg = db ? sqlite3_errmsg(db) : "Unknown SQLite error";
            return SqliteError(errorCode, msg ? msg : "", std::move(sql));
        }

      private:
        static std::string FormatMessage(int code, const std::string& msg, const std::string& sql) {
            std::string result = "SQLite error (" + std::to_string(code) + "): " + msg;
            if (!sql.empty()) {
                result += " [SQL: " + sql + "]";
            }
            return result;
        }

        int m_errorCode = 0;
        std::string m_message;
        std::string m_sql;
    };

} // namespace database::sqlite

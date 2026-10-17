#pragma once

#include <sqlite3.h>
#include <string>

namespace DatabaseDetail {

    inline std::string SqlColumnText(sqlite3_stmt* stmt, int column) {
        const unsigned char* value = sqlite3_column_text(stmt, column);
        return value ? reinterpret_cast<const char*>(value) : "";
    }

    inline bool SqliteTableHasColumn(sqlite3* db,
                                     const char* tableName,
                                     const char* columnName) {
        if (!db || !tableName || !columnName) return false;
        const std::string sql =
            "PRAGMA table_info(" + std::string(tableName) + ");";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql.c_str(), -1, &stmt, nullptr) !=
            SQLITE_OK) {
            return false;
        }

        bool found = false;
        while (sqlite3_step(stmt) == SQLITE_ROW) {
            const std::string name = SqlColumnText(stmt, 1);
            if (name == columnName) {
                found = true;
                break;
            }
        }
        sqlite3_finalize(stmt);
        return found;
    }

    inline bool SqliteTableExists(sqlite3* db, const char* tableName) {
        if (!db || !tableName) return false;
        const char* sql = "SELECT 1 FROM sqlite_master WHERE type='table' AND name = ? LIMIT 1;";
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, sql, -1, &stmt, nullptr) != SQLITE_OK) return false;
        sqlite3_bind_text(stmt, 1, tableName, -1, SQLITE_STATIC);
        const bool found = (sqlite3_step(stmt) == SQLITE_ROW);
        sqlite3_finalize(stmt);
        return found;
    }

    inline int ReadUserVersion(sqlite3* db) {
        if (!db) return 0;
        sqlite3_stmt* stmt = nullptr;
        if (sqlite3_prepare_v2(db, "PRAGMA user_version;", -1, &stmt, nullptr) != SQLITE_OK) return 0;
        int version = 0;
        if (sqlite3_step(stmt) == SQLITE_ROW) {
            version = sqlite3_column_int(stmt, 0);
        }
        sqlite3_finalize(stmt);
        return version;
    }

    inline bool IsInMemoryDbPath(const std::string& dbPath) {
        if (dbPath.empty() || dbPath == ":memory:") return true;
        return dbPath.rfind("file:", 0) == 0 && dbPath.find("mode=memory") != std::string::npos;
    }

    inline bool HasStatsTables(sqlite3* db) {
        return SqliteTableExists(db, "MatchPlayerStats") && SqliteTableExists(db, "MatchLocalStats");
    }

    inline bool HasSessionsTable(sqlite3* db) {
        return SqliteTableExists(db, "Sessions") && SqliteTableHasColumn(db, "Matches", "session_id");
    }

} // namespace DatabaseDetail

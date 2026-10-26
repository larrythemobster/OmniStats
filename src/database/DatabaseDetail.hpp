#pragma once

#include "database/sqlite/Connection.hpp"
#include <sqlite3.h>
#include <string>

namespace DatabaseDetail {

    inline bool SqliteTableHasColumn(sqlite3* db,
                                     const char* tableName,
                                     const char* columnName) {
        if (!db || !tableName || !columnName) return false;
        try {
            database::sqlite::Connection conn(db, false);
            return conn.TableHasColumn(tableName, columnName);
        } catch (...) {
            return false;
        }
    }

    inline bool SqliteTableExists(sqlite3* db, const char* tableName) {
        if (!db || !tableName) return false;
        try {
            database::sqlite::Connection conn(db, false);
            return conn.TableExists(tableName);
        } catch (...) {
            return false;
        }
    }

    inline int ReadUserVersion(sqlite3* db) {
        if (!db) return 0;
        try {
            database::sqlite::Connection conn(db, false);
            return conn.UserVersion();
        } catch (...) {
            return 0;
        }
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

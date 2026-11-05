#pragma once

#include "database/sqlite/Connection.hpp"
#include <string>

namespace database::repository {

    class Migrations {
      public:
        static int LatestVersion();
        static bool Migrate(sqlite::Connection& db, const std::string& dbPath);
        // Backs up the file, then clears or deletes rows whose referenced parent no longer exists.
        static bool RepairForeignKeyViolations(sqlite::Connection& db, const std::string& dbPath, std::string& error);
        static void RepairArenaNames(sqlite::Connection& db);
        static bool HasStatsTables(sqlite::Connection& db);
        static bool HasSessionsTable(sqlite::Connection& db);
        static bool HasAnyUserTable(sqlite::Connection& db);
    };

} // namespace database::repository

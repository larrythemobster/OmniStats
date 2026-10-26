#pragma once

#include "database/sqlite/Connection.hpp"
#include <string>

namespace database::repository {

    class Migrations {
      public:
        static bool Migrate(sqlite::Connection& db, const std::string& dbPath);
        static void RepairArenaNames(sqlite::Connection& db);
        static bool HasStatsTables(sqlite::Connection& db);
        static bool HasSessionsTable(sqlite::Connection& db);
        static bool HasAnyUserTable(sqlite::Connection& db);
    };

} // namespace database::repository

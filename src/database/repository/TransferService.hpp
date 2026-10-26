#pragma once

#include "database/DatabaseTypes.hpp"
#include "database/sqlite/Connection.hpp"
#include <string>

namespace database::repository {

    class TransferService {
      public:
        explicit TransferService(sqlite::Connection& db, std::string dbPath = "")
            : m_db(db), m_dbPath(std::move(dbPath)) {}

        static std::string CsvEscape(const std::string& value);
        static bool ExportLocalData(sqlite::Connection& db, std::string& exportPath, std::string& error);
        static bool DeleteLocalMatchHistory(sqlite::Connection& db, std::string& error);
        static DbMergeResult MergeDatabase(sqlite::Connection& db, const std::string& sourceDbPath, const std::string& activeDbPath = "");

      private:
        sqlite::Connection& m_db;
        std::string m_dbPath;
    };

} // namespace database::repository

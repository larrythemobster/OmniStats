#pragma once

#include "core/accounts/PlayerNotes.hpp"
#include "core/accounts/PlayerScout.hpp"
#include "database/sqlite/Connection.hpp"
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace database::repository {

    class PlayerNotesRepository {
      public:
        explicit PlayerNotesRepository(sqlite::Connection& db) : m_db(db) {}

        static std::optional<std::vector<PlayerNotes::Note>> ListPlayerNotes(sqlite::Connection& db);
        static std::optional<std::vector<PlayerNotes::Tag>> ListPlayerTags(sqlite::Connection& db);
        static std::optional<std::vector<std::string>> GetPlayerTags(sqlite::Connection& db, const PlayerNotes::Identity& identity);
        static bool SetPlayerTag(sqlite::Connection& db, const PlayerNotes::Identity& identity, std::string_view tag, bool add,
                                 int64_t nowUnix, std::string* error = nullptr);
        static std::optional<PlayerNotes::Note> GetPlayerNote(sqlite::Connection& db, const PlayerNotes::Identity& identity);
        static bool SavePlayerNote(sqlite::Connection& db, const PlayerNotes::Identity& identity, std::string_view text,
                                   std::string_view lastKnownName, int64_t nowUnix, std::string* error = nullptr);
        static bool DeletePlayerNote(sqlite::Connection& db, const PlayerNotes::Identity& identity, std::string* error = nullptr);
        static std::optional<PlayerScoutProfile> GetPlayerScoutProfile(sqlite::Connection& db, const std::string& account, const std::string& playerId);

      private:
        sqlite::Connection& m_db;
    };

} // namespace database::repository

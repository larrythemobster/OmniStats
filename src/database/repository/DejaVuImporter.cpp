#include "DejaVuImporter.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "database/sqlite/Statement.hpp"
#include "database/sqlite/Transaction.hpp"
#include "utils/Env.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <charconv>
#include <ctime>
#include <fstream>
#include <iterator>
#include <limits>
#include <sstream>
#include <string_view>

namespace fs = std::filesystem;

namespace database::repository {

    namespace {
        constexpr const char* kAnswerSetting = "dejavu_import";

        void SaveAnswer(sqlite::Connection& db, const char* answer) {
            sqlite::Statement setting(db, "INSERT OR REPLACE INTO Settings (key, value) VALUES (?1, ?2);");
            setting.Bind(1, kAnswerSetting);
            setting.Bind(2, answer);
            setting.Execute();
        }

        bool IsDigits(std::string_view text) {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) { return c >= '0' && c <= '9'; });
        }

        bool IsHex(std::string_view text) {
            return !text.empty() && std::all_of(text.begin(), text.end(), [](char c) {
                return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F');
            });
        }

        // DejaVu keys players by bare account ID, so the platform is inferred from the ID's shape.
        std::string PrimaryIdFromKey(std::string_view key) {
            if (key.size() == 32 && IsHex(key)) {
                std::string id(key);
                std::transform(id.begin(), id.end(), id.begin(), [](char c) {
                    return (c >= 'A' && c <= 'F') ? static_cast<char>(c - 'A' + 'a') : c;
                });
                return "Epic|" + id + "|0";
            }
            if (!IsDigits(key)) return {};
            uint64_t value = 0;
            if (std::from_chars(key.data(), key.data() + key.size(), value).ec != std::errc{} || value == 0) return {};
            const std::string id = std::to_string(value);
            if (id.size() == 17 && id.starts_with("7656119")) return "Steam|" + id + "|0";
            // XUIDs occupy 0x0009000000000000-0x0009FFFFFFFFFFFF; other numeric IDs are PlayStation account IDs.
            if (value >= 0x0009000000000000ULL && value <= 0x0009FFFFFFFFFFFFULL) return "XboxOne|" + id + "|0";
            return "PS4|" + id + "|0";
        }

        std::string StringField(const nlohmann::json& object, const char* key) {
            const auto it = object.find(key);
            return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
        }

        int Count(const nlohmann::json& side, const char* key) {
            const auto it = side.find(key);
            if (it == side.end() || !it->is_number_integer()) return 0;
            return static_cast<int>(std::clamp<int64_t>(it->get<int64_t>(), 0, std::numeric_limits<int>::max()));
        }

        // DejaVu writes ctime() local time, e.g. "Sun Apr  7 23:53:04 2024\n".
        int64_t ParseCtime(const std::string& text) {
            static constexpr std::array<std::string_view, 12> kMonths = {
                "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
            std::istringstream in(text);
            std::string weekday;
            std::string month;
            std::tm tm{};
            char colon1 = 0;
            char colon2 = 0;
            int year = 0;
            if (!(in >> weekday >> month >> tm.tm_mday >> tm.tm_hour >> colon1 >> tm.tm_min >> colon2 >> tm.tm_sec >> year) ||
                colon1 != ':' || colon2 != ':') {
                return 0;
            }
            const auto found = std::find(kMonths.begin(), kMonths.end(), month);
            if (found == kMonths.end()) return 0;
            tm.tm_mon = static_cast<int>(found - kMonths.begin());
            tm.tm_year = year - 1900;
            tm.tm_isdst = -1;
            const std::time_t unix = std::mktime(&tm);
            return unix == static_cast<std::time_t>(-1) ? 0 : static_cast<int64_t>(unix);
        }
    } // namespace

    fs::path DejaVuImporter::DefaultPlayerCounterPath() {
        const auto appData = Env::Get("APPDATA");
        if (!appData || appData->empty()) return {};
        return fs::path(*appData) / "bakkesmod" / "bakkesmod" / "data" / "dejavu" / "player_counter.json";
    }

    int DejaVuImporter::CountPlayers(const std::vector<DejaVuEncounter>& rows) {
        int players = 0;
        for (size_t i = 0; i < rows.size(); ++i) {
            if (i == 0 || rows[i].primaryId != rows[i - 1].primaryId) ++players;
        }
        return players;
    }

    bool DejaVuImporter::Answered(sqlite::Connection& db) {
        if (!db.IsOpen()) return false;
        try {
            sqlite::Statement stmt(db, "SELECT 1 FROM Settings WHERE key = ?1;");
            stmt.Bind(1, kAnswerSetting);
            return stmt.Step();
        } catch (...) {
            return false;
        }
    }

    std::optional<std::vector<DejaVuEncounter>> DejaVuImporter::LoadFile(const fs::path& path, std::string& error) {
        std::ifstream file(path, std::ios::binary);
        if (!file) {
            error = "Cannot open player_counter.json.";
            return std::nullopt;
        }
        const std::string text{std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>()};
        const auto root = nlohmann::json::parse(text, nullptr, false);
        if (root.is_discarded() || !root.is_object()) {
            error = "player_counter.json is not valid JSON.";
            return std::nullopt;
        }
        const auto players = root.find("players");
        if (players == root.end() || !players->is_object()) {
            error = "player_counter.json has no players object.";
            return std::nullopt;
        }

        std::vector<DejaVuEncounter> rows;
        for (const auto& [key, player] : players->items()) {
            if (!player.is_object()) continue;
            const auto playlists = player.find("playlistData");
            if (playlists == player.end() || !playlists->is_object()) continue;
            std::string primaryId = PrimaryIdFromKey(key);
            if (primaryId.empty()) continue;

            const std::string name = StringField(player, "name");
            int64_t lastSeen = ParseCtime(StringField(player, "updatedAt"));
            if (lastSeen == 0) lastSeen = ParseCtime(StringField(player, "timeMet"));

            for (const auto& [playlistKey, playlist] : playlists->items()) {
                int playlistId = 0;
                const auto parsed = std::from_chars(playlistKey.data(), playlistKey.data() + playlistKey.size(), playlistId);
                if (parsed.ec != std::errc{} || parsed.ptr != playlistKey.data() + playlistKey.size()) continue;
                if (PlaylistMetadata::IsNonRecordable(playlistId) || !playlist.is_object()) continue;
                const auto records = playlist.find("records");
                if (records == playlist.end() || !records->is_object()) continue;

                DejaVuEncounter row{.primaryId = primaryId, .name = name, .playlistId = playlistId, .lastSeenUnix = lastSeen};
                if (const auto with = records->find("with"); with != records->end() && with->is_object()) {
                    row.winsWith = Count(*with, "wins");
                    row.lossesWith = Count(*with, "losses");
                }
                if (const auto against = records->find("against"); against != records->end() && against->is_object()) {
                    row.winsAgainst = Count(*against, "wins");
                    row.lossesAgainst = Count(*against, "losses");
                }
                if (row.winsWith + row.lossesWith + row.winsAgainst + row.lossesAgainst == 0) continue;
                rows.push_back(std::move(row));
            }
        }
        return rows;
    }

    bool DejaVuImporter::Import(sqlite::Connection& db, const std::string& account, const std::vector<DejaVuEncounter>& rows,
                                std::string& error) {
        if (!db.IsOpen()) {
            error = "Database is not open.";
            return false;
        }
        if (account.empty()) {
            error = "No account selected.";
            return false;
        }
        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);
            sqlite::Statement insert(db, R"(
                INSERT OR REPLACE INTO ImportedEncounters
                    (account, primary_id, playlist_id, name, wins_with, losses_with, wins_against, losses_against, last_seen)
                VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9);
            )");
            for (const auto& row : rows) {
                if (row.primaryId == account) continue;
                insert.Bind(1, account);
                insert.Bind(2, row.primaryId);
                insert.Bind(3, row.playlistId);
                insert.Bind(4, row.name);
                insert.Bind(5, row.winsWith);
                insert.Bind(6, row.lossesWith);
                insert.Bind(7, row.winsAgainst);
                insert.Bind(8, row.lossesAgainst);
                insert.Bind(9, row.lastSeenUnix);
                insert.Execute();
            }
            SaveAnswer(db, "imported");
            tx.Commit();
            error.clear();
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

    bool DejaVuImporter::Decline(sqlite::Connection& db, std::string& error) {
        if (!db.IsOpen()) {
            error = "Database is not open.";
            return false;
        }
        try {
            SaveAnswer(db, "declined");
            error.clear();
            return true;
        } catch (const std::exception& e) {
            error = e.what();
            return false;
        }
    }

} // namespace database::repository

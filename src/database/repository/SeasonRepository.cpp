#include "SeasonRepository.hpp"
#include "core/game/RankTiers.hpp"
#include "core/accounts/TrackedAccounts.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "database/sqlite/Statement.hpp"
#include "database/sqlite/Transaction.hpp"
#include "network/mmr/MmrService.hpp"
#include <algorithm>
#include <iostream>
#include <limits>
#include <map>
#include <utility>

namespace database::repository {

    bool SeasonRepository::UpsertSeasons(sqlite::Connection& db, const std::vector<SeasonRecord>& seasons) {
        if (!db.IsOpen()) return false;
        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);
            sqlite::Statement stmt(db, R"(
            INSERT INTO Seasons(season, starts_at, ends_at, source) VALUES(?1, ?2, ?3, ?4)
            ON CONFLICT(season) DO UPDATE SET starts_at=excluded.starts_at,
                ends_at=excluded.ends_at, source=excluded.source, updated_at=unixepoch()
            WHERE excluded.source != 'observed' OR Seasons.source = 'observed';
        )");
            for (const auto& season : seasons) {
                if (season.season <= 0 || season.startsAt <= 0 || season.endsAt < season.startsAt) return false;
                stmt.Reset();
                stmt.Bind(1, season.season);
                stmt.Bind(2, season.startsAt);
                stmt.Bind(3, season.endsAt);
                stmt.Bind(4, season.source);
                stmt.Execute();
            }
            tx.Commit();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[Database] UpsertSeasons failed: " << e.what() << "\n";
            return false;
        }
    }

    std::vector<SeasonRecord> SeasonRepository::GetSeasons(sqlite::Connection& db) {
        std::vector<SeasonRecord> result;
        if (!db.IsOpen()) return result;
        try {
            sqlite::Statement stmt(db, "SELECT season, starts_at, ends_at, source FROM Seasons ORDER BY starts_at DESC, season DESC;");
            while (stmt.Step())
                result.push_back({stmt.Get<int>(0), stmt.Get<int64_t>(1), stmt.Get<int64_t>(2), stmt.Get<std::string>(3)});
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetSeasons failed: " << e.what() << "\n";
        }
        return result;
    }

    bool SeasonRepository::ObserveRankedSeason(sqlite::Connection& db, const std::string& primaryId, const std::string& playlistKey,
                                               int matchesPlayed, int64_t observedAt) {
        if (primaryId.empty() || !TrackedAccounts::IsSupportedPlaylistKey(playlistKey) ||
            playlistKey == "casual" || playlistKey == "t" || matchesPlayed < 0 || observedAt <= 0) return false;
        if (!db.IsOpen()) return false;
        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);
            sqlite::Statement previous(db, "SELECT matches_played, observed_at FROM RankedSeasonObservation WHERE primary_id=?1 AND playlist_key=?2;");
            previous.Bind(1, primaryId);
            previous.Bind(2, playlistKey);
            if (previous.Step()) {
                const int oldMatches = previous.Get<int>(0);
                const int64_t oldAt = previous.Get<int64_t>(1);
                if (observedAt <= oldAt) return true;
                if (matchesPlayed < oldMatches) {
                    sqlite::Statement latest(db, "SELECT season, starts_at, ends_at FROM Seasons WHERE starts_at<=?1 ORDER BY starts_at DESC, season DESC LIMIT 1;");
                    latest.Bind(1, observedAt);
                    if (latest.Step() && oldAt >= latest.Get<int64_t>(1)) {
                        sqlite::Statement insert(db, "INSERT OR IGNORE INTO Seasons(season, starts_at, ends_at, source) VALUES(?1, ?2, ?2, 'observed');");
                        insert.Bind(1, latest.Get<int>(0) + 1);
                        insert.Bind(2, observedAt);
                        insert.Execute();
                    }
                }
            }
            sqlite::Statement save(db, R"(
            INSERT INTO RankedSeasonObservation(primary_id, playlist_key, matches_played, observed_at)
            VALUES(?1, ?2, ?3, ?4) ON CONFLICT(primary_id, playlist_key) DO UPDATE SET
            matches_played=excluded.matches_played, observed_at=excluded.observed_at;
        )");
            save.Bind(1, primaryId);
            save.Bind(2, playlistKey);
            save.Bind(3, matchesPlayed);
            save.Bind(4, observedAt);
            save.Execute();
            tx.Commit();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[Database] ObserveRankedSeason failed: " << e.what() << "\n";
            return false;
        }
    }

    bool SeasonRepository::SaveTrackedAccountRewards(sqlite::Connection& db, const std::string& primaryId, int season, int level, int wins, int64_t observedAt) {
        if (primaryId.empty() || season <= 0 || level < 0 || wins < 0 || observedAt <= 0) return false;
        if (!db.IsOpen()) return false;
        try {
            sqlite::Statement stmt(db, R"(
            INSERT INTO TrackedAccountRewards(primary_id, season, level, wins, observed_at) VALUES(?1, ?2, ?3, ?4, ?5)
            ON CONFLICT(primary_id, season) DO UPDATE SET level=excluded.level, wins=excluded.wins, observed_at=excluded.observed_at
            WHERE excluded.observed_at >= TrackedAccountRewards.observed_at;
        )");
            stmt.Bind(1, primaryId);
            stmt.Bind(2, season);
            stmt.Bind(3, level);
            stmt.Bind(4, wins);
            stmt.Bind(5, observedAt);
            stmt.Execute();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[Database] SaveTrackedAccountRewards failed: " << e.what() << "\n";
            return false;
        }
    }

    std::optional<TrackedAccountReward> SeasonRepository::GetTrackedAccountRewards(sqlite::Connection& db, const std::string& primaryId, int season) {
        if (!db.IsOpen()) return std::nullopt;
        try {
            sqlite::Statement stmt(db, "SELECT season, level, wins, observed_at FROM TrackedAccountRewards WHERE primary_id=?1 AND season=?2;");
            stmt.Bind(1, primaryId);
            stmt.Bind(2, season);
            if (stmt.Step()) return TrackedAccountReward{stmt.Get<int>(0), stmt.Get<int>(1), stmt.Get<int>(2), stmt.Get<int64_t>(3)};
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetTrackedAccountRewards failed: " << e.what() << "\n";
        }
        return std::nullopt;
    }

    std::vector<SeasonSummary> SeasonRepository::GetSeasonSummaries(sqlite::Connection& db, const std::string& primaryId) {
        std::vector<SeasonSummary> result;
        if (!db.IsOpen()) return result;
        try {
            std::optional<sqlite::Transaction> tx;
            if (sqlite3_get_autocommit(db.Handle())) tx.emplace(db);
            sqlite::Statement seasons(db, "SELECT season, starts_at, ends_at, source FROM Seasons ORDER BY starts_at DESC, season DESC;");
            while (seasons.Step()) {
                SeasonSummary summary;
                summary.season = {seasons.Get<int>(0), seasons.Get<int64_t>(1), seasons.Get<int64_t>(2), seasons.Get<std::string>(3)};
                result.push_back(std::move(summary));
            }
            for (size_t i = 0; i < result.size(); ++i) {
                auto& summary = result[i];
                const int64_t end = i > 0 ? result[i - 1].season.startsAt
                                          : (summary.season.endsAt > summary.season.startsAt
                                                 ? summary.season.endsAt + 1
                                                 : std::numeric_limits<int64_t>::max());
                struct Rating {
                    int64_t time;
                    int mmr;
                };
                std::map<std::string, std::vector<Rating>> ratings;
                sqlite::Statement matches(db, R"(
                SELECT m.playlist_id, m.gamemode, m.win, CAST(strftime('%s',m.timestamp) AS INTEGER),
                       p.mmr, p.mmr_estimated, p.is_opponent
                FROM Matches m JOIN MatchPlayers p ON p.id = (
                    SELECT MIN(id) FROM MatchPlayers WHERE match_id=m.id AND primary_id=?1
                )
                WHERE COALESCE(m.result_pending,0)=0 AND m.win IS NOT NULL
                  AND CAST(strftime('%s',m.timestamp) AS INTEGER)>=?2
                  AND CAST(strftime('%s',m.timestamp) AS INTEGER)<?3
                ORDER BY m.timestamp, m.id;
            )");
                matches.Bind(1, primaryId);
                matches.Bind(2, summary.season.startsAt);
                matches.Bind(3, end);
                while (matches.Step()) {
                    std::string key;
                    if (!matches.IsNull(0)) {
                        const auto* info = PlaylistMetadata::Find(matches.Get<int>(0));
                        if (!info || info->playlistClass != PlaylistMetadata::PlaylistClass::Ranked) continue;
                        key = info->mmrKey;
                    } else {
                        key = matches.Get<std::string>(1);
                        if (!TrackedAccounts::IsSupportedPlaylistKey(key) || key == "casual" || key == "t") continue;
                    }
                    if (key.empty()) continue;
                    const bool won = (matches.Get<int>(2) != 0) != (matches.Get<int>(6) != 0);
                    auto& playlist = summary.playlists[key];
                    if (won) {
                        ++summary.wins;
                        ++playlist.wins;
                    } else {
                        ++summary.losses;
                        ++playlist.losses;
                    }
                    const int mmr = matches.Get<int>(4);
                    if (mmr > 0 && matches.Get<int>(5) == 0) {
                        ratings[key].push_back({matches.Get<int64_t>(3), mmr});
                        if (mmr > summary.peakMmr) {
                            summary.peakMmr = mmr;
                            summary.peakRank = RankTiers::GetRankTierForPlaylistMmr(key, mmr);
                            summary.peakPlaylist = key;
                        }
                    }
                }
                sqlite::Statement observed(db, R"(
                SELECT playlist_key, mmr, COALESCE(rank_tier,''), observed_at
                FROM TrackedAccountRankObservations WHERE primary_id=?1 AND observed_at>=?2 AND observed_at<?3
                ORDER BY observed_at, id;
            )");
                observed.Bind(1, primaryId);
                observed.Bind(2, summary.season.startsAt);
                observed.Bind(3, end);
                while (observed.Step()) {
                    const std::string key = observed.Get<std::string>(0);
                    if (!TrackedAccounts::IsSupportedPlaylistKey(key) || key == "casual" || key == "t") continue;
                    const int mmr = observed.Get<int>(1);
                    ratings[key].push_back({observed.Get<int64_t>(3), mmr});
                    if (mmr > summary.peakMmr || (mmr == summary.peakMmr && summary.peakRank.empty())) {
                        summary.peakMmr = mmr;
                        summary.peakRank = observed.Get<std::string>(2);
                        summary.peakPlaylist = key;
                    }
                }
                if (summary.peakMmr > 0 && summary.peakRank.empty()) {
                    summary.peakRank = RankTiers::GetRankTierForPlaylistMmr(summary.peakPlaylist, summary.peakMmr);
                }
                for (auto& [key, points] : ratings) {
                    std::stable_sort(points.begin(), points.end(), [](const Rating& a, const Rating& b) { return a.time < b.time; });
                    if (points.size() < 2) continue;
                    const int change = points.back().mmr - points.front().mmr;
                    summary.mmrChangeByPlaylist[key] = change;
                    summary.playlists[key].mmrChange = change;
                    summary.totalMmrChange += change;
                }
                const int games = summary.wins + summary.losses;
                if (games) summary.winRate = static_cast<double>(summary.wins) / games;
            }
            if (tx) tx->Commit();
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetSeasonSummaries failed: " << e.what() << "\n";
            return {};
        }
        return result;
    }

} // namespace database::repository

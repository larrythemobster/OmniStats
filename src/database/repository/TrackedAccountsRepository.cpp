#include "TrackedAccountsRepository.hpp"
#include "database/sqlite/Statement.hpp"
#include "database/sqlite/Transaction.hpp"
#include <chrono>
#include <iostream>
#include <set>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace database::repository {

    namespace {

        int64_t NowUnix() {
            return std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                .count();
        }

    } // namespace

    TrackedAccountInsertResult TrackedAccountsRepository::RegisterTrackedAccount(sqlite::Connection& db, const TrackedAccounts::AccountRecord& record) {
        if (record.primaryId.empty() || !db.IsOpen() || !db.TableExists("TrackedAccounts")) {
            return TrackedAccountInsertResult::Failed;
        }

        TrackedAccounts::Platform platform = record.platform;
        std::string platformAccountId = record.platformAccountId;
        if (platformAccountId.empty()) {
            if (auto id = TrackedAccounts::FromPrimaryId(record.primaryId)) {
                platform = id->platform;
                platformAccountId = id->accountId;
            }
        }
        if (platformAccountId.empty()) {
            return TrackedAccountInsertResult::Failed;
        }

        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);

            const char* existsSql =
                "SELECT 1 FROM TrackedAccounts WHERE primary_id = ?1 OR (platform = ?2 AND platform_account_id = ?3) LIMIT 1;";
            sqlite::Statement checkStmt(db, existsSql);
            checkStmt.Bind(1, record.primaryId);
            checkStmt.Bind(2, TrackedAccounts::PlatformKey(platform));
            checkStmt.Bind(3, platformAccountId);
            if (checkStmt.Step()) {
                tx.Commit();
                return TrackedAccountInsertResult::AlreadyExists;
            }

            int64_t sortOrder = record.sortOrder;
            if (sortOrder <= 0) {
                const char* maxSql = "SELECT COALESCE(MAX(sort_order), 0) + 1 FROM TrackedAccounts;";
                sqlite::Statement maxStmt(db, maxSql);
                if (maxStmt.Step()) {
                    sortOrder = maxStmt.Get<int64_t>(0);
                }
                if (sortOrder <= 0) sortOrder = 1;
            }

            const int64_t createdAt = record.createdAtUnix > 0 ? record.createdAtUnix : NowUnix();

            const char* insSql = R"(
                INSERT INTO TrackedAccounts (
                    primary_id, platform, platform_account_id, nickname, last_known_name,
                    created_at, last_seen_at, last_refresh_attempt_at, last_successful_refresh_at, sort_order, discovery_source
                ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);
            )";
            sqlite::Statement insStmt(db, insSql);
            insStmt.Bind(1, record.primaryId);
            insStmt.Bind(2, TrackedAccounts::PlatformKey(platform));
            insStmt.Bind(3, platformAccountId);
            insStmt.Bind(4, record.nickname);
            if (record.lastKnownName.empty())
                insStmt.BindNull(5);
            else
                insStmt.Bind(5, record.lastKnownName);
            insStmt.Bind(6, createdAt);
            if (record.lastSeenAtUnix <= 0)
                insStmt.BindNull(7);
            else
                insStmt.Bind(7, record.lastSeenAtUnix);
            if (record.lastRefreshAttemptAtUnix <= 0)
                insStmt.BindNull(8);
            else
                insStmt.Bind(8, record.lastRefreshAttemptAtUnix);
            if (record.lastSuccessfulRefreshAtUnix <= 0)
                insStmt.BindNull(9);
            else
                insStmt.Bind(9, record.lastSuccessfulRefreshAtUnix);
            insStmt.Bind(10, sortOrder);
            insStmt.Bind(11, TrackedAccounts::DiscoverySourceKey(record.discoverySource));
            insStmt.Execute();

            if (!record.ranks.empty() && db.TableExists("TrackedAccountRanks")) {
                const char* rankSql = R"(
                    INSERT INTO TrackedAccountRanks
                    (primary_id, playlist_key, mmr, rank_tier, matches_played, source, fetched_at)
                    VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
                    ON CONFLICT(primary_id, playlist_key) DO UPDATE SET
                    mmr = excluded.mmr, rank_tier = excluded.rank_tier, matches_played = excluded.matches_played,
                    source = excluded.source, fetched_at = excluded.fetched_at;
                )";
                sqlite::Statement rankStmt(db, rankSql);
                for (const auto& [key, rank] : record.ranks) {
                    rankStmt.Reset();
                    rankStmt.Bind(1, record.primaryId);
                    const std::string& playlistKey = rank.playlistKey.empty() ? key : rank.playlistKey;
                    rankStmt.Bind(2, playlistKey);
                    rankStmt.Bind(3, rank.mmr);
                    rankStmt.Bind(4, rank.rankTier.empty() ? std::nullopt : std::optional{rank.rankTier});
                    rankStmt.Bind(5, rank.matchesPlayed);
                    rankStmt.Bind(6, rank.source.empty() ? std::nullopt : std::optional{rank.source});
                    rankStmt.Bind(7, rank.fetchedAtUnix > 0 ? rank.fetchedAtUnix : createdAt);
                    rankStmt.Execute();
                }

                if (db.TableExists("TrackedAccountRankObservations")) {
                    const char* obsSql = R"(
                        INSERT OR IGNORE INTO TrackedAccountRankObservations
                        (primary_id, playlist_key, mmr, rank_tier, matches_played, source, observed_at)
                        VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7);
                    )";
                    sqlite::Statement obsStmt(db, obsSql);
                    for (const auto& [key, rank] : record.ranks) {
                        if (!rank.mmr || *rank.mmr <= 0 || rank.fetchedAtUnix <= 0) continue;
                        const std::string& playlistKey = rank.playlistKey.empty() ? key : rank.playlistKey;
                        if (!TrackedAccounts::IsSupportedPlaylistKey(playlistKey)) continue;
                        obsStmt.Reset();
                        obsStmt.Bind(1, record.primaryId);
                        obsStmt.Bind(2, playlistKey);
                        obsStmt.Bind(3, *rank.mmr);
                        obsStmt.Bind(4, rank.rankTier.empty() ? std::nullopt : std::optional{rank.rankTier});
                        obsStmt.Bind(5, rank.matchesPlayed);
                        obsStmt.Bind(6, rank.source.empty() ? std::nullopt : std::optional{rank.source});
                        obsStmt.Bind(7, rank.fetchedAtUnix);
                        obsStmt.Execute();
                    }
                }
            }

            tx.Commit();
            return TrackedAccountInsertResult::Inserted;
        } catch (const std::exception& e) {
            std::cerr << "[Database] RegisterTrackedAccount failed: " << e.what() << "\n";
            return TrackedAccountInsertResult::Failed;
        }
    }

    std::vector<TrackedAccounts::AccountRecord> TrackedAccountsRepository::ListTrackedAccounts(sqlite::Connection& db) {
        std::vector<TrackedAccounts::AccountRecord> records;
        if (!db.IsOpen() || !db.TableExists("TrackedAccounts")) return records;

        const char* sql = R"(
            SELECT primary_id, platform, platform_account_id, nickname, last_known_name,
                   created_at, last_seen_at, last_refresh_attempt_at, last_successful_refresh_at, sort_order, discovery_source
            FROM TrackedAccounts
            ORDER BY sort_order ASC, created_at ASC, primary_id ASC;
        )";

        try {
            sqlite::Statement stmt(db, sql);
            while (stmt.Step()) {
                TrackedAccounts::AccountRecord rec;
                rec.primaryId = stmt.Get<std::string>(0);
                rec.platform = TrackedAccounts::ParsePlatform(stmt.Get<std::string>(1)).value_or(TrackedAccounts::Platform::Epic);
                rec.platformAccountId = stmt.Get<std::string>(2);
                if (!stmt.IsNull(3)) rec.nickname = stmt.Get<std::string>(3);
                if (!stmt.IsNull(4)) rec.lastKnownName = stmt.Get<std::string>(4);
                rec.createdAtUnix = stmt.Get<int64_t>(5);
                if (!stmt.IsNull(6)) rec.lastSeenAtUnix = stmt.Get<int64_t>(6);
                if (!stmt.IsNull(7)) rec.lastRefreshAttemptAtUnix = stmt.Get<int64_t>(7);
                if (!stmt.IsNull(8)) rec.lastSuccessfulRefreshAtUnix = stmt.Get<int64_t>(8);
                rec.sortOrder = stmt.Get<int64_t>(9);
                rec.discoverySource = TrackedAccounts::ParseDiscoverySource(stmt.Get<std::string>(10));
                records.push_back(std::move(rec));
            }

            if (!records.empty() && db.TableExists("TrackedAccountRanks")) {
                const char* rankSql = R"(
                SELECT primary_id, playlist_key, mmr, rank_tier, matches_played, source, fetched_at
                FROM TrackedAccountRanks;
            )";
                sqlite::Statement rankStmt(db, rankSql);
                std::unordered_map<std::string, std::map<std::string, TrackedAccounts::PlaylistRank>> rankMap;
                while (rankStmt.Step()) {
                    std::string pid = rankStmt.Get<std::string>(0);
                    TrackedAccounts::PlaylistRank rank;
                    rank.playlistKey = rankStmt.Get<std::string>(1);
                    if (!rankStmt.IsNull(2)) rank.mmr = rankStmt.Get<int>(2);
                    if (!rankStmt.IsNull(3)) rank.rankTier = rankStmt.Get<std::string>(3);
                    if (!rankStmt.IsNull(4)) rank.matchesPlayed = rankStmt.Get<int>(4);
                    if (!rankStmt.IsNull(5)) rank.source = rankStmt.Get<std::string>(5);
                    if (!rankStmt.IsNull(6)) rank.fetchedAtUnix = rankStmt.Get<int64_t>(6);
                    rankMap[pid][rank.playlistKey] = std::move(rank);
                }
                for (auto& rec : records) {
                    if (auto it = rankMap.find(rec.primaryId); it != rankMap.end()) {
                        rec.ranks = std::move(it->second);
                    }
                }
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] ListTrackedAccounts failed: " << e.what() << "\n";
        }
        return records;
    }

    std::optional<TrackedAccounts::AccountRecord> TrackedAccountsRepository::GetTrackedAccount(sqlite::Connection& db, const std::string& primaryId) {
        if (primaryId.empty() || !db.IsOpen() || !db.TableExists("TrackedAccounts")) return std::nullopt;

        const char* sql = R"(
            SELECT primary_id, platform, platform_account_id, nickname, last_known_name,
                   created_at, last_seen_at, last_refresh_attempt_at, last_successful_refresh_at, sort_order, discovery_source
            FROM TrackedAccounts
            WHERE primary_id = ?1
            LIMIT 1;
        )";

        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, primaryId);
            if (!stmt.Step()) return std::nullopt;

            TrackedAccounts::AccountRecord rec;
            rec.primaryId = stmt.Get<std::string>(0);
            rec.platform = TrackedAccounts::ParsePlatform(stmt.Get<std::string>(1)).value_or(TrackedAccounts::Platform::Epic);
            rec.platformAccountId = stmt.Get<std::string>(2);
            if (!stmt.IsNull(3)) rec.nickname = stmt.Get<std::string>(3);
            if (!stmt.IsNull(4)) rec.lastKnownName = stmt.Get<std::string>(4);
            rec.createdAtUnix = stmt.Get<int64_t>(5);
            if (!stmt.IsNull(6)) rec.lastSeenAtUnix = stmt.Get<int64_t>(6);
            if (!stmt.IsNull(7)) rec.lastRefreshAttemptAtUnix = stmt.Get<int64_t>(7);
            if (!stmt.IsNull(8)) rec.lastSuccessfulRefreshAtUnix = stmt.Get<int64_t>(8);
            rec.sortOrder = stmt.Get<int64_t>(9);
            rec.discoverySource = TrackedAccounts::ParseDiscoverySource(stmt.Get<std::string>(10));
            if (db.TableExists("TrackedAccountRanks")) {
                rec.ranks = GetTrackedAccountRanks(db, primaryId);
            }
            return rec;
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetTrackedAccount failed: " << e.what() << "\n";
            return std::nullopt;
        }
    }

    bool TrackedAccountsRepository::UpdateTrackedAccountNickname(sqlite::Connection& db, const std::string& primaryId, const std::string& nickname) {
        if (primaryId.empty() || !db.IsOpen() || !db.TableExists("TrackedAccounts")) return false;

        try {
            const char* sql = "UPDATE TrackedAccounts SET nickname = ?1 WHERE primary_id = ?2;";
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, nickname);
            stmt.Bind(2, primaryId);
            stmt.Execute();
            return db.Changes() > 0;
        } catch (...) {
            return false;
        }
    }

    bool TrackedAccountsRepository::UpdateTrackedAccountLastSeen(sqlite::Connection& db, const std::string& primaryId, const std::string& lastKnownName, int64_t seenAtUnix) {
        if (primaryId.empty() || !db.IsOpen() || !db.TableExists("TrackedAccounts")) return false;

        try {
            if (!lastKnownName.empty()) {
                sqlite::Statement stmt(db, "UPDATE TrackedAccounts SET last_seen_at = ?1, last_known_name = ?2 WHERE primary_id = ?3;");
                stmt.Bind(1, seenAtUnix);
                stmt.Bind(2, lastKnownName);
                stmt.Bind(3, primaryId);
                stmt.Execute();
            } else {
                sqlite::Statement stmt(db, "UPDATE TrackedAccounts SET last_seen_at = ?1 WHERE primary_id = ?2;");
                stmt.Bind(1, seenAtUnix);
                stmt.Bind(2, primaryId);
                stmt.Execute();
            }
            return db.Changes() > 0;
        } catch (...) {
            return false;
        }
    }

    bool TrackedAccountsRepository::UpdateTrackedAccountRefreshAttempt(sqlite::Connection& db, const std::string& primaryId, int64_t attemptedAtUnix) {
        if (primaryId.empty() || !db.IsOpen() || !db.TableExists("TrackedAccounts")) return false;

        try {
            const char* sql = "UPDATE TrackedAccounts SET last_refresh_attempt_at = ?1 WHERE primary_id = ?2;";
            sqlite::Statement stmt(db, sql);
            if (attemptedAtUnix <= 0)
                stmt.BindNull(1);
            else
                stmt.Bind(1, attemptedAtUnix);
            stmt.Bind(2, primaryId);
            stmt.Execute();
            return db.Changes() > 0;
        } catch (...) {
            return false;
        }
    }

    bool TrackedAccountsRepository::UpdateTrackedAccountLastKnownName(sqlite::Connection& db, const std::string& primaryId, const std::string& lastKnownName) {
        if (primaryId.empty() || lastKnownName.empty() || !db.IsOpen() || !db.TableExists("TrackedAccounts")) return false;

        try {
            const char* sql = "UPDATE TrackedAccounts SET last_known_name = ?1 WHERE primary_id = ?2;";
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, lastKnownName);
            stmt.Bind(2, primaryId);
            stmt.Execute();
            return db.Changes() > 0;
        } catch (...) {
            return false;
        }
    }

    std::optional<std::string> TrackedAccountsRepository::FindLatestPlayerName(sqlite::Connection& db, const std::string& primaryId) {
        if (primaryId.empty() || !db.IsOpen()) return std::nullopt;

        try {
            const char* sql = R"(
                SELECT mp.name
                FROM MatchPlayers mp
                JOIN Matches m ON m.id = mp.match_id
                WHERE (lower(mp.primary_id) = lower(?1) OR lower(mp.primary_id) LIKE lower(?2))
                  AND mp.name IS NOT NULL AND mp.name != ''
                ORDER BY m.timestamp DESC, m.id DESC
                LIMIT 1;
            )";
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, primaryId);
            stmt.Bind(2, primaryId + "|%");
            if (stmt.Step()) {
                return stmt.Get<std::string>(0);
            }
        } catch (...) {
        }
        return std::nullopt;
    }

    bool TrackedAccountsRepository::SaveTrackedAccountRanks(sqlite::Connection& db, const std::string& primaryId,
                                                            const std::vector<TrackedAccounts::PlaylistRank>& ranks,
                                                            int64_t fetchedAtUnix) {
        if (primaryId.empty() || !db.IsOpen() || !db.TableExists("TrackedAccounts") || !db.TableExists("TrackedAccountRanks")) {
            return false;
        }

        int64_t effectiveFetched = fetchedAtUnix > 0 ? fetchedAtUnix : NowUnix();

        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);

            const char* checkSql = "SELECT 1 FROM TrackedAccounts WHERE primary_id = ?1 LIMIT 1;";
            sqlite::Statement checkStmt(db, checkSql);
            checkStmt.Bind(1, primaryId);
            if (!checkStmt.Step()) {
                return false;
            }

            const char* updSql = R"(
                UPDATE TrackedAccounts SET
                    last_refresh_attempt_at = MAX(COALESCE(last_refresh_attempt_at, 0), ?1),
                    last_successful_refresh_at = MAX(COALESCE(last_successful_refresh_at, 0), ?1)
                WHERE primary_id = ?2;
            )";
            sqlite::Statement updStmt(db, updSql);
            updStmt.Bind(1, effectiveFetched);
            updStmt.Bind(2, primaryId);
            updStmt.Execute();

            if (!ranks.empty()) {
                const char* rankSql = R"(
                    INSERT INTO TrackedAccountRanks
                    (primary_id, playlist_key, mmr, rank_tier, matches_played, source, fetched_at)
                    VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
                    ON CONFLICT(primary_id, playlist_key) DO UPDATE SET
                    mmr = excluded.mmr, rank_tier = excluded.rank_tier, matches_played = excluded.matches_played,
                    source = excluded.source, fetched_at = excluded.fetched_at
                    WHERE excluded.fetched_at >= TrackedAccountRanks.fetched_at;
                )";
                sqlite::Statement rankStmt(db, rankSql);
                for (const auto& rank : ranks) {
                    if (rank.playlistKey.empty()) continue;
                    rankStmt.Reset();
                    rankStmt.Bind(1, primaryId);
                    rankStmt.Bind(2, rank.playlistKey);
                    rankStmt.Bind(3, rank.mmr);
                    rankStmt.Bind(4, rank.rankTier.empty() ? std::nullopt : std::optional{rank.rankTier});
                    rankStmt.Bind(5, rank.matchesPlayed);
                    rankStmt.Bind(6, rank.source.empty() ? std::nullopt : std::optional{rank.source});
                    rankStmt.Bind(7, rank.fetchedAtUnix <= 0 ? effectiveFetched : rank.fetchedAtUnix);
                    rankStmt.Execute();
                }

                if (db.TableExists("TrackedAccountRankObservations")) {
                    const char* observeSql = R"(
                    INSERT OR IGNORE INTO TrackedAccountRankObservations
                    (primary_id, playlist_key, mmr, rank_tier, matches_played, source, observed_at)
                    SELECT ?1, ?2, ?3, ?4, ?5, ?6, ?7
                    WHERE NOT EXISTS (
                        SELECT 1 FROM TrackedAccountRankObservations
                        WHERE primary_id=?1 AND playlist_key=?2 AND mmr=?3
                          AND COALESCE(rank_tier,'')=COALESCE(?4,'')
                          AND COALESCE(source,'')=COALESCE(?6,'')
                          AND observed_at BETWEEN ?7 - 21600 AND ?7
                    );
                )";
                    sqlite::Statement observeStmt(db, observeSql);
                    for (const auto& rank : ranks) {
                        if (!rank.mmr || *rank.mmr <= 0 || !TrackedAccounts::IsSupportedPlaylistKey(rank.playlistKey)) continue;
                        const int64_t observedAt = rank.fetchedAtUnix > 0 ? rank.fetchedAtUnix : effectiveFetched;
                        if (observedAt <= 0) continue;
                        observeStmt.Reset();
                        observeStmt.Bind(1, primaryId);
                        observeStmt.Bind(2, rank.playlistKey);
                        observeStmt.Bind(3, *rank.mmr);
                        if (rank.rankTier.empty())
                            observeStmt.BindNull(4);
                        else
                            observeStmt.Bind(4, rank.rankTier);
                        if (rank.matchesPlayed)
                            observeStmt.Bind(5, *rank.matchesPlayed);
                        else
                            observeStmt.BindNull(5);
                        if (rank.source.empty())
                            observeStmt.BindNull(6);
                        else
                            observeStmt.Bind(6, rank.source);
                        observeStmt.Bind(7, observedAt);
                        observeStmt.Execute();
                    }
                }
            }

            tx.Commit();
            return true;
        } catch (const std::exception& e) {
            std::cerr << "[Database] SaveTrackedAccountRanks failed: " << e.what() << "\n";
            return false;
        }
    }

    std::map<std::string, TrackedAccounts::PlaylistRank> TrackedAccountsRepository::GetTrackedAccountRanks(sqlite::Connection& db, const std::string& primaryId) {
        std::map<std::string, TrackedAccounts::PlaylistRank> ranks;
        if (primaryId.empty() || !db.IsOpen() || !db.TableExists("TrackedAccountRanks")) return ranks;

        const char* sql =
            "SELECT playlist_key, mmr, rank_tier, matches_played, source, fetched_at "
            "FROM TrackedAccountRanks WHERE primary_id = ?1;";
        try {
            sqlite::Statement stmt(db, sql);
            stmt.Bind(1, primaryId);
            while (stmt.Step()) {
                TrackedAccounts::PlaylistRank rank;
                rank.playlistKey = stmt.Get<std::string>(0);
                if (!stmt.IsNull(1)) rank.mmr = stmt.Get<int>(1);
                if (!stmt.IsNull(2)) rank.rankTier = stmt.Get<std::string>(2);
                if (!stmt.IsNull(3)) rank.matchesPlayed = stmt.Get<int>(3);
                if (!stmt.IsNull(4)) rank.source = stmt.Get<std::string>(4);
                if (!stmt.IsNull(5)) rank.fetchedAtUnix = stmt.Get<int64_t>(5);
                ranks[rank.playlistKey] = std::move(rank);
            }
        } catch (...) {
        }
        return ranks;
    }

    std::vector<TrackedAccounts::RankHistorySeries> TrackedAccountsRepository::GetTrackedAccountRankHistory(
        sqlite::Connection& db, const std::vector<std::string>& primaryIds, const std::string& playlistKey, int64_t sinceUnix) {
        std::vector<TrackedAccounts::RankHistorySeries> result;
        if (!TrackedAccounts::IsSupportedPlaylistKey(playlistKey) || !db.IsOpen() || !db.TableExists("TrackedAccountRankObservations")) {
            return result;
        }

        constexpr size_t kMaxAccounts = 5;
        const char* statsSql =
            "SELECT MAX(mmr), COUNT(*) FROM TrackedAccountRankObservations "
            "WHERE primary_id=?1 AND playlist_key=?2;";
        const char* samplesSql =
            "WITH ordered AS ("
            " SELECT mmr,rank_tier,matches_played,source,observed_at,id,"
            " ROW_NUMBER() OVER (ORDER BY observed_at,id) AS rn,"
            " COUNT(*) OVER () AS n"
            " FROM TrackedAccountRankObservations"
            " WHERE primary_id=?1 AND playlist_key=?2 AND observed_at>=?3"
            ") SELECT mmr,rank_tier,matches_played,source,observed_at"
            " FROM ordered WHERE rn=1 OR rn=n OR (rn-1) % MAX(1,(n+238)/239)=0"
            " ORDER BY observed_at,id LIMIT 242;";

        try {
            sqlite::Statement stats(db, statsSql);
            sqlite::Statement samples(db, samplesSql);

            std::set<std::string> unique;
            for (const auto& id : primaryIds) {
                if (result.size() >= kMaxAccounts) break;
                const auto canonical = TrackedAccounts::CanonicalPrimaryId(id);
                if (canonical.empty() || !unique.insert(canonical).second) continue;

                TrackedAccounts::RankHistorySeries series;
                series.primaryId = canonical;

                stats.Reset();
                stats.Bind(1, canonical);
                stats.Bind(2, playlistKey);
                if (stats.Step()) {
                    if (!stats.IsNull(0)) series.highestObservedMmr = stats.Get<int>(0);
                    series.totalObservations = stats.Get<int64_t>(1);
                }

                samples.Reset();
                samples.Bind(1, canonical);
                samples.Bind(2, playlistKey);
                samples.Bind(3, (std::max)(int64_t{0}, sinceUnix));
                while (samples.Step()) {
                    TrackedAccounts::RankObservation item;
                    item.primaryId = canonical;
                    item.playlistKey = playlistKey;
                    item.mmr = samples.Get<int>(0);
                    item.rankTier = samples.Get<std::string>(1);
                    if (!samples.IsNull(2)) item.matchesPlayed = samples.Get<int>(2);
                    item.source = samples.Get<std::string>(3);
                    item.observedAtUnix = samples.Get<int64_t>(4);
                    series.observations.push_back(std::move(item));
                }
                result.push_back(std::move(series));
            }
        } catch (const std::exception& e) {
            std::cerr << "[Database] GetTrackedAccountRankHistory failed: " << e.what() << "\n";
        }
        return result;
    }

    bool TrackedAccountsRepository::DeleteTrackedAccount(sqlite::Connection& db, const std::string& primaryId) {
        if (primaryId.empty() || !db.IsOpen() || !db.TableExists("TrackedAccounts")) return false;

        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);

            if (db.TableExists("TrackedAccountRanks")) {
                sqlite::Statement rStmt(db, "DELETE FROM TrackedAccountRanks WHERE primary_id = ?1;");
                rStmt.Bind(1, primaryId);
                rStmt.Execute();
            }

            sqlite::Statement accStmt(db, "DELETE FROM TrackedAccounts WHERE primary_id = ?1;");
            accStmt.Bind(1, primaryId);
            accStmt.Execute();

            int changes = db.Changes();
            tx.Commit();
            return changes > 0;
        } catch (const std::exception& e) {
            std::cerr << "[Database] DeleteTrackedAccount failed: " << e.what() << "\n";
            return false;
        }
    }

    int TrackedAccountsRepository::ImportTrackedAccounts(sqlite::Connection& db, const std::vector<TrackedAccounts::AccountRecord>& records) {
        if (records.empty() || !db.IsOpen() || !db.TableExists("TrackedAccounts")) return 0;

        try {
            sqlite::Transaction tx(db, sqlite::TransactionType::Immediate);

            std::unordered_set<std::string> knownPrimaryIds;
            std::set<std::pair<std::string, std::string>> knownPlatformAccounts;
            sqlite::Statement existStmt(db, "SELECT primary_id, platform, platform_account_id FROM TrackedAccounts;");
            while (existStmt.Step()) {
                knownPrimaryIds.insert(existStmt.Get<std::string>(0));
                knownPlatformAccounts.insert({existStmt.Get<std::string>(1), existStmt.Get<std::string>(2)});
            }

            int64_t maxSortOrder = 0;
            sqlite::Statement maxStmt(db, "SELECT COALESCE(MAX(sort_order), 0) FROM TrackedAccounts;");
            if (maxStmt.Step()) {
                maxSortOrder = maxStmt.Get<int64_t>(0);
            }

            int64_t now = NowUnix();
            const char* insSql = R"(
                INSERT INTO TrackedAccounts (
                    primary_id, platform, platform_account_id, nickname, last_known_name,
                    created_at, last_seen_at, last_refresh_attempt_at, last_successful_refresh_at, sort_order, discovery_source
                ) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10, ?11);
            )";
            sqlite::Statement insStmt(db, insSql);

            const bool hasRanks = db.TableExists("TrackedAccountRanks");
            const char* rankSql = R"(
                INSERT INTO TrackedAccountRanks
                (primary_id, playlist_key, mmr, rank_tier, matches_played, source, fetched_at)
                VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)
                ON CONFLICT(primary_id, playlist_key) DO UPDATE SET
                mmr = excluded.mmr, rank_tier = excluded.rank_tier, matches_played = excluded.matches_played,
                source = excluded.source, fetched_at = excluded.fetched_at;
            )";
            std::unique_ptr<sqlite::Statement> rankStmt;
            if (hasRanks) {
                rankStmt = std::make_unique<sqlite::Statement>(db, rankSql);
            }

            int importedCount = 0;
            for (const auto& rec : records) {
                if (rec.primaryId.empty()) continue;
                TrackedAccounts::Platform platform = rec.platform;
                std::string platformAccountId = rec.platformAccountId;
                if (platformAccountId.empty()) {
                    if (auto id = TrackedAccounts::FromPrimaryId(rec.primaryId)) {
                        platform = id->platform;
                        platformAccountId = id->accountId;
                    }
                }
                if (platformAccountId.empty()) continue;

                const std::string platKey = TrackedAccounts::PlatformKey(platform);
                if (knownPrimaryIds.count(rec.primaryId) > 0 ||
                    knownPlatformAccounts.count({platKey, platformAccountId}) > 0) {
                    continue;
                }

                int64_t sortOrder = rec.sortOrder > 0 ? rec.sortOrder : ++maxSortOrder;
                if (sortOrder > maxSortOrder) {
                    maxSortOrder = sortOrder;
                }
                const int64_t createdAt = rec.createdAtUnix > 0 ? rec.createdAtUnix : now;

                insStmt.Reset();
                insStmt.Bind(1, rec.primaryId);
                insStmt.Bind(2, platKey);
                insStmt.Bind(3, platformAccountId);
                insStmt.Bind(4, rec.nickname);
                if (rec.lastKnownName.empty())
                    insStmt.BindNull(5);
                else
                    insStmt.Bind(5, rec.lastKnownName);
                insStmt.Bind(6, createdAt);
                if (rec.lastSeenAtUnix <= 0)
                    insStmt.BindNull(7);
                else
                    insStmt.Bind(7, rec.lastSeenAtUnix);
                if (rec.lastRefreshAttemptAtUnix <= 0)
                    insStmt.BindNull(8);
                else
                    insStmt.Bind(8, rec.lastRefreshAttemptAtUnix);
                if (rec.lastSuccessfulRefreshAtUnix <= 0)
                    insStmt.BindNull(9);
                else
                    insStmt.Bind(9, rec.lastSuccessfulRefreshAtUnix);
                insStmt.Bind(10, sortOrder);
                insStmt.Bind(11, TrackedAccounts::DiscoverySourceKey(rec.discoverySource));
                insStmt.Execute();

                knownPrimaryIds.insert(rec.primaryId);
                knownPlatformAccounts.insert({platKey, platformAccountId});
                ++importedCount;

                if (rankStmt && !rec.ranks.empty()) {
                    for (const auto& [key, rank] : rec.ranks) {
                        rankStmt->Reset();
                        rankStmt->Bind(1, rec.primaryId);
                        const std::string& playlistKey = rank.playlistKey.empty() ? key : rank.playlistKey;
                        rankStmt->Bind(2, playlistKey);
                        rankStmt->Bind(3, rank.mmr);
                        rankStmt->Bind(4, rank.rankTier.empty() ? std::nullopt : std::optional{rank.rankTier});
                        rankStmt->Bind(5, rank.matchesPlayed);
                        rankStmt->Bind(6, rank.source.empty() ? std::nullopt : std::optional{rank.source});
                        rankStmt->Bind(7, rank.fetchedAtUnix > 0 ? rank.fetchedAtUnix : createdAt);
                        rankStmt->Execute();
                    }
                }
            }

            tx.Commit();
            return importedCount;
        } catch (const std::exception& e) {
            std::cerr << "[Database] ImportTrackedAccounts failed: " << e.what() << "\n";
            return 0;
        }
    }

} // namespace database::repository

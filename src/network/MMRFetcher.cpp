#include "MMRFetcher.hpp"
#include "CurlImpersonate.hpp"
#include "core/Config.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PrivacyLog.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <shared_mutex>
#include <array>
#include <initializer_list>
#include <map>

namespace {

    struct TournamentRankThreshold {
        int minMmr;
        const char* tier;
        const char* division;
    };

    static constexpr std::array<TournamentRankThreshold, 85> TournamentRankThresholds = {{{-4, "Bronze I", "Div I"},
                                                                                          {119, "Bronze I", "Div II"},
                                                                                          {138, "Bronze I", "Div III"},
                                                                                          {157, "Bronze I", "Div IV"},
                                                                                          {162, "Bronze II", "Div I"},
                                                                                          {179, "Bronze II", "Div II"},
                                                                                          {198, "Bronze II", "Div III"},
                                                                                          {217, "Bronze II", "Div IV"},
                                                                                          {221, "Bronze III", "Div I"},
                                                                                          {239, "Bronze III", "Div II"},
                                                                                          {258, "Bronze III", "Div III"},
                                                                                          {277, "Bronze III", "Div IV"},
                                                                                          {283, "Silver I", "Div I"},
                                                                                          {299, "Silver I", "Div II"},
                                                                                          {318, "Silver I", "Div III"},
                                                                                          {337, "Silver I", "Div IV"},
                                                                                          {342, "Silver II", "Div I"},
                                                                                          {359, "Silver II", "Div II"},
                                                                                          {378, "Silver II", "Div III"},
                                                                                          {397, "Silver II", "Div IV"},
                                                                                          {403, "Silver III", "Div I"},
                                                                                          {419, "Silver III", "Div II"},
                                                                                          {438, "Silver III", "Div III"},
                                                                                          {457, "Silver III", "Div IV"},
                                                                                          {461, "Gold I", "Div I"},
                                                                                          {478, "Gold I", "Div II"},
                                                                                          {498, "Gold I", "Div III"},
                                                                                          {517, "Gold I", "Div IV"},
                                                                                          {521, "Gold II", "Div I"},
                                                                                          {539, "Gold II", "Div II"},
                                                                                          {558, "Gold II", "Div III"},
                                                                                          {577, "Gold II", "Div IV"},
                                                                                          {581, "Gold III", "Div I"},
                                                                                          {598, "Gold III", "Div II"},
                                                                                          {618, "Gold III", "Div III"},
                                                                                          {637, "Gold III", "Div IV"},
                                                                                          {641, "Platinum I", "Div I"},
                                                                                          {659, "Platinum I", "Div II"},
                                                                                          {678, "Platinum I", "Div III"},
                                                                                          {697, "Platinum I", "Div IV"},
                                                                                          {701, "Platinum II", "Div I"},
                                                                                          {719, "Platinum II", "Div II"},
                                                                                          {738, "Platinum II", "Div III"},
                                                                                          {757, "Platinum II", "Div IV"},
                                                                                          {761, "Platinum III", "Div I"},
                                                                                          {779, "Platinum III", "Div II"},
                                                                                          {798, "Platinum III", "Div III"},
                                                                                          {817, "Platinum III", "Div IV"},
                                                                                          {824, "Diamond I", "Div I"},
                                                                                          {844, "Diamond I", "Div II"},
                                                                                          {868, "Diamond I", "Div III"},
                                                                                          {892, "Diamond I", "Div IV"},
                                                                                          {901, "Diamond II", "Div I"},
                                                                                          {924, "Diamond II", "Div II"},
                                                                                          {948, "Diamond II", "Div III"},
                                                                                          {972, "Diamond II", "Div IV"},
                                                                                          {981, "Diamond III", "Div I"},
                                                                                          {1004, "Diamond III", "Div II"},
                                                                                          {1028, "Diamond III", "Div III"},
                                                                                          {1052, "Diamond III", "Div IV"},
                                                                                          {1061, "Champion I", "Div I"},
                                                                                          {1096, "Champion I", "Div II"},
                                                                                          {1128, "Champion I", "Div III"},
                                                                                          {1162, "Champion I", "Div IV"},
                                                                                          {1181, "Champion II", "Div I"},
                                                                                          {1215, "Champion II", "Div II"},
                                                                                          {1254, "Champion II", "Div III"},
                                                                                          {1282, "Champion II", "Div IV"},
                                                                                          {1301, "Champion III", "Div I"},
                                                                                          {1337, "Champion III", "Div II"},
                                                                                          {1368, "Champion III", "Div III"},
                                                                                          {1402, "Champion III", "Div IV"},
                                                                                          {1421, "Grand Champion I", "Div I"},
                                                                                          {1460, "Grand Champion I", "Div II"},
                                                                                          {1498, "Grand Champion I", "Div III"},
                                                                                          {1537, "Grand Champion I", "Div IV"},
                                                                                          {1561, "Grand Champion II", "Div I"},
                                                                                          {1600, "Grand Champion II", "Div II"},
                                                                                          {1638, "Grand Champion II", "Div III"},
                                                                                          {1677, "Grand Champion II", "Div IV"},
                                                                                          {1701, "Grand Champion III", "Div I"},
                                                                                          {1745, "Grand Champion III", "Div II"},
                                                                                          {1788, "Grand Champion III", "Div III"},
                                                                                          {1832, "Grand Champion III", "Div IV"},
                                                                                          {1861, "Supersonic Legend", ""}}};

    static bool TryReadStatValue(const nlohmann::json& stats, std::initializer_list<const char*> keys, int& out) {
        for (const char* key : keys) {
            if (!stats.contains(key) || !stats[key].is_object()) continue;
            const auto& stat = stats[key];
            if (stat.contains("value") && stat["value"].is_number()) {
                out = stat["value"].get<int>();
                return true;
            }
        }
        return false;
    }

    static bool ShouldReplacePlaylistBucket(const std::map<std::string, int>& playlistMMRs, const std::string& playlistName, int mmr) {
        auto it = playlistMMRs.find(playlistName);
        return it == playlistMMRs.end() || mmr > it->second;
    }

} // namespace

// Helper function for libcurl to write the HTTP response into a std::string
static size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    ((std::string*)userp)->append((char*)contents, size * nmemb);
    return size * nmemb;
}

static int CIProgressCallback(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
    std::atomic<bool>* running = (std::atomic<bool>*)clientp;
    if (running && !running->load()) {
        return 1; // Abort
    }
    return 0;
}

MMRFetcher::MMRFetcher(std::shared_ptr<SessionState> state) : m_state(state) {}

MMRFetcher::~MMRFetcher() {
    Stop();
}

std::string MMRFetcher::GetTournamentTierForMmr(int mmr) {
    if (mmr <= 0) return "Unranked";

    const TournamentRankThreshold* threshold = nullptr;
    for (const auto& entry : TournamentRankThresholds) {
        if (mmr < entry.minMmr) break;
        threshold = &entry;
    }

    if (!threshold) return "Unranked";
    if (threshold->division[0] == '\0') return threshold->tier;
    return std::string(threshold->tier) + " " + threshold->division;
}

std::string MMRFetcher::PlaylistNameForTrackerId(int playlistId) {
    switch (playlistId) {
    case 10:
        return "1v1";
    case 11:
        return "2v2";
    case 13:
        return "3v3";
    case 27:
        return "hoops";
    case 28:
        return "rumble";
    case 29:
        return "dropshot";
    case 30:
        return "snowday";
    case 34:
        return "t";
    case 43:
        return "heatseeker";
    case 0:
        return "casual";
    default:
        return "";
    }
}

MMRProfileTotals MMRFetcher::ExtractProfileTotals(const nlohmann::json& jsonResp) {
    MMRProfileTotals totals;
    if (!jsonResp.contains("data") || !jsonResp["data"].is_object() ||
        !jsonResp["data"].contains("segments") || !jsonResp["data"]["segments"].is_array()) {
        return totals;
    }

    for (const auto& seg : jsonResp["data"]["segments"]) {
        if (!seg.is_object()) continue;
        if (seg.contains("type") && seg["type"].is_string() && seg["type"] == "playlist") continue;
        if (!seg.contains("stats") || !seg["stats"].is_object()) continue;

        const auto& stats = seg["stats"];
        if (totals.totalWins < 0) {
            (void)TryReadStatValue(stats, {"wins", "Wins"}, totals.totalWins);
        }
    }

    return totals;
}

void MMRFetcher::Start() {
    if (m_isRunning) return;
    m_isRunning = true;
    m_workerThread = std::jthread(&MMRFetcher::WorkerLoop, this);
    std::cout << "[MMRFetcher] Background thread started.\n";
}

void MMRFetcher::Stop() {
    m_isRunning = false;
    m_cv.notify_all();
    if (m_workerThread.joinable()) {
        m_workerThread.join();
    }
}

void MMRFetcher::Enqueue(const std::string& primaryId, const std::string& name) {
    if (!Config::Read().enable_mmr_tracking) return;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        if (m_queuedOrInFlight.count(primaryId)) return;
        m_queuedOrInFlight.insert(primaryId);
        m_queue.push({primaryId, name});
    }
    m_cv.notify_one();
}

std::string MMRFetcher::GetTRNPlatform(const std::string& primaryId) {
    size_t delim = primaryId.find('|');
    if (delim == std::string::npos) return "";

    std::string plat = primaryId.substr(0, delim);
    // Trim potential spaces
    plat.erase(0, plat.find_first_not_of(" \t\r\n"));
    plat.erase(plat.find_last_not_of(" \t\r\n") + 1);

    // Convert to lowercase for case-insensitive matching
    std::transform(plat.begin(), plat.end(), plat.begin(), ::tolower);

    if (plat == "epic" || plat == "epicgames") return "epic";
    if (plat == "steam") return "steam";
    if (plat == "ps4" || plat == "psn" || plat == "playstation") return "psn";
    if (plat == "xboxone" || plat == "xbox" || plat == "xbl") return "xbl";
    if (plat == "switch" || plat == "nintendo") return "switch";
    return "";
}

void MMRFetcher::WorkerLoop() {
    // Initialize curl-impersonate on the worker thread
    auto& ci = CurlImpersonate::Instance();
    if (!ci.EnsureAvailable()) {
        std::cout << "[MMRFetcher] WARNING: curl-impersonate not available. "
                  << "MMR fetching will be disabled.\n";
    }

    while (m_isRunning) {
        MMRRequest req;
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait(lock, [this] { return !m_queue.empty() || !m_isRunning; });

            if (!m_isRunning && m_queue.empty()) break;

            req = m_queue.front();
            m_queue.pop();
        }

        bool rateLimited = false;
        FetchProfile(req, rateLimited);

        if (!rateLimited) {
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_queuedOrInFlight.erase(req.primaryId);
        }

        // Tracker.Network rate limit protection (0.5 seconds between requests)
        {
            std::unique_lock<std::mutex> lock(m_queueMutex);
            m_cv.wait_for(lock, std::chrono::milliseconds(500), [this] { return !m_isRunning; });
        }
    }
}

void MMRFetcher::FetchProfile(const MMRRequest& req, bool& rateLimited) {
    auto& ci = CurlImpersonate::Instance();
    if (!ci.IsReady()) {
        std::cout << "[MMRFetcher] Skipping " << PrivacyLog::Sensitive(req.name, "player name") << ": curl-impersonate not loaded\n";
        return;
    }

    std::string plat = GetTRNPlatform(req.primaryId);
    if (plat.empty()) return;

    void* ci_curl = ci.easy_init();
    if (!ci_curl) return;

    size_t delim = req.primaryId.find('|');
    std::string ident;
    if (plat == "steam") {
        if (delim != std::string::npos) {
            std::string sub = req.primaryId.substr(delim + 1);
            size_t secondDelim = sub.find('|');
            if (secondDelim != std::string::npos) {
                ident = sub.substr(0, secondDelim);
            } else {
                ident = sub;
            }
        } else {
            ident = req.primaryId;
        }
    } else {
        ident = req.name;
    }

    char* escaped_ident = ci.easy_escape(ci_curl, ident.c_str(), (int)ident.length());
    std::string final_ident = escaped_ident;
    ci.free_ptr(escaped_ident);

    std::string url = "https://api.tracker.gg/api/v2/rocket-league/standard/profile/" + plat + "/" + final_ident;

    std::cout << "[MMRFetcher] Fetching " << PrivacyLog::Sensitive(req.name, "player name") << " via " << plat << "...\n";

    // Impersonate Chrome's TLS fingerprint
    ci.easy_impersonate(ci_curl, "chrome136", 0);

    std::string readBuffer;
    void* headers = nullptr;
    headers = ci.slist_append(headers, "Accept: application/json, text/plain, */*");
    headers = ci.slist_append(headers, "Accept-Language: en-US,en;q=0.9");
    headers = ci.slist_append(headers, "Origin: https://rocketleague.tracker.network");
    headers = ci.slist_append(headers, "Referer: https://rocketleague.tracker.network/");
    headers = ci.slist_append(headers, "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/136.0.0.0 Safari/537.36");

    ci.easy_setopt(ci_curl, CI_CURLOPT_URL, url.c_str());
    ci.easy_setopt(ci_curl, CI_CURLOPT_HTTPHEADER, headers);
    ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEFUNCTION, WriteCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEDATA, &readBuffer);
    ci.easy_setopt(ci_curl, CI_CURLOPT_TIMEOUT, 15L);
    ci.easy_setopt(ci_curl, CI_CURLOPT_SSL_OPTIONS, (long)CI_CURLSSLOPT_NATIVE_CA);

    ci.easy_setopt(ci_curl, CI_CURLOPT_FOLLOWLOCATION, 1L);

    // Abort if shutting down
    ci.easy_setopt(ci_curl, CI_CURLOPT_XFERINFOFUNCTION, CIProgressCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_XFERINFODATA, &m_isRunning);
    ci.easy_setopt(ci_curl, CI_CURLOPT_NOPROGRESS, 0L);

    int res = ci.easy_perform(ci_curl);
    long http_code = 0;
    ci.easy_getinfo(ci_curl, CI_CURLINFO_RESPONSE_CODE, &http_code);

    ci.slist_free_all(headers);
    ci.easy_cleanup(ci_curl);

    if (res != 0 || http_code != 200) {
        std::cout << "[MMRFetcher] Failed to fetch " << PrivacyLog::Sensitive(req.name, "player name")
                  << " (HTTP " << http_code << ") - Curl error: " << res << "\n";

        if (http_code == 429) {
            std::cout << "[MMRFetcher] Rate limited (HTTP 429). Sleeping worker for 15 seconds...\n";
            {
                std::unique_lock<std::mutex> lock(m_queueMutex);
                m_cv.wait_for(lock, std::chrono::seconds(15), [this] { return !m_isRunning; });
            }
            if (!m_isRunning) return;

            // Re-enqueue the failed request
            std::lock_guard<std::mutex> lock(m_queueMutex);
            m_queue.push(req);
            m_cv.notify_one();
            rateLimited = true;
            return;
        }

        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        if (m_state->game.roster.count(req.primaryId)) {
            auto& player = m_state->game.roster[req.primaryId];
            player.fetched = true;
            player.fetchFailed = true;
        }
        return;
    }

    try {
        nlohmann::json jsonResp = nlohmann::json::parse(readBuffer);
        MMRProfileTotals profileTotals = ExtractProfileTotals(jsonResp);
        if (jsonResp.contains("data") && jsonResp["data"].is_object() &&
            jsonResp["data"].contains("segments") && jsonResp["data"]["segments"].is_array()) {

            int bestMMR = 0;
            std::string bestTier = "Unranked";
            std::string bestPlaylistName = "best";
            std::map<std::string, int> playlistMMRs;
            std::map<std::string, std::string> playlistTiers;
            std::map<std::string, int> playlistMatches;

            for (const auto& seg : jsonResp["data"]["segments"]) {
                if (seg.is_object() &&
                    seg.contains("type") && seg["type"].is_string() && seg["type"] == "playlist" &&
                    seg.contains("attributes") && seg["attributes"].is_object()) {

                    auto attrs = seg["attributes"];
                    if (attrs.contains("playlistId") && attrs["playlistId"].is_number_integer()) {
                        int playlistId = attrs["playlistId"].get<int>();

                        std::string playlistName = PlaylistNameForTrackerId(playlistId);

                        if (!playlistName.empty() && seg.contains("stats") && seg["stats"].is_object()) {
                            auto stats = seg["stats"];
                            if (stats.contains("rating") && stats["rating"].is_object()) {
                                auto rating = stats["rating"];
                                if (rating.contains("value") && rating["value"].is_number()) {
                                    int mmr = rating["value"].get<int>();

                                    std::string tier = "Unranked";
                                    if (stats.contains("tier") && stats["tier"].is_object()) {
                                        auto tierObj = stats["tier"];
                                        if (tierObj.contains("metadata") && tierObj["metadata"].is_object()) {
                                            auto meta = tierObj["metadata"];
                                            if (meta.contains("name") && meta["name"].is_string()) {
                                                tier = meta["name"].get<std::string>();
                                            }
                                        }
                                    }

                                    std::string division = "";
                                    if (stats.contains("division") && stats["division"].is_object()) {
                                        auto divObj = stats["division"];
                                        if (divObj.contains("metadata") && divObj["metadata"].is_object()) {
                                            auto meta = divObj["metadata"];
                                            if (meta.contains("name") && meta["name"].is_string()) {
                                                division = meta["name"].get<std::string>();
                                            }
                                        }
                                        if (division.empty() && divObj.contains("displayValue") && divObj["displayValue"].is_string()) {
                                            division = divObj["displayValue"].get<std::string>();
                                        }
                                        if (division.empty() && divObj.contains("value") && divObj["value"].is_number()) {
                                            int divVal = divObj["value"].get<int>();
                                            if (divVal == 1)
                                                division = "Div I";
                                            else if (divVal == 2)
                                                division = "Div II";
                                            else if (divVal == 3)
                                                division = "Div III";
                                            else if (divVal == 4)
                                                division = "Div IV";
                                        }
                                    }

                                    if (!division.empty()) {
                                        // Abbreviate "Division" to "Div" for compact display in the overlay
                                        size_t pos = division.find("Division ");
                                        if (pos != std::string::npos) {
                                            division.replace(pos, 9, "Div ");
                                        }
                                        if (tier != "Unranked") {
                                            tier += " " + division;
                                        }
                                    }

                                    if (playlistName == "t") {
                                        tier = GetTournamentTierForMmr(mmr);
                                    }

                                    int matches = 0;
                                    if (stats.contains("matchesPlayed") && stats["matchesPlayed"].is_object()) {
                                        auto mp = stats["matchesPlayed"];
                                        if (mp.contains("value") && mp["value"].is_number()) {
                                            matches = mp["value"].get<int>();
                                        }
                                    }

                                    if (ShouldReplacePlaylistBucket(playlistMMRs, playlistName, mmr)) {
                                        playlistMMRs[playlistName] = mmr;
                                        playlistTiers[playlistName] = tier;
                                    }
                                    playlistMatches[playlistName] += matches;

                                    if (playlistName != "casual" && playlistName != "t" && mmr > bestMMR) {
                                        bestMMR = mmr;
                                        bestTier = tier;
                                        bestPlaylistName = playlistName;
                                    }
                                }
                            }
                        }
                    }
                }
            }

            playlistMMRs["best"] = bestMMR;
            playlistTiers["best"] = bestTier;
            if (playlistMatches.count(bestPlaylistName)) {
                playlistMatches["best"] = playlistMatches[bestPlaylistName];
            } else {
                playlistMatches["best"] = 0;
            }

            std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
            std::unique_lock<std::shared_mutex> historyLock(m_state->history.mutex);
            if (m_state->game.roster.count(req.primaryId)) {
                auto& player = m_state->game.roster[req.primaryId];
                player.playlists = playlistMMRs;
                player.playlistTiers = playlistTiers;
                player.playlistMatches = playlistMatches;
                player.totalWins = profileTotals.totalWins;
                player.mmr = bestMMR;
                player.rankTier = bestTier;
                player.fetched = true;
                player.fetchFailed = false;

                if (req.primaryId == m_state->game.myPrimaryId) {
                    if (m_state->history.initialMmr == -1 && bestMMR > 0) {
                        m_state->history.initialMmr = bestMMR;
                    }
                    if (bestMMR > 0) {
                        m_state->history.mmrHistoryY.push_back((float)bestMMR);
                        m_state->history.mmrHistoryX.push_back((float)m_state->history.mmrHistoryY.size());
                    }
                }

                // Record playlist-specific MMR history for the local player
                if (req.primaryId == m_state->game.myPrimaryId) {
                    for (const auto& entry : playlistMMRs) {
                        std::string playlistName = entry.first; // "1v1", "2v2", "3v3", extra modes, "best", "casual"
                        int mmr = entry.second;
                        if (mmr > 0) {
                            // Seed initial MMR if empty
                            if (m_state->history.playlistInitialMmr.count(playlistName) == 0) {
                                m_state->history.playlistInitialMmr[playlistName] = mmr;
                            }

                            // Calculate playlist-specific MMR change
                            m_state->game.sessionTotals.mmrChangeByPlaylist[playlistName] = mmr - m_state->history.playlistInitialMmr[playlistName];

                            // Append to playlist history if empty or if last element is different
                            auto& history = m_state->history.playlistHistoryY[playlistName];
                            if (history.empty() || history.back() != mmr) {
                                history.push_back((float)mmr);
                            }
                        }
                    }

                    // Inferred active competitive playlist
                    std::string arenaKey = !m_state->game.arenaAsset.empty() ? m_state->game.arenaAsset : m_state->game.arenaName;
                    std::string activePlaylist = GamemodeUtils::InferFromSnapshot(
                        m_state->game.maxPlayersSeen,
                        static_cast<int>(m_state->game.roster.size()),
                        m_state->ui.rosterMmrCategory.load(),
                        m_state->ui.graphMmrCategory.load(),
                        arenaKey);

                    // If active playlist is valid, set totalMmrChange from that playlist's delta,
                    // otherwise fall back to the active graph category's delta.
                    if (activePlaylist != "Unknown" && m_state->game.sessionTotals.mmrChangeByPlaylist.count(activePlaylist)) {
                        m_state->game.sessionTotals.totalMmrChange = (float)m_state->game.sessionTotals.mmrChangeByPlaylist[activePlaylist];
                    } else {
                        // Fallback: active graph category
                        std::string graphCatStr = MmrCategoryToString(m_state->ui.graphMmrCategory.load());
                        if (m_state->game.sessionTotals.mmrChangeByPlaylist.count(graphCatStr)) {
                            m_state->game.sessionTotals.totalMmrChange = (float)m_state->game.sessionTotals.mmrChangeByPlaylist[graphCatStr];
                        } else if (bestMMR > 0 && m_state->history.initialMmr != -1) {
                            m_state->game.sessionTotals.totalMmrChange = (float)(bestMMR - m_state->history.initialMmr);
                        }
                    }
                }
                std::cout << "[MMRFetcher] Updated: " << PrivacyLog::Sensitive(req.name, "player name") << " -> Best: " << bestMMR << "\n";
                m_state->game.version++;
                m_state->history.version++;
            }
        }
    } catch (const std::exception& e) {
        std::cout << "[MMRFetcher] JSON Parse Error: " << e.what() << "\n";
        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        if (m_state->game.roster.count(req.primaryId)) {
            auto& player = m_state->game.roster[req.primaryId];
            player.fetched = true;
            player.fetchFailed = true;
            m_state->game.version++;
        }
    }
}

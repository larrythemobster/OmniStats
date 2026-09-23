#include "MMRFetcher.hpp"
#include "CurlImpersonate.hpp"
#include "core/Config.hpp"
#include "core/GamemodeUtils.hpp"
#include "core/PlaylistMetadata.hpp"
#include "core/PrivacyLog.hpp"
#include "database/DatabaseManager.hpp"
#include <nlohmann/json.hpp>
#include <iostream>
#include <chrono>
#include <shared_mutex>
#include <algorithm>
#include <array>
#include <initializer_list>
#include <map>
#include <cmath>
#include <cctype>
#include <string_view>
#include "network/MMRFetcherDetail.hpp"

using namespace MMRFetcherDetail;

namespace {
    struct FetchHeaderState {
        long retryAfterSeconds = 0;
    };

    size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
        const size_t total = size * nitems;
        if (!userdata || !buffer) return total;
        auto* state = static_cast<FetchHeaderState*>(userdata);

        std::string_view line(buffer, total);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
            line.remove_suffix(1);
        }

        constexpr std::string_view kRetryAfter = "retry-after:";
        if (line.size() >= kRetryAfter.size()) {
            bool match = true;
            for (size_t i = 0; i < kRetryAfter.size(); ++i) {
                if (std::tolower(static_cast<unsigned char>(line[i])) != kRetryAfter[i]) {
                    match = false;
                    break;
                }
            }
            if (match) {
                std::string_view val = line.substr(kRetryAfter.size());
                while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
                    val.remove_prefix(1);
                }
                try {
                    long parsed = std::stol(std::string(val));
                    if (parsed > 0) {
                        state->retryAfterSeconds = parsed;
                    }
                } catch (...) {
                }
            }
        }
        return total;
    }

    // Helper function for libcurl to write the HTTP response into a std::string
    size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
        ((std::string*)userp)->append((char*)contents, size * nmemb);
        return size * nmemb;
    }

    int CIProgressCallback(void* clientp, double dltotal, double dlnow, double ultotal, double ulnow) {
        std::atomic<bool>* running = (std::atomic<bool>*)clientp;
        if (running && !running->load()) {
            return 1; // Abort
        }
        return 0;
    }
}

CustomApiFetchResult MMRFetcher::FetchProfileFromCustomApi(const MMRRequest& req) {
    auto& ci = CurlImpersonate::Instance();
    if (!ci.IsReady()) {
        return CustomApiFetchResult::DisabledOrNotReady;
    }

    const auto config = Config::Read();
    if (!config.custom_api_enabled || config.custom_api_key.empty()) {
        return CustomApiFetchResult::DisabledOrNotReady;
    }
    std::string baseUrl = config.custom_api_base_url;
    if (baseUrl.empty()) {
        baseUrl = "https://api.omnistats.org";
    }
    while (!baseUrl.empty() && baseUrl.back() == '/') {
        baseUrl.pop_back();
    }

    const size_t delim = req.primaryId.find('|');
    if (delim == std::string::npos) {
        std::cout << "[MMRFetcher] Custom API skipped for "
                  << PrivacyLog::Sensitive(req.name, "player name")
                  << ": invalid primaryId format.\n";
        return CustomApiFetchResult::UnusableData;
    }

    std::string rawPlat = req.primaryId.substr(0, delim);
    rawPlat.erase(0, rawPlat.find_first_not_of(" \t\r\n"));
    rawPlat.erase(rawPlat.find_last_not_of(" \t\r\n") + 1);
    std::string platLower = rawPlat;
    std::transform(platLower.begin(), platLower.end(), platLower.begin(), ::tolower);

    std::string platform;
    if (platLower == "epic" || platLower == "epicgames")
        platform = "Epic";
    else if (platLower == "steam")
        platform = "Steam";
    else if (platLower == "ps4" || platLower == "ps5" || platLower == "psn" || platLower == "playstation")
        platform = "PS4";
    else if (platLower == "xbox" || platLower == "xboxone" || platLower == "xbl")
        platform = "Xbox";
    else if (platLower == "switch" || platLower == "nintendo")
        platform = "Switch";
    else {
        std::cout << "[MMRFetcher] Custom API skipped for "
                  << PrivacyLog::Sensitive(req.name, "player name")
                  << ": unsupported platform '" << rawPlat << "'.\n";
        return CustomApiFetchResult::UnusableData;
    }

    std::string accountId = req.primaryId.substr(delim + 1);
    const size_t secondDelim = accountId.find('|');
    if (secondDelim != std::string::npos) {
        accountId = accountId.substr(0, secondDelim);
    }
    accountId.erase(0, accountId.find_first_not_of(" \t\r\n"));
    accountId.erase(accountId.find_last_not_of(" \t\r\n") + 1);
    if (accountId.empty()) {
        std::cout << "[MMRFetcher] Custom API skipped for "
                  << PrivacyLog::Sensitive(req.name, "player name")
                  << ": empty account ID in primaryId.\n";
        return CustomApiFetchResult::UnusableData;
    }

    nlohmann::json reqBody = {
        {"players", nlohmann::json::array({{{"platform", platform}, {"account_id", accountId}}})}};
    if (req.reason == MMRRequestReason::PostMatch && !req.playlist.empty()) {
        int pid = -1;
        if (req.playlist == "1v1")
            pid = 10;
        else if (req.playlist == "2v2")
            pid = 11;
        else if (req.playlist == "3v3")
            pid = 13;
        else if (req.playlist == "hoops")
            pid = 27;
        else if (req.playlist == "rumble")
            pid = 28;
        else if (req.playlist == "dropshot")
            pid = 29;
        else if (req.playlist == "snowday")
            pid = 30;
        else if (req.playlist == "t")
            pid = 34;
        else if (req.playlist == "heatseeker")
            pid = 43;
        if (pid > 0) reqBody["playlist"] = pid;
    }

    const std::string reqBodyStr = reqBody.dump();
    const std::string url = baseUrl + "/v1/ranks";

    void* ci_curl = ci.easy_init();
    if (!ci_curl) return CustomApiFetchResult::DisabledOrNotReady;

    void* headers = nullptr;
    headers = ci.slist_append(headers, "Content-Type: application/json");
    headers = ci.slist_append(headers, "Accept: application/json");
    headers = ci.slist_append(headers, ("X-API-Key: " + config.custom_api_key).c_str());

    std::string readBuffer;
    FetchHeaderState headerState;

    ci.easy_setopt(ci_curl, CI_CURLOPT_URL, url.c_str());
    ci.easy_setopt(ci_curl, CI_CURLOPT_POST, 1L);
    ci.easy_setopt(ci_curl, CI_CURLOPT_POSTFIELDS, reqBodyStr.c_str());
    ci.easy_setopt(ci_curl, CI_CURLOPT_POSTFIELDSIZE, static_cast<long>(reqBodyStr.size()));
    ci.easy_setopt(ci_curl, CI_CURLOPT_HTTPHEADER, headers);
    ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEFUNCTION, WriteCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEDATA, &readBuffer);
    ci.easy_setopt(ci_curl, CI_CURLOPT_HEADERFUNCTION, HeaderCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_HEADERDATA, &headerState);
    ci.easy_setopt(ci_curl, CI_CURLOPT_TIMEOUT, 10L);
    ci.easy_setopt(ci_curl, CI_CURLOPT_SSL_OPTIONS, static_cast<long>(CI_CURLSSLOPT_NATIVE_CA));
    ci.easy_setopt(ci_curl, CI_CURLOPT_FOLLOWLOCATION, 1L);
    ci.easy_setopt(ci_curl, CI_CURLOPT_NOPROGRESS, 1L);

    const int res = ci.easy_perform(ci_curl);
    long httpCode = 0;
    ci.easy_getinfo(ci_curl, CI_CURLINFO_RESPONSE_CODE, &httpCode);

    ci.slist_free_all(headers);
    ci.easy_cleanup(ci_curl);

    if (res != 0) {
        std::cout << "[MMRFetcher] Custom API network error for "
                  << PrivacyLog::Sensitive(req.name, "player name")
                  << ": curl error " << res << ".\n";
        return CustomApiFetchResult::TransientError;
    }
    if (httpCode == 401 || httpCode == 403) {
        std::cout << "[MMRFetcher] Custom API authentication failed (HTTP "
                  << httpCode << ") for "
                  << PrivacyLog::Sensitive(req.name, "player name")
                  << ". Check your custom API key.\n";
        return CustomApiFetchResult::AuthFailure;
    }
    if (httpCode == 429) {
        std::cout << "[MMRFetcher] Custom API rate limited (HTTP 429) for "
                  << PrivacyLog::Sensitive(req.name, "player name") << ".\n";
        return CustomApiFetchResult::TransientError;
    }
    if (httpCode == 404) {
        std::cout << "[MMRFetcher] Custom API returned HTTP 404 (not found) for "
                  << PrivacyLog::Sensitive(req.name, "player name") << ".\n";
        return CustomApiFetchResult::UnusableData;
    }
    if (httpCode >= 500 && httpCode <= 599) {
        std::cout << "[MMRFetcher] Custom API server error (HTTP "
                  << httpCode << ") for "
                  << PrivacyLog::Sensitive(req.name, "player name") << ".\n";
        return CustomApiFetchResult::TransientError;
    }
    if (httpCode != 200) {
        std::cout << "[MMRFetcher] Custom API returned unexpected HTTP "
                  << httpCode << " for "
                  << PrivacyLog::Sensitive(req.name, "player name") << ".\n";
        return CustomApiFetchResult::UnusableData;
    }

    nlohmann::json jsonResp;
    try {
        jsonResp = nlohmann::json::parse(readBuffer);
    } catch (...) {
        std::cout << "[MMRFetcher] Custom API returned malformed JSON for "
                  << PrivacyLog::Sensitive(req.name, "player name") << ".\n";
        return CustomApiFetchResult::UnusableData;
    }

    if (jsonResp.contains("error") && jsonResp["error"].is_object()) {
        std::string errCode = jsonResp["error"].value("code", "unknown");
        std::string errMsg = jsonResp["error"].value("message", "");
        std::cout << "[MMRFetcher] Custom API error for "
                  << PrivacyLog::Sensitive(req.name, "player name")
                  << ": " << errCode;
        if (!errMsg.empty()) {
            std::cout << " (" << errMsg << ")";
        }
        std::cout << ".\n";
        return CustomApiFetchResult::UnusableData;
    }

    if (!jsonResp.contains("players") || !jsonResp["players"].is_array() || jsonResp["players"].empty()) {
        std::cout << "[MMRFetcher] Custom API returned HTTP 200 but missing player data for "
                  << PrivacyLog::Sensitive(req.name, "player name") << ".\n";
        return CustomApiFetchResult::UnusableData;
    }

    const nlohmann::json* matchedPlayer = nullptr;
    for (const auto& p : jsonResp["players"]) {
        if (!p.is_object()) continue;
        if (p.contains("account_id") && p["account_id"].is_string()) {
            if (CaseInsensitiveEquals(p["account_id"].get<std::string>(), accountId)) {
                matchedPlayer = &p;
                break;
            }
        }
    }
    if (!matchedPlayer) {
        if (jsonResp["players"].size() == 1 && jsonResp["players"][0].is_object()) {
            matchedPlayer = &jsonResp["players"][0];
            if (matchedPlayer->contains("account_id") && (*matchedPlayer)["account_id"].is_string()) {
                const std::string respId = (*matchedPlayer)["account_id"].get<std::string>();
                if (!respId.empty() && !CaseInsensitiveEquals(respId, accountId)) {
                    matchedPlayer = nullptr;
                }
            }
        }
    }

    if (!matchedPlayer) {
        std::cout << "[MMRFetcher] Custom API returned HTTP 200 but player "
                  << PrivacyLog::Sensitive(req.name, "player name")
                  << " was not in response.\n";
        return CustomApiFetchResult::UnusableData;
    }

    if (matchedPlayer->contains("error") && !(*matchedPlayer)["error"].is_null()) {
        std::cout << "[MMRFetcher] Custom API returned player-level error for "
                  << PrivacyLog::Sensitive(req.name, "player name") << ".\n";
        return CustomApiFetchResult::UnusableData;
    }

    if (!matchedPlayer->contains("skills") || !(*matchedPlayer)["skills"].is_array() ||
        (*matchedPlayer)["skills"].empty()) {
        std::cout << "[MMRFetcher] Custom API returned HTTP 200 but no skills for "
                  << PrivacyLog::Sensitive(req.name, "player name") << ".\n";
        return CustomApiFetchResult::UnusableData;
    }

    const size_t totalSkills = (*matchedPlayer)["skills"].size();
    size_t unrecognizedPlaylists = 0;
    size_t invalidMmrCount = 0;
    int bestMmr = 0;
    std::string bestTier = "Unranked";
    std::string bestPlaylistName = "best";
    std::map<std::string, int> playlistMMRs;
    std::map<std::string, std::string> playlistTiers;
    std::map<std::string, int> playlistMatches;

    for (const auto& skill : (*matchedPlayer)["skills"]) {
        if (!skill.is_object()) continue;
        if (!skill.contains("playlist") || !skill["playlist"].is_number_integer()) {
            unrecognizedPlaylists++;
            continue;
        }
        int pid = skill["playlist"].get<int>();
        std::string plName = PlaylistNameForTrackerId(pid);
        if (plName.empty()) {
            unrecognizedPlaylists++;
            continue;
        }

        if (!skill.contains("mmr") || !skill["mmr"].is_number()) {
            invalidMmrCount++;
            continue;
        }
        double mmrDouble = skill["mmr"].get<double>();
        int mmrInt = static_cast<int>(std::lround(mmrDouble));
        if (mmrInt <= 0) {
            invalidMmrCount++;
            continue;
        }

        int tier = skill.value("tier", 0);
        int div = skill.value("division", 0);
        int matches = skill.value("matches_played", -1);

        std::string tierName = (plName == "t") ? GetTournamentTierForMmr(mmrInt) : RankTierName(tier, div);

        if (ShouldReplacePlaylistBucket(playlistMMRs, plName, mmrInt)) {
            playlistMMRs[plName] = mmrInt;
            playlistTiers[plName] = tierName;
        }
        if (matches >= 0) {
            playlistMatches[plName] += matches;
        }

        if (plName != "casual" && plName != "t" && mmrInt > bestMmr) {
            bestMmr = mmrInt;
            bestTier = tierName;
            bestPlaylistName = plName;
        }
    }

    if (playlistMMRs.empty()) {
        if (unrecognizedPlaylists == totalSkills) {
            std::cout << "[MMRFetcher] Custom API returned HTTP 200 but no recognized playlist IDs for "
                      << PrivacyLog::Sensitive(req.name, "player name") << ".\n";
        } else if (invalidMmrCount == totalSkills) {
            std::cout << "[MMRFetcher] Custom API returned HTTP 200 but invalid/missing MMR for "
                      << PrivacyLog::Sensitive(req.name, "player name") << ".\n";
        } else {
            std::cout << "[MMRFetcher] Custom API returned HTTP 200 but no usable rank data for "
                      << PrivacyLog::Sensitive(req.name, "player name") << ".\n";
        }
        return CustomApiFetchResult::UnusableData;
    }

    playlistMMRs["best"] = bestMmr;
    playlistTiers["best"] = bestTier;
    if (playlistMatches.count(bestPlaylistName)) {
        playlistMatches["best"] = playlistMatches[bestPlaylistName];
    }

    std::cout << "[MMRFetcher] Custom API updated "
              << PrivacyLog::Sensitive(req.name, "player name")
              << ": skills=" << totalSkills
              << ", usable=" << (playlistMMRs.size() - 1)
              << ", best=" << bestMmr << "\n";

    NormalizedProfileResult profile;
    profile.bestMmr = bestMmr;
    profile.bestTier = bestTier;
    profile.bestPlaylistName = bestPlaylistName;
    profile.playlistMMRs = std::move(playlistMMRs);
    profile.playlistTiers = std::move(playlistTiers);
    profile.playlistMatches = std::move(playlistMatches);
    int totalWins = -1;
    if (matchedPlayer->contains("wins") && (*matchedPlayer)["wins"].is_number_integer()) {
        totalWins = (*matchedPlayer)["wins"].get<int>();
    } else if (matchedPlayer->contains("total_wins") && (*matchedPlayer)["total_wins"].is_number_integer()) {
        totalWins = (*matchedPlayer)["total_wins"].get<int>();
    }
    profile.totalWins = totalWins;
    profile.rankVerificationSource = "ServerA";

    const bool requeued = PublishProfileResult(req, profile);
    return requeued ? CustomApiFetchResult::SuccessRequeued : CustomApiFetchResult::SuccessFinished;
}

bool MMRFetcher::FetchProfile(MMRRequest req) {
    if (req.reason == MMRRequestReason::PostMatch) {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        const auto recordIt = m_postMatchRecordsByGuid.find(req.matchGuid);
        if (recordIt != m_postMatchRecordsByGuid.end() &&
            recordIt->second.reconciliationState == PostMatchReconciliationState::Confirmed) {
            return false;
        }
    }
    if (m_useCustomApiFallback.load()) {
        const auto customResult = FetchProfileFromCustomApi(req);
        if (customResult == CustomApiFetchResult::SuccessFinished) {
            return false;
        }
        if (customResult == CustomApiFetchResult::SuccessRequeued) {
            return true;
        }
        if (customResult == CustomApiFetchResult::AuthFailure) {
            m_useCustomApiFallback.store(false);
        } else if (customResult == CustomApiFetchResult::TransientError) {
            if (ScheduleRetry(req, kTransientRetryDelay, "transient custom API failure")) {
                return true;
            }
            std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
            if (m_state->game.roster.count(req.primaryId)) {
                auto& player = m_state->game.roster[req.primaryId];
                player.fetched = true;
                player.fetchFailed = true;
                m_state->game.version++;
            }
            return false;
        } else if (customResult == CustomApiFetchResult::UnusableData) {
            const auto now = std::chrono::steady_clock::now();
            if (m_rateLimitedUntil > now) {
                std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
                if (m_state->game.roster.count(req.primaryId)) {
                    auto& player = m_state->game.roster[req.primaryId];
                    player.fetched = true;
                    player.fetchFailed = true;
                    m_state->game.version++;
                }
                return false;
            }
        }
    }
    auto& ci = CurlImpersonate::Instance();
    if (!ci.IsReady()) {
        std::cout << "[MMRFetcher] Skipping " << PrivacyLog::Sensitive(req.name, "player name")
                  << ": curl-impersonate not loaded\n";
        return false;
    }

    const std::string plat = GetTRNPlatform(req.primaryId);
    if (plat.empty()) return false;

    void* ci_curl = ci.easy_init();
    if (!ci_curl) return false;

    const size_t delim = req.primaryId.find('|');
    std::string ident;
    if (plat == "steam") {
        if (delim != std::string::npos) {
            std::string sub = req.primaryId.substr(delim + 1);
            const size_t secondDelim = sub.find('|');
            ident = secondDelim != std::string::npos ? sub.substr(0, secondDelim) : sub;
        } else {
            ident = req.primaryId;
        }
    } else {
        ident = req.name;
    }

    char* escapedIdent = ci.easy_escape(ci_curl, ident.c_str(), static_cast<int>(ident.length()));
    if (!escapedIdent) {
        ci.easy_cleanup(ci_curl);
        return false;
    }
    const std::string finalIdent = escapedIdent;
    ci.free_ptr(escapedIdent);

    const std::string url = "https://api.tracker.gg/api/v2/rocket-league/standard/profile/" + plat + "/" + finalIdent;
    std::cout << "[MMRFetcher] Fetching " << PrivacyLog::Sensitive(req.name, "player name") << " via " << plat << "...\n";

    ci.easy_impersonate(ci_curl, kTrackerImpersonation, 0);

    std::string readBuffer;
    FetchHeaderState headerState;
    void* headers = nullptr;
    headers = ci.slist_append(headers, "Accept: application/json, text/plain, */*");
    headers = ci.slist_append(headers, "Accept-Language: en-US,en;q=0.9");
    headers = ci.slist_append(headers, "Origin: https://rocketleague.tracker.network");
    headers = ci.slist_append(headers, "Referer: https://rocketleague.tracker.network/");
    headers = ci.slist_append(
        headers, (std::string("User-Agent: ") + kTrackerUserAgent).c_str());

    ci.easy_setopt(ci_curl, CI_CURLOPT_URL, url.c_str());
    ci.easy_setopt(ci_curl, CI_CURLOPT_HTTPHEADER, headers);
    ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEFUNCTION, WriteCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_WRITEDATA, &readBuffer);
    ci.easy_setopt(ci_curl, CI_CURLOPT_HEADERFUNCTION, HeaderCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_HEADERDATA, &headerState);
    ci.easy_setopt(ci_curl, CI_CURLOPT_ACCEPT_ENCODING, "");
    ci.easy_setopt(ci_curl, CI_CURLOPT_TIMEOUT, 15L);
    ci.easy_setopt(ci_curl, CI_CURLOPT_SSL_OPTIONS, static_cast<long>(CI_CURLSSLOPT_NATIVE_CA));
    ci.easy_setopt(ci_curl, CI_CURLOPT_FOLLOWLOCATION, 1L);
    ci.easy_setopt(ci_curl, CI_CURLOPT_XFERINFOFUNCTION, CIProgressCallback);
    ci.easy_setopt(ci_curl, CI_CURLOPT_XFERINFODATA, &m_isRunning);
    ci.easy_setopt(ci_curl, CI_CURLOPT_NOPROGRESS, 0L);

    int res = 0;
    long httpCode = 0;
    for (int attempt = 1; attempt <= kTrackerForbiddenAttempts; ++attempt) {
        readBuffer.clear();
        headerState = {};
        res = ci.easy_perform(ci_curl);
        ci.easy_getinfo(ci_curl, CI_CURLINFO_RESPONSE_CODE, &httpCode);
        if (res != 0 || httpCode != 403 ||
            attempt == kTrackerForbiddenAttempts) {
            break;
        }
        std::cout
            << "[MMRFetcher] Tracker.gg returned HTTP 403; retrying with "
            << "Chrome 136 (attempt " << attempt + 1 << "/"
            << kTrackerForbiddenAttempts << ").\n";
        std::this_thread::sleep_for(kForbiddenAttemptDelay);
    }

    ci.slist_free_all(headers);
    ci.easy_cleanup(ci_curl);

    if (res != 0 || httpCode != 200) {
        std::cout << "[MMRFetcher] Failed to fetch " << PrivacyLog::Sensitive(req.name, "player name")
                  << " (HTTP " << httpCode << ") - Curl error: " << res << "\n";

        if (httpCode == 403) {
            std::cout
                << "[MMRFetcher] Tracker.gg returned HTTP 403 after "
                << kTrackerForbiddenAttempts
                << " attempts. Attempting fallback to custom API...\n";
            const auto customResult = FetchProfileFromCustomApi(req);
            if (customResult == CustomApiFetchResult::SuccessFinished) {
                std::cout << "[MMRFetcher] Successfully fetched ranks from custom API for "
                          << PrivacyLog::Sensitive(req.name, "player name") << "!\n";
                m_useCustomApiFallback.store(true);
                return false;
            }
            if (customResult == CustomApiFetchResult::SuccessRequeued) {
                std::cout << "[MMRFetcher] Successfully fetched ranks from custom API for "
                          << PrivacyLog::Sensitive(req.name, "player name") << "!\n";
                m_useCustomApiFallback.store(true);
                return true;
            }
            std::cout << "[MMRFetcher] Custom API fallback was unavailable or failed.\n";
            if (!m_isRunning) return false;

            size_t strike = 0;
            std::chrono::steady_clock::duration lockout{};
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                strike = ++m_forbiddenStrikeCount;
                lockout = ForbiddenLockoutForStrike(strike);
                const auto retryAt = std::chrono::steady_clock::now() + lockout;

                // Treat a 403 as a Tracker-wide/WAF block. Keep the failed
                // request queued without consuming its ordinary retry budget.
                // The global gate means only one queued request probes Tracker
                // after each cooldown expires.
                m_rateLimitedUntil = retryAt;
                req.notBefore = retryAt;
                m_queue.push_front(std::move(req));
            }
            std::cout
                << "[MMRFetcher] Tracker.gg blocked requests (HTTP 403). Circuit breaker strike "
                << strike << "; pausing all Tracker requests for "
                << std::chrono::duration_cast<std::chrono::seconds>(lockout).count()
                << " seconds.\n";
            m_cv.notify_one();
            return true;
        } else if (httpCode == 429) {
            const auto retryDuration = std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::seconds(headerState.retryAfterSeconds));
            const auto lockout = (retryDuration > kRateLimitLockoutMinimum) ? retryDuration : kRateLimitLockoutMinimum;
            {
                std::lock_guard<std::mutex> lock(m_queueMutex);
                m_rateLimitedUntil = std::chrono::steady_clock::now() + lockout;
            }
            std::cout << "[MMRFetcher] Rate limited by Tracker.gg (HTTP 429). Pausing requests for "
                      << std::chrono::duration_cast<std::chrono::seconds>(lockout).count() << " seconds.\n";
            if (ScheduleRetry(req, lockout, "rate limited")) return true;
        } else if (res != 0 || httpCode == 408 || (httpCode >= 500 && httpCode <= 599)) {
            if (ScheduleRetry(req, kTransientRetryDelay, "transient Tracker failure")) return true;
        }

        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        if (m_state->game.roster.count(req.primaryId)) {
            auto& player = m_state->game.roster[req.primaryId];
            player.fetched = true;
            player.fetchFailed = true;
            m_state->game.version++;
        }
        return false;
    }

    bool recoveredFromForbidden = false;
    {
        std::lock_guard<std::mutex> lock(m_queueMutex);
        recoveredFromForbidden = m_forbiddenStrikeCount > 0;
        m_forbiddenStrikeCount = 0;
        m_rateLimitedUntil = {};
    }
    if (recoveredFromForbidden) {
        m_useCustomApiFallback.store(false);
        std::cout
            << "[MMRFetcher] Tracker.gg requests recovered; circuit breaker reset.\n";
    }

    try {
        const nlohmann::json jsonResp = nlohmann::json::parse(readBuffer);
        const MMRProfileTotals profileTotals = ExtractProfileTotals(jsonResp);
        if (!jsonResp.contains("data") || !jsonResp["data"].is_object() ||
            !jsonResp["data"].contains("segments") || !jsonResp["data"]["segments"].is_array()) {
            if (ScheduleRetry(req, kTransientRetryDelay, "incomplete Tracker response")) return true;
            return false;
        }

        int bestMMR = 0;
        std::string bestTier = "Unranked";
        std::string bestPlaylistName = "best";
        std::map<std::string, int> playlistMMRs;
        std::map<std::string, std::string> playlistTiers;
        std::map<std::string, int> playlistMatches;

        for (const auto& seg : jsonResp["data"]["segments"]) {
            if (!seg.is_object() || !seg.contains("type") || !seg["type"].is_string() || seg["type"] != "playlist" ||
                !seg.contains("attributes") || !seg["attributes"].is_object()) {
                continue;
            }

            const auto& attrs = seg["attributes"];
            if (!attrs.contains("playlistId") || !attrs["playlistId"].is_number_integer()) continue;

            const std::string playlistName = PlaylistNameForTrackerId(attrs["playlistId"].get<int>());
            if (playlistName.empty() || !seg.contains("stats") || !seg["stats"].is_object()) continue;

            const auto& stats = seg["stats"];
            if (!stats.contains("rating") || !stats["rating"].is_object()) continue;
            const auto& rating = stats["rating"];
            if (!rating.contains("value") || !rating["value"].is_number()) continue;

            const int mmr = rating["value"].get<int>();
            std::string tier = "Unranked";
            if (stats.contains("tier") && stats["tier"].is_object()) {
                const auto& tierObj = stats["tier"];
                if (tierObj.contains("metadata") && tierObj["metadata"].is_object() &&
                    tierObj["metadata"].contains("name") && tierObj["metadata"]["name"].is_string()) {
                    tier = tierObj["metadata"]["name"].get<std::string>();
                }
            }

            std::string division;
            if (stats.contains("division") && stats["division"].is_object()) {
                const auto& divObj = stats["division"];
                if (divObj.contains("metadata") && divObj["metadata"].is_object() &&
                    divObj["metadata"].contains("name") && divObj["metadata"]["name"].is_string()) {
                    division = divObj["metadata"]["name"].get<std::string>();
                }
                if (division.empty() && divObj.contains("displayValue") && divObj["displayValue"].is_string()) {
                    division = divObj["displayValue"].get<std::string>();
                }
                if (division.empty() && divObj.contains("value") && divObj["value"].is_number()) {
                    switch (divObj["value"].get<int>()) {
                    case 1:
                        division = "Div I";
                        break;
                    case 2:
                        division = "Div II";
                        break;
                    case 3:
                        division = "Div III";
                        break;
                    case 4:
                        division = "Div IV";
                        break;
                    default:
                        break;
                    }
                }
            }

            if (!division.empty()) {
                const size_t pos = division.find("Division ");
                if (pos != std::string::npos) division.replace(pos, 9, "Div ");
                if (tier != "Unranked") tier += " " + division;
            }
            if (playlistName == "t") tier = GetTournamentTierForMmr(mmr);

            int matches = -1;
            if (stats.contains("matchesPlayed") && stats["matchesPlayed"].is_object() &&
                stats["matchesPlayed"].contains("value") && stats["matchesPlayed"]["value"].is_number()) {
                matches = stats["matchesPlayed"]["value"].get<int>();
            }

            if (ShouldReplacePlaylistBucket(playlistMMRs, playlistName, mmr)) {
                playlistMMRs[playlistName] = mmr;
                playlistTiers[playlistName] = tier;
            }
            if (matches >= 0) playlistMatches[playlistName] += matches;

            if (playlistName != "casual" && playlistName != "t" && mmr > bestMMR) {
                bestMMR = mmr;
                bestTier = tier;
                bestPlaylistName = playlistName;
            }
        }

        playlistMMRs["best"] = bestMMR;
        playlistTiers["best"] = bestTier;
        if (playlistMatches.count(bestPlaylistName)) {
            playlistMatches["best"] = playlistMatches[bestPlaylistName];
        }
        NormalizedProfileResult profile;
        profile.bestMmr = bestMMR;
        profile.bestTier = bestTier;
        profile.bestPlaylistName = bestPlaylistName;
        profile.playlistMMRs = std::move(playlistMMRs);
        profile.playlistTiers = std::move(playlistTiers);
        profile.playlistMatches = std::move(playlistMatches);
        profile.totalWins = profileTotals.totalWins;
        profile.rankVerificationSource = "Tracker";

        return PublishProfileResult(req, profile);
    } catch (const std::exception& e) {
        std::cout << "[MMRFetcher] JSON Parse Error: " << e.what() << "\n";
        if (ScheduleRetry(req, kTransientRetryDelay, "invalid Tracker response")) return true;

        std::unique_lock<std::shared_mutex> gameLock(m_state->game.mutex);
        if (m_state->game.roster.count(req.primaryId)) {
            auto& player = m_state->game.roster[req.primaryId];
            player.fetched = true;
            player.fetchFailed = true;
            m_state->game.version++;
        }
        return false;
    }
}

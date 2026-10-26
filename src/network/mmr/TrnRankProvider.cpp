#include "network/mmr/TrnRankProvider.hpp"
#include "network/http/CurlImpersonateHttpClient.hpp"
#include "network/mmr/MmrServiceDetail.hpp"
#include "core/config/Config.hpp"
#include "utils/PrivacyLog.hpp"
#include "network/mmr/MmrService.hpp"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <iostream>
#include <thread>

using namespace MmrServiceDetail;

std::string TrnRankProvider::GetTRNPlatform(const std::string& primaryId) {
    const size_t delim = primaryId.find('|');
    if (delim == std::string::npos) return "";

    std::string plat = primaryId.substr(0, delim);
    std::transform(plat.begin(), plat.end(), plat.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

    if (plat == "steam") return "steam";
    if (plat == "epic") return "epic";
    if (plat == "ps4" || plat == "ps5" || plat == "psn") return "psn";
    if (plat == "xbox" || plat == "xbl") return "xbl";
    if (plat == "switch") return "switch";

    return "";
}

TrnRankProvider::TrnRankProvider(std::shared_ptr<Http::IHttpClient> httpClient,
                                 std::function<bool()> abortCheck,
                                 MmrTimings timings)
    : m_httpClient(httpClient ? std::move(httpClient) : std::make_shared<Http::CurlImpersonateHttpClient>()),
      m_abortCheck(std::move(abortCheck)), m_timings(timings) {}

bool TrnRankProvider::IsAvailable() const {
    const auto config = Config::Read();
    if (!config.enable_mmr_tracking) return false;
    return m_httpClient && m_httpClient->IsAvailable();
}

MmrServiceDetail::CustomApiBatchResult TrnRankProvider::FetchBatch(const AssembledBatch&) {
    CustomApiBatchResult res;
    res.httpResult = CustomApiFetchResult::DisabledOrNotReady;
    return res;
}

bool TrnRankProvider::FetchProfile(const MMRRequest& req,
                                   NormalizedProfileResult& outProfile,
                                   std::string& outDetail) {
    m_lastResult = {};
    if (!IsAvailable()) {
        m_lastResult.failure = Failure::Unavailable;
        outDetail = "The rank lookup component is not available.";
        return false;
    }

    const std::string plat = GetTRNPlatform(req.primaryId);
    if (plat.empty()) {
        m_lastResult.failure = Failure::Unsupported;
        outDetail = "Tracker does not support this platform.";
        return false;
    }
    if (req.reason == MMRRequestReason::TrackedAccount && plat != "steam" &&
        req.name.find_first_not_of(" \t") == std::string::npos) {
        m_lastResult.failure = Failure::Unsupported;
        outDetail = "Tracker looks up this platform by in-game name. Play once on this account with OmniStats running, or use the OmniStats rank API.";
        return false;
    }

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

    // Escape identity for URL path segment
    std::string escapedIdent;
    for (unsigned char c : ident) {
        if (std::isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~') {
            escapedIdent.push_back(static_cast<char>(c));
        } else {
            char hexBuf[4];
            snprintf(hexBuf, sizeof(hexBuf), "%%%02X", c);
            escapedIdent.append(hexBuf);
        }
    }

    const std::string url = "https://api.tracker.gg/api/v2/rocket-league/standard/profile/" + plat + "/" + escapedIdent;

    Http::HttpRequest httpReq;
    httpReq.url = url;
    httpReq.method = Http::Method::Get;
    httpReq.impersonateTarget = kTrackerImpersonation;
    httpReq.timeout = std::chrono::seconds(15);
    httpReq.followLocation = true;
    httpReq.shouldAbort = m_abortCheck;
    httpReq.headers = {
        "Accept: application/json, text/plain, */*",
        "Accept-Language: en-US,en;q=0.9",
        "Origin: https://rocketleague.tracker.network",
        "Referer: https://rocketleague.tracker.network/",
        std::string("User-Agent: ") + kTrackerUserAgent};

    Http::HttpResponse httpResp;
    for (int attempt = 1; attempt <= kTrackerForbiddenAttempts; ++attempt) {
        httpResp = m_httpClient->Request(httpReq);
        m_lastResult.statusCode = httpResp.statusCode;
        m_lastResult.transportError = httpResp.transportError;
        m_lastResult.retryAfterSeconds = httpResp.retryAfterSeconds;
        if (httpResp.transportError != 0 || httpResp.statusCode != 403 || attempt == kTrackerForbiddenAttempts) {
            break;
        }
        std::cout << "[MmrService] Tracker.gg returned HTTP 403; retrying with Chrome 136 (attempt "
                  << attempt + 1 << "/" << kTrackerForbiddenAttempts << ").\n";
        std::this_thread::sleep_for(m_timings.forbiddenAttemptDelay);
    }

    if (httpResp.transportError != 0 || httpResp.statusCode != 200) {
        m_lastResult.failure = Failure::Http;
        return false;
    }

    try {
        const nlohmann::json jsonResp = nlohmann::json::parse(httpResp.body);

        if (!jsonResp.contains("data") || !jsonResp["data"].is_object() ||
            !jsonResp["data"].contains("segments") || !jsonResp["data"]["segments"].is_array()) {
            m_lastResult.failure = Failure::Incomplete;
            outDetail = "Tracker response contains no segment data";
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

            const std::string playlistName = MmrService::PlaylistNameForTrackerId(attrs["playlistId"].get<int>());
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
            if (playlistName == "t") tier = MmrService::GetTournamentTierForMmr(mmr);

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

        outProfile.bestMmr = bestMMR;
        outProfile.bestTier = bestTier;
        outProfile.bestPlaylistName = bestPlaylistName;
        outProfile.playlistMMRs = std::move(playlistMMRs);
        outProfile.playlistTiers = std::move(playlistTiers);
        outProfile.playlistMatches = std::move(playlistMatches);
        outProfile.totalWins = MmrService::ExtractProfileTotals(jsonResp).totalWins;
        outProfile.rankVerificationSource = "Tracker";
        if (jsonResp["data"].contains("platformInfo") && jsonResp["data"]["platformInfo"].is_object()) {
            const auto& pInfo = jsonResp["data"]["platformInfo"];
            if (pInfo.contains("platformUserHandle") && pInfo["platformUserHandle"].is_string()) {
                outProfile.displayName = pInfo["platformUserHandle"].get<std::string>();
            }
        }

        return true;
    } catch (const std::exception& e) {
        std::cout << "[MmrService] JSON Parse Error: " << e.what() << "\n";
        m_lastResult.failure = Failure::Invalid;
        outDetail = "Tracker returned an invalid response.";
        return false;
    }
}

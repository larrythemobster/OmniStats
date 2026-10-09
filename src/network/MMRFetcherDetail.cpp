#include "network/MMRFetcherDetail.hpp"
#include "MMRFetcher.hpp"

#include <algorithm>
#include <cctype>
#include <cmath>
namespace MMRFetcherDetail {
    bool CaseInsensitiveEquals(std::string_view a, std::string_view b) {
        if (a.size() != b.size()) return false;
        for (size_t i = 0; i < a.size(); ++i) {
            if (std::tolower(static_cast<unsigned char>(a[i])) !=
                std::tolower(static_cast<unsigned char>(b[i]))) {
                return false;
            }
        }
        return true;
    }
    bool TryReadStatValue(const nlohmann::json& stats, std::initializer_list<const char*> keys, int& out) {
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

    bool ShouldReplacePlaylistBucket(const std::map<std::string, int>& playlistMMRs, const std::string& playlistName, int mmr) {
        auto it = playlistMMRs.find(playlistName);
        return it == playlistMMRs.end() || mmr > it->second;
    }

    std::chrono::milliseconds ForbiddenLockoutForStrike(size_t strike) {
        if (strike <= 1) return kForbiddenLockoutFirst;
        if (strike == 2) return kForbiddenLockoutSecond;
        return kForbiddenLockoutMaximum;
    }

    std::vector<int> BuildDirectionalMmrPath(
        int initialMmr,
        int finalMmr,
        const std::vector<bool>& results) {
        if (results.empty() || initialMmr <= 0 || finalMmr <= 0)
            return {};

        const size_t n = results.size();
        size_t wins = 0;
        size_t losses = 0;
        for (bool won : results) {
            if (won)
                ++wins;
            else
                ++losses;
        }
        const int totalDelta = finalMmr - initialMmr;
        if (totalDelta > 0 && wins == 0) return {};
        if (totalDelta < 0 && losses == 0) return {};
        if (totalDelta == 0 && (wins == 0 || losses == 0) && (wins > 0 || losses > 0)) return {};

        double winStep = 9.0;
        double lossStep = 9.0;

        if (totalDelta > 0) {
            lossStep = 9.0;
            winStep = (totalDelta + lossStep * losses) / static_cast<double>(wins);
        } else if (totalDelta < 0) {
            winStep = 9.0;
            lossStep = (winStep * wins - totalDelta) / static_cast<double>(losses);
        } else if (wins > 0 && losses > 0) {
            winStep = 9.0;
            lossStep = (winStep * wins) / static_cast<double>(losses);
        }

        std::vector<int> path;
        path.reserve(n);
        double currentMmr = static_cast<double>(initialMmr);

        for (size_t i = 0; i < n; ++i) {
            if (results[i]) {
                currentMmr += winStep;
            } else {
                currentMmr -= lossStep;
            }
            if (currentMmr <= 0)
                return {};
            if (i == n - 1) {
                path.push_back(finalMmr);
            } else {
                path.push_back(static_cast<int>(std::round(currentMmr)));
            }
        }

        return MmrPathPreservesResults(initialMmr, path, results)
                   ? path
                   : std::vector<int>{};
    }

    bool MmrPathPreservesResults(
        int initialMmr,
        const std::vector<int>& path,
        const std::vector<bool>& results) {
        if (initialMmr <= 0 || path.size() != results.size())
            return false;

        int previousMmr = initialMmr;
        for (size_t i = 0; i < path.size(); ++i) {
            const int currentMmr = path[i];
            if (currentMmr <= 0)
                return false;
            if (results[i] ? currentMmr <= previousMmr
                           : currentMmr >= previousMmr) {
                return false;
            }
            previousMmr = currentMmr;
        }
        return true;
    }

    std::vector<int> ReconcileEstimatedPath(
        int initialMmr,
        int finalMmr,
        const std::vector<int>& estimatedPath,
        const std::vector<bool>& results) {
        if (estimatedPath.empty() ||
            estimatedPath.size() != results.size() ||
            initialMmr <= 0 ||
            finalMmr <= 0) {
            return {};
        }

        std::vector<int> path = estimatedPath;
        path.back() = finalMmr;
        if (MmrPathPreservesResults(initialMmr, path, results))
            return path;

        const int estimatedFinal = estimatedPath.back();
        const int error = finalMmr - estimatedFinal;
        const int pathLength = static_cast<int>(path.size());

        for (int i = 0; i < pathLength; ++i) {
            const int correction =
                static_cast<int>(std::round(
                    static_cast<double>(error) * (i + 1) /
                    pathLength));
            path[i] = estimatedPath[i] + correction;
        }
        path.back() = finalMmr;

        if (MmrPathPreservesResults(initialMmr, path, results))
            return path;

        return BuildDirectionalMmrPath(
            initialMmr, finalMmr, results);
    }
    bool TryParseCustomApiTarget(const std::string& primaryId, CustomApiPlayerTarget& outTarget) {
        const size_t delim = primaryId.find('|');
        if (delim == std::string::npos) return false;

        std::string rawPlat = primaryId.substr(0, delim);
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
        else
            return false;

        std::string accountId = primaryId.substr(delim + 1);
        const size_t secondDelim = accountId.find('|');
        if (secondDelim != std::string::npos) {
            accountId = accountId.substr(0, secondDelim);
        }
        accountId.erase(0, accountId.find_first_not_of(" \t\r\n"));
        accountId.erase(accountId.find_last_not_of(" \t\r\n") + 1);
        if (accountId.empty()) return false;

        outTarget.platform = std::move(platform);
        outTarget.accountId = std::move(accountId);
        return true;
    }

    int CustomApiPlaylistIdForName(const std::string& playlist) {
        if (playlist == "1v1") return 10;
        if (playlist == "2v2") return 11;
        if (playlist == "3v3") return 13;
        if (playlist == "hoops") return 27;
        if (playlist == "rumble") return 28;
        if (playlist == "dropshot") return 29;
        if (playlist == "snowday") return 30;
        if (playlist == "t") return 34;
        if (playlist == "heatseeker") return 43;
        return 0;
    }

    AssembledBatch AssembleCustomApiBatch(
        const std::vector<MMRRequest>& candidates,
        std::chrono::steady_clock::time_point now,
        size_t maxBatchSize) {
        AssembledBatch batch;
        if (candidates.empty() || maxBatchSize == 0) return batch;

        std::string targetPlaylist;
        bool foundFirst = false;

        for (const auto& req : candidates) {
            if (req.reason != MMRRequestReason::Roster) continue;
            if (req.notBefore > now) continue;

            if (!foundFirst) {
                targetPlaylist = req.playlist;
                batch.playlist = targetPlaylist;
                batch.playlistId = CustomApiPlaylistIdForName(targetPlaylist);
                foundFirst = true;
            }

            if (req.playlist == targetPlaylist) {
                batch.requests.push_back(req);
                if (batch.requests.size() >= maxBatchSize) {
                    break;
                }
            }
        }
        return batch;
    }

    nlohmann::json BuildCustomApiBatchJson(const AssembledBatch& batch) {
        nlohmann::json playersArray = nlohmann::json::array();
        for (const auto& req : batch.requests) {
            CustomApiPlayerTarget target;
            if (TryParseCustomApiTarget(req.primaryId, target)) {
                playersArray.push_back({{"platform", std::move(target.platform)},
                                        {"account_id", std::move(target.accountId)}});
            }
        }
        nlohmann::json reqBody = {{"players", std::move(playersArray)}};
        if (batch.playlistId > 0) {
            reqBody["playlist"] = batch.playlistId;
        }
        return reqBody;
    }

    bool TryParseCustomApiPlayerProfile(
        const nlohmann::json& playerJson,
        NormalizedProfileResult& outProfile) {
        if (!playerJson.is_object()) return false;
        if (playerJson.contains("error") && !playerJson["error"].is_null()) {
            return false;
        }
        if (!playerJson.contains("skills") || !playerJson["skills"].is_array() ||
            playerJson["skills"].empty()) {
            return false;
        }

        int bestMmr = 0;
        std::string bestTier = "Unranked";
        std::string bestPlaylistName = "best";
        std::map<std::string, int> playlistMMRs;
        std::map<std::string, std::string> playlistTiers;
        std::map<std::string, int> playlistMatches;

        for (const auto& skill : playerJson["skills"]) {
            if (!skill.is_object()) continue;
            if (!skill.contains("playlist") || !skill["playlist"].is_number_integer()) {
                continue;
            }
            int pid = skill["playlist"].get<int>();
            std::string plName = MMRFetcher::PlaylistNameForTrackerId(pid);
            if (plName.empty()) {
                continue;
            }

            if (!skill.contains("mmr") || !skill["mmr"].is_number()) {
                continue;
            }
            double mmrDouble = skill["mmr"].get<double>();
            int mmrInt = static_cast<int>(std::lround(mmrDouble));
            if (mmrInt <= 0) {
                continue;
            }

            int tier = skill.value("tier", 0);
            int div = skill.value("division", 0);
            int matches = skill.value("matches_played", -1);

            std::string tierName = (plName == "t")
                                       ? MMRFetcher::GetTournamentTierForMmr(mmrInt)
                                       : MMRFetcher::RankTierName(tier, div);

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
            return false;
        }

        playlistMMRs["best"] = bestMmr;
        playlistTiers["best"] = bestTier;
        if (playlistMatches.count(bestPlaylistName)) {
            playlistMatches["best"] = playlistMatches[bestPlaylistName];
        }

        outProfile.bestMmr = bestMmr;
        outProfile.bestTier = std::move(bestTier);
        outProfile.bestPlaylistName = std::move(bestPlaylistName);
        outProfile.playlistMMRs = std::move(playlistMMRs);
        outProfile.playlistTiers = std::move(playlistTiers);
        outProfile.playlistMatches = std::move(playlistMatches);
        int totalWins = -1;
        if (playerJson.contains("wins") && playerJson["wins"].is_number_integer()) {
            totalWins = playerJson["wins"].get<int>();
        } else if (playerJson.contains("total_wins") && playerJson["total_wins"].is_number_integer()) {
            totalWins = playerJson["total_wins"].get<int>();
        }
        outProfile.totalWins = totalWins;
        outProfile.rankVerificationSource = "ServerA";
        return true;
    }

    std::vector<BatchPlayerResult> ParseCustomApiBatchResponse(
        const std::vector<MMRRequest>& batchRequests,
        const nlohmann::json& jsonResp) {
        std::vector<BatchPlayerResult> results;
        results.reserve(batchRequests.size());

        if (jsonResp.is_object() && jsonResp.contains("error") && jsonResp["error"].is_object()) {
            std::string code = jsonResp["error"].value("code", "unknown");
            std::string msg = jsonResp["error"].value("message", "");
            for (const auto& req : batchRequests) {
                BatchPlayerResult res;
                res.request = req;
                res.status = (code == "not_found") ? BatchPlayerStatus::NotFound : BatchPlayerStatus::Error;
                res.errorCode = code;
                res.errorMessage = msg;
                results.push_back(std::move(res));
            }
            return results;
        }

        const bool hasPlayersArray = jsonResp.is_object() &&
                                     jsonResp.contains("players") &&
                                     jsonResp["players"].is_array();

        for (const auto& req : batchRequests) {
            BatchPlayerResult res;
            res.request = req;

            CustomApiPlayerTarget target;
            if (!TryParseCustomApiTarget(req.primaryId, target)) {
                res.status = BatchPlayerStatus::UnusableData;
                res.errorCode = "invalid_primary_id";
                res.errorMessage = "Invalid primaryId format or unsupported platform";
                results.push_back(std::move(res));
                continue;
            }

            if (!hasPlayersArray) {
                res.status = BatchPlayerStatus::UnusableData;
                res.errorCode = "missing_players";
                res.errorMessage = "Response does not contain players array";
                results.push_back(std::move(res));
                continue;
            }

            const nlohmann::json* matched = nullptr;
            for (const auto& p : jsonResp["players"]) {
                if (!p.is_object()) continue;
                if (p.contains("account_id") && p["account_id"].is_string()) {
                    if (CaseInsensitiveEquals(p["account_id"].get<std::string>(), target.accountId)) {
                        matched = &p;
                        break;
                    }
                }
            }

            if (!matched && batchRequests.size() == 1 && jsonResp["players"].size() == 1) {
                const auto& firstP = jsonResp["players"][0];
                if (firstP.is_object()) {
                    if (!firstP.contains("account_id") || !firstP["account_id"].is_string() ||
                        firstP["account_id"].get<std::string>().empty() ||
                        CaseInsensitiveEquals(firstP["account_id"].get<std::string>(), target.accountId)) {
                        matched = &firstP;
                    }
                }
            }

            if (!matched) {
                res.status = BatchPlayerStatus::NotFound;
                res.errorCode = "not_found";
                res.errorMessage = "Player missing from response";
                results.push_back(std::move(res));
                continue;
            }

            if (matched->contains("error") && !(*matched)["error"].is_null()) {
                std::string errCode = "player_error";
                std::string errMsg;
                if ((*matched)["error"].is_object()) {
                    errCode = (*matched)["error"].value("code", "player_error");
                    errMsg = (*matched)["error"].value("message", "");
                } else if ((*matched)["error"].is_string()) {
                    errMsg = (*matched)["error"].get<std::string>();
                }
                res.status = (errCode == "not_found") ? BatchPlayerStatus::NotFound : BatchPlayerStatus::Error;
                res.errorCode = std::move(errCode);
                res.errorMessage = std::move(errMsg);
                results.push_back(std::move(res));
                continue;
            }

            NormalizedProfileResult profile;
            if (TryParseCustomApiPlayerProfile(*matched, profile)) {
                res.status = BatchPlayerStatus::Success;
                res.profile = std::move(profile);
            } else {
                res.status = BatchPlayerStatus::UnusableData;
                res.errorCode = "unusable_skills";
                res.errorMessage = "Player skills missing or invalid";
            }
            results.push_back(std::move(res));
        }

        return results;
    }
}

#include "network/MMRFetcherDetail.hpp"

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
}

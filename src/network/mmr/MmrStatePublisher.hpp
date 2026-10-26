#pragma once

#include "core/state/SessionState.hpp"
#include <map>
#include <optional>
#include <string>
#include <unordered_set>
#include <vector>

struct MMRRequest;
struct NormalizedProfileResult;

namespace MmrPublisher {

    struct ReconciledMatchUpdate {
        std::string matchGuid;
        int mmr = 0;
        int matchesPlayed = -1;
        int sessionStartMmr = 0;
        bool won = false;
        bool provisional = false;
        bool valueEstimated = false;
        bool databaseRowUpdated = false;
        bool inSession = true;
        uint64_t sessionGeneration = 0;
    };

    struct ReconcilePublication {
        std::string primaryId;
        std::string playlist;
        uint64_t sessionGeneration = 0;
        std::vector<ReconciledMatchUpdate> updates;
        bool hasRecapUpdate = false;
        int recapLatestMmr = 0;
        int currentSessionSeedMmr = 0;
        bool shouldUpdatePreMatchSnapshots = false;
        int fetchedMatches = -1;
        std::unordered_set<std::string> protectedSnapshotGuids;
        std::unordered_set<uint64_t> pendingSessionGenerations;
    };

    struct ProvisionalPublication {
        std::string primaryId;
        std::string playlist;
        std::string matchGuid;
        uint64_t sessionGeneration = 0;
        int provisionalMmr = 0;
        int sessionStartMmr = 0;
        int matchesPlayed = -1;
        bool won = false;
        bool databaseRowUpdated = false;
        bool replaceExisting = false;
    };

    void PublishRosterFetchFailure(SessionState& state, const std::string& primaryId);

    bool PublishCachedLocalRoster(SessionState& state,
                                  const std::string& primaryId,
                                  const std::map<std::string, int>& playlists,
                                  const std::map<std::string, std::string>& playlistTiers,
                                  const std::map<std::string, int>& playlistMatches,
                                  int bestMmr,
                                  const std::string& bestTier,
                                  int totalWins);

    bool PublishProfileResult(SessionState& state,
                              const MMRRequest& req,
                              const NormalizedProfileResult& profile,
                              const std::unordered_set<std::string>* playlistsAwaitingPostMatch = nullptr);

    std::optional<SessionRecap> PublishReconciledPoints(SessionState& state,
                                                        const ReconcilePublication& pub);

    // Caller holds the exclusive history lock.
    std::optional<SessionRecap> PublishReconciledPointsLocked(SessionState& state,
                                                              const ReconcilePublication& pub);

    void PublishProvisionalPoint(SessionState& state,
                                 const ProvisionalPublication& pub);

    // Caller holds the exclusive history lock.
    void PublishProvisionalPointLocked(SessionState& state,
                                       const ProvisionalPublication& pub);

} // namespace MmrPublisher

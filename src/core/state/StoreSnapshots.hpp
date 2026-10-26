#pragma once

#include "core/accounts/PlayerNotes.hpp"
#include "core/accounts/PlayerScout.hpp"
#include "core/game/Insights.hpp"
#include "core/state/SessionTypes.hpp"
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace database::store {

    struct HistorySnapshot {
        std::vector<float> mmrHistoryY;
        std::vector<float> mmrHistoryX;
        std::map<std::string, std::vector<float>> playlistHistoryY;
        std::map<std::string, std::vector<SessionMmrPoint>> playlistMatchPoints;
        std::map<std::string, int> playlistInitialMmr;
        std::string mmrOwnerPrimaryId;
        std::map<std::string, AccountMmrHistory> inactiveMmrHistories;
        std::vector<float> lifetimeMmrY;
        std::vector<int64_t> lifetimeMmrX;
        bool showLifetimeGraph = false;
        std::vector<SessionMatchSummary> recentSavedMatches;
        std::vector<SessionMatchSummary> pendingRecentMatches;
        bool recentSavedMatchesLoaded = false;
        int initialMmr = -1;
        uint64_t version = 0;
    };

    struct InsightsSnapshot {
        bool loaded = false;
        std::string primaryId;
        std::vector<PersonRecord> people;
        std::vector<MatchOutcome> outcomes;
        std::vector<MatchMmrContext> mmrContext;
        std::vector<SessionRecap> sessions;
        std::vector<AnalyticsMatchRow> analyticsMatches;
        uint64_t version = 0;
    };

    struct HistoryViewSnapshot {
        uint64_t requestId = 0;
        uint64_t detailRequestId = 0;
        bool loading = false;
        bool detailLoading = false;
        bool appendMode = false;
        std::vector<MatchRow> rows;
        int total = 0;
        std::optional<MatchDetail> detail;
        uint64_t version = 0;
    };

    struct PlayerNotesSnapshot {
        bool loaded = false;
        bool loadFailed = false;
        std::unordered_map<std::string, PlayerNotes::Note> byKey;
        std::unordered_map<std::string, std::vector<std::string>> tagsByKey;
        uint64_t completedRequestId = 0;
        bool lastRequestOk = true;
        std::string lastRequestError;
        uint64_t completedTagRequestId = 0;
        bool lastTagRequestOk = true;
        std::string lastTagRequestError;
        uint64_t version = 0;
    };

    struct PlayerScoutSnapshot {
        uint64_t requestId = 0;
        std::string playerId;
        bool loaded = false;
        bool success = false;
        PlayerScoutProfile profile;
        uint64_t version = 0;
    };

} // namespace database::store

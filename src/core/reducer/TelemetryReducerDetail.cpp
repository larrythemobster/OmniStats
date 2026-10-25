#include "core/reducer/TelemetryReducerDetail.hpp"

#include <algorithm>
#include <cctype>
#include <iostream>
#include <optional>
#include <utility>

#include "core/config/Config.hpp"
#include "core/config/Constants.hpp"
#include "core/game/GamemodeUtils.hpp"
#include "core/game/PlaylistMetadata.hpp"
#include "utils/PrivacyLog.hpp"

namespace TelemetryReducerDetail {

    MmrCategory CategoryFromTeamCounts(const std::array<int, 2>& teamCounts, bool roundStarted) {
        int teamSize = (std::min)(teamCounts[0], teamCounts[1]);
        if (teamSize >= 3) return MmrCategory::ThreeVThree;
        if (teamSize >= 2) return MmrCategory::TwoVTwo;
        if (roundStarted && teamCounts[0] == 1 && teamCounts[1] == 1) return MmrCategory::OneVOne;
        return MmrCategory::Best;
    }

    MmrCategory CategoryFromMode(const std::string& mode) {
        if (mode == "1v1") return MmrCategory::OneVOne;
        if (mode == "2v2") return MmrCategory::TwoVTwo;
        if (mode == "3v3") return MmrCategory::ThreeVThree;
        if (mode == "hoops") return MmrCategory::Hoops;
        if (mode == "rumble") return MmrCategory::Rumble;
        if (mode == "dropshot") return MmrCategory::Dropshot;
        if (mode == "snowday") return MmrCategory::SnowDay;
        if (mode == "heatseeker") return MmrCategory::Heatseeker;
        return MmrCategory::Best;
    }

    template <typename T>
    static std::string InferModeImpl(const T& match, MmrCategory rosterCategory) {
        if (PlaylistMetadata::HasAuthoritativeId(match.playlistId)) {
            const std::string playlistMode = PlaylistMetadata::CanonicalMode(match.playlistId);
            return playlistMode.empty() ? "unknown" : playlistMode;
        }

        if (match.fallbackCasualContext) return "casual";
        if (match.fallbackNonRecordableContext) return "unknown";

        std::string arenaKey = !match.arenaAsset.empty() ? match.arenaAsset : match.arenaName;
        return GamemodeUtils::InferFromSnapshot(
            match.legacyMaxPlayersSeen,
            static_cast<int>(match.roster.size()),
            rosterCategory,
            arenaKey);
    }

    std::string InferModeFromMatchState(const MatchState& match, MmrCategory rosterCategory) {
        return InferModeImpl(match, rosterCategory);
    }

    std::string InferModeFromMatchState(const LiveMatchSnapshot& match, MmrCategory rosterCategory) {
        return InferModeImpl(match, rosterCategory);
    }

    template <typename T>
    static MmrCategory CategoryFromMatchContextImpl(const T& match) {
        if (PlaylistMetadata::HasAuthoritativeId(match.playlistId)) {
            return PlaylistMetadata::Category(match.playlistId);
        }

        if (match.fallbackNonRecordableContext) return MmrCategory::Best;
        if (match.fallbackCasualContext) return MmrCategory::Casual;
        std::string arenaKey = !match.arenaAsset.empty() ? match.arenaAsset : match.arenaName;
        MmrCategory arenaCategory = CategoryFromMode(GamemodeUtils::InferFromArenaName(arenaKey));
        if (arenaCategory != MmrCategory::Best) return arenaCategory;
        return CategoryFromTeamCounts(match.legacyMaxTeamPlayersSeen, match.roundEverStarted);
    }

    MmrCategory CategoryFromMatchContext(const MatchState& match) {
        return CategoryFromMatchContextImpl(match);
    }

    MmrCategory CategoryFromMatchContext(const LiveMatchSnapshot& match) {
        return CategoryFromMatchContextImpl(match);
    }

    bool IsSupportedMmrCategory(MmrCategory category) {
        return category == MmrCategory::OneVOne ||
               category == MmrCategory::TwoVTwo ||
               category == MmrCategory::ThreeVThree ||
               category == MmrCategory::Casual ||
               category == MmrCategory::Tourny ||
               IsExtraMmrCategory(category);
    }

    bool IsTrackedRankedEarlyExitMode(const std::string& mode) {
        return mode == "1v1" || mode == "2v2" || mode == "3v3" ||
               mode == "hoops" || mode == "rumble" || mode == "dropshot" ||
               mode == "snowday" || mode == "heatseeker";
    }

    std::string FormatPendingMatchHistoryMode(const std::string& mode) {
        if (mode == "1v1") return "Duel";
        if (mode == "2v2") return "Doubles";
        if (mode == "3v3") return "Standard";
        if (mode == "hoops") return "Hoops";
        if (mode == "rumble") return "Rumble";
        if (mode == "dropshot") return "Dropshot";
        if (mode == "snowday") return "Snow Day";
        if (mode == "heatseeker") return "Heatseeker";
        return mode.empty() ? "Unknown" : mode;
    }

    std::string Lowercase(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    bool AcceptsAnalyticsEvent(const MatchState& state, const std::string& eventMatchGuid, bool duringReplay) {
        if (!state.inMatch || state.matchFinalized || (state.inReplay && !duringReplay) || state.nonLiveReplayActive ||
            state.fallbackNonRecordableContext || PlaylistMetadata::IsNonRecordable(state.playlistId)) {
            return false;
        }
        if (eventMatchGuid.empty()) {
            return !(state.missingGuidAssociationBlockedByReconnect && state.matchGuid.empty());
        }
        if (!state.matchGuid.empty() && eventMatchGuid != state.matchGuid) return false;
        return state.finalizedMatchGuids.count(eventMatchGuid) == 0 &&
               state.pendingDestroyedMatches.count(eventMatchGuid) == 0;
    }

    void PublishUiTimestamps(MatchState& state, SideEffects& effects) {
        UiStateUpdate& ui = effects.uiUpdate ? *effects.uiUpdate : effects.uiUpdate.emplace();
        ui.resetTimestamps = false;
        if (state.hideOverlayRequested) {
            ui.matchReset = true;
            state.hideOverlayRequested = false;
        }
        ui.lastMatchStartMs = state.lastMatchStartMs;
        ui.firstCountdownOfMatchMs = state.firstCountdownOfMatchMs;
        ui.lastCountdownMs = state.lastCountdownMs;
        ui.lastGoalMs = state.lastGoalMs;
        ui.lastMatchEndMs = state.lastMatchEndMs;
        ui.lastPodiumMs = state.lastPodiumMs;
        ui.inGoalReplay = state.inGoalReplay;
        ui.matchSummaryStartMs = state.matchSummaryStartMs;
    }

} // namespace TelemetryReducerDetail

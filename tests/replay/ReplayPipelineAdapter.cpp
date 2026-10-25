#include "ReplayPipelineAdapter.hpp"

#include "core/effects/EffectRouter.hpp"
#include "core/reducer/TelemetryReducer.hpp"
#include "core/state/LiveMatchStore.hpp"
#include "network/telemetry/TelemetryEvents.hpp"

#include <cmath>
#include <iostream>

namespace omnistats::replay {

    static StepSideEffectsSummary SummarizeSideEffects(const std::string& eventName, const SideEffects& effects) {
        StepSideEffectsSummary summary;
        summary.eventName = eventName;
        nlohmann::json actions = nlohmann::json::array();

        if (effects.saveMatch) {
            summary.hasEffects = true;
            const auto& snap = effects.saveSnapshot;
            actions.push_back({{"action", "SaveMatch"},
                               {"matchGuid", snap.matchGuid},
                               {"arena", snap.arenaName},
                               {"playlistId", snap.playlistId},
                               {"gamemode", snap.gamemode},
                               {"score", {snap.score[0], snap.score[1]}},
                               {"winnerTeam", snap.winnerTeam},
                               {"validResult", snap.validResult},
                               {"resultPending", snap.resultPending},
                               {"playerCount", snap.roster.size()}});
        }

        if (effects.provisionalSaveSnapshot.has_value()) {
            summary.hasEffects = true;
            const auto& snap = *effects.provisionalSaveSnapshot;
            actions.push_back({{"action", "ProvisionalSaveMatch"},
                               {"matchGuid", snap.matchGuid},
                               {"arena", snap.arenaName},
                               {"playlistId", snap.playlistId},
                               {"gamemode", snap.gamemode},
                               {"score", {snap.score[0], snap.score[1]}},
                               {"winnerTeam", snap.winnerTeam}});
        }

        if (effects.persistSession.has_value()) {
            summary.hasEffects = true;
            const auto& recap = *effects.persistSession;
            actions.push_back({{"action", "PersistSession"},
                               {"account", recap.mmrOwnerPrimaryId},
                               {"wins", recap.totals.wins},
                               {"losses", recap.totals.losses},
                               {"matchCount", recap.matchGuids.size()}});
        }

        if (effects.updateSession.has_value()) {
            summary.hasEffects = true;
            const auto& recap = *effects.updateSession;
            actions.push_back({{"action", "UpdateSession"},
                               {"account", recap.mmrOwnerPrimaryId},
                               {"wins", recap.totals.wins},
                               {"losses", recap.totals.losses}});
        }

        if (effects.postMatchMmrRefresh.has_value()) {
            summary.hasEffects = true;
            const auto& post = *effects.postMatchMmrRefresh;
            actions.push_back({{"action", "PostMatchMmrRefresh"},
                               {"primaryId", post.primaryId},
                               {"matchGuid", post.matchGuid},
                               {"playlist", post.playlist},
                               {"won", post.won},
                               {"previousMmr", post.previousMmr}});
        }

        if (effects.pendingDestroyedMatch.has_value()) {
            summary.hasEffects = true;
            const auto& pending = *effects.pendingDestroyedMatch;
            actions.push_back({{"action", "PendingDestroyedMatch"},
                               {"primaryId", pending.primaryId},
                               {"matchGuid", pending.matchGuid},
                               {"playlist", pending.playlist},
                               {"validCompetitiveMatch", pending.validCompetitiveMatch},
                               {"localTeam", pending.localTeam}});
        }

        if (effects.resolvedDestroyedMatch.has_value()) {
            summary.hasEffects = true;
            const auto& res = *effects.resolvedDestroyedMatch;
            actions.push_back({{"action", "ResolvedDestroyedMatch"},
                               {"matchGuid", res.matchGuid},
                               {"won", res.won}});
        }

        if (effects.confirmedLocalAccount.has_value()) {
            summary.hasEffects = true;
            actions.push_back({{"action", "ConfirmedLocalAccount"},
                               {"primaryId", effects.confirmedLocalAccount->primaryId},
                               {"name", effects.confirmedLocalAccount->name}});
        }

        if (effects.pushDiscord) {
            summary.hasEffects = true;
            const auto& snap = effects.discordSnapshot;
            actions.push_back({{"action", "DiscordPresence"},
                               {"inMatch", snap.inMatch},
                               {"arenaName", snap.arenaName},
                               {"score", {snap.score[0], snap.score[1]}}});
        }

        if (effects.fetchLifetimeHistory) {
            summary.hasEffects = true;
            actions.push_back({{"action", "FetchLifetimeHistory"},
                               {"primaryId", effects.lifetimePrimaryId},
                               {"category", effects.lifetimeCategory}});
        }

        if (effects.refreshDbStats) {
            summary.hasEffects = true;
            actions.push_back({{"action", "RefreshDbStats"},
                               {"primaryId", effects.refreshStatsPrimaryId}});
        }

        if (!effects.fetchMmrQueue.empty()) {
            summary.hasEffects = true;
            std::vector<std::string> pids;
            for (const auto& [pid, name] : effects.fetchMmrQueue) {
                pids.push_back(pid);
            }
            actions.push_back({{"action", "FetchMmrQueue"},
                               {"players", pids}});
        }

        if (!effects.fetchEncounterQueue.empty()) {
            summary.hasEffects = true;
            actions.push_back({{"action", "FetchEncounterQueue"},
                               {"players", effects.fetchEncounterQueue}});
        }

        if (summary.hasEffects) {
            summary.details["event"] = eventName;
            summary.details["actions"] = std::move(actions);
        }
        return summary;
    }

    StepSideEffectsSummary DispatchWireLine(ReplayPipelineContext& ctx, const std::string& rawLine) {
        if (rawLine.empty()) return {};

        nlohmann::json msg;
        try {
            msg = nlohmann::json::parse(rawLine);
        } catch (const std::exception& e) {
            std::cerr << "[ReplayPipelineAdapter] JSON parse error: " << e.what() << "\n";
            return {};
        }

        if (!msg.contains("Event") || !msg["Event"].is_string() || !msg.contains("Data")) {
            return {};
        }

        const std::string eventName = msg["Event"].get<std::string>();
        const std::string dataStr = msg["Data"].is_string() ? msg["Data"].get<std::string>() : msg["Data"].dump();

        // Test-stream marker for a dropped Stats API connection.
        if (eventName == "TelemetryDisconnected" || eventName == "__Disconnect__") {
            if (ctx.state) {
                ctx.state->liveMatch->OnTelemetryDisconnected();
                ctx.state->clearActiveMatchOnDisconnect();
            }
            StepSideEffectsSummary discSummary;
            discSummary.hasEffects = true;
            discSummary.eventName = eventName;
            discSummary.details = {
                {"event", eventName},
                {"actions", {{{"action", "OnTelemetryDisconnected"}}}}};
            return discSummary;
        }

        // Recordings compress idle gaps; age the last match end so inactivity rules see the recorded gap.
        const int64_t elapsedMs = ctx.lastMatchEndedClockMs > 0 ? (ctx.currentClockMs - ctx.lastMatchEndedClockMs) : 0;
        if (eventName == "MatchCreated" && elapsedMs >= 7200 * 1000 && ctx.state) {
            ctx.state->liveMatch->UpdateMatchStateForTests([elapsedMs](MatchState& match) {
                if (match.lastMatchEndedAtUnix > 0) match.lastMatchEndedAtUnix -= elapsedMs / 1000;
            });
        }

        const auto event = TelemetryEvents::ParseEventFromJson(eventName, dataStr);
        if (!event || !ctx.state) return {};

        ReducerContext reducerContext = MakeReducerContext(ctx.state->ui);
        reducerContext.nowMs = ctx.currentClockMs > 0 ? ctx.currentClockMs : 1;
        reducerContext.nowSeconds = static_cast<double>(ctx.currentClockMs) / 1000.0;
        SideEffects effects = ctx.state->liveMatch->ApplyEvent(*event, reducerContext);
        if (effects.saveMatch || eventName == "MatchEnded") {
            ctx.lastMatchEndedClockMs = ctx.currentClockMs;
        }

        StepSideEffectsSummary summary = SummarizeSideEffects(eventName, effects);
        EffectRouter::Route(std::move(effects), ctx.dbManager, nullptr, ctx.mmrFetcher, nullptr, ctx.state);
        return summary;
    }

} // namespace omnistats::replay

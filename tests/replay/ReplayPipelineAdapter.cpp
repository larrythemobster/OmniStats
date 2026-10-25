#include "ReplayPipelineAdapter.hpp"
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
        nlohmann::json dataObj;
        if (msg["Data"].is_string()) {
            try {
                dataObj = nlohmann::json::parse(msg["Data"].get<std::string>());
            } catch (...) {
                dataObj = msg["Data"];
            }
        } else {
            dataObj = msg["Data"];
        }

        // Special test stream events for connection drops:
        if (eventName == "TelemetryDisconnected" || eventName == "__Disconnect__") {
            if (ctx.reducer) ctx.reducer->OnTelemetryDisconnected();
            if (ctx.state) {
                std::unique_lock<std::shared_mutex> lock(ctx.state->game.mutex);
                ctx.state->clearActiveMatchOnDisconnect();
                ctx.state->game.version++;
            }
            StepSideEffectsSummary discSummary;
            discSummary.hasEffects = true;
            discSummary.eventName = eventName;
            discSummary.details = {
                {"event", eventName},
                {"actions", {{{"action", "OnTelemetryDisconnected"}}}}};
            return discSummary;
        }

        const int64_t elapsedMs = ctx.lastMatchEndedClockMs > 0 ? (ctx.currentClockMs - ctx.lastMatchEndedClockMs) : 0;
        if (eventName == "MatchCreated" && elapsedMs >= 7200 * 1000 && ctx.state) {
            std::unique_lock<std::shared_mutex> lock(ctx.state->game.mutex);
            if (ctx.state->game.lastMatchEndedAtUnix > 0) {
                ctx.state->game.lastMatchEndedAtUnix -= (elapsedMs / 1000);
            }
        }

        SideEffects effects;
        if (ctx.reducer) {
            effects = ctx.reducer->Reduce(eventName, dataObj);
        }
        if (effects.saveMatch || eventName == "MatchEnded") {
            ctx.lastMatchEndedClockMs = ctx.currentClockMs;
        }

        StepSideEffectsSummary summary = SummarizeSideEffects(eventName, effects);

        if (ctx.executor) {
            ctx.executor->Execute(
                std::move(effects),
                ctx.dbManager,
                nullptr /* discordManager */,
                ctx.mmrFetcher,
                nullptr /* accountManager */);
        }

        return summary;
    }

} // namespace omnistats::replay

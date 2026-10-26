#include "core/effects/EffectRouter.hpp"

#include "core/config/Config.hpp"
#include "database/DatabaseManager.hpp"
#include "network/DiscordManager.hpp"
#include "network/mmr/MmrService.hpp"
#include "core/accounts/AccountManager.hpp"
#include "core/state/SessionState.hpp"
#include "core/state/LiveMatchStore.hpp"
#include <algorithm>
#include <mutex>

#include <utility>

void EffectRouter::Route(SideEffects&& effects,
                         std::shared_ptr<DatabaseManager> dbManager,
                         std::shared_ptr<DiscordManager> discordManager,
                         std::shared_ptr<MmrService> mmrFetcher,
                         std::shared_ptr<AccountManagerService> accountManager,
                         std::shared_ptr<SessionState> sessionState) {
    if (effects.selectMmrOwner && sessionState) {
        std::unique_lock lock(sessionState->history.mutex);
        sessionState->selectMmrOwnerLocked(*effects.selectMmrOwner);
    }
    if (effects.closeSessionIfInactiveAtUnix && sessionState && sessionState->liveMatch) {
        std::unique_lock lock(sessionState->history.mutex);
        if (sessionState->closeSessionIfInactiveLocked(*effects.closeSessionIfInactiveAtUnix)) {
            effects.persistSession = sessionState->liveMatch->GetSnapshot()->lastSessionRecap;
        }
    }
    if (effects.persistLocalPrimaryId) {
        Config::Update([&id = *effects.persistLocalPrimaryId](ConfigData& c) { c.last_primary_id = id; });
    }
    if (effects.uiUpdate && sessionState) {
        const auto& ui = *effects.uiUpdate;
        if (ui.matchReset) {
            sessionState->ui.showOverlay = false;
            sessionState->ui.showMatchSummary = false;
        }
        if (ui.resetTimestamps) {
            sessionState->ui.ResetMatchTimestamps(ui.resetMatchStartMs);
        }
        if (ui.lastMatchStartMs) sessionState->ui.lastMatchStartMs.store(*ui.lastMatchStartMs, std::memory_order_relaxed);
        if (ui.firstCountdownOfMatchMs) sessionState->ui.firstCountdownOfMatchMs.store(*ui.firstCountdownOfMatchMs, std::memory_order_relaxed);
        if (ui.lastCountdownMs) sessionState->ui.lastCountdownMs.store(*ui.lastCountdownMs, std::memory_order_relaxed);
        if (ui.lastGoalMs) sessionState->ui.lastGoalMs.store(*ui.lastGoalMs, std::memory_order_relaxed);
        if (ui.lastMatchEndMs) sessionState->ui.lastMatchEndMs.store(*ui.lastMatchEndMs, std::memory_order_relaxed);
        if (ui.lastPodiumMs) sessionState->ui.lastPodiumMs.store(*ui.lastPodiumMs, std::memory_order_relaxed);
        if (ui.inGoalReplay) sessionState->ui.inGoalReplay.store(*ui.inGoalReplay, std::memory_order_relaxed);
        if (ui.showMatchSummary) sessionState->ui.showMatchSummary = *ui.showMatchSummary;
        if (ui.matchSummaryStartMs) sessionState->ui.matchSummaryStartMs.store(*ui.matchSummaryStartMs, std::memory_order_relaxed);
        if (ui.rosterMmrCategory) sessionState->ui.rosterMmrCategory.store(*ui.rosterMmrCategory, std::memory_order_relaxed);
        if (ui.graphMmrCategory) sessionState->ui.graphMmrCategory.store(*ui.graphMmrCategory, std::memory_order_relaxed);
        if (ui.rankProgressMmrCategory) sessionState->ui.rankProgressMmrCategory.store(*ui.rankProgressMmrCategory, std::memory_order_relaxed);
    }

    if (effects.pendingMatchHistory && sessionState) {
        std::unique_lock lock(sessionState->history.mutex);
        const auto duplicate = std::find_if(
            sessionState->history.pendingRecentMatches.begin(),
            sessionState->history.pendingRecentMatches.end(),
            [&](const SessionMatchSummary& summary) {
                return summary.matchGuid == effects.pendingMatchHistory->matchGuid;
            });
        if (duplicate == sessionState->history.pendingRecentMatches.end()) {
            sessionState->history.pendingRecentMatches.insert(
                sessionState->history.pendingRecentMatches.begin(),
                std::move(*effects.pendingMatchHistory));
            if (sessionState->history.pendingRecentMatches.size() >
                static_cast<size_t>(kPreviousGamesMaxLimit)) {
                sessionState->history.pendingRecentMatches.resize(kPreviousGamesMaxLimit);
            }
            sessionState->history.version++;
        }
    }

    if (effects.confirmedLocalAccount && accountManager) {
        accountManager->ObserveLocalAccount(effects.confirmedLocalAccount->primaryId,
                                            effects.confirmedLocalAccount->name);
    }
    if (effects.pushDiscord && discordManager) {
        discordManager->PushPresenceUpdate(effects.discordSnapshot);
    }
    if (effects.fetchLifetimeHistory && dbManager) {
        dbManager->AsyncGetLifetimeMmrHistory(effects.lifetimePrimaryId, effects.lifetimeCategory);
    }
    if (effects.refreshDbStats && dbManager) {
        dbManager->AsyncRefreshDbStats(effects.refreshStatsPrimaryId);
    }
    for (const auto& [pid, name] : effects.fetchMmrQueue) {
        if (mmrFetcher) mmrFetcher->Enqueue(pid, name);
    }
    if (effects.persistSession && dbManager) {
        dbManager->AsyncSaveSession(std::move(*effects.persistSession));
    }
    if (effects.saveMatch) {
        if (dbManager) dbManager->AsyncSaveMatch(std::move(effects.saveSnapshot));
    }
    if (effects.provisionalSaveSnapshot && dbManager) {
        dbManager->AsyncSaveMatch(std::move(*effects.provisionalSaveSnapshot));
    }
    if (effects.updateSession && dbManager) {
        dbManager->AsyncUpdateSession(std::move(*effects.updateSession));
    }

    if (effects.resolvedDestroyedMatch && mmrFetcher) {
        mmrFetcher->ResolvePendingDestroyedMatch(
            effects.resolvedDestroyedMatch->matchGuid,
            effects.resolvedDestroyedMatch->won);
    }
    if (effects.pendingDestroyedMatch && mmrFetcher) {
        mmrFetcher->EnqueuePendingDestroyedMatch(
            *effects.pendingDestroyedMatch);
    }

    if (effects.postMatchMmrRefresh && mmrFetcher) {
        const auto& refresh = *effects.postMatchMmrRefresh;
        mmrFetcher->EnqueuePostMatch(refresh.primaryId,
                                     refresh.name,
                                     refresh.matchGuid,
                                     refresh.playlist,
                                     refresh.previousMmr,
                                     refresh.previousMatches,
                                     refresh.previousMmrIsPlaylistSpecific,
                                     refresh.won,
                                     refresh.provisionalImmediately);
    }
    if (dbManager) {
        for (const auto& pid : effects.fetchEncounterQueue) {
            dbManager->AsyncGetPlayerEncounterRecord(pid);
        }
    }
}

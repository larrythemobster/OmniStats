#include "ui/rml/controller/RmlUiController.hpp"
#include "core/state/LiveMatchStore.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <shellapi.h>

#include <cmath>
#include <ctime>
#include <mutex>
#include <shared_mutex>
#include <unordered_set>

#include "database/DatabaseManager.hpp"
#include "ui/rml/backend/RmlCapture.hpp"
#include "network/api/AccountClient.hpp"
#include "ui/Formatting.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"
#include "core/state/MatchCardUrl.hpp"
#include "network/telemetry/TelemetryManager.hpp"

using namespace RmlUiDetail;

namespace {
    InsightsView::Tab TabFromName(const std::string& name) {
        if (name == "sessions") return InsightsView::Tab::Sessions;
        if (name == "people") return InsightsView::Tab::People;
        if (name == "trends") return InsightsView::Tab::Trends;
        if (name == "playstyle") return InsightsView::Tab::Playstyle;
        return InsightsView::Tab::Recap;
    }
}

bool RmlUiController::WantsAttention() const {
    // A recap captured when Rocket League closed is only opened by Update(),
    // and the host skips Update() while the window is hidden, so the pending
    // flag itself has to keep the window drawn.
    return m_insightsVisible || m_historyVisible || m_accountsVisible || m_playerNoteVisible || m_onboardingVisible ||
           m_accountPromptPending || m_statsApiPrompt != StatsApiPrompt::Hidden ||
           (m_state && m_state->ui.showSessionRecap.load()) || (m_state && m_state->ui.showMenu.load());
}

void RmlUiController::OpenInsights() {
    if (m_telemetry) m_telemetry->RecordMilestone(TelemetryManager::Milestone::SessionInsights);
    ShowInsights(m_insights.CurrentTab(), false);
}

void RmlUiController::OpenHistory() {
    ShowHistory();
}
void RmlUiController::OpenAccounts() {
    ShowAccounts();
}

void RmlUiController::LoadViewDocuments() {
    if (!m_context) return;
    m_insightsDoc = m_context->LoadDocument("res://insights.rml");
    m_onboardingDoc = m_context->LoadDocument("res://onboarding.rml");
    m_historyDoc = m_context->LoadDocument("res://history.rml");
    m_accountsDoc = m_context->LoadDocument("res://accounts.rml");
    m_playerNoteDoc = m_context->LoadDocument("res://player_note.rml");
    if (m_insightsVisible && m_insightsDoc) m_insightsDoc->Show();
    if (m_onboardingVisible && m_onboardingDoc) m_onboardingDoc->Show();
    if (m_historyVisible && m_historyDoc) m_historyDoc->Show();
    if (m_accountsVisible && m_accountsDoc) m_accountsDoc->Show();
    if (m_playerNoteVisible && m_playerNoteDoc) m_playerNoteDoc->Show();
}

void RmlUiController::CloseViewDocuments() {
    if (m_insightsDoc) m_insightsDoc->Close();
    if (m_onboardingDoc) m_onboardingDoc->Close();
    if (m_historyDoc) m_historyDoc->Close();
    if (m_accountsDoc) m_accountsDoc->Close();
    if (m_playerNoteDoc) m_playerNoteDoc->Close();
    m_insightsDoc = nullptr;
    m_onboardingDoc = nullptr;
    m_historyDoc = nullptr;
    m_accountsDoc = nullptr;
    m_playerNoteDoc = nullptr;
    m_lastOnboardingIdentityRml.clear();
}

void RmlUiController::ShowInsights(InsightsView::Tab tab, bool endedSession) {
    if (!m_insightsDoc) return;
    // After a reset the live session is empty; the session that just ended is
    // the useful recap.
    bool showEnded = endedSession;
    if (!showEnded && m_state && m_snap.sessionTotals.wins + m_snap.sessionTotals.losses == 0) {
        if (m_state->liveMatch) {
            auto liveSnap = m_state->liveMatch->GetSnapshot();
            showEnded = liveSnap ? liveSnap->lastSessionRecap.valid : false;
        }
    }
    m_insightsShowsEndedSession = showEnded;
    m_insights.ClearArchivedSelection();
    m_insights.SetTab(tab);
    RefreshInsights(true);
    if (!m_insightsVisible) {
        m_insightsDoc->Show();
        m_insightsVisible = true;
    }
    // Onboarding stays on top if both are open.
    if (m_onboardingVisible && m_onboardingDoc) m_onboardingDoc->PullToFront();
    if (m_dbManager && !m_snap.myPrimaryId.empty()) m_dbManager->AsyncLoadInsights(m_snap.myPrimaryId);
    m_renderDirty = true;
}

void RmlUiController::HideInsights() {
    if (m_insightsDoc) m_insightsDoc->Hide();
    m_insightsVisible = false;
    m_insightsShowsEndedSession = false;
    m_recapCapturePending = false;
    m_renderDirty = true;
}

void RmlUiController::RefreshInsights(bool force) {
    if (!m_state) return;
    if (force || m_lastRecapGameVersion != m_lastGameVersion) {
        m_lastRecapGameVersion = m_lastGameVersion;
        SessionRecap endedRecap;
        if (m_state && m_state->liveMatch) {
            auto liveSnap = m_state->liveMatch->GetSnapshot();
            if (liveSnap) {
                endedRecap = liveSnap->lastSessionRecap;
            }
        }
        SessionRecap liveRecap{true, 0, m_snap.sessionTotals, m_snap.sessionGamemodes};
        m_insights.SetLiveAndEndedSession(liveRecap, endedRecap, m_insightsShowsEndedSession);
        m_renderDirty = true;
    }

    auto snap = m_state->insights.GetSnapshot();
    if (!snap) return;
    const uint64_t version = snap->version;
    if (!force && version == m_lastInsightsVersion) return;
    m_lastInsightsVersion = version;
    bool loaded = snap->loaded && snap->primaryId == m_snap.myPrimaryId;
    if (loaded) {
        m_insights.SetHistory(m_snap.myPrimaryId, true, snap->people, snap->outcomes, snap->mmrContext, snap->sessions, snap->analyticsMatches);
    } else {
        m_insights.SetHistory(m_snap.myPrimaryId, false, {}, {}, {}, {}, {});
    }
    m_renderDirty = true;
}

void RmlUiController::ShowHistory(int64_t detailMatchId, const std::string& detailMatchGuid) {
    if (!m_historyDoc) return;
    const std::string effectivePrimary = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    m_history.SetAccount(effectivePrimary);
    if (!m_historyVisible) {
        m_historyDoc->Show();
        m_historyVisible = true;
    }
    m_historyDoc->PullToFront();
    if (m_onboardingVisible && m_onboardingDoc) m_onboardingDoc->PullToFront();
    TriggerHistoryQuery(false);
    if (detailMatchId > 0 || !detailMatchGuid.empty()) {
        OpenHistoryDetail(detailMatchId, detailMatchGuid);
    }
    m_renderDirty = true;
}

void RmlUiController::HideHistory() {
    if (m_historyDoc) m_historyDoc->Hide();
    m_historyVisible = false;
    m_history.CloseDetail();
    m_renderDirty = true;
}
void RmlUiController::ShowAccounts() {
    if (!m_accountsDoc) return;
    if (!m_accountsVisible) {
        m_accountsDoc->Show();
        m_accountsVisible = true;
    }
    m_accountsDoc->PullToFront();
    if (m_onboardingVisible && m_onboardingDoc) m_onboardingDoc->PullToFront();
    RefreshAccounts(true);
    m_renderDirty = true;
}

void RmlUiController::HideAccounts() {
    if (m_accountsDoc) m_accountsDoc->Hide();
    m_accountsVisible = false;
    m_accounts.CloseDetail();
    m_accounts.CloseAdd();
    m_accounts.CloseImport();
    m_renderDirty = true;
}

void RmlUiController::RefreshAccounts(bool force) {
    if (!m_accountsService) {
        m_accounts.SetUnavailable(true);
        m_renderDirty = true;
        return;
    }
    const uint64_t v = m_accountsService->Version();
    const auto nowSteady = std::chrono::steady_clock::now();
    const bool timeToReRender = (nowSteady - m_lastAccountsRelativeRender) >= std::chrono::seconds(30);
    const std::string effectivePrimary = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    std::string canonicalActive = TrackedAccounts::CanonicalPrimaryId(effectivePrimary);
    if (!force && v == m_lastAccountsVersion && !timeToReRender && canonicalActive == m_lastAccountsActive) {
        return;
    }
    m_lastAccountsVersion = v;
    m_lastAccountsRelativeRender = nowSteady;
    m_lastAccountsActive = canonicalActive;

    auto snapshot = m_accountsService->GetSnapshot();
    auto candidates = m_accountsService->ImportCandidates();
    m_accounts.UpdateSnapshot(snapshot, canonicalActive, candidates.size());
    m_accountsService->RequestHistory(m_accounts.HistoryAccountIds(),
                                      m_accounts.HistoryPlaylist(), m_accounts.HistoryWindow());
    m_renderDirty = true;
}

void RmlUiController::SnapshotNotes() {
    if (!m_state) return;
    auto snap = m_state->notes.GetSnapshot();
    if (!snap) return;
    const uint64_t version = snap->version;
    if (version == m_snap.notesVersion) return;
    m_snap.notePreviews.clear();
    m_snap.noteTags.clear();
    for (const auto& [key, note] : snap->byKey)
        m_snap.notePreviews.emplace(key, PlayerNotes::Preview(note.text, 140));
    for (const auto& [key, tags] : snap->tagsByKey) {
        if (tags.empty()) continue;
        std::string list = "Tags: ";
        for (size_t i = 0; i < tags.size(); ++i)
            list += (i ? ", " : "") + tags[i];
        auto& preview = m_snap.notePreviews[key];
        preview = preview.empty() ? list : preview + " \xC2\xB7 " + list;
        m_snap.noteTags.emplace(key, tags);
    }
    m_snap.notesLoaded = snap->loaded;
    m_snap.notesVersion = version;
}

void RmlUiController::ShowPlayerNote(const std::string& primaryId, const std::string& name, const std::string& context) {
    if (!m_playerNoteDoc || !m_state) return;
    if (m_telemetry) m_telemetry->RecordMilestone(TelemetryManager::Milestone::PlayerLookup);
    const auto identity = PlayerNotes::IdentityFromPrimaryId(primaryId);
    if (m_playerNoteVisible && m_playerNote.IsDirty()) {
        if (!identity || !m_playerNote.Identity() || !(*identity == *m_playerNote.Identity()))
            m_playerNote.ShowStatus("Save or discard this note before opening another player's note.", true);
        m_playerNoteDoc->PullToFront();
        m_renderDirty = true;
        return;
    }

    std::optional<PlayerNotes::Note> saved;
    std::vector<std::string> tags;
    {
        auto snap = m_state->notes.GetSnapshot();
        if (!snap || !snap->loaded) {
            ShowToast((snap && snap->loadFailed) ? "Player notes could not be read from the local database." : "Player notes are still loading.",
                      snap ? snap->loadFailed : true);
            return;
        }
        if (identity) {
            if (auto it = snap->byKey.find(identity->Key()); it != snap->byKey.end()) saved = it->second;
            if (auto it = snap->tagsByKey.find(identity->Key()); it != snap->tagsByKey.end()) tags = it->second;
        }
    }

    std::string contextLine = context;
    if (contextLine.empty()) {
        if (auto it = m_snap.roster.find(primaryId); it != m_snap.roster.end()) {
            const PlayerData& p = it->second;
            if (!p.rankTier.empty() && p.rankTier != "Unranked") contextLine = Format::RankTier(p.rankTier, m_config.use_roman_numerals);
            if (p.hasLifetimeData) {
                PersonRecord record;
                record.winsWith = p.lifetimeWinsWith;
                record.lossesWith = p.lifetimeLossesWith;
                record.winsAgainst = p.lifetimeWinsAgainst;
                record.lossesAgainst = p.lifetimeLossesAgainst;
                if (!contextLine.empty()) contextLine += " · ";
                contextLine += EncounterSummary(record);
            }
        }
    }

    m_playerNote.Open({primaryId, name, contextLine}, saved);
    m_playerNote.SetTags(tags);
    std::string currentRank;
    std::string rankColor;
    if (auto it = m_snap.roster.find(primaryId); it != m_snap.roster.end() && it->second.fetched) {
        if (const std::string tier = BestRankTier(it->second); !tier.empty()) {
            currentRank = Format::RankTier(tier, m_config.use_roman_numerals);
            rankColor = CssColor(Format::RankColor(tier));
        }
    }
    m_playerNote.SetScoutLoading(currentRank, rankColor);
    ++m_nextScoutRequestId;
    if (m_dbManager && identity) {
        const std::string ownId = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
        m_dbManager->AsyncGetPlayerScout(ownId, primaryId, m_nextScoutRequestId);
    } else {
        m_playerNote.SetScoutResult(std::nullopt);
    }
    if (!m_playerNoteVisible) {
        m_playerNoteDoc->Show();
        m_playerNoteVisible = true;
    }
    m_playerNoteDoc->PullToFront();
    if (m_onboardingVisible && m_onboardingDoc) m_onboardingDoc->PullToFront();
    m_renderDirty = true;
}

void RmlUiController::HidePlayerNote() {
    if (m_playerNoteDoc) m_playerNoteDoc->Hide();
    m_playerNoteVisible = false;
    m_playerNote.Close();
    m_renderDirty = true;
}

void RmlUiController::RequestClosePlayerNote() {
    if (m_playerNote.RequestClose())
        HidePlayerNote();
    else
        m_renderDirty = true;
}

void RmlUiController::RefreshPlayerNote() {
    if (!m_state) return;
    auto notesSnap = m_state->notes.GetSnapshot();
    if (notesSnap && notesSnap->version != m_lastViewNotesVersion) {
        m_lastViewNotesVersion = notesSnap->version;
        std::unordered_set<std::string> notedKeys;
        for (const auto& [key, _] : notesSnap->byKey)
            notedKeys.insert(key);
        for (const auto& [key, tags] : notesSnap->tagsByKey)
            if (!tags.empty()) notedKeys.insert(key);
        std::optional<PlayerNotes::Note> editorNote;
        std::vector<std::string> editorTags;
        if (const auto& identity = m_playerNote.Identity()) {
            const auto key = identity->Key();
            if (auto it = notesSnap->byKey.find(key); it != notesSnap->byKey.end()) editorNote = it->second;
            if (auto it = notesSnap->tagsByKey.find(key); it != notesSnap->tagsByKey.end()) editorTags = it->second;
        }
        if (m_playerNoteVisible) {
            if (m_playerNote.IsSaving() && notesSnap->completedRequestId == m_playerNote.PendingRequestId())
                m_playerNote.ApplyResult(notesSnap->completedRequestId, notesSnap->lastRequestOk, notesSnap->lastRequestError, editorNote);
            else
                m_playerNote.SyncSaved(editorNote);
            if (m_playerNote.PendingTagRequestId() && notesSnap->completedTagRequestId == m_playerNote.PendingTagRequestId())
                m_playerNote.ApplyTagResult(notesSnap->completedTagRequestId, notesSnap->lastTagRequestOk, notesSnap->lastTagRequestError, editorTags);
            else if (!m_playerNote.PendingTagRequestId())
                m_playerNote.SetTags(editorTags);
        }
        m_history.SetNotedPlayers(notedKeys, notesSnap->loaded);
        m_insights.SetNotedPlayers(notedKeys, notesSnap->loaded);
        m_renderDirty = true;
    }
    auto scoutSnap = m_state->scout.GetSnapshot();
    if (scoutSnap && m_playerNoteVisible && scoutSnap->version != m_lastScoutViewVersion) {
        m_lastScoutViewVersion = scoutSnap->version;
        if (scoutSnap->loaded && scoutSnap->requestId == m_nextScoutRequestId &&
            m_playerNote.PrimaryId() == scoutSnap->playerId) {
            m_playerNote.SetScoutResult(scoutSnap->success ? std::optional{scoutSnap->profile} : std::nullopt);
            m_renderDirty = true;
        }
    }
}

void RmlUiController::RefreshAccountRow(const std::string& primaryId) {
    if (!m_accountsService) return;
    auto res = m_accountsService->Refresh(primaryId);
    switch (res) {
    case AccountManagerService::RefreshRequest::Queued:
        ShowToast("Rank refresh queued.");
        break;
    case AccountManagerService::RefreshRequest::AlreadyQueued:
        ShowToast("Rank refresh already in progress.");
        break;
    case AccountManagerService::RefreshRequest::CoolingDown:
        ShowToast("Refreshed recently; try again in a minute.");
        break;
    case AccountManagerService::RefreshRequest::ProviderUnavailable:
        ShowToast("Connect live ranks in Settings > Accounts & services to refresh ranks.");
        break;
    case AccountManagerService::RefreshRequest::Unsupported:
        ShowToast("Ranks can't be refreshed for this account.");
        break;
    case AccountManagerService::RefreshRequest::UnknownAccount:
        ShowToast("This account is no longer tracked.", true);
        break;
    }
    RefreshAccounts(true);
}

void RmlUiController::TriggerHistoryQuery(bool append) {
    const std::string effectivePrimary = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    m_history.SetAccount(effectivePrimary);
    const int offset = append ? m_history.LoadedRowCount() : 0;
    const uint64_t reqId = m_history.BeginQuery(append);
    if (m_dbManager && !effectivePrimary.empty()) {
        MatchQuery q = m_history.BuildQuery(offset, HistoryView::kPageSize);
        m_dbManager->AsyncQueryMatches(std::move(q), reqId, append);
    }
    m_renderDirty = true;
}

void RmlUiController::OpenHistoryDetail(int64_t matchId, const std::string& matchGuid) {
    if (m_telemetry) m_telemetry->RecordMilestone(TelemetryManager::Milestone::MatchHistoryDetail);
    const std::string effectivePrimary = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    const uint64_t reqId = m_history.BeginDetail(matchId);
    if (m_dbManager) {
        if (matchId > 0) {
            m_dbManager->AsyncGetMatchDetail(matchId, reqId, effectivePrimary);
        } else if (!matchGuid.empty()) {
            m_dbManager->AsyncGetMatchDetailByGuid(matchGuid, reqId, effectivePrimary);
        }
    }
    m_renderDirty = true;
}

void RmlUiController::RefreshHistory(bool force) {
    if (!m_state) return;
    const std::string effectivePrimary = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    if (m_history.Account() != effectivePrimary) {
        m_history.SetAccount(effectivePrimary);
    }
    auto snap = m_state->historyView.GetSnapshot();
    if (!snap) return;
    const uint64_t version = snap->version;
    if (!force && version == m_lastHistoryViewVersion) return;
    m_lastHistoryViewVersion = version;

    if (!snap->loading && snap->requestId > 0) {
        m_history.ApplyQueryResult(snap->requestId, snap->rows, snap->total, snap->appendMode);
    }
    if (!snap->detailLoading && (snap->detailRequestId > 0 || snap->detail.has_value())) {
        m_history.ApplyDetailResult(snap->detailRequestId, snap->detail);
    }
    m_renderDirty = true;
}
// Runs from Render() after the frame is drawn, while the render target that
// holds the recap card is still bound.
void RmlUiController::ExportRecapPng() {
    m_recapCapturePending = false;
    Rml::Element* card = m_insightsDoc && m_insightsVisible ? m_insightsDoc->GetElementById("recap-card") : nullptr;
    if (!card || !m_d3dContext) return;
    const Rml::Vector2f offset = card->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = card->GetBox().GetSize(Rml::BoxArea::Border);
    const std::wstring path = RmlCapture::NewPicturePath(L"OmniStats-recap");
    std::string error;
    if (!RmlCapture::SaveBoundRenderTargetRegion(m_d3dContext, static_cast<int>(std::floor(offset.x)), static_cast<int>(std::floor(offset.y)),
                                                 static_cast<int>(std::ceil(size.x)), static_cast<int>(std::ceil(size.y)),
                                                 card->GetComputedValues().border_top_left_radius(), path, error)) {
        ShowToast("Could not save the recap: " + error, true);
        return;
    }
    ShowToast("Saved the recap to Pictures\\OmniStats.");
    const std::wstring args = L"/select,\"" + path + L"\"";
    ShellExecuteW(nullptr, L"open", L"explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

void RmlUiController::ShowOnboarding() {
    if (!m_onboardingDoc) return;
    m_onboarding.SetStep(0);
    if (m_state && !m_state->ui.statsApiChecked.load()) CheckStatsApi(false, false);
    RefreshOnboarding();
    m_onboardingDoc->Show();
    m_onboardingDoc->PullToFront();
    m_onboardingVisible = true;
    m_renderDirty = true;
}

void RmlUiController::FinishOnboarding() {
    if (!Config::HasCurrentConsent(Config::Read())) return;
    if (m_telemetry) m_telemetry->RecordMilestone(TelemetryManager::Milestone::OnboardingCompleted);
    Config::Update([](ConfigData& c) {
        c.onboarding_completed = true;
        c.account_link_prompt_shown = true;
    });
    m_config = Config::Read();
    if (m_onboardingDoc) m_onboardingDoc->Hide();
    m_onboardingVisible = false;
    RebuildVisibleUi(true);
}

void RmlUiController::RefreshOnboarding() {
    StatsApiConfig::CheckResult result;
    bool checked = false;
    if (m_state) {
        std::lock_guard lock(m_state->ui.statsApiMutex);
        result = m_state->ui.statsApiResult;
        checked = m_state->ui.statsApiChecked.load();
    }
    m_onboarding.Refresh(m_config, result, checked, !m_snap.myPrimaryId.empty());

    // The account list changes with the live roster, so it is rendered with
    // the same select helper as Settings instead of a bound option list.
    if (Rml::Element* slot = m_onboardingDoc ? m_onboardingDoc->GetElementById("onboarding-identity") : nullptr) {
        const std::string rml = SelectRow("identity", "Account", "", IdentityOptions(), m_config.last_primary_id);
        if (rml != m_lastOnboardingIdentityRml) {
            m_lastOnboardingIdentityRml = rml;
            SetElementRml(slot, rml, false);
        }
    }
    m_renderDirty = true;
}
void RmlUiController::OpenStatsApiFixIfNeeded() {
    if (!m_state || !m_config.check_stats_api_config_on_startup) return;
    bool needsFix = false;
    bool hasPath = false;
    {
        std::lock_guard lock(m_state->ui.statsApiMutex);
        needsFix = m_state->ui.statsApiChecked.load() && m_state->ui.statsApiResult.status != StatsApiConfig::Status::Valid;
        hasPath = !m_state->ui.statsApiResult.path.empty();
    }
    if (needsFix) SetStatsApiPrompt(hasPath ? StatsApiPrompt::Asking : StatsApiPrompt::NotFound);
}

void RmlUiController::SetStatsApiPrompt(StatsApiPrompt state) {
    m_statsApiPrompt = state;
    RebuildStatsApiPrompt();
    m_renderDirty = true;
}

void RmlUiController::RebuildStatsApiPrompt() {
    if (m_statsApiPrompt == StatsApiPrompt::Hidden) {
        SetRootRml("prompt-root", "");
        return;
    }
    StatsApiConfig::CheckResult result;
    if (m_state) {
        std::lock_guard lock(m_state->ui.statsApiMutex);
        result = m_state->ui.statsApiResult;
    }
    std::string title;
    std::string body;
    std::string detail;
    std::string actions;
    switch (m_statsApiPrompt) {
    case StatsApiPrompt::Asking:
        title = "Turn on live match data?";
        body = "Rocket League's Stats API is switched off, so OmniStats can't see your matches. OmniStats can turn it on by editing one Rocket League config file.";
        detail = "Windows will ask for permission because the file is in Rocket League's install folder.";
        if (result.rlRunning) detail += " Restart Rocket League afterwards.";
        actions = Button("statsapi-prompt-never", "Don't ask again", "ghost") + "<div class='grow'></div>" +
                  Button("statsapi-prompt-close", "Not now", "ghost") + Button("statsapi-prompt-fix", "Turn it on", "primary");
        break;
    case StatsApiPrompt::Repairing:
        title = "Turning on the Stats API...";
        body = "Approve the Windows permission prompt to continue. It may be behind other windows.";
        break;
    case StatsApiPrompt::Repaired:
        title = "Live match data is on";
        body = result.rlRunning ? "Restart Rocket League so it picks up the change." : "OmniStats will see your next match.";
        actions = "<div class='grow'></div>" + Button("statsapi-prompt-close", "Done", "primary");
        break;
    case StatsApiPrompt::Failed:
        title = "Couldn't turn on the Stats API";
        body = result.message.empty() ? "The config file wasn't changed." : result.message;
        actions = Button("statsapi-prompt-troubleshoot", "Troubleshooting", "ghost") + "<div class='grow'></div>" +
                  Button("statsapi-prompt-close", "Close", "ghost") + Button("statsapi-prompt-fix", "Try again", "primary");
        break;
    case StatsApiPrompt::NotFound:
        title = "Couldn't find Rocket League's Stats API config";
        body = "Set the file location in Settings > Data & support > Troubleshooting, then try again.";
        actions = "<div class='grow'></div>" + Button("statsapi-prompt-close", "Close", "ghost") +
                  Button("statsapi-prompt-troubleshoot", "Open troubleshooting", "primary");
        break;
    case StatsApiPrompt::Hidden:
        break;
    }
    std::string html = "<div class='prompt-backdrop'><div class='card prompt-card'>"
                       "<div class='row prompt-head'><img class='brand-logo' src='res://images/Logo.png'/><div class='prompt-title grow'>" +
                       Escape(title) + "</div></div><div class='prompt-body'>" + Escape(body) + "</div>";
    if (!detail.empty()) html += "<div class='prompt-detail'>" + Escape(detail) + "</div>";
    if (m_statsApiPrompt == StatsApiPrompt::Asking && !result.path.empty()) {
        html += "<div class='prompt-path mono'>" + Escape(result.path) + "</div>";
    }
    if (!actions.empty()) html += "<div class='row gap-sm prompt-actions'>" + actions + "</div>";
    SetRootRml("prompt-root", html + "</div></div>");
}

void RmlUiController::CheckAccountLinkPrompt() {
    if (!m_config.onboarding_completed || m_config.account_link_prompt_shown) {
        m_accountPromptPending = false;
        return;
    }
    const AccountStatusSnapshot accountStatus = AccountClient::Instance().GetStatus();
    if (accountStatus.state == AccountAuthState::Migrating) {
        m_accountPromptPending = true;
        return;
    }
    m_accountPromptPending = false;
    if (accountStatus.state == AccountAuthState::SignedIn || AccountClient::Instance().IsSignedIn() ||
        !m_config.account_refresh_token.empty()) {
        return;
    }
    m_settingsPage = SettingsPage::Accounts;
    if (m_state) {
        m_state->ui.showMenu.store(true);
        m_state->ui.dashboardLayoutEditMode.store(false);
    }
    RebuildSettings();
    Config::Update([](ConfigData& c) { c.account_link_prompt_shown = true; });
    m_config = Config::Read();
    m_renderDirty = true;
}

bool RmlUiController::HandleViewAction(const std::string& action, Rml::Element* target) {
    if (action == "player-note-open") {
        ShowPlayerNote(Attribute(target, "data-pid"), Attribute(target, "data-name"), Attribute(target, "data-context"));
    } else if (action == "player-note-close") {
        RequestClosePlayerNote();
    } else if (action == "player-note-discard") {
        HidePlayerNote();
    } else if (action == "player-note-keep-editing") {
        m_playerNote.KeepEditing();
        m_renderDirty = true;
    } else if (action == "player-note-save") {
        if (!m_dbManager) {
            m_playerNote.ShowStatus("Couldn't save: the local database is unavailable. Your text is still here.", true);
        } else if (m_playerNote.Identity() && m_playerNote.BeginSave(m_nextPlayerNoteRequestId + 1)) {
            ++m_nextPlayerNoteRequestId;
            m_dbManager->AsyncSavePlayerNote(*m_playerNote.Identity(), m_playerNote.Draft(), m_playerNote.PlayerName(), m_nextPlayerNoteRequestId);
        }
        m_renderDirty = true;
    } else if (action == "player-note-delete") {
        m_playerNote.RequestDelete();
        m_renderDirty = true;
    } else if (action == "player-note-cancel-delete") {
        m_playerNote.CancelDelete();
        m_renderDirty = true;
    } else if (action == "player-note-confirm-delete") {
        if (!m_dbManager) {
            m_playerNote.ShowStatus("Couldn't delete: the local database is unavailable.", true);
        } else if (m_playerNote.Identity() && m_playerNote.BeginDelete(m_nextPlayerNoteRequestId + 1)) {
            ++m_nextPlayerNoteRequestId;
            m_dbManager->AsyncDeletePlayerNote(*m_playerNote.Identity(), m_nextPlayerNoteRequestId);
        }
        m_renderDirty = true;
    } else if (action == "player-scout-add-tag" || action == "player-scout-remove-tag") {
        const bool add = action == "player-scout-add-tag";
        const std::string tag = PlayerNotes::NormalizeTag(add ? m_playerNote.TagDraft() : Attribute(target, "data-tag"));
        if (tag.empty()) {
            m_playerNote.ShowStatus("Enter a valid tag.", true);
        } else if (!m_dbManager) {
            m_playerNote.ShowStatus("The local database is unavailable.", true);
        } else if (m_playerNote.Identity() && m_playerNote.BeginTagChange(m_nextPlayerTagRequestId + 1)) {
            ++m_nextPlayerTagRequestId;
            m_dbManager->AsyncSetPlayerTag(*m_playerNote.Identity(), tag, add, m_nextPlayerTagRequestId);
        }
        m_renderDirty = true;
    } else if (action == "player-scout-open-match") {
        if (m_playerNote.IsDirty()) {
            m_playerNote.ShowStatus("Save or discard your note before opening a match.", true);
        } else {
            const std::string idText = Attribute(target, "data-id");
            try {
                const int64_t id = std::stoll(idText);
                if (id > 0) {
                    HidePlayerNote();
                    ShowHistory(id);
                }
            } catch (const std::exception&) {
                m_playerNote.ShowStatus("Could not open this match.", true);
            }
        }
        m_renderDirty = true;
    } else if (action == "insights-open") {
        const std::string tab = Attribute(target, "data-tab");
        ShowInsights(tab.empty() ? m_insights.CurrentTab() : TabFromName(tab), false);
    } else if (action == "insights-close") {
        HideInsights();
    } else if (action == "insights-tab") {
        m_insights.SetTab(TabFromName(Attribute(target, "data-tab")));
        m_renderDirty = true;
    } else if (action == "people-filter") {
        m_insights.SetPeopleFilter(Attribute(target, "data-filter") == "rivals" ? Insights::PeopleFilter::Rivals
                                                                                : Insights::PeopleFilter::Teammates);
        m_renderDirty = true;
    } else if (action == "trend-playlist-filter") {
        std::string playlist = Attribute(target, "data-playlist");
        if (playlist.empty() && target) playlist = target->GetInnerRML().c_str();
        m_insights.SetTrendPlaylistFilter(playlist);
        m_renderDirty = true;
    } else if (action == "recap-export") {
        m_recapCapturePending = true;
        m_renderDirty = true;
    } else if (action == "session-open") {
        const int idx = std::atoi(Attribute(target, "data-session-index").c_str());
        if (idx >= 0 && m_insights.OpenSession(static_cast<size_t>(idx))) {
            m_insightsShowsEndedSession = m_insights.ViewingArchivedSession();
            m_renderDirty = true;
        }
    } else if (action == "recap-prev-session") {
        if (m_insights.StepSession(-1)) {
            m_insightsShowsEndedSession = m_insights.ViewingArchivedSession();
            m_renderDirty = true;
        }
    } else if (action == "recap-next-session") {
        if (m_insights.StepSession(1)) {
            m_insightsShowsEndedSession = m_insights.ViewingArchivedSession();
            m_renderDirty = true;
        }
    } else if (action == "recap-compare") {
        m_insights.ToggleCompareWithPrevious();
        m_renderDirty = true;
    } else if (action == "sessions-load-more") {
        m_insights.LoadMoreSessions();
        m_renderDirty = true;
    } else if (action == "history-open") {
        const std::string idAttr = Attribute(target, "data-match-id");
        std::string guidAttr = Attribute(target, "data-match-guid");
        const std::string sourceAttr = Attribute(target, "data-source");
        int64_t matchId = 0;
        if (!idAttr.empty()) {
            matchId = std::strtoll(idAttr.c_str(), nullptr, 10);
        }
        if (matchId <= 0 && sourceAttr == "match-summary") {
            if (guidAttr.empty()) guidAttr = m_snap.matchGuid;
            if (!guidAttr.empty()) {
                for (const auto& m : m_snap.recentSavedMatches) {
                    if (m.matchGuid == guidAttr && m.matchId > 0) {
                        matchId = m.matchId;
                        break;
                    }
                }
            }
            if (matchId <= 0 && guidAttr.empty() && !m_snap.recentSavedMatches.empty()) {
                matchId = m_snap.recentSavedMatches.front().matchId;
                guidAttr = m_snap.recentSavedMatches.front().matchGuid;
            }
        }
        if (m_historyVisible && (matchId > 0 || !guidAttr.empty())) {
            OpenHistoryDetail(matchId, guidAttr);
        } else {
            ShowHistory(matchId, guidAttr);
        }
    } else if (action == "history-close") {
        HideHistory();
    } else if (action == "history-close-detail") {
        m_history.CloseDetail();
        m_renderDirty = true;
    } else if (action == "history-detail-tab") {
        const std::string tab = Attribute(target, "data-tab");
        if (!tab.empty()) {
            m_history.SetDetailTab(tab);
            if (m_historyDoc) {
                if (auto* body = m_historyDoc->QuerySelector(".history-detail-body")) {
                    body->SetScrollTop(0.0f);
                }
            }
        }
        m_renderDirty = true;
    } else if (action == "history-filter") {
        const std::string playlist = Attribute(target, "data-playlist");
        const std::string result = Attribute(target, "data-result");
        const std::string date = Attribute(target, "data-date");
        const std::string sort = Attribute(target, "data-sort");
        if (!playlist.empty()) m_history.SetPlaylistFilter(playlist);
        if (!result.empty()) m_history.SetResultFilter(result);
        if (!date.empty()) m_history.SetDateFilter(date);
        if (!sort.empty()) m_history.SetSortFilter(sort);
        TriggerHistoryQuery(false);
    } else if (action == "history-event-team") {
        m_history.SetEventTeamFilter(Attribute(target, "data-team-filter"));
        m_renderDirty = true;
    } else if (action == "history-event-filter") {
        m_history.SetEventFilter(Attribute(target, "data-event-filter"));
        m_renderDirty = true;
    } else if (action == "history-clear-filters") {
        m_history.ClearFilters();
        TriggerHistoryQuery(false);
    } else if (action == "history-more") {
        TriggerHistoryQuery(true);
    } else if (action == "history-copy") {
        const std::string text = m_history.FormatScoreboardText();
        if (!text.empty()) {
            m_systemInterface.SetClipboardText(text);
            ShowToast("Copied match summary to clipboard.");
        }
    } else if (action == "history-share-card") {
        const auto& detailOpt = m_history.Detail();
        if (detailOpt.has_value() && detailOpt->found) {
            const std::string url = OmniStats::MatchCard::BuildMatchCardUrl(*detailOpt);
            m_systemInterface.SetClipboardText(url);
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ShowToast("Opened the match card in your browser. Link copied.");
            if (m_telemetry) m_telemetry->RecordMilestone(TelemetryManager::Milestone::MatchCardShared);
        } else {
            ShowToast("Match details not loaded.", true);
        }
    } else if (action == "accounts-open") {
        ShowAccounts();
        return true;
    } else if (action == "accounts-close") {
        HideAccounts();
        return true;
    } else if (action == "accounts-refresh-all") {
        if (m_accountsService) {
            size_t count = m_accountsService->RefreshAll();
            if (count > 0) {
                ShowToast("Queued refresh for " + std::to_string(count) + (count == 1 ? " account." : " accounts."));
            } else {
                ShowToast("Refreshed recently; try again in a minute.");
            }
            RefreshAccounts(true);
        }
        return true;
    } else if (action == "accounts-refresh-row") {
        const std::string id = Attribute(target, "data-id");
        if (!id.empty()) RefreshAccountRow(id);
        return true;
    } else if (action == "accounts-column-set") {
        const std::string set = Attribute(target, "data-set");
        if (!set.empty()) {
            m_accounts.SetColumnSet(set);
            m_renderDirty = true;
        }
        return true;
    } else if (action == "accounts-sort") {
        const std::string sort = Attribute(target, "data-sort");
        if (!sort.empty()) {
            m_accounts.SetSort(sort);
            m_renderDirty = true;
        }
        return true;
    } else if (action == "accounts-history-toggle") {
        const std::string id = Attribute(target, "data-id");
        if (m_accounts.ToggleHistoryAccount(id)) RefreshAccounts(true);
        return true;
    } else if (action == "accounts-history-playlist") {
        const std::string playlist = Attribute(target, "data-playlist");
        m_accounts.SetHistoryPlaylist(playlist);
        RefreshAccounts(true);
        return true;
    } else if (action == "accounts-history-range") {
        m_accounts.SetHistoryWindow(Attribute(target, "data-window"));
        RefreshAccounts(true);
        return true;
    } else if (action == "accounts-open-detail") {
        const std::string id = Attribute(target, "data-id");
        if (!id.empty()) {
            m_accounts.OpenDetail(id);
            m_renderDirty = true;
        }
        return true;
    } else if (action == "accounts-close-detail") {
        m_accounts.CloseDetail();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-detail-start-rename") {
        m_accounts.StartRename();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-detail-cancel-rename") {
        m_accounts.CancelRename();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-detail-save-rename") {
        if (m_accountsService) {
            m_accountsService->Rename(m_accounts.DetailPrimaryId(), m_accounts.RenameValue());
            m_accounts.FinishRename();
            RefreshAccounts(true);
        }
        return true;
    } else if (action == "accounts-detail-refresh") {
        if (!m_accounts.DetailPrimaryId().empty()) {
            RefreshAccountRow(m_accounts.DetailPrimaryId());
        }
        return true;
    } else if (action == "accounts-detail-copy-id") {
        if (!m_accounts.DetailCanonicalId().empty()) {
            m_systemInterface.SetClipboardText(m_accounts.DetailCanonicalId());
            ShowToast("Copied account ID to clipboard.");
        }
        return true;
    } else if (action == "accounts-detail-start-remove") {
        m_accounts.StartRemoveConfirm();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-detail-cancel-remove") {
        m_accounts.CancelRemoveConfirm();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-detail-confirm-remove") {
        if (m_accountsService) {
            m_accountsService->Remove(m_accounts.DetailPrimaryId());
            m_accounts.CloseDetail();
            ShowToast("Account removed.");
            RefreshAccounts(true);
        }
        return true;
    } else if (action == "accounts-open-add") {
        m_accounts.OpenAdd();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-add-close") {
        m_accounts.CloseAdd();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-add-platform") {
        const std::string plat = Attribute(target, "data-platform");
        if (!plat.empty()) {
            m_accounts.SetAddPlatform(plat);
            m_renderDirty = true;
        }
        return true;
    } else if (action == "accounts-add-submit") {
        if (m_accountsService) {
            auto res = m_accountsService->AddManual(m_accounts.AddPlatform(), m_accounts.AddAccountId(), m_accounts.AddNickname());
            if (res.status == AccountManagerService::AddResult::Status::Added) {
                m_accounts.CloseAdd();
                ShowToast("Account added.");
                RefreshAccounts(true);
            } else if (res.status == AccountManagerService::AddResult::Status::AlreadyTracked) {
                m_accounts.SetAddAlreadyTracked(res.primaryId, res.message);
                m_renderDirty = true;
            } else {
                m_accounts.SetAddError(res.message);
                m_renderDirty = true;
            }
        }
        return true;
    } else if (action == "accounts-add-rename-existing") {
        std::string existingId = Attribute(target, "data-id");
        if (existingId.empty()) existingId = m_accounts.AddExistingId();
        m_accounts.CloseAdd();
        m_accounts.OpenDetail(existingId, true);
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-open-import") {
        if (m_accountsService) {
            m_accounts.OpenImport(m_accountsService->ImportCandidates());
        }
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-import-close") {
        m_accounts.CloseImport();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-import-submit") {
        if (m_accountsService) {
            size_t count = m_accountsService->ImportKnown();
            m_accounts.CloseImport();
            ShowToast(count > 0 ? "Imported " + std::to_string(count) + (count == 1 ? " account." : " accounts.") : "No new accounts to import.");
            RefreshAccounts(true);
        }
        return true;
    } else if (action == "statsapi-prompt-fix") {
        if (CheckStatsApi(true, false)) SetStatsApiPrompt(StatsApiPrompt::Repairing);
        return true;
    } else if (action == "statsapi-prompt-close") {
        SetStatsApiPrompt(StatsApiPrompt::Hidden);
        return true;
    } else if (action == "statsapi-prompt-never") {
        Config::Update([](ConfigData& c) { c.check_stats_api_config_on_startup = false; });
        m_config = Config::Read();
        SetStatsApiPrompt(StatsApiPrompt::Hidden);
        ShowToast("You can fix the Stats API later in Settings > Data & support > Troubleshooting.");
        return true;
    } else if (action == "statsapi-prompt-troubleshoot") {
        SetStatsApiPrompt(StatsApiPrompt::Hidden);
        m_settingsPage = SettingsPage::DataSupport;
        m_showTroubleshooting = true;
        if (m_state) m_state->ui.showMenu.store(true);
        m_renderDirty = true;
        return true;
    } else if (action == "open-rank-settings") {
        m_settingsPage = SettingsPage::Accounts;
        if (m_state) m_state->ui.showMenu.store(true);
        m_renderDirty = true;
        return true;
    } else if (action == "onboarding-next") {
        m_onboarding.SetStep(m_onboarding.Step() + 1);
        RefreshOnboarding();
    } else if (action == "onboarding-back") {
        m_onboarding.SetStep(m_onboarding.Step() - 1);
        RefreshOnboarding();
    } else if (action == "onboarding-finish") {
        FinishOnboarding();
    } else if (action == "onboarding-accept-terms") {
        const std::string acceptedAt = std::to_string(std::time(nullptr));
        Config::Update([&acceptedAt](ConfigData& c) {
            c.privacy_policy_accepted_version = Config::CurrentPrivacyPolicyVersion;
            c.terms_accepted_version = Config::CurrentTermsVersion;
            c.privacy_accepted_at = acceptedAt;
        });
        Config::Save();
        m_config = Config::Read();
        if (m_config.onboarding_completed) {
            // Returning users only had to re-accept updated terms.
            if (m_onboardingDoc) m_onboardingDoc->Hide();
            m_onboardingVisible = false;
            RebuildVisibleUi(true);
            CheckAccountLinkPrompt();
        } else {
            m_onboarding.SetStep(0);
            RefreshOnboarding();
        }
    } else if (action == "onboarding-exit") {
        if (m_state) m_state->ui.appExitRequested.store(true);
    } else if (action == "open-legal-link") {
        const std::string url = Attribute(target, "data-url");
        if (url == "https://omnistats.org/privacy" || url == "https://omnistats.org/terms") {
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
        }
    } else if (action == "onboarding-layout") {
        const bool dashboard = Attribute(target, "data-mode") == "dashboard";
        Config::Update([dashboard](ConfigData& c) { c.second_monitor_mode = dashboard; });
        m_config = Config::Read();
        RefreshOnboarding();
    } else if (action == "onboarding-statsapi-check" || action == "onboarding-statsapi-fix") {
        CheckStatsApi(action == "onboarding-statsapi-fix", false);
        RefreshOnboarding();
    } else if (action == "onboarding-statsapi-browse") {
        BrowseStatsApiPath();
        RefreshOnboarding();
    } else if (action == "account-sign-in") {
        AccountClient::Instance().BeginSignInAsync(true);
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
        if (m_onboardingVisible) RefreshOnboarding();
    } else if (action == "account-retry-migration") {
        AccountClient::Instance().RetryKeyMigrationAsync();
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
        if (m_onboardingVisible) RefreshOnboarding();
    } else if (action == "account-cancel") {
        AccountClient::Instance().CancelAuthorization();
        m_onboarding.SetShowFallback(false);
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
        if (m_onboardingVisible) RefreshOnboarding();
    } else if (action == "account-sign-out") {
        AccountClient::Instance().LogoutAsync();
        m_config = Config::Read();
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
        if (m_onboardingVisible) RefreshOnboarding();
    } else if (action == "account-open-verify") {
        AccountClient::Instance().OpenVerificationBrowser();
    } else if (action == "account-copy-link") {
        const auto status = AccountClient::Instance().GetStatus();
        if (!status.verificationUri.empty()) {
            m_systemInterface.SetClipboardText(status.verificationUri);
            ShowToast("Copied sign-in link to clipboard.");
        }
    } else if (action == "account-copy-code") {
        const auto status = AccountClient::Instance().GetStatus();
        if (!status.userCode.empty()) {
            m_systemInterface.SetClipboardText(status.userCode);
            ShowToast("Copied verification code to clipboard.");
        }
    } else if (action == "account-toggle-fallback") {
        m_onboarding.ToggleFallback();
        if (m_onboardingVisible) RefreshOnboarding();
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
    } else if (action == "account-manage-devices") {
        AccountClient::Instance().OpenManageDevicesBrowser();
    } else {
        return false;
    }
    return true;
}

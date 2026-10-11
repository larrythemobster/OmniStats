#include "ui/rml/RmlUiController.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <shellapi.h>

#include <cmath>
#include <mutex>
#include <shared_mutex>

#include "database/DatabaseManager.hpp"
#include "ui/rml/RmlCapture.hpp"
#include "network/AccountClient.hpp"
#include "ui/rml/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

namespace {
    InsightsView::Tab TabFromName(const std::string& name) {
        if (name == "sessions") return InsightsView::Tab::Sessions;
        if (name == "people") return InsightsView::Tab::People;
        if (name == "trends") return InsightsView::Tab::Trends;
        return InsightsView::Tab::Recap;
    }
}

bool RmlUiController::WantsAttention() const {
    // A recap captured when Rocket League closed is only opened by Update(),
    // and the host skips Update() while the window is hidden, so the pending
    // flag itself has to keep the window drawn.
    return m_insightsVisible || m_historyVisible || m_accountsVisible || m_onboardingVisible || (m_state && m_state->ui.showSessionRecap.load());
}

void RmlUiController::OpenInsights() {
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
    if (m_insightsVisible && m_insightsDoc) m_insightsDoc->Show();
    if (m_onboardingVisible && m_onboardingDoc) m_onboardingDoc->Show();
    if (m_historyVisible && m_historyDoc) m_historyDoc->Show();
    if (m_accountsVisible && m_accountsDoc) m_accountsDoc->Show();
}

void RmlUiController::CloseViewDocuments() {
    if (m_insightsDoc) m_insightsDoc->Close();
    if (m_onboardingDoc) m_onboardingDoc->Close();
    if (m_historyDoc) m_historyDoc->Close();
    if (m_accountsDoc) m_accountsDoc->Close();
    m_insightsDoc = nullptr;
    m_onboardingDoc = nullptr;
    m_historyDoc = nullptr;
    m_accountsDoc = nullptr;
    m_lastOnboardingIdentityRml.clear();
}

void RmlUiController::ShowInsights(InsightsView::Tab tab, bool endedSession) {
    if (!m_insightsDoc) return;
    // After a reset the live session is empty; the session that just ended is
    // the useful recap.
    bool showEnded = endedSession;
    if (!showEnded && m_state && m_snap.sessionTotals.wins + m_snap.sessionTotals.losses == 0) {
        std::shared_lock lock(m_state->game.mutex);
        showEnded = m_state->game.lastSessionRecap.valid;
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
        {
            std::shared_lock lock(m_state->game.mutex);
            endedRecap = m_state->game.lastSessionRecap;
        }
        SessionRecap liveRecap{true, 0, m_snap.sessionTotals, m_snap.sessionGamemodes};
        m_insights.SetLiveAndEndedSession(liveRecap, endedRecap, m_insightsShowsEndedSession);
        m_renderDirty = true;
    }

    const uint64_t version = m_state->insights.version.load(std::memory_order_relaxed);
    if (!force && version == m_lastInsightsVersion) return;
    m_lastInsightsVersion = version;
    std::vector<PersonRecord> people;
    std::vector<MatchOutcome> outcomes;
    std::vector<MatchMmrContext> mmrContext;
    std::vector<SessionRecap> sessions;
    bool loaded = false;
    {
        std::lock_guard lock(m_state->insights.mutex);
        loaded = m_state->insights.loaded && m_state->insights.primaryId == m_snap.myPrimaryId;
        if (loaded) {
            people = m_state->insights.people;
            outcomes = m_state->insights.outcomes;
            mmrContext = m_state->insights.mmrContext;
            sessions = m_state->insights.sessions;
        }
    }
    m_insights.SetHistory(m_snap.myPrimaryId, loaded, people, outcomes, mmrContext, sessions);
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
        if (m_accountsService) {
            m_accountsService->RefreshStale();
        }
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
    m_renderDirty = true;
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
        ShowToast("No rank source is configured.");
        break;
    case AccountManagerService::RefreshRequest::Unsupported:
        ShowToast("Rank refresh is unsupported for this account.");
        break;
    case AccountManagerService::RefreshRequest::UnknownAccount:
        ShowToast("Account not found.", true);
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
    const uint64_t version = m_state->historyView.version.load(std::memory_order_relaxed);
    if (!force && version == m_lastHistoryViewVersion) return;
    m_lastHistoryViewVersion = version;

    uint64_t reqId = 0;
    uint64_t detailReqId = 0;
    bool loading = false;
    bool detailLoading = false;
    bool appendMode = false;
    std::vector<MatchRow> rows;
    int total = 0;
    std::optional<MatchDetail> detail;
    {
        std::lock_guard lock(m_state->historyView.mutex);
        reqId = m_state->historyView.requestId;
        detailReqId = m_state->historyView.detailRequestId;
        loading = m_state->historyView.loading;
        detailLoading = m_state->historyView.detailLoading;
        appendMode = m_state->historyView.appendMode;
        rows = m_state->historyView.rows;
        total = m_state->historyView.total;
        detail = m_state->historyView.detail;
    }

    if (!loading && reqId > 0) {
        m_history.ApplyQueryResult(reqId, rows, total, appendMode);
    }
    if (!detailLoading && (detailReqId > 0 || detail.has_value())) {
        m_history.ApplyDetailResult(detailReqId, detail);
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
    Config::Update([](ConfigData& c) { c.onboarding_completed = true; });
    m_config = Config::Read();
    if (m_onboardingDoc) m_onboardingDoc->Hide();
    m_onboardingVisible = false;
    m_renderDirty = true;
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

bool RmlUiController::HandleViewAction(const std::string& action, Rml::Element* target) {
    if (action == "insights-open") {
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
    } else if (action == "accounts-open-settings") {
        m_settingsPage = SettingsPage::Integrations;
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
    } else if (action == "onboarding-layout") {
        const bool dashboard = Attribute(target, "data-mode") == "dashboard";
        Config::Update([dashboard](ConfigData& c) { c.second_monitor_mode = dashboard; });
        m_config = Config::Read();
        RefreshOnboarding();
    } else if (action == "onboarding-statsapi-check" || action == "onboarding-statsapi-fix") {
        CheckStatsApi(action == "onboarding-statsapi-fix", false);
        RefreshOnboarding();
    } else if (action == "account-sign-in") {
        AccountClient::Instance().BeginSignInAsync(true);
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
        if (m_onboardingVisible) RefreshOnboarding();
    } else if (action == "account-cancel") {
        AccountClient::Instance().CancelAuthorization();
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
        if (m_onboardingVisible) RefreshOnboarding();
    } else if (action == "account-sign-out") {
        AccountClient::Instance().LogoutAsync();
        m_config = Config::Read();
        if (m_state && m_state->ui.showMenu.load()) RebuildSettings();
        if (m_onboardingVisible) RefreshOnboarding();
    } else if (action == "account-open-verify") {
        AccountClient::Instance().OpenVerificationBrowser();
    } else if (action == "account-manage-devices") {
        AccountClient::Instance().OpenManageDevicesBrowser();
    } else {
        return false;
    }
    return true;
}

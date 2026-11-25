#include "ui/rml/controller/RmlUiController.hpp"
#include "core/state/LiveMatchStore.hpp"

#include <RmlUi/Core.h>
#include <cmath>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <shellapi.h>

#include <ctime>

#include "database/DatabaseManager.hpp"
#include "ui/rml/backend/RmlCapture.hpp"
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
        if (name == "season") return InsightsView::Tab::Season;
        return InsightsView::Tab::Recap;
    }
}

void RmlUiController::OpenInsights() {
    if (m_telemetry) m_telemetry->RecordMilestone(TelemetryManager::Milestone::SessionInsights);
    ShowInsights(m_insights.model.CurrentTab(), false);
}

void RmlUiController::ShowInsights(InsightsView::Tab tab, bool endedSession) {
    if (!m_insights.Document()) return;
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
    m_sessionRecapDismissAtMs = endedSession ? std::optional<int64_t>(SteadyNowMs() + kSessionRecapIdleTimeoutMs) : std::nullopt;
    m_insights.model.ClearArchivedSelection();
    m_insights.model.SetTab(tab);
    RefreshInsights(true);
    m_insights.Show();
    // Onboarding stays on top if both are open.
    m_onboarding.Raise();
    if (m_dbManager && !m_snap.myPrimaryId.empty()) m_dbManager->AsyncLoadInsights(m_snap.myPrimaryId);
    if (m_dbManager) {
        const std::string account = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
        m_dbManager->AsyncGetSeasonSummaries(account, ++m_seasonSummaryRequestId);
    }
    m_renderDirty = true;
}

void RmlUiController::HideInsights() {
    m_insights.Hide();
    m_seasonRecapSelection = 0;
    m_sessionRecapDismissAtMs.reset();
    m_insightsShowsEndedSession = false;
    m_recapCapturePending = false;
    m_renderDirty = true;
}

void RmlUiController::RefreshInsights(bool force) {
    if (!m_state) return;
    const std::string account = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    const auto seasonSnap = m_state->seasonSummaries.load();
    if (seasonSnap && seasonSnap->loaded && seasonSnap->primaryId == account &&
        (force || seasonSnap->requestId != m_lastSeasonSummaryRequestId)) {
        m_lastSeasonSummaryRequestId = seasonSnap->requestId;
        std::optional<TrackedAccounts::SeasonReward> reward;
        if (m_accountsService) {
            const auto accounts = m_accountsService->GetSnapshot();
            for (const auto& entry : accounts.accounts) {
                if (entry.record.primaryId == account) {
                    reward = entry.record.seasonRewards;
                    break;
                }
            }
        }
        m_insights.model.SetSeason(seasonSnap->seasons, reward, static_cast<int64_t>(std::time(nullptr)), m_seasonRecapSelection);
        m_renderDirty = true;
    }
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
        m_insights.model.SetLiveAndEndedSession(liveRecap, endedRecap, m_insightsShowsEndedSession);
        m_renderDirty = true;
    }

    auto snap = m_state->insights.GetSnapshot();
    if (!snap) return;
    const uint64_t version = snap->version;
    if (!force && version == m_lastInsightsVersion) return;
    m_lastInsightsVersion = version;
    bool loaded = snap->loaded && snap->primaryId == m_snap.myPrimaryId;
    if (loaded) {
        m_insights.model.SetHistory(m_snap.myPrimaryId, true, snap->people, snap->outcomes, snap->mmrContext, snap->sessions, snap->analyticsMatches);
    } else {
        m_insights.model.SetHistory(m_snap.myPrimaryId, false, {}, {}, {}, {}, {});
    }
    m_renderDirty = true;
}

void RmlUiController::ExportRecapPng() {
    m_recapCapturePending = false;
    const bool season = m_insights.model.CurrentTab() == InsightsView::Tab::Season;
    Rml::Element* card = m_insights.Document() && m_insights.IsVisible() ? m_insights.Document()->GetElementById(season ? "season-card" : "recap-card") : nullptr;
    if (!card || !m_d3dContext) return;
    const Rml::Vector2f offset = card->GetAbsoluteOffset(Rml::BoxArea::Border);
    const Rml::Vector2f size = card->GetBox().GetSize(Rml::BoxArea::Border);
    const std::wstring path = RmlCapture::NewPicturePath(season ? L"OmniStats-season" : L"OmniStats-recap");
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

bool RmlUiController::HandleInsightsAction(const std::string& action, Rml::Element* target) {
    if (action == "insights-open") {
        const std::string tab = Attribute(target, "data-tab");
        ShowInsights(tab.empty() ? m_insights.model.CurrentTab() : TabFromName(tab), false);
    } else if (action == "insights-close") {
        HideInsights();
    } else if (action == "insights-tab") {
        m_seasonRecapSelection = 0;
        m_insights.model.SetTab(TabFromName(Attribute(target, "data-tab")));
        if (m_insights.model.CurrentTab() == InsightsView::Tab::Season) RefreshInsights(true);
        m_renderDirty = true;
    } else if (action == "people-filter") {
        m_insights.model.SetPeopleFilter(Attribute(target, "data-filter") == "rivals" ? Insights::PeopleFilter::Rivals
                                                                                      : Insights::PeopleFilter::Teammates);
        m_renderDirty = true;
    } else if (action == "trend-playlist-filter") {
        std::string playlist = Attribute(target, "data-playlist");
        if (playlist.empty() && target) playlist = target->GetInnerRML().c_str();
        m_insights.model.SetTrendPlaylistFilter(playlist);
        m_renderDirty = true;
    } else if (action == "recap-export") {
        m_recapCapturePending = true;
        m_renderDirty = true;
    } else if (action == "season-export") {
        m_recapCapturePending = true;
        m_renderDirty = true;
    } else if (action == "season-history") {
        const bool previousSeason = m_seasonRecapSelection > 0;
        HideInsights();
        ShowHistory();
        m_history.model.SetSeasonOptions(true, {});
        m_history.model.SetDateFilter(previousSeason ? "last_season" : "this_season");
        TriggerHistoryQuery(false);
    } else if (action == "session-open") {
        const int idx = std::atoi(Attribute(target, "data-session-index").c_str());
        if (idx >= 0 && m_insights.model.OpenSession(static_cast<size_t>(idx))) {
            m_insightsShowsEndedSession = m_insights.model.ViewingArchivedSession();
            m_renderDirty = true;
        }
    } else if (action == "recap-prev-session") {
        if (m_insights.model.StepSession(-1)) {
            m_insightsShowsEndedSession = m_insights.model.ViewingArchivedSession();
            m_renderDirty = true;
        }
    } else if (action == "recap-next-session") {
        if (m_insights.model.StepSession(1)) {
            m_insightsShowsEndedSession = m_insights.model.ViewingArchivedSession();
            m_renderDirty = true;
        }
    } else if (action == "recap-compare") {
        m_insights.model.ToggleCompareWithPrevious();
        m_renderDirty = true;
    } else if (action == "sessions-load-more") {
        m_insights.model.LoadMoreSessions();
        m_renderDirty = true;
    } else {
        return false;
    }
    return true;
}

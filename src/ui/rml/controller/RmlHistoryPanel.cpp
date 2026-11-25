#include "ui/rml/controller/RmlUiController.hpp"
#include "core/state/LiveMatchStore.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <shellapi.h>

#include "database/DatabaseManager.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"
#include "core/state/MatchCardUrl.hpp"
#include "network/telemetry/TelemetryManager.hpp"

using namespace RmlUiDetail;

void RmlUiController::OpenHistory() {
    ShowHistory();
}

void RmlUiController::ShowHistory(int64_t detailMatchId, const std::string& detailMatchGuid) {
    if (!m_history.Document()) return;
    const std::string effectivePrimary = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    m_history.model.SetAccount(effectivePrimary);
    if (m_dbManager) m_dbManager->AsyncGetSeasonSummaries(effectivePrimary, ++m_seasonSummaryRequestId);
    m_history.Show();
    m_history.Document()->PullToFront();
    m_onboarding.Raise();
    TriggerHistoryQuery(false);
    if (detailMatchId > 0 || !detailMatchGuid.empty()) {
        OpenHistoryDetail(detailMatchId, detailMatchGuid);
    }
    m_renderDirty = true;
}

void RmlUiController::HideHistory() {
    m_history.Hide();
    m_history.model.CloseDetail();
    m_renderDirty = true;
}

void RmlUiController::TriggerHistoryQuery(bool append) {
    if (append && m_history.model.IsLoading()) return;
    const std::string effectivePrimary = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    m_history.model.SetAccount(effectivePrimary);
    const int offset = append ? m_history.model.LoadedRowCount() : 0;
    const uint64_t reqId = m_history.model.BeginQuery(append);
    if (m_dbManager && !effectivePrimary.empty()) {
        MatchQuery q = m_history.model.BuildQuery(offset, HistoryView::kPageSize);
        m_dbManager->AsyncQueryMatches(std::move(q), reqId, append);
    }
    m_renderDirty = true;
}

void RmlUiController::OpenHistoryDetail(int64_t matchId, const std::string& matchGuid) {
    if (m_telemetry) m_telemetry->RecordMilestone(TelemetryManager::Milestone::MatchHistoryDetail);
    const std::string effectivePrimary = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    const uint64_t reqId = m_history.model.BeginDetail(matchId);
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
    const auto seasonSnap = m_state->seasonSummaries.load();
    if (seasonSnap && seasonSnap->loaded && seasonSnap->primaryId == effectivePrimary &&
        (force || seasonSnap->requestId != m_lastHistorySeasonRequestId)) {
        m_lastHistorySeasonRequestId = seasonSnap->requestId;
        std::vector<SeasonRecord> seasons;
        for (const auto& summary : seasonSnap->seasons)
            seasons.push_back(summary.season);
        const bool seasonFiltered = m_history.model.DateFilter() == "this_season" || m_history.model.DateFilter() == "last_season";
        const bool changed = m_history.model.SetSeasonOptions(m_config.group_history_by_season || seasonFiltered, std::move(seasons));
        if (changed || seasonFiltered) TriggerHistoryQuery(false);
        m_renderDirty = true;
    }
    if (m_history.model.Account() != effectivePrimary) {
        m_history.model.SetAccount(effectivePrimary);
    }
    auto snap = m_state->historyView.GetSnapshot();
    if (!snap) return;
    const uint64_t version = snap->version;
    if (!force && version == m_lastHistoryViewVersion) return;
    m_lastHistoryViewVersion = version;

    if (!snap->loading && snap->requestId > 0) {
        m_history.model.ApplyQueryResult(snap->requestId, snap->rows, snap->total);
    }
    if (!snap->detailLoading && (snap->detailRequestId > 0 || snap->detail.has_value())) {
        m_history.model.ApplyDetailResult(snap->detailRequestId, snap->detail);
    }
    m_renderDirty = true;
}

bool RmlUiController::HandleHistoryAction(const std::string& action, Rml::Element* target) {
    if (action == "history-open") {
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
        if (m_history.IsVisible() && (matchId > 0 || !guidAttr.empty())) {
            OpenHistoryDetail(matchId, guidAttr);
        } else {
            ShowHistory(matchId, guidAttr);
        }
    } else if (action == "history-close") {
        HideHistory();
    } else if (action == "history-close-detail") {
        m_history.model.CloseDetail();
        m_renderDirty = true;
    } else if (action == "history-detail-tab") {
        const std::string tab = Attribute(target, "data-tab");
        if (!tab.empty()) {
            m_history.model.SetDetailTab(tab);
            if (m_history.Document()) {
                if (auto* body = m_history.Document()->QuerySelector(".history-detail-body")) {
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
        if (!playlist.empty()) m_history.model.SetPlaylistFilter(playlist);
        if (!result.empty()) m_history.model.SetResultFilter(result);
        if (!date.empty()) m_history.model.SetDateFilter(date);
        if (!sort.empty()) m_history.model.SetSortFilter(sort);
        TriggerHistoryQuery(false);
    } else if (action == "history-event-team") {
        m_history.model.SetEventTeamFilter(Attribute(target, "data-team-filter"));
        m_renderDirty = true;
    } else if (action == "history-event-filter") {
        m_history.model.SetEventFilter(Attribute(target, "data-event-filter"));
        m_renderDirty = true;
    } else if (action == "history-clear-filters") {
        m_history.model.ClearFilters();
        TriggerHistoryQuery(false);
    } else if (action == "history-more") {
        TriggerHistoryQuery(true);
    } else if (action == "history-copy") {
        const std::string text = m_history.model.FormatScoreboardText();
        if (!text.empty()) {
            m_systemInterface.SetClipboardText(text);
            ShowToast("Copied match summary to clipboard.");
        }
    } else if (action == "history-share-card") {
        const auto& detailOpt = m_history.model.Detail();
        if (detailOpt.has_value() && detailOpt->found) {
            const std::string url = OmniStats::MatchCard::BuildMatchCardUrl(*detailOpt);
            m_systemInterface.SetClipboardText(url);
            ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
            ShowToast("Opened the match card in your browser. Link copied.");
            if (m_telemetry) m_telemetry->RecordMilestone(TelemetryManager::Milestone::MatchCardShared);
        } else {
            ShowToast("Match details not loaded.", true);
        }
    } else {
        return false;
    }
    return true;
}

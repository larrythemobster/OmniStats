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
#include "ui/rml/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

namespace {
    InsightsView::Tab TabFromName(const std::string& name) {
        if (name == "people") return InsightsView::Tab::People;
        if (name == "trends") return InsightsView::Tab::Trends;
        return InsightsView::Tab::Recap;
    }
}

bool RmlUiController::WantsAttention() const {
    // A recap captured when Rocket League closed is only opened by Update(),
    // and the host skips Update() while the window is hidden, so the pending
    // flag itself has to keep the window drawn.
    return m_insightsVisible || m_onboardingVisible || (m_state && m_state->ui.showSessionRecap.load());
}

void RmlUiController::OpenInsights() {
    ShowInsights(m_insights.CurrentTab(), false);
}

void RmlUiController::LoadViewDocuments() {
    if (!m_context) return;
    m_insightsDoc = m_context->LoadDocument("res://insights.rml");
    m_onboardingDoc = m_context->LoadDocument("res://onboarding.rml");
    if (m_insightsVisible && m_insightsDoc) m_insightsDoc->Show();
    if (m_onboardingVisible && m_onboardingDoc) m_onboardingDoc->Show();
}

void RmlUiController::CloseViewDocuments() {
    if (m_insightsDoc) m_insightsDoc->Close();
    if (m_onboardingDoc) m_onboardingDoc->Close();
    m_insightsDoc = nullptr;
    m_onboardingDoc = nullptr;
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
    if (m_insightsShowsEndedSession) {
        if (force) {
            SessionRecap recap;
            {
                std::shared_lock lock(m_state->game.mutex);
                recap = m_state->game.lastSessionRecap;
            }
            m_insights.SetRecap(recap, false);
        }
    } else if (force || m_lastRecapGameVersion != m_lastGameVersion) {
        m_lastRecapGameVersion = m_lastGameVersion;
        m_insights.SetRecap({true, 0, m_snap.sessionTotals, m_snap.sessionGamemodes}, true);
        m_renderDirty = true;
    }

    const uint64_t version = m_state->insights.version.load(std::memory_order_relaxed);
    if (!force && version == m_lastInsightsVersion) return;
    m_lastInsightsVersion = version;
    std::vector<PersonRecord> people;
    std::vector<MatchOutcome> outcomes;
    bool loaded = false;
    {
        std::lock_guard lock(m_state->insights.mutex);
        loaded = m_state->insights.loaded && m_state->insights.primaryId == m_snap.myPrimaryId;
        if (loaded) {
            people = m_state->insights.people;
            outcomes = m_state->insights.outcomes;
        }
    }
    m_insights.SetHistory(m_snap.myPrimaryId, loaded, people, outcomes);
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
                                                 static_cast<int>(std::ceil(size.x)), static_cast<int>(std::ceil(size.y)), path, error)) {
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
    } else if (action == "recap-export") {
        m_recapCapturePending = true;
        m_renderDirty = true;
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
    } else {
        return false;
    }
    return true;
}

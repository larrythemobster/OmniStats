#include "ui/rml/controller/RmlUiController.hpp"

#include <RmlUi/Core/Context.h>

#include "database/DatabaseManager.hpp"
bool RmlUiController::WantsAttention() const {
    // A recap captured when Rocket League closed is only opened by Update(),
    // and the host skips Update() while the window is hidden, so the pending
    // flag itself has to keep the window drawn.
    return m_insights.IsVisible() || m_history.IsVisible() || m_accounts.IsVisible() || m_playerNote.IsVisible() || m_onboarding.IsVisible() ||
           m_accountPromptPending || m_statsApiPrompt != StatsApiPrompt::Hidden || m_sessionExpiredPrompt || m_dejaVuPrompt ||
           DejaVuPromptReady() ||
           (m_state && m_state->ui.showSessionRecap.load()) || (m_state && m_state->ui.showMenu.load());
}

void RmlUiController::LoadViewDocuments() {
    if (!m_context) return;
    for (auto* view : ModalDocuments())
        view->Load(*m_context);
}

void RmlUiController::CloseViewDocuments() {
    for (auto* view : ModalDocuments())
        view->Unload();
    m_lastOnboardingIdentityRml.clear();
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

bool RmlUiController::HandleViewAction(const std::string& action, Rml::Element* target) {
    if (action.starts_with("player-note-") || action.starts_with("player-scout-"))
        return HandlePlayerNoteAction(action, target);
    if (action.starts_with("history-")) return HandleHistoryAction(action, target);
    if (action.starts_with("accounts-")) return HandleAccountsAction(action, target);
    if (action.starts_with("onboarding-") || action.starts_with("account-") || action == "open-legal-link")
        return HandleOnboardingAction(action, target);
    if (action.starts_with("statsapi-prompt-") || action.starts_with("session-prompt-") || action.starts_with("dejavu-prompt-") ||
        action == "session-sign-in" || action == "open-rank-settings") return HandlePromptsAction(action, target);
    return HandleInsightsAction(action, target);
}

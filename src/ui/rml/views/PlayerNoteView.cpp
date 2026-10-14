#include "ui/rml/views/PlayerNoteView.hpp"

#include <RmlUi/Core/Context.h>

#include <chrono>
#include <ctime>

#include "core/accounts/TrackedAccounts.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"

namespace {
    constexpr size_t kNearLimitCodePoints = PlayerNotes::kMaxNoteCodePoints - 200;

    std::string FormatEditedAt(int64_t unixSeconds) {
        if (unixSeconds <= 0) return {};
        const std::time_t time = static_cast<std::time_t>(unixSeconds);
        std::tm local{};
        if (localtime_s(&local, &time) != 0) return {};
        char buffer[64]{};
        std::strftime(buffer, sizeof(buffer), "%d %b %Y, %H:%M", &local);
        return buffer;
    }
}

bool PlayerNoteView::Create(Rml::Context* context) {
    Reset();
    if (!context) return false;
    Rml::DataModelConstructor constructor = context->CreateDataModel("player_note");
    if (!constructor) return false;

    constructor.Bind("open", &m_open);
    constructor.Bind("available", &m_available);
    constructor.Bind("unavailable_message", &m_unavailableMessage);
    constructor.Bind("player_name", &m_playerName);
    constructor.Bind("platform_label", &m_platformLabel);
    constructor.Bind("platform_class", &m_platformClass);
    constructor.Bind("account_label", &m_accountLabel);
    constructor.Bind("context", &m_context);
    constructor.Bind("note_value", &m_noteValue);
    constructor.Bind("char_count", &m_charCount);
    constructor.Bind("near_limit", &m_nearLimit);
    constructor.Bind("dirty", &m_dirty);
    constructor.Bind("saving", &m_saving);
    constructor.Bind("has_saved", &m_hasSaved);
    constructor.Bind("confirm_delete", &m_confirmDelete);
    constructor.Bind("confirm_discard", &m_confirmDiscard);
    constructor.Bind("status", &m_status);
    constructor.Bind("status_error", &m_statusError);
    constructor.Bind("updated_label", &m_updatedLabel);
    constructor.Bind("save_label", &m_saveLabel);

    m_handle = constructor.GetModelHandle();
    m_bound = true;
    return true;
}

void PlayerNoteView::Reset() {
    m_handle = {};
    m_bound = false;
}

void PlayerNoteView::Dirty(const char* name) {
    if (m_bound) m_handle.DirtyVariable(name);
}

void PlayerNoteView::Open(const PlayerNoteOpenRequest& request, const std::optional<PlayerNotes::Note>& saved) {
    m_open = true;
    m_identity = PlayerNotes::IdentityFromPrimaryId(request.primaryId);
    m_available = m_identity.has_value();
    m_playerNameSource = request.name;
    m_pendingRequestId = 0;
    m_confirmDelete = false;
    m_confirmDiscard = false;
    m_status.clear();
    m_statusError = false;

    std::string displayName = request.name;
    if (displayName.empty() && saved) displayName = saved->lastKnownName;
    m_playerName = displayName.empty() ? "Unknown player" : displayName;
    m_context = request.context;

    std::string platform = request.primaryId.substr(0, request.primaryId.find('|'));
    if (request.primaryId.find('|') == std::string::npos) platform.clear();
    const auto kind = RmlUiDetail::PlatformKindFor(platform);
    m_platformLabel = platform.empty() ? "" : RmlUiDetail::PlatformDisplayName(kind, platform);
    m_platformClass = std::string("badge ") + RmlUiDetail::PlatformClass(kind);

    m_accountLabel.clear();
    if (m_identity) {
        m_accountLabel = "Account " + TrackedAccounts::MaskedAccountId(m_identity->accountId);
        if (m_identity->splitscreen != 0) m_accountLabel += " · splitscreen " + std::to_string(m_identity->splitscreen);
        m_unavailableMessage.clear();
    } else {
        m_unavailableMessage =
            "This player has no platform account ID (bots and some local players), so a note could end up attached to the wrong person. Notes are only available for identified accounts.";
    }

    SetSaved(m_available ? saved : std::nullopt, true);
    for (const char* name : {"open", "available", "unavailable_message", "player_name", "platform_label", "platform_class",
                             "account_label", "context", "confirm_delete", "confirm_discard", "status", "status_error"})
        Dirty(name);
}

void PlayerNoteView::Close() {
    m_open = false;
    m_confirmDelete = false;
    m_confirmDiscard = false;
    m_pendingRequestId = 0;
    m_identity.reset();
    m_draft.clear();
    m_savedText.clear();
    Dirty("open");
    Dirty("confirm_delete");
    Dirty("confirm_discard");
}

void PlayerNoteView::SetSaved(const std::optional<PlayerNotes::Note>& saved, bool replaceDraft) {
    m_savedText = saved ? saved->text : std::string{};
    m_hasSaved = saved.has_value();
    m_updatedLabel = saved ? "Last edited " + FormatEditedAt(saved->updatedAtUnix)
                           : "No note yet. Notes are stored only on this PC.";
    if (replaceDraft) {
        m_draft = m_savedText;
        m_noteValue = m_draft;
        Dirty("note_value");
    }
    Dirty("has_saved");
    Dirty("updated_label");
    UpdateDerived();
}

void PlayerNoteView::SetDraft(const std::string& text) {
    if (!m_open || !m_available) return;
    m_draft = text;
    // Keep the bound copy current without dirtying it, so a later unrelated
    // rebind cannot restore stale text.
    m_noteValue = text;
    if (m_status != "" && !m_statusError) {
        m_status.clear();
        Dirty("status");
    }
    UpdateDerived();
}

bool PlayerNoteView::IsDirty() const {
    return m_available && PlayerNotes::NormalizeNote(m_draft) != m_savedText;
}

void PlayerNoteView::UpdateDerived() {
    const size_t count = PlayerNotes::CodePointCount(m_draft);
    m_charCount = std::to_string(count) + " / " + std::to_string(PlayerNotes::kMaxNoteCodePoints);
    m_nearLimit = count >= kNearLimitCodePoints;
    m_dirty = IsDirty();
    m_saving = m_pendingRequestId != 0;
    m_saveLabel = m_saving ? "Saving..." : "Save";
    for (const char* name : {"char_count", "near_limit", "dirty", "saving", "save_label"})
        Dirty(name);
}

bool PlayerNoteView::BeginSave(uint64_t requestId) {
    if (!m_open || !m_available || m_pendingRequestId != 0 || !IsDirty()) return false;
    m_pendingRequestId = requestId;
    m_confirmDiscard = false;
    m_status.clear();
    m_statusError = false;
    Dirty("confirm_discard");
    Dirty("status");
    Dirty("status_error");
    UpdateDerived();
    return true;
}

bool PlayerNoteView::BeginDelete(uint64_t requestId) {
    if (!m_open || !m_available || m_pendingRequestId != 0 || !m_hasSaved) return false;
    m_pendingRequestId = requestId;
    m_confirmDelete = false;
    m_status.clear();
    m_statusError = false;
    Dirty("confirm_delete");
    Dirty("status");
    Dirty("status_error");
    UpdateDerived();
    return true;
}

void PlayerNoteView::ApplyResult(uint64_t requestId, bool ok, const std::string& error, const std::optional<PlayerNotes::Note>& saved) {
    if (!m_open || requestId == 0 || requestId != m_pendingRequestId) return;
    m_pendingRequestId = 0;
    if (ok) {
        // Show the stored (normalized) text so the editor matches what reloads.
        SetSaved(saved, true);
        m_status = saved ? "Saved." : "Note deleted.";
        m_statusError = false;
    } else {
        m_status = "Couldn't save: " + (error.empty() ? std::string("the database reported an error.") : error) +
                   " Your text is still here.";
        m_statusError = true;
        UpdateDerived();
    }
    Dirty("status");
    Dirty("status_error");
}

void PlayerNoteView::SyncSaved(const std::optional<PlayerNotes::Note>& saved) {
    if (!m_open || !m_available || m_pendingRequestId != 0) return;
    const std::string incoming = saved ? saved->text : std::string{};
    if (incoming == m_savedText && saved.has_value() == m_hasSaved) return;
    SetSaved(saved, !IsDirty());
}

void PlayerNoteView::ShowStatus(const std::string& message, bool error) {
    m_status = message;
    m_statusError = error;
    Dirty("status");
    Dirty("status_error");
}

void PlayerNoteView::RequestDelete() {
    if (!m_hasSaved || m_pendingRequestId != 0) return;
    m_confirmDelete = true;
    m_confirmDiscard = false;
    Dirty("confirm_delete");
    Dirty("confirm_discard");
}

void PlayerNoteView::CancelDelete() {
    m_confirmDelete = false;
    Dirty("confirm_delete");
}

bool PlayerNoteView::RequestClose() {
    if (!m_open) return true;
    if (IsDirty() && !m_confirmDiscard) {
        m_confirmDiscard = true;
        m_confirmDelete = false;
        Dirty("confirm_discard");
        Dirty("confirm_delete");
        return false;
    }
    Close();
    return true;
}

void PlayerNoteView::KeepEditing() {
    m_confirmDiscard = false;
    Dirty("confirm_discard");
}

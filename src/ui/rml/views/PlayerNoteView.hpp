#pragma once

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>
#include "core/accounts/PlayerScout.hpp"

#include "core/accounts/PlayerNotes.hpp"

namespace Rml {
    class Context;
}

struct PlayerNoteOpenRequest {
    std::string primaryId;
    std::string name;
    // Plain-text line under the name, e.g. "Champion III · With 5-3 · Vs 4-2".
    std::string context;
};

// State behind player_note.rml. The draft lives here while the user types;
// nothing reaches SQLite until Save or Delete.
struct ScoutEncounterRow {
    std::string match_id;
    std::string when;
    std::string playlist;
    std::string relation;
    std::string outcome;
    bool win = false;
};

class PlayerNoteView {
  public:
    bool Create(Rml::Context* context);
    void Reset();

    void Open(const PlayerNoteOpenRequest& request, const std::optional<PlayerNotes::Note>& saved);
    void Close();
    const std::string& PrimaryId() const {
        return m_primaryId;
    }
    void SetTags(const std::vector<std::string>& tags);
    void SetTagDraft(const std::string& value);
    const std::string& TagDraft() const {
        return m_tagDraft;
    }
    bool BeginTagChange(uint64_t requestId);
    void ApplyTagResult(uint64_t requestId, bool ok, const std::string& error, const std::vector<std::string>& tags);
    uint64_t PendingTagRequestId() const {
        return m_pendingTagRequestId;
    }
    void SetScoutLoading(const std::string& knownRank, const std::string& rankColor = {});
    void SetScoutResult(const std::optional<PlayerScoutProfile>& profile);
    bool IsOpen() const {
        return m_open;
    }
    const std::optional<PlayerNotes::Identity>& Identity() const {
        return m_identity;
    }
    const std::string& PlayerName() const {
        return m_playerNameSource;
    }

    // Text typed into the editor. The control already shows it, so the bound
    // value is not pushed back (that would reset the caret).
    void SetDraft(const std::string& text);
    const std::string& Draft() const {
        return m_draft;
    }
    bool IsDirty() const;
    bool IsSaving() const {
        return m_pendingRequestId != 0;
    }
    uint64_t PendingRequestId() const {
        return m_pendingRequestId;
    }

    // False when there is nothing to save or a request is already in flight.
    bool BeginSave(uint64_t requestId);
    bool BeginDelete(uint64_t requestId);
    // `saved` is the stored note after the request completed (nullopt when none).
    void ApplyResult(uint64_t requestId, bool ok, const std::string& error, const std::optional<PlayerNotes::Note>& saved);
    // Picks up a change made outside this editor while there are no unsaved edits.
    void SyncSaved(const std::optional<PlayerNotes::Note>& saved);
    void ShowStatus(const std::string& message, bool error);

    void RequestDelete();
    void CancelDelete();
    bool ConfirmingDelete() const {
        return m_confirmDelete;
    }
    // Returns true when the editor closed; false when it is asking to discard edits.
    bool RequestClose();
    void KeepEditing();
    bool ConfirmingDiscard() const {
        return m_confirmDiscard;
    }

  private:
    void SetSaved(const std::optional<PlayerNotes::Note>& saved, bool replaceDraft);
    void UpdateDerived();
    void Dirty(const char* name);

    Rml::DataModelHandle m_handle;
    bool m_bound = false;

    std::optional<PlayerNotes::Identity> m_identity;
    std::string m_playerNameSource;
    std::string m_primaryId;
    std::vector<Rml::String> m_tags;
    std::vector<ScoutEncounterRow> m_recent;
    std::string m_tagDraft;
    Rml::String m_tagValue;
    Rml::String m_tagStatus;
    bool m_tagStatusError = false;
    bool m_tagSaving = false;
    bool m_tagCanAdd = false;
    uint64_t m_pendingTagRequestId = 0;
    bool m_scoutLoading = false;
    bool m_scoutReady = false;
    Rml::String m_scoutStatus;
    Rml::String m_rankLabel;
    Rml::String m_rankColor;
    Rml::String m_encounterTotal;
    Rml::String m_withRecord;
    Rml::String m_againstRecord;
    Rml::String m_lastEncounter;
    Rml::String m_commonPlaylist;
    std::string m_savedText;
    std::string m_draft;
    uint64_t m_pendingRequestId = 0;

    bool m_open = false;
    bool m_available = false;
    Rml::String m_unavailableMessage;
    Rml::String m_playerName;
    Rml::String m_platformLabel;
    Rml::String m_platformClass;
    Rml::String m_accountLabel;
    Rml::String m_context;
    Rml::String m_noteValue;
    Rml::String m_charCount;
    bool m_nearLimit = false;
    bool m_dirty = false;
    bool m_saving = false;
    bool m_hasSaved = false;
    bool m_confirmDelete = false;
    bool m_confirmDiscard = false;
    Rml::String m_status;
    bool m_statusError = false;
    Rml::String m_updatedLabel;
    Rml::String m_saveLabel = "Save";
};

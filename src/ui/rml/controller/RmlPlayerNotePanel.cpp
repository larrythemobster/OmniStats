#include "ui/rml/controller/RmlUiController.hpp"
#include "core/state/LiveMatchStore.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include "database/DatabaseManager.hpp"
#include "ui/Formatting.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"
#include "network/telemetry/TelemetryManager.hpp"

using namespace RmlUiDetail;

void RmlUiController::ShowPlayerNote(const std::string& primaryId, const std::string& name, const std::string& context) {
    if (!m_playerNote.Document() || !m_state) return;
    if (m_telemetry) m_telemetry->RecordMilestone(TelemetryManager::Milestone::PlayerLookup);
    const auto identity = PlayerNotes::IdentityFromPrimaryId(primaryId);
    if (m_playerNote.IsVisible() && m_playerNote.model.IsDirty()) {
        if (!identity || !m_playerNote.model.Identity() || !(*identity == *m_playerNote.model.Identity()))
            m_playerNote.model.ShowStatus("Save or discard this note before opening another player's note.", true);
        m_playerNote.Document()->PullToFront();
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

    m_playerNote.model.Open({primaryId, name, contextLine}, saved);
    m_playerNote.model.SetTags(tags);
    std::string currentRank;
    std::string rankColor;
    if (auto it = m_snap.roster.find(primaryId); it != m_snap.roster.end() && it->second.fetched) {
        if (const std::string tier = BestRankTier(it->second); !tier.empty()) {
            currentRank = Format::RankTier(tier, m_config.use_roman_numerals);
            rankColor = CssColor(Format::RankColor(tier));
        }
    }
    m_playerNote.model.SetScoutLoading(currentRank, rankColor);
    ++m_nextScoutRequestId;
    if (m_dbManager && identity) {
        const std::string ownId = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
        m_dbManager->AsyncGetPlayerScout(ownId, primaryId, m_nextScoutRequestId);
    } else {
        m_playerNote.model.SetScoutResult(std::nullopt);
    }
    m_playerNote.Show();
    m_playerNote.Document()->PullToFront();
    m_onboarding.Raise();
    m_renderDirty = true;
}

void RmlUiController::HidePlayerNote() {
    m_playerNote.Hide();
    m_playerNote.model.Close();
    m_renderDirty = true;
}

void RmlUiController::RequestClosePlayerNote() {
    if (m_playerNote.model.RequestClose())
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
        if (const auto& identity = m_playerNote.model.Identity()) {
            const auto key = identity->Key();
            if (auto it = notesSnap->byKey.find(key); it != notesSnap->byKey.end()) editorNote = it->second;
            if (auto it = notesSnap->tagsByKey.find(key); it != notesSnap->tagsByKey.end()) editorTags = it->second;
        }
        if (m_playerNote.IsVisible()) {
            if (m_playerNote.model.IsSaving() && notesSnap->completedRequestId == m_playerNote.model.PendingRequestId())
                m_playerNote.model.ApplyResult(notesSnap->completedRequestId, notesSnap->lastRequestOk, notesSnap->lastRequestError, editorNote);
            else
                m_playerNote.model.SyncSaved(editorNote);
            if (m_playerNote.model.PendingTagRequestId() && notesSnap->completedTagRequestId == m_playerNote.model.PendingTagRequestId())
                m_playerNote.model.ApplyTagResult(notesSnap->completedTagRequestId, notesSnap->lastTagRequestOk, notesSnap->lastTagRequestError, editorTags);
            else if (!m_playerNote.model.PendingTagRequestId())
                m_playerNote.model.SetTags(editorTags);
        }
        m_history.model.SetNotedPlayers(notedKeys, notesSnap->loaded);
        m_insights.model.SetNotedPlayers(notedKeys, notesSnap->loaded);
        m_renderDirty = true;
    }
    auto scoutSnap = m_state->scout.GetSnapshot();
    if (scoutSnap && m_playerNote.IsVisible() && scoutSnap->version != m_lastScoutViewVersion) {
        m_lastScoutViewVersion = scoutSnap->version;
        if (scoutSnap->loaded && scoutSnap->requestId == m_nextScoutRequestId &&
            m_playerNote.model.PrimaryId() == scoutSnap->playerId) {
            m_playerNote.model.SetScoutResult(scoutSnap->success ? std::optional{scoutSnap->profile} : std::nullopt);
            m_renderDirty = true;
        }
    }
}

bool RmlUiController::HandlePlayerNoteAction(const std::string& action, Rml::Element* target) {
    if (action == "player-note-open") {
        ShowPlayerNote(Attribute(target, "data-pid"), Attribute(target, "data-name"), Attribute(target, "data-context"));
    } else if (action == "player-note-close") {
        RequestClosePlayerNote();
    } else if (action == "player-note-discard") {
        HidePlayerNote();
    } else if (action == "player-note-keep-editing") {
        m_playerNote.model.KeepEditing();
        m_renderDirty = true;
    } else if (action == "player-note-save") {
        if (!m_dbManager) {
            m_playerNote.model.ShowStatus("Couldn't save: the local database is unavailable. Your text is still here.", true);
        } else if (m_playerNote.model.Identity() && m_playerNote.model.BeginSave(m_nextPlayerNoteRequestId + 1)) {
            ++m_nextPlayerNoteRequestId;
            m_dbManager->AsyncSavePlayerNote(*m_playerNote.model.Identity(), m_playerNote.model.Draft(), m_playerNote.model.PlayerName(), m_nextPlayerNoteRequestId);
        }
        m_renderDirty = true;
    } else if (action == "player-note-delete") {
        m_playerNote.model.RequestDelete();
        m_renderDirty = true;
    } else if (action == "player-note-cancel-delete") {
        m_playerNote.model.CancelDelete();
        m_renderDirty = true;
    } else if (action == "player-note-confirm-delete") {
        if (!m_dbManager) {
            m_playerNote.model.ShowStatus("Couldn't delete: the local database is unavailable.", true);
        } else if (m_playerNote.model.Identity() && m_playerNote.model.BeginDelete(m_nextPlayerNoteRequestId + 1)) {
            ++m_nextPlayerNoteRequestId;
            m_dbManager->AsyncDeletePlayerNote(*m_playerNote.model.Identity(), m_nextPlayerNoteRequestId);
        }
        m_renderDirty = true;
    } else if (action == "player-scout-add-tag" || action == "player-scout-remove-tag") {
        const bool add = action == "player-scout-add-tag";
        const std::string tag = PlayerNotes::NormalizeTag(add ? m_playerNote.model.TagDraft() : Attribute(target, "data-tag"));
        if (tag.empty()) {
            m_playerNote.model.ShowStatus("Enter a valid tag.", true);
        } else if (!m_dbManager) {
            m_playerNote.model.ShowStatus("The local database is unavailable.", true);
        } else if (m_playerNote.model.Identity() && m_playerNote.model.BeginTagChange(m_nextPlayerTagRequestId + 1)) {
            ++m_nextPlayerTagRequestId;
            m_dbManager->AsyncSetPlayerTag(*m_playerNote.model.Identity(), tag, add, m_nextPlayerTagRequestId);
        }
        m_renderDirty = true;
    } else if (action == "player-scout-open-match") {
        if (m_playerNote.model.IsDirty()) {
            m_playerNote.model.ShowStatus("Save or discard your note before opening a match.", true);
        } else {
            const std::string idText = Attribute(target, "data-id");
            try {
                const int64_t id = std::stoll(idText);
                if (id > 0) {
                    HidePlayerNote();
                    ShowHistory(id);
                }
            } catch (const std::exception&) {
                m_playerNote.model.ShowStatus("Could not open this match.", true);
            }
        }
        m_renderDirty = true;
    } else {
        return false;
    }
    return true;
}

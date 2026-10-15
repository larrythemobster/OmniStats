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

    std::string FormatShortDate(int64_t unixSeconds) {
        if (unixSeconds <= 0) return {};
        const std::time_t time = static_cast<std::time_t>(unixSeconds);
        std::tm local{};
        if (localtime_s(&local, &time) != 0) return {};
        char buffer[32]{};
        std::strftime(buffer, sizeof(buffer), "%d %b %Y", &local);
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
    constructor.RegisterArray<std::vector<Rml::String>>();
    if (auto row = constructor.RegisterStruct<ScoutEncounterRow>()) {
        row.RegisterMember("match_id", &ScoutEncounterRow::match_id);
        row.RegisterMember("when", &ScoutEncounterRow::when);
        row.RegisterMember("playlist", &ScoutEncounterRow::playlist);
        row.RegisterMember("relation", &ScoutEncounterRow::relation);
        row.RegisterMember("outcome", &ScoutEncounterRow::outcome);
        row.RegisterMember("win", &ScoutEncounterRow::win);
    }
    constructor.RegisterArray<std::vector<ScoutEncounterRow>>();
    constructor.Bind("tags", &m_tags);
    constructor.Bind("tag_value", &m_tagValue);
    constructor.Bind("tag_status", &m_tagStatus);
    constructor.Bind("tag_status_error", &m_tagStatusError);
    constructor.Bind("tag_saving", &m_tagSaving);
    constructor.Bind("tag_can_add", &m_tagCanAdd);
    constructor.Bind("scout_loading", &m_scoutLoading);
    constructor.Bind("scout_ready", &m_scoutReady);
    constructor.Bind("scout_status", &m_scoutStatus);
    constructor.Bind("scout_rank", &m_rankLabel);
    constructor.Bind("scout_rank_color", &m_rankColor);
    constructor.Bind("scout_encounters", &m_encounterTotal);
    constructor.Bind("scout_with", &m_withRecord);
    constructor.Bind("scout_against", &m_againstRecord);
    constructor.Bind("scout_last", &m_lastEncounter);
    constructor.Bind("scout_playlist", &m_commonPlaylist);
    constructor.Bind("scout_recent", &m_recent);

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
    m_primaryId = request.primaryId;
    m_tagDraft.clear();
    m_tagValue.clear();
    m_pendingTagRequestId = 0;
    m_tagStatus.clear();
    m_tagStatusError = false;
    m_tagSaving = false;
    m_tagCanAdd = false;
    m_tags.clear();
    m_recent.clear();
    SetScoutLoading({});
    for (const char* field : {"tag_value", "tag_status", "tag_status_error", "tag_saving", "tag_can_add", "tags", "scout_recent"})
        Dirty(field);
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
    m_primaryId.clear();
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

void PlayerNoteView::SetTags(const std::vector<std::string>& tags) {
    m_tags.clear();
    for (const auto& tag : tags)
        m_tags.push_back(tag);
    m_tagCanAdd = m_available && tags.size() < PlayerNotes::kMaxTagsPerPlayer && !m_tagSaving;
    Dirty("tags");
    Dirty("tag_can_add");
}

void PlayerNoteView::SetTagDraft(const std::string& text) {
    if (!m_open || !m_available) return;
    m_tagDraft = text;
    m_tagValue = text; // do not dirty, preserving caret
    if (!m_tagSaving) {
        m_tagStatus.clear();
        Dirty("tag_status");
    }
}

bool PlayerNoteView::BeginTagChange(uint64_t requestId) {
    if (!m_open || !m_available || m_tagSaving || requestId == 0) return false;
    m_pendingTagRequestId = requestId;
    m_tagSaving = true;
    m_tagCanAdd = false;
    m_tagStatus.clear();
    Dirty("tag_saving");
    Dirty("tag_can_add");
    Dirty("tag_status");
    return true;
}

void PlayerNoteView::ApplyTagResult(uint64_t requestId, bool ok, const std::string& error,
                                    const std::vector<std::string>& tags) {
    if (!m_open || requestId == 0 || requestId != m_pendingTagRequestId) return;
    m_pendingTagRequestId = 0;
    m_tagSaving = false;
    if (ok) {
        SetTags(tags);
        m_tagDraft.clear();
        m_tagValue.clear();
        Dirty("tag_value");
        m_tagStatus = "Tag updated.";
    } else {
        m_tagStatus = error.empty() ? "Could not update tag." : error;
    }
    m_tagStatusError = !ok;
    m_tagCanAdd = m_available && m_tags.size() < PlayerNotes::kMaxTagsPerPlayer;
    Dirty("tag_saving");
    Dirty("tag_can_add");
    Dirty("tag_status_error");
    Dirty("tag_status");
}

void PlayerNoteView::SetScoutLoading(const std::string& knownRank, const std::string& rankColor) {
    m_scoutLoading = true;
    m_scoutReady = false;
    m_scoutStatus = "Loading saved matches...";
    m_rankLabel = knownRank;
    m_rankColor = rankColor.empty() ? "#eef1f5" : rankColor;
    m_encounterTotal.clear();
    m_withRecord.clear();
    m_againstRecord.clear();
    m_lastEncounter.clear();
    m_commonPlaylist.clear();
    m_recent.clear();
    for (const char* name : {"scout_loading", "scout_ready", "scout_status", "scout_rank", "scout_rank_color", "scout_encounters",
                             "scout_with", "scout_against", "scout_last", "scout_playlist", "scout_recent"})
        Dirty(name);
}

void PlayerNoteView::SetScoutResult(const std::optional<PlayerScoutProfile>& profile) {
    m_scoutLoading = false;
    m_scoutReady = profile.has_value();
    m_scoutStatus = profile ? std::string{} : "Couldn't load saved matches with this player.";
    m_recent.clear();
    if (profile) {
        const auto& r = profile->record;
        const int count = r.GamesWith() + r.GamesAgainst();
        m_encounterTotal = std::to_string(count);
        m_withRecord = std::to_string(r.winsWith) + "-" + std::to_string(r.lossesWith);
        m_againstRecord = std::to_string(r.winsAgainst) + "-" + std::to_string(r.lossesAgainst);
        m_lastEncounter = r.lastSeenUnix > 0 ? FormatShortDate(r.lastSeenUnix) : "--";
        m_commonPlaylist = profile->mostFrequentPlaylist;
        for (const auto& match : profile->recent) {
            ScoutEncounterRow row;
            row.match_id = std::to_string(match.matchId);
            row.when = FormatShortDate(match.playedAtUnix);
            row.playlist = match.playlist.empty() ? "Unknown playlist" : match.playlist;
            row.relation = match.opponent ? "Vs" : "With";
            row.outcome = match.win ? "W" : "L";
            row.win = match.win;
            m_recent.push_back(std::move(row));
        }
    }
    for (const char* name : {"scout_loading", "scout_ready", "scout_status", "scout_encounters",
                             "scout_with", "scout_against", "scout_last", "scout_playlist", "scout_recent"})
        Dirty(name);
}

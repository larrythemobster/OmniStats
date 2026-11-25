#include "PlayerNotesStore.hpp"

#include <utility>

namespace database::store {

    void PlayerNotesStore::PublishLoaded(bool inLoadFailed,
                                         std::unordered_map<std::string, PlayerNotes::Note> inNotes,
                                         std::unordered_map<std::string, std::vector<std::string>> inTags) {
        std::lock_guard lock(m_mutex);
        m_state.loaded = true;
        m_state.loadFailed = inLoadFailed;
        m_state.byKey = std::move(inNotes);
        m_state.tagsByKey = std::move(inTags);
        PublishCurrentLocked();
    }

    void PlayerNotesStore::PublishNoteSaved(uint64_t reqId, bool ok, const std::string& err,
                                            const PlayerNotes::Identity& identity,
                                            std::string_view text, std::string_view lastKnownName, int64_t nowUnix) {
        std::lock_guard lock(m_mutex);
        const auto key = identity.Key();
        if (ok) {
            std::string norm = PlayerNotes::NormalizeNote(text);
            if (norm.empty()) {
                m_state.byKey.erase(key);
            } else {
                auto& n = m_state.byKey[key];
                n.identity = identity;
                n.text = std::move(norm);
                if (!lastKnownName.empty()) n.lastKnownName = std::string(lastKnownName);
                if (n.createdAtUnix <= 0) n.createdAtUnix = nowUnix;
                n.updatedAtUnix = nowUnix;
            }
        }
        m_state.completedRequestId = reqId;
        m_state.lastRequestOk = ok;
        m_state.lastRequestError = ok ? std::string{} : err;
        PublishCurrentLocked();
    }

    void PlayerNotesStore::PublishNoteDeleted(uint64_t reqId, bool ok, const std::string& err,
                                              const PlayerNotes::Identity& identity) {
        std::lock_guard lock(m_mutex);
        if (ok) {
            m_state.byKey.erase(identity.Key());
        }
        m_state.completedRequestId = reqId;
        m_state.lastRequestOk = ok;
        m_state.lastRequestError = ok ? std::string{} : err;
        PublishCurrentLocked();
    }

    void PlayerNotesStore::PublishTagUpdated(uint64_t reqId, bool ok, const std::string& err,
                                             const PlayerNotes::Identity& identity,
                                             std::optional<std::vector<std::string>> newTags) {
        std::lock_guard lock(m_mutex);
        if (ok && newTags.has_value()) {
            if (newTags->empty()) {
                m_state.tagsByKey.erase(identity.Key());
            } else {
                m_state.tagsByKey[identity.Key()] = std::move(*newTags);
            }
        }
        m_state.completedTagRequestId = reqId;
        m_state.lastTagRequestOk = ok;
        m_state.lastTagRequestError = ok ? std::string{} : err;
        PublishCurrentLocked();
    }

} // namespace database::store

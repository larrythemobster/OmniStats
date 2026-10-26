#include "PlayerNotesStore.hpp"

namespace database::store {

    PlayerNotesStore::PlayerNotesStore() {
        m_snapshot.store(std::make_shared<const PlayerNotesSnapshot>());
    }

    std::shared_ptr<const PlayerNotesSnapshot> PlayerNotesStore::GetSnapshot() const {
        return m_snapshot.load(std::memory_order_acquire);
    }

    void PlayerNotesStore::PublishCurrentLocked() {
        auto snap = std::make_shared<PlayerNotesSnapshot>();
        snap->loaded = m_loaded;
        snap->loadFailed = m_loadFailed;
        snap->byKey = m_byKey;
        snap->tagsByKey = m_tagsByKey;
        snap->completedRequestId = m_completedRequestId;
        snap->lastRequestOk = m_lastRequestOk;
        snap->lastRequestError = m_lastRequestError;
        snap->completedTagRequestId = m_completedTagRequestId;
        snap->lastTagRequestOk = m_lastTagRequestOk;
        snap->lastTagRequestError = m_lastTagRequestError;
        snap->version = m_version.load(std::memory_order_relaxed);

        m_snapshot.store(std::move(snap), std::memory_order_release);
    }

    void PlayerNotesStore::PublishLoaded(bool inLoadFailed,
                                         std::unordered_map<std::string, PlayerNotes::Note> inNotes,
                                         std::unordered_map<std::string, std::vector<std::string>> inTags) {
        std::lock_guard lock(m_mutex);
        m_loaded = true;
        m_loadFailed = inLoadFailed;
        m_byKey = std::move(inNotes);
        m_tagsByKey = std::move(inTags);
        m_version.fetch_add(1, std::memory_order_relaxed);
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
                m_byKey.erase(key);
            } else {
                auto& n = m_byKey[key];
                n.identity = identity;
                n.text = std::move(norm);
                if (!lastKnownName.empty()) n.lastKnownName = std::string(lastKnownName);
                if (n.createdAtUnix <= 0) n.createdAtUnix = nowUnix;
                n.updatedAtUnix = nowUnix;
            }
        }
        m_completedRequestId = reqId;
        m_lastRequestOk = ok;
        m_lastRequestError = ok ? std::string{} : err;
        m_version.fetch_add(1, std::memory_order_relaxed);
        PublishCurrentLocked();
    }

    void PlayerNotesStore::PublishNoteDeleted(uint64_t reqId, bool ok, const std::string& err,
                                              const PlayerNotes::Identity& identity) {
        std::lock_guard lock(m_mutex);
        if (ok) {
            m_byKey.erase(identity.Key());
        }
        m_completedRequestId = reqId;
        m_lastRequestOk = ok;
        m_lastRequestError = ok ? std::string{} : err;
        m_version.fetch_add(1, std::memory_order_relaxed);
        PublishCurrentLocked();
    }

    void PlayerNotesStore::PublishTagUpdated(uint64_t reqId, bool ok, const std::string& err,
                                             const PlayerNotes::Identity& identity,
                                             std::optional<std::vector<std::string>> newTags) {
        std::lock_guard lock(m_mutex);
        if (ok && newTags.has_value()) {
            if (newTags->empty()) {
                m_tagsByKey.erase(identity.Key());
            } else {
                m_tagsByKey[identity.Key()] = std::move(*newTags);
            }
        }
        m_completedTagRequestId = reqId;
        m_lastTagRequestOk = ok;
        m_lastTagRequestError = ok ? std::string{} : err;
        m_version.fetch_add(1, std::memory_order_relaxed);
        PublishCurrentLocked();
    }

} // namespace database::store

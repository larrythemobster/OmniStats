#include "PlayerNotesStore.hpp"

namespace database::store {

    PlayerNotesStore::PlayerNotesStore() {
        m_snapshot.store(std::make_shared<const PlayerNotesSnapshot>());
    }

    std::shared_ptr<const PlayerNotesSnapshot> PlayerNotesStore::GetSnapshot() const {
        auto snapshot = m_snapshot.load(std::memory_order_acquire);
        if (snapshot && snapshot->version == version.load(std::memory_order_relaxed)) return snapshot;

        std::lock_guard lock(mutex);
        snapshot = m_snapshot.load(std::memory_order_acquire);
        if (snapshot && snapshot->version == version.load(std::memory_order_relaxed)) return snapshot;

        auto refreshed = std::make_shared<PlayerNotesSnapshot>();
        refreshed->loaded = loaded;
        refreshed->loadFailed = loadFailed;
        refreshed->byKey = byKey;
        refreshed->tagsByKey = tagsByKey;
        refreshed->completedRequestId = completedRequestId;
        refreshed->lastRequestOk = lastRequestOk;
        refreshed->lastRequestError = lastRequestError;
        refreshed->completedTagRequestId = completedTagRequestId;
        refreshed->lastTagRequestOk = lastTagRequestOk;
        refreshed->lastTagRequestError = lastTagRequestError;
        refreshed->version = version.load(std::memory_order_relaxed);
        m_snapshot.store(refreshed, std::memory_order_release);
        return refreshed;
    }

    void PlayerNotesStore::PublishCurrentLocked() {
        auto snap = std::make_shared<PlayerNotesSnapshot>();
        snap->loaded = loaded;
        snap->loadFailed = loadFailed;
        snap->byKey = byKey;
        snap->tagsByKey = tagsByKey;
        snap->completedRequestId = completedRequestId;
        snap->lastRequestOk = lastRequestOk;
        snap->lastRequestError = lastRequestError;
        snap->completedTagRequestId = completedTagRequestId;
        snap->lastTagRequestOk = lastTagRequestOk;
        snap->lastTagRequestError = lastTagRequestError;
        snap->version = version.load(std::memory_order_relaxed);

        m_snapshot.store(std::move(snap), std::memory_order_release);
    }

    void PlayerNotesStore::PublishLoaded(bool inLoadFailed,
                                         std::unordered_map<std::string, PlayerNotes::Note> inNotes,
                                         std::unordered_map<std::string, std::vector<std::string>> inTags) {
        std::lock_guard lock(mutex);
        loaded = true;
        loadFailed = inLoadFailed;
        byKey = std::move(inNotes);
        tagsByKey = std::move(inTags);
        version.fetch_add(1, std::memory_order_relaxed);
        PublishCurrentLocked();
    }

    void PlayerNotesStore::PublishNoteSaved(uint64_t reqId, bool ok, const std::string& err,
                                            const PlayerNotes::Identity& identity,
                                            std::string_view text, std::string_view lastKnownName, int64_t nowUnix) {
        std::lock_guard lock(mutex);
        const auto key = identity.Key();
        if (ok) {
            std::string norm = PlayerNotes::NormalizeNote(text);
            if (norm.empty()) {
                byKey.erase(key);
            } else {
                auto& n = byKey[key];
                n.identity = identity;
                n.text = std::move(norm);
                if (!lastKnownName.empty()) n.lastKnownName = std::string(lastKnownName);
                if (n.createdAtUnix <= 0) n.createdAtUnix = nowUnix;
                n.updatedAtUnix = nowUnix;
            }
        }
        completedRequestId = reqId;
        lastRequestOk = ok;
        lastRequestError = ok ? std::string{} : err;
        version.fetch_add(1, std::memory_order_relaxed);
        PublishCurrentLocked();
    }

    void PlayerNotesStore::PublishNoteDeleted(uint64_t reqId, bool ok, const std::string& err,
                                              const PlayerNotes::Identity& identity) {
        std::lock_guard lock(mutex);
        if (ok) {
            byKey.erase(identity.Key());
        }
        completedRequestId = reqId;
        lastRequestOk = ok;
        lastRequestError = ok ? std::string{} : err;
        version.fetch_add(1, std::memory_order_relaxed);
        PublishCurrentLocked();
    }

    void PlayerNotesStore::PublishTagUpdated(uint64_t reqId, bool ok, const std::string& err,
                                             const PlayerNotes::Identity& identity,
                                             std::optional<std::vector<std::string>> newTags) {
        std::lock_guard lock(mutex);
        if (ok && newTags.has_value()) {
            if (newTags->empty()) {
                tagsByKey.erase(identity.Key());
            } else {
                tagsByKey[identity.Key()] = std::move(*newTags);
            }
        }
        completedTagRequestId = reqId;
        lastTagRequestOk = ok;
        lastTagRequestError = ok ? std::string{} : err;
        version.fetch_add(1, std::memory_order_relaxed);
        PublishCurrentLocked();
    }

} // namespace database::store

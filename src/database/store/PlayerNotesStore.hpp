#pragma once

#include "core/accounts/PlayerNotes.hpp"
#include "core/state/StoreSnapshots.hpp"
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace database::store {

    class PlayerNotesStore {
      public:
        PlayerNotesStore();
        ~PlayerNotesStore() = default;

        PlayerNotesStore(const PlayerNotesStore&) = delete;
        PlayerNotesStore& operator=(const PlayerNotesStore&) = delete;

        std::shared_ptr<const PlayerNotesSnapshot> GetSnapshot() const;

        void PublishLoaded(bool loadFailed,
                           std::unordered_map<std::string, PlayerNotes::Note> inNotes,
                           std::unordered_map<std::string, std::vector<std::string>> inTags);

        void PublishNoteSaved(uint64_t reqId, bool ok, const std::string& err,
                              const PlayerNotes::Identity& identity,
                              std::string_view text, std::string_view lastKnownName, int64_t nowUnix);

        void PublishNoteDeleted(uint64_t reqId, bool ok, const std::string& err,
                                const PlayerNotes::Identity& identity);

        void PublishTagUpdated(uint64_t reqId, bool ok, const std::string& err,
                               const PlayerNotes::Identity& identity,
                               std::optional<std::vector<std::string>> newTags);

      private:
        void PublishCurrentLocked();

        mutable std::mutex m_mutex;
        std::atomic<uint64_t> m_version{0};
        bool m_loaded = false;
        bool m_loadFailed = false;
        std::unordered_map<std::string, PlayerNotes::Note> m_byKey;
        std::unordered_map<std::string, std::vector<std::string>> m_tagsByKey;
        uint64_t m_completedRequestId = 0;
        bool m_lastRequestOk = true;
        std::string m_lastRequestError;
        uint64_t m_completedTagRequestId = 0;
        bool m_lastTagRequestOk = true;
        std::string m_lastTagRequestError;

        mutable std::atomic<std::shared_ptr<const PlayerNotesSnapshot>> m_snapshot;
    };

} // namespace database::store

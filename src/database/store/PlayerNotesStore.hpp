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

        // Fields maintained for backward compatibility and tests
        mutable std::mutex mutex;
        std::atomic<uint64_t> version{0};
        bool loaded = false;
        bool loadFailed = false;
        std::unordered_map<std::string, PlayerNotes::Note> byKey;
        std::unordered_map<std::string, std::vector<std::string>> tagsByKey;
        uint64_t completedRequestId = 0;
        bool lastRequestOk = true;
        std::string lastRequestError;
        uint64_t completedTagRequestId = 0;
        bool lastTagRequestOk = true;
        std::string lastTagRequestError;

      private:
        void PublishCurrentLocked();

        mutable std::atomic<std::shared_ptr<const PlayerNotesSnapshot>> m_snapshot;
    };

} // namespace database::store

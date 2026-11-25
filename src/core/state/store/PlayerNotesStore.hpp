#pragma once

#include "core/accounts/PlayerNotes.hpp"
#include "core/state/StoreSnapshots.hpp"
#include "core/state/store/SnapshotStore.hpp"
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

namespace stores {

    class PlayerNotesStore : public SnapshotStore<PlayerNotesSnapshot> {
      public:
        PlayerNotesStore() = default;
        ~PlayerNotesStore() = default;

        PlayerNotesStore(const PlayerNotesStore&) = delete;
        PlayerNotesStore& operator=(const PlayerNotesStore&) = delete;

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
    };

} // namespace stores

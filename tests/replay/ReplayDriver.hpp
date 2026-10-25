#pragma once

#include <filesystem>
#include <memory>
#include <string>
#include <vector>
#include <sqlite3.h>
#include <nlohmann/json.hpp>

#include "ReplayPipelineAdapter.hpp"
#include "network/telemetry/TelemetryRecorder.hpp"

namespace omnistats::replay {

    struct ReplayResult {
        bool success = false;
        std::string errorMessage;
        std::string goldenJson;
        nlohmann::json goldenObj;
    };

    class ReplayDriver {
      public:
        struct Options {
            std::string initialPrimaryId = "steam|76561198000000001";
            std::string initialHeroName = "Hero";
            int initialMmr = 1200;
            int initialMatchesPlayed = 20;
            std::string initialPlaylist = "2v2";
            bool resetSessionOnClose = false;
            std::filesystem::path tempDbDirectory;
        };

        explicit ReplayDriver(Options options = {});
        ~ReplayDriver();

        // Replays a recorded or authored file from disk.
        ReplayResult ReplayFile(const std::filesystem::path& fixturePath);

        // Replays an in-memory sequence of TelemetryRecords.
        ReplayResult ReplayRecords(const std::vector<TelemetryRecord>& records);

        // Replays an in-memory sequence of raw wire lines.
        ReplayResult ReplayLines(const std::vector<std::string>& rawLines);

        // Waits until all pending database operations have completed.
        static bool DrainDatabase(const std::shared_ptr<DatabaseManager>& dbManager, int timeoutMs = 5000);

        // Queries SQLite and SessionState to produce the canonical, deterministic golden JSON.
        static nlohmann::json BuildCanonicalGolden(
            sqlite3* db,
            const std::shared_ptr<SessionState>& state,
            const std::vector<StepSideEffectsSummary>& emittedEffects);

      private:
        Options m_options;
    };

} // namespace omnistats::replay

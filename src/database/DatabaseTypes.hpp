#pragma once

#include <cstdint>
#include <optional>
#include <string>

struct DbMergeResult {
    int matchesImported = 0;
    int matchesSkipped = 0;
    int playersImported = 0;
    bool success = false;
    std::string error;
};

enum class TrackedAccountInsertResult {
    Inserted,
    AlreadyExists,
    Failed
};

struct ReplayUploadRecord {
    int64_t id = 0;
    std::string filePath;
    std::string fileName;
    std::string matchGuid;
    std::string state; // pending, uploading, done, duplicate, failed, skipped
    int attempts = 0;
    int64_t nextAttemptAt = 0;
    int lastHttpStatus = 0;
    std::string lastError;
    std::string ballchasingId;
    std::string ballchasingUrl;
    int64_t createdAt = 0;
    int64_t updatedAt = 0;
};

struct PlayerStatRow {
    int64_t matchId = 0;
    std::string matchGuid;
    int64_t timestampUnix = 0;
    std::string playlist;
    int playlistId = -1;
    bool win = false;
    int ourScore = 0;
    int theirScore = 0;
    int mmr = 0;
    std::optional<int> score;
    int goals = 0;
    int assists = 0;
    int saves = 0;
    int shots = 0;
    int demos = 0;
    std::optional<int> touches;
    std::optional<int> carTouches;
    std::optional<float> maxGoalSpeed;
    std::optional<float> fastestGoalTime;
    bool hasLocalStats = false;
    int boostCollected = 0;
    int demoed = 0;
    int crossbars = 0;
    std::optional<float> hardestCrossbar;
    std::optional<float> maxBallSpeed;
    int ownGoals = 0;
    std::optional<float> durationSeconds;
    std::optional<float> overtimeSeconds;
    int statsVersion = 1;
};

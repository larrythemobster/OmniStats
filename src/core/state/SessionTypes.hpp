#pragma once

#include "core/accounts/PlayerNotes.hpp"
#include "core/accounts/PlayerScout.hpp"
#include "core/game/Insights.hpp"
#include "core/game/MatchAnalytics.hpp"
#include "core/state/MatchEvent.hpp"
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <optional>
#include <string>
#include <vector>

struct SessionTotals {
    int wins = 0;
    int losses = 0;
    int goals = 0;
    int saves = 0;
    int savesTotal = 0;
    int shots = 0;
    int shotsTotal = 0;
    int demos = 0;
    int demosTotal = 0;
    int demoed = 0;
    int assists = 0;
    int assistsTotal = 0;
    int crossbars = 0;
    int crossbarsTotal = 0;

    float maxGoalSpeed = 0.0f;
    float maxGoalSpeedSelf = 0.0f;
    float maxBallSpeed = 0.0f;
    float maxBallSpeedSelf = 0.0f;
    float maxImpactForce = 0.0f;
    float maxImpactForceSelf = 0.0f;

    float fastestGoalTime = 0.0f;
    float fastestGoalTimeSelf = 0.0f;
    int ownGoals = 0;
    int ownGoalsSelf = 0;

    int boostPickedUp = 0;
    float totalMmrChange = 0.0f;
    std::map<std::string, int> mmrChangeByPlaylist;

    int teamGoals = 0;          // Total goals scored by user's team this session
    int goalParticipations = 0; // User goals + user assists, clamped per match
};

struct SessionMmrPoint {
    std::string matchGuid;
    size_t historyIndex = 0;
    int mmr = 0;
    int trackerMatchesPlayed = -1;
    bool trackerCovered = false;
    bool valueEstimated = true;
};

struct LocalPreMatchMmrSnapshot {
    std::map<std::string, int> playlistMmrs;
    std::map<std::string, int> playlistMatches;
};

struct SessionMatchSummary {
    int64_t matchId = 0;
    bool ranked = true;
    std::string mode;
    std::string matchGuid;
    int ourScore = 0;
    int theirScore = 0;
    int mmr = 0;
    bool mmrEstimated = false;
    bool win = false;
    bool pendingTrackerConfirmation = false;
    int64_t endedAtUnix = 0;
};

struct AccountMmrHistory {
    std::vector<float> mmrHistoryY;
    std::vector<float> mmrHistoryX;
    std::map<std::string, std::vector<float>> playlistHistoryY;
    std::map<std::string, std::vector<SessionMmrPoint>> playlistMatchPoints;
    std::map<std::string, int> playlistInitialMmr;
    int initialMmr = -1;
};

inline constexpr int kPreviousGamesDefaultLimit = 20;
inline constexpr int kPreviousGamesMaxLimit = 50;

enum class MmrCategory : int {
    Best = 0,
    OneVOne = 1,
    TwoVTwo = 2,
    ThreeVThree = 3,
    Casual = 4,
    Tourny = 5,
    Hoops = 6,
    Rumble = 7,
    Dropshot = 8,
    SnowDay = 9,
    Heatseeker = 10
};

std::string MmrCategoryToString(MmrCategory cat);
MmrCategory StringToMmrCategory(const std::string& str);
bool IsExtraMmrCategory(MmrCategory cat);

struct GamemodeStat {
    int wins = 0;
    int losses = 0;
    int total = 0;
};

struct CachedDbStats {
    int currentWins = 0;
    int currentLosses = 0;
    int longestWins = 0;
    int longestLosses = 0;
    std::map<std::string, GamemodeStat> gamemodes;
    int detailedStatsMatchCount = 0;
    std::string detailedStatsSinceDate;
};

inline int CalculateTrackedSessionMmrChange(const std::map<std::string, int>& changes) {
    int total = 0;
    for (const auto& [playlist, change] : changes) {
        if (playlist == "1v1" || playlist == "2v2" || playlist == "3v3" ||
            playlist == "hoops" || playlist == "rumble" || playlist == "dropshot" ||
            playlist == "snowday" || playlist == "heatseeker") {
            total += change;
        }
    }
    return total;
}

// Session totals captured when a session ends so the recap can still be shown after reset.
struct SessionRecap {
    bool valid = false;
    int64_t endedAtUnix = 0;
    SessionTotals totals;
    std::map<std::string, GamemodeStat> gamemodes;
    uint64_t sessionGeneration = 0;
    std::string mmrOwnerPrimaryId;
    std::map<std::string, int> playlistInitialMmr;
    int64_t id = 0;
    std::string account;
    int64_t startedAtUnix = 0;
    std::string source = "live";
    std::vector<std::string> matchGuids;

    int Games() const {
        return totals.wins + totals.losses;
    }
    bool HasKnownMmrChange() const {
        return !totals.mmrChangeByPlaylist.empty();
    }
    int NetMmrChange() const {
        return CalculateTrackedSessionMmrChange(totals.mmrChangeByPlaylist);
    }
};

struct MatchQuery {
    enum Sort {
        Newest = 0,
        Oldest = 1,
        BiggestWin = 2,
        BiggestLoss = 3,
        ScoreMargin = 4,
    };

    std::string account;
    std::optional<std::string> playlistLabel;
    std::optional<bool> ranked;
    std::optional<bool> win;
    int64_t fromUnix = 0;
    int64_t toUnix = 0;
    std::string arena;
    std::string withPlayer;
    std::string againstPlayer;
    std::string nameSearch;
    int offset = 0;
    int limit = 100;
    Sort sort = Sort::Newest;
};

struct MatchRow {
    int64_t matchId = 0;
    std::string matchGuid;
    int64_t timestampUnix = 0;
    std::string playlist;
    std::string gamemode;
    int playlistId = -1;
    bool ranked = true;
    bool win = false;
    int ourScore = 0;
    int theirScore = 0;
    int myMmr = 0;
    bool mmrEstimated = false;
    std::optional<int> mmrDelta;
    std::string arena;
    std::vector<std::string> teammates;
    bool hasBallchasing = false;
    std::string ballchasingUrl;
    std::optional<int> myScore;
    std::optional<int> myGoals;
    std::optional<int> myAssists;
    std::optional<int> mySaves;
    std::optional<int> myShots;
    bool mvp = false;
    bool overtime = false;
    bool endedEarly = false;
    std::optional<ScorelineSummary> scoreline;
};

struct MatchDetailPlayer {
    std::string primaryId;
    std::string name;
    std::string platform;
    int team = 0;
    bool isOpponent = false;
    bool isMe = false;
    int mmr = 0;
    bool mmrEstimated = false;
    std::string tier = "Unranked";
    bool hasStats = false;
    std::optional<int> score;
    std::optional<int> goals;
    std::optional<int> assists;
    std::optional<int> saves;
    std::optional<int> shots;
    std::optional<int> demos;
    std::optional<int> touches;
    std::optional<int> carTouches;
    std::optional<float> maxGoalSpeed;
    std::optional<float> fastestGoalTime;
    std::optional<PersonRecord> encounter;
};

struct MatchLocalStatsRow {
    int boostCollected = 0;
    int demoed = 0;
    int crossbars = 0;
    std::optional<float> hardestCrossbar;
    std::optional<float> maxBallSpeed;
    int ownGoals = 0;
};

struct MatchDetail {
    bool found = false;
    int64_t matchId = 0;
    std::string matchGuid;
    int64_t timestampUnix = 0;
    std::string arena;
    std::string playlist;
    std::string gamemode;
    int playlistId = -1;
    bool ranked = true;
    bool win = false;
    int ourScore = 0;
    int theirScore = 0;
    std::string accountPrimaryId;
    std::optional<int> mmrBefore;
    std::optional<int> mmrAfter;
    std::optional<int> mmrDelta;
    bool mmrEstimated = false;
    bool hasPlayerStats = false;
    bool hasLocalStats = false;
    std::optional<float> durationSeconds;
    std::optional<float> overtimeSeconds;
    std::optional<MatchLocalStatsRow> localStats;
    std::string ballchasingId;
    std::string ballchasingUrl;
    std::vector<MatchDetailPlayer> ourTeam;
    std::vector<MatchDetailPlayer> theirTeam;
    std::vector<MatchJournalEvent> events;
    std::optional<MatchAnalytics> analytics;
};

struct AnalyticsMatchRow {
    int64_t matchId = 0;
    int64_t timestampUnix = 0;
    std::string playlist;
    int playlistId = -1;
    bool ranked = true;
    bool win = false;
    int ourScore = 0;
    int theirScore = 0;
    int myTeam = -1;
    std::string arena;
    std::optional<float> durationSeconds;
    std::optional<float> overtimeSeconds;
    std::optional<int> score;
    std::optional<int> goals;
    std::optional<int> assists;
    std::optional<int> saves;
    std::optional<int> shots;
    std::optional<int> demos;
    std::optional<int> touches;
    std::optional<MatchLocalStatsRow> localStats;
    std::optional<MatchAnalytics> analytics;
};

struct ReplayUploadStatus {
    int uploadedToday = 0;
    int retrying = 0;
    int failed = 0;
    int total = 0;

    bool operator==(const ReplayUploadStatus&) const = default;
};

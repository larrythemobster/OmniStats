#pragma once
#include <string>
#include <array>
#include <ctime>

struct DiscordPresenceSnapshot {
    bool showPresence = false;
    bool inMatch = false;
    std::string matchGuid;
    std::string arenaName;
    std::array<int, 2> score{};
    int myTeam = -1;
    int sessionWins = 0;
    int sessionLosses = 0;
};

// Keeps Discord's elapsed timer anchored to the current match or menu stay
// across goal and score pushes.
class DiscordActivityClock {
  public:
    std::time_t StartFor(const DiscordPresenceSnapshot& snapshot, std::time_t now) {
        std::string key = snapshot.inMatch ? "match:" + snapshot.matchGuid + "|" + snapshot.arenaName : "menus";
        if (m_startedAt == 0 || key != m_key) {
            m_key = std::move(key);
            m_startedAt = now;
        }
        return m_startedAt;
    }

    void Reset() {
        m_key.clear();
        m_startedAt = 0;
    }

  private:
    std::string m_key;
    std::time_t m_startedAt = 0;
};

#include "ui/config/WidgetSettings.hpp"
#include "core/tournaments/TournamentSchedule.hpp"
#include "core/tournaments/RegionDetector.hpp"
#include "core/config/Config.hpp"
#include "core/config/AppVersion.hpp"
#include "network/omninet/OmniNet.hpp"
#include "network/api/AccountClient.hpp"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <ctime>

namespace Tournaments {
    namespace {
        bool ValidTitle(const std::string& title) {
            return !title.empty() && title.size() <= 256 && std::none_of(title.begin(), title.end(), [](unsigned char c) { return c < 32 || c == 127; });
        }
    }

    bool ParseSchedule(std::string_view body, ScheduleSnapshot& snapshot) {
        const auto json = nlohmann::json::parse(body, nullptr, false);
        if (!json.is_object() || !json.contains("group") || !json["group"].is_string() ||
            !json.contains("slots") || !json["slots"].is_array() || json["slots"].size() > 128) return false;
        const std::string group = json["group"].get<std::string>();
        if (group.empty() || RegionGroup(group) != group) return false;
        std::vector<TournamentSlot> slots;
        try {
            for (const auto& item : json["slots"]) {
                if (!item.is_object() || !item.contains("start_time") || !item["start_time"].is_number_integer() ||
                    !item.contains("title") || !item["title"].is_string() ||
                    !item.contains("standard") || !item["standard"].is_boolean() ||
                    !item.contains("team_size") || !item["team_size"].is_number_integer()) continue;
                TournamentSlot slot;
                slot.startUnix = item["start_time"].get<int64_t>();
                slot.title = item["title"].get<std::string>();
                slot.standard = item["standard"].get<bool>();
                slot.teamSize = item["team_size"].get<int>();
                if (slot.startUnix <= 0 || !ValidTitle(slot.title) || slot.teamSize < 1 || slot.teamSize > 8) continue;
                if (item.contains("second_chance") && item["second_chance"].is_object()) {
                    const auto& second = item["second_chance"];
                    if (second.contains("title") && second["title"].is_string() && second.contains("start_time") && second["start_time"].is_number_integer()) {
                        SecondChance chance{second["title"].get<std::string>(), second["start_time"].get<int64_t>()};
                        if (ValidTitle(chance.title) && chance.startUnix > slot.startUnix) slot.secondChance = std::move(chance);
                    }
                }
                slots.push_back(std::move(slot));
            }
        } catch (const nlohmann::json::exception&) {
            return false;
        }
        std::sort(slots.begin(), slots.end(), [](const auto& a, const auto& b) { return a.startUnix < b.startUnix; });
        snapshot.group = group;
        snapshot.slots = std::move(slots);
        return true;
    }

    const TournamentSlot* NextSlot(const std::vector<TournamentSlot>& slots, int64_t nowUnix, bool standardOnly) {
        for (const auto& slot : slots)
            if (slot.startUnix > nowUnix && (!standardOnly || slot.standard)) return &slot;
        return nullptr;
    }

    bool ReminderTracker::ShouldFire(const TournamentSlot& slot, int64_t nowUnix, int leadMinutes, bool standardOnly) {
        std::erase_if(m_fired, [nowUnix](int64_t time) { return time <= nowUnix; });
        if (leadMinutes <= 0 || (standardOnly && !slot.standard) || slot.startUnix <= nowUnix ||
            slot.startUnix - nowUnix > static_cast<int64_t>(leadMinutes) * 60 ||
            std::find(m_fired.begin(), m_fired.end(), slot.startUnix) != m_fired.end()) return false;
        m_fired.push_back(slot.startUnix);
        return true;
    }

    TournamentScheduleService& TournamentScheduleService::Instance() {
        static TournamentScheduleService service;
        return service;
    }
    TournamentScheduleService::~TournamentScheduleService() {
        Stop();
    }
    void TournamentScheduleService::Start() {
#ifdef OMNISTATS_TEST_ENVIRONMENT
        return;
#else
        std::lock_guard lock(m_mutex);
        if (m_thread.joinable()) return;
        m_stop = false;
        m_thread = std::jthread([this] { Worker(); });
#endif
    }
    void TournamentScheduleService::Stop() {
        m_stop = true;
        m_cv.notify_all();
        if (m_thread.joinable()) m_thread.join();
    }
    void TournamentScheduleService::RequestRefresh() {
        {
            std::lock_guard lock(m_mutex);
            m_refresh = true;
        }
        m_cv.notify_all();
    }
    std::shared_ptr<const ScheduleSnapshot> TournamentScheduleService::Snapshot() const {
        std::lock_guard lock(m_mutex);
        return m_snapshot;
    }

    bool TournamentScheduleService::Fetch(ScheduleSnapshot& snapshot) {
        if (!OmniNet::Available()) return false;
        auto& account = AccountClient::Instance();
        for (int attempt = 0; attempt < 2 && !m_stop; ++attempt) {
            const std::string token = account.AccessToken();
            const std::string device = account.DevicePublicId();
            if (token.empty() || device.empty()) return false;

            OmniNet::Request req;
            req.op = OmniNet::Op::TournamentSchedule;
            req.accessToken = token;
            req.devicePublicId = device;
            req.query["region"] = snapshot.region;
            req.shouldAbort = [this]() { return m_stop.load(); };

            const auto resp = OmniNet::Send(req);
            if (resp.updateRequired) {
                return false;
            }
            if (resp.transportError != 0) return false;
            if (resp.status == 401) {
                if (attempt == 0 && account.Refresh()) continue;
                if (attempt != 0) account.MarkSignedOut("Your OmniStats session expired. Sign in again in Settings > Accounts & services.");
                return false;
            }
            if (resp.status != 200 || !ParseSchedule(resp.body, snapshot)) return false;
            snapshot.fetchedAtUnix = static_cast<int64_t>(std::time(nullptr));
            return true;
        }
        return false;
    }

    void TournamentScheduleService::Worker() {
        int64_t nextFetch = 0;
        std::string lastRegion;
        bool lastSignedIn = false, lastEnabled = false;
        ConfigData config;
        uint64_t revision = 0;
        while (!m_stop) {
            const uint64_t currentRevision = Config::Revision();
            if (revision == 0 || revision != currentRevision) {
                config = Config::Read();
                revision = currentRevision;
            }
            const bool signedIn = AccountClient::Instance().IsSignedIn(config);
            const bool enabled = config.tournament_reminder_minutes > 0 ||
                                 WidgetSettings::Enabled(config, DashboardLayout::WidgetId::Tournaments, false) ||
                                 WidgetSettings::Enabled(config, DashboardLayout::WidgetId::Tournaments, true);
            const int64_t now = static_cast<int64_t>(std::time(nullptr));
            bool requested = false;
            {
                std::lock_guard lock(m_mutex);
                requested = m_refresh;
                m_refresh = false;
            }
            const bool changed = signedIn != lastSignedIn || enabled != lastEnabled ||
                                 config.tournament_region != lastRegion;
            if (changed || requested || now >= nextFetch) {
                const auto previous = Snapshot();
                ScheduleSnapshot value;
                if (!changed) value = *previous;
                value.status = !signedIn ? ScheduleStatus::SignedOut : !enabled ? ScheduleStatus::Disabled
                                                                                : ScheduleStatus::NoRegion;
                if (signedIn && enabled) {
                    if (config.tournament_region != "auto") {
                        value.region = config.tournament_region;
                        value.regionSource = "manual";
                    } else if (const auto detected = DetectRegion()) {
                        value.region = detected->region;
                        value.regionSource = detected->source;
                        value.detectedServer = detected->server;
                        value.detectedPingMs = detected->pingMs;
                    }
                    if (!value.region.empty()) {
                        if (value.region != previous->region) {
                            value.slots.clear();
                            value.fetchedAtUnix = 0;
                        }
                        value.status = ScheduleStatus::Loading;
                        {
                            std::lock_guard lock(m_mutex);
                            value.version = m_snapshot->version + 1;
                            m_snapshot = std::make_shared<ScheduleSnapshot>(value);
                        }
                        const bool success = Fetch(value);
                        value.status = success || value.fetchedAtUnix > 0 ? ScheduleStatus::Ready : ScheduleStatus::Unavailable;
                        nextFetch = now + (success ? 1800 : 300);
                    } else
                        nextFetch = now + 60;
                } else {
                    value.slots.clear();
                    nextFetch = now + 60;
                }
                if (!AccountClient::Instance().IsSignedIn()) {
                    value = {};
                    value.status = ScheduleStatus::SignedOut;
                }
                {
                    std::lock_guard lock(m_mutex);
                    value.version = m_snapshot->version + 1;
                    m_snapshot = std::make_shared<ScheduleSnapshot>(std::move(value));
                }
            }
            lastSignedIn = signedIn;
            lastEnabled = enabled;
            lastRegion = config.tournament_region;
            std::unique_lock lock(m_mutex);
            m_cv.wait_for(lock, std::chrono::seconds(1), [this] { return m_stop.load() || m_refresh; });
        }
    }
}

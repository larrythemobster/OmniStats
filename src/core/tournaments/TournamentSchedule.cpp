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

    ScheduledEvent NextEvent(const std::vector<TournamentSlot>& slots, int64_t nowUnix, bool standardOnly) {
        ScheduledEvent next;
        for (const auto& slot : slots) {
            if (standardOnly && !slot.standard) continue;
            if (next && slot.startUnix > next.Start()) break;
            if (slot.startUnix > nowUnix && (!next || slot.startUnix <= next.Start()))
                next = {&slot};
            if (slot.secondChance && slot.secondChance->startUnix > nowUnix &&
                (!next || slot.secondChance->startUnix < next.Start()))
                next = {&slot, &*slot.secondChance};
        }
        return next;
    }

    void ReminderTracker::Observe(int64_t nowUnix, bool connected, bool inOfficialMatch, std::string_view matchGuid) {
        if (!connected) {
            m_observedSince = 0;
        } else if (m_observedSince == 0 || nowUnix < m_lastObservation || nowUnix - m_lastObservation > 60) {
            m_observedSince = nowUnix;
        }
        m_lastObservation = nowUnix;
        if (connected && inOfficialMatch) {
            m_lastOfficialMatch = nowUnix;
            if (m_lastMatchGuid != matchGuid) m_lastMatchGuid = matchGuid;
        }
    }

    std::optional<std::string_view> ReminderTracker::ObservedMatchSince(int64_t startUnix) const {
        if (m_lastOfficialMatch < startUnix) return std::nullopt;
        return m_lastMatchGuid;
    }

    bool ReminderTracker::SecondChanceDue(const TournamentSlot& slot, int64_t nowUnix, int leadMinutes) const {
        if (!slot.secondChance || leadMinutes <= 0 || nowUnix <= slot.startUnix) return false;
        const int64_t start = slot.secondChance->startUnix;
        return start > nowUnix && start - nowUnix <= static_cast<int64_t>(leadMinutes) * 60 &&
               std::none_of(m_fired.begin(), m_fired.end(), [start](const Fired& fired) {
                   return fired.start == start && fired.secondChance;
               });
    }

    bool ReminderTracker::ShouldFire(ScheduledEvent event, int64_t nowUnix, int leadMinutes, BracketResult result) {
        constexpr int64_t joinGraceSeconds = 5 * 60;
        std::erase_if(m_fired, [nowUnix](const Fired& fired) { return fired.start <= nowUnix; });
        if (!event || leadMinutes <= 0) return false;
        const int64_t start = event.Start();
        if (start <= nowUnix || start - nowUnix > static_cast<int64_t>(leadMinutes) * 60) return false;
        if (event.secondChance && (!SecondChanceDue(*event.slot, nowUnix, leadMinutes) ||
                                   (result != BracketResult::Lost && (result != BracketResult::NoMatch || m_observedSince == 0 ||
                                                                      m_observedSince > event.slot->startUnix || nowUnix - event.slot->startUnix < joinGraceSeconds ||
                                                                      m_lastOfficialMatch >= event.slot->startUnix)))) return false;
        if (std::any_of(m_fired.begin(), m_fired.end(), [&](const Fired& fired) {
                return fired.start == start && fired.secondChance == (event.secondChance != nullptr);
            })) return false;
        m_fired.push_back({start, event.secondChance != nullptr});
        return true;
    }
    TournamentScheduleService::~TournamentScheduleService() {
        Stop();
    }
    void TournamentScheduleService::Start() {
        if (!m_enableWorker) return;
        std::lock_guard lock(m_mutex);
        if (m_thread.joinable()) return;
        m_stop = false;
        m_thread = std::jthread([this] { Worker(); });
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
        if (!m_accountClient) return false;
        OmniNet::Request req;
        req.op = OmniNet::Op::TournamentSchedule;
        req.query["region"] = snapshot.region;
        req.shouldAbort = [this]() { return m_stop.load(); };
        const auto auth = m_accountClient->SendAuthenticated(std::move(req), AccountClient::AuthenticatedRequestPolicy::Schedule);
        const auto& resp = auth.response;
        if (auth.missingCredentials || auth.refreshFailed || auth.signedOut ||
            resp.updateRequired || resp.transportError != 0 || resp.status != 200 ||
            !ParseSchedule(resp.body, snapshot)) return false;
        snapshot.fetchedAtUnix = static_cast<int64_t>(std::time(nullptr));
        return true;
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
            const bool signedIn = m_accountClient && m_accountClient->IsSignedIn(config);
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
                if (!m_accountClient || !m_accountClient->IsSignedIn()) {
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

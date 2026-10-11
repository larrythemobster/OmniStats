#include "core/AccountManager.hpp"

#include "core/Config.hpp"
#include "core/PrivacyLog.hpp"
#include "database/DatabaseManager.hpp"

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <set>

using TrackedAccounts::AccountRecord;
using TrackedAccounts::DiscoverySource;
using TrackedAccounts::Identity;
using TrackedAccounts::PlaylistRank;

namespace {
    int64_t SystemNowUnix() {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }

    int64_t ToUnix(std::chrono::system_clock::time_point point) {
        return std::chrono::duration_cast<std::chrono::seconds>(point.time_since_epoch()).count();
    }

    std::string ProviderUnavailableDetail() {
        return "No rank source is configured. Enable Tracker rank lookup or sign in to OmniStats in Settings > Integrations.";
    }
}

const char* RefreshStateKey(AccountManagerService::RefreshState state) {
    using State = AccountManagerService::RefreshState;
    switch (state) {
    case State::Idle:
        return "idle";
    case State::Queued:
        return "queued";
    case State::Fetching:
        return "fetching";
    case State::RateLimited:
        return "rate_limited";
    case State::AuthRequired:
        return "auth_required";
    case State::NotFound:
        return "not_found";
    case State::NoRankedData:
        return "no_ranked_data";
    case State::ProviderUnavailable:
        return "provider_unavailable";
    case State::Unsupported:
        return "unsupported";
    case State::Failed:
        return "failed";
    }
    return "idle";
}

AccountManagerService::AccountManagerService(std::shared_ptr<DatabaseManager> dbManager,
                                             std::shared_ptr<MMRFetcher> mmrFetcher)
    : m_db(std::move(dbManager)), m_fetcher(std::move(mmrFetcher)), m_sinkGuard(std::make_shared<SinkGuard>()) {
    m_sinkGuard->owner = this;
}

AccountManagerService::~AccountManagerService() {
    Stop();
}

void AccountManagerService::Start() {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (m_started) return;
        m_started = true;
    }
    if (m_fetcher) {
        m_fetcher->SetTrackedAccountSink([guard = m_sinkGuard](const TrackedAccountFetchEvent& event) {
            std::lock_guard<std::mutex> lock(guard->mutex);
            if (guard->owner) guard->owner->ApplyFetchEvent(event);
        });
    }
    m_worker.Enqueue([this]() {
        std::vector<AccountRecord> records;
        if (m_db) records = m_db->ListTrackedAccounts();
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            m_accounts.clear();
            m_accounts.reserve(records.size());
            for (auto& record : records) {
                m_nextSortOrder = (std::max)(m_nextSortOrder, record.sortOrder + 1);
                AccountView view;
                view.record = std::move(record);
                m_accounts.push_back(std::move(view));
            }
            m_loaded = true;
            const int64_t now = NowUnix();
            auto pending = std::move(m_pendingObservations);
            m_pendingObservations.clear();
            for (const auto& [primaryId, name] : pending)
                ObserveLocked(primaryId, name, now);
        }
        Publish();

        std::vector<std::string> unnamed;
        {
            std::lock_guard<std::mutex> lock(m_mutex);
            for (const auto& account : m_accounts) {
                if (account.record.lastKnownName.empty()) unnamed.push_back(account.record.primaryId);
            }
        }
        for (const auto& candidate : ImportCandidates())
            unnamed.push_back(candidate.identity.primaryId);
        ResolveNamesOnWorker(unnamed, false);
    },
                     true);
}

void AccountManagerService::Stop() {
    {
        std::lock_guard<std::mutex> lock(m_sinkGuard->mutex);
        m_sinkGuard->owner = nullptr;
    }
    bool started = false;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        started = m_started;
    }
    if (started && m_fetcher) m_fetcher->SetTrackedAccountSink(nullptr);
    m_worker.Stop();
}

int64_t AccountManagerService::NowUnix() const {
    return m_clock ? m_clock() : SystemNowUnix();
}

void AccountManagerService::Publish() {
    m_version.fetch_add(1, std::memory_order_acq_rel);
}

void AccountManagerService::PostDb(std::function<void(DatabaseManager&)> job) {
    if (!m_db) return;
    m_worker.Enqueue([db = m_db, job = std::move(job)]() { job(*db); }, true);
}

RankProvider AccountManagerService::CurrentProvider() {
    return MMRFetcher::ActiveRankProvider();
}

AccountManagerService::Snapshot AccountManagerService::GetSnapshot() const {
    Snapshot snapshot;
    snapshot.provider = CurrentProvider();
    std::lock_guard<std::mutex> lock(m_mutex);
    snapshot.version = m_version.load(std::memory_order_acquire);
    snapshot.loaded = m_loaded;
    snapshot.accounts = m_accounts;
    return snapshot;
}

AccountManagerService::AccountView* AccountManagerService::FindLocked(const std::string& primaryId) {
    const auto it = std::find_if(m_accounts.begin(), m_accounts.end(),
                                 [&](const AccountView& view) { return view.record.primaryId == primaryId; });
    return it == m_accounts.end() ? nullptr : &*it;
}

AccountManagerService::AddResult AccountManagerService::RegisterLocked(const Identity& identity,
                                                                       std::string_view nickname,
                                                                       std::string_view lastKnownName,
                                                                       DiscoverySource source,
                                                                       int64_t now) {
    AccountRecord record = TrackedAccounts::MakeRecord(identity, nickname, lastKnownName, source);
    record.createdAtUnix = now;
    record.sortOrder = m_nextSortOrder++;
    if (source == DiscoverySource::Auto) record.lastSeenAtUnix = now;

    AccountView view;
    view.record = record;
    m_accounts.push_back(std::move(view));
    PostDb([record](DatabaseManager& db) {
        if (db.RegisterTrackedAccount(record) == TrackedAccountInsertResult::Failed) {
            std::cout << "[Accounts] Failed to save tracked account "
                      << PrivacyLog::Sensitive(record.primaryId, "player ID") << ".\n";
        }
    });

    AddResult result;
    result.status = AddResult::Status::Added;
    result.primaryId = identity.primaryId;
    return result;
}

AccountManagerService::AddResult AccountManagerService::AddManual(TrackedAccounts::Platform platform,
                                                                  std::string_view accountInput,
                                                                  std::string_view nickname) {
    const auto parsed = TrackedAccounts::ParseManualInput(platform, accountInput);
    if (!parsed.identity) {
        AddResult result;
        result.message = parsed.message;
        return result;
    }

    AddResult result;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_loaded) {
            result.message = "Accounts are still loading. Try again in a moment.";
            return result;
        }
        if (const AccountView* existing = FindLocked(parsed.identity->primaryId)) {
            result.status = AddResult::Status::AlreadyTracked;
            result.primaryId = existing->record.primaryId;
            result.message = "This account is already tracked as \"" + TrackedAccounts::DisplayLabel(existing->record) + "\".";
            return result;
        }
        const int64_t now = NowUnix();
        result = RegisterLocked(*parsed.identity, nickname, {}, DiscoverySource::Manual, now);
        PostResolveNames({result.primaryId}, true);
    }
    Publish();
    return result;
}

bool AccountManagerService::Rename(const std::string& primaryId, std::string_view nickname) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        AccountView* account = FindLocked(primaryId);
        if (!account) return false;
        std::string normalized = TrackedAccounts::NormalizeLabel(nickname, TrackedAccounts::kMaxNicknameLength);
        if (normalized == account->record.nickname) return true;
        account->record.nickname = normalized;
        PostDb([primaryId, normalized](DatabaseManager& db) { (void)db.UpdateTrackedAccountNickname(primaryId, normalized); });
    }
    Publish();
    return true;
}

bool AccountManagerService::Remove(const std::string& primaryId) {
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const auto it = std::find_if(m_accounts.begin(), m_accounts.end(),
                                     [&](const AccountView& view) { return view.record.primaryId == primaryId; });
        if (it == m_accounts.end()) return false;
        m_accounts.erase(it);
        m_pending.erase(primaryId);
        m_lastManualRefresh.erase(primaryId);
        m_seenThisRun.erase(primaryId);
        if (m_fetcher) m_fetcher->CancelTrackedAccount(primaryId);
        PostDb([primaryId](DatabaseManager& db) { (void)db.DeleteTrackedAccount(primaryId); });
    }
    Publish();
    return true;
}

AccountManagerService::RefreshRequest AccountManagerService::RefreshLocked(AccountView& account, int64_t now, bool manual) {
    const std::string& primaryId = account.record.primaryId;
    if (m_pending.count(primaryId)) return RefreshRequest::AlreadyQueued;
    if (manual) {
        const auto last = m_lastManualRefresh.find(primaryId);
        if (last != m_lastManualRefresh.end() && now - last->second < kManualRefreshCooldownSeconds) {
            return RefreshRequest::CoolingDown;
        }
    }
    if (!m_fetcher || CurrentProvider() == RankProvider::Unavailable) {
        account.refreshState = RefreshState::ProviderUnavailable;
        account.refreshDetail = ProviderUnavailableDetail();
        return RefreshRequest::ProviderUnavailable;
    }

    switch (m_fetcher->EnqueueTrackedAccount(primaryId, account.record.lastKnownName)) {
    case TrackedEnqueueResult::Queued:
    case TrackedEnqueueResult::AlreadyQueued:
        m_pending.insert(primaryId);
        account.refreshState = RefreshState::Queued;
        account.refreshDetail.clear();
        account.retryAtUnix = 0;
        if (manual) m_lastManualRefresh[primaryId] = now;
        return RefreshRequest::Queued;
    case TrackedEnqueueResult::ProviderUnavailable:
        account.refreshState = RefreshState::ProviderUnavailable;
        account.refreshDetail = ProviderUnavailableDetail();
        return RefreshRequest::ProviderUnavailable;
    case TrackedEnqueueResult::InvalidIdentity:
        account.refreshState = RefreshState::Unsupported;
        account.refreshDetail = "This identity cannot be looked up.";
        return RefreshRequest::Unsupported;
    }
    return RefreshRequest::Unsupported;
}

AccountManagerService::RefreshRequest AccountManagerService::Refresh(const std::string& primaryId) {
    RefreshRequest result = RefreshRequest::UnknownAccount;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        AccountView* account = FindLocked(primaryId);
        if (!account) return RefreshRequest::UnknownAccount;
        result = RefreshLocked(*account, NowUnix(), true);
    }
    Publish();
    return result;
}

size_t AccountManagerService::RefreshAll() {
    size_t queued = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        const int64_t now = NowUnix();
        for (auto& account : m_accounts) {
            if (RefreshLocked(account, now, true) == RefreshRequest::Queued) ++queued;
        }
    }
    Publish();
    return queued;
}

size_t AccountManagerService::RefreshStale() {
    size_t queued = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_loaded) return 0;
        const int64_t now = NowUnix();
        for (auto& account : m_accounts) {
            const auto& record = account.record;
            const bool stale = record.lastSuccessfulRefreshAtUnix <= 0 ||
                               now - record.lastSuccessfulRefreshAtUnix >= kStaleAfterSeconds;
            const bool retryDue = record.lastRefreshAttemptAtUnix <= 0 ||
                                  now - record.lastRefreshAttemptAtUnix >= kAutoRefreshRetrySeconds;
            if (!stale || !retryDue) continue;
            if (RefreshLocked(account, now, false) == RefreshRequest::Queued) ++queued;
        }
    }
    Publish();
    return queued;
}

std::vector<AccountManagerService::ImportCandidate> AccountManagerService::ImportCandidates() const {
    const ConfigData config = Config::Read();
    std::vector<std::string> ids = config.known_primary_ids;
    ids.push_back(config.last_primary_id);

    std::vector<ImportCandidate> candidates;
    std::set<std::string> seen;
    std::lock_guard<std::mutex> lock(m_mutex);
    for (const auto& id : ids) {
        const auto identity = TrackedAccounts::FromPrimaryId(id);
        if (!identity || !seen.insert(identity->primaryId).second) continue;
        const bool tracked = std::any_of(m_accounts.begin(), m_accounts.end(), [&](const AccountView& view) {
            return view.record.primaryId == identity->primaryId;
        });
        if (tracked) continue;
        ImportCandidate candidate{*identity, {}};
        if (const auto name = m_resolvedNames.find(identity->primaryId); name != m_resolvedNames.end()) {
            candidate.lastKnownName = name->second;
        }
        candidates.push_back(std::move(candidate));
    }
    return candidates;
}

size_t AccountManagerService::ImportKnown() {
    const auto candidates = ImportCandidates();
    size_t imported = 0;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_loaded) return 0;
        const int64_t now = NowUnix();
        std::vector<AccountRecord> records;
        for (const auto& candidate : candidates) {
            if (FindLocked(candidate.identity.primaryId)) continue;
            AccountRecord record = TrackedAccounts::MakeRecord(candidate.identity, {}, candidate.lastKnownName, DiscoverySource::Import);
            record.createdAtUnix = now;
            record.sortOrder = m_nextSortOrder++;
            AccountView view;
            view.record = record;
            m_accounts.push_back(std::move(view));
            records.push_back(std::move(record));
            ++imported;
        }
        if (!records.empty()) {
            PostDb([records](DatabaseManager& db) { (void)db.ImportTrackedAccounts(records); });
            std::vector<std::string> ids;
            ids.reserve(records.size());
            for (const auto& record : records)
                ids.push_back(record.primaryId);
            PostResolveNames(std::move(ids), true);
        }
    }
    if (imported > 0) Publish();
    return imported;
}

void AccountManagerService::ObserveLocalAccount(const std::string& primaryId, const std::string& displayName) {
    if (!TrackedAccounts::FromPrimaryId(primaryId)) return;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        if (!m_loaded) {
            m_pendingObservations.emplace_back(primaryId, displayName);
            return;
        }
        ObserveLocked(primaryId, displayName, NowUnix());
    }
    Publish();
}

void AccountManagerService::ObserveLocked(const std::string& primaryId, const std::string& displayName, int64_t now) {
    const auto identity = TrackedAccounts::FromPrimaryId(primaryId);
    if (!identity) return;
    const std::string name = TrackedAccounts::NormalizeLabel(displayName, TrackedAccounts::kMaxDisplayNameLength);

    AccountView* account = FindLocked(identity->primaryId);
    if (!account) {
        RegisterLocked(*identity, {}, name, DiscoverySource::Auto, now);
        m_seenThisRun.insert(identity->primaryId);
        std::cout << "[Accounts] Registered local account "
                  << PrivacyLog::Sensitive(identity->primaryId, "player ID") << ".\n";
        if (AccountView* added = FindLocked(identity->primaryId)) RefreshLocked(*added, now, false);
        return;
    }

    const bool nameChanged = !name.empty() && name != account->record.lastKnownName;
    const bool firstSighting = m_seenThisRun.insert(identity->primaryId).second;
    if (!nameChanged && !firstSighting) return;
    if (nameChanged) account->record.lastKnownName = name;
    account->record.lastSeenAtUnix = now;
    PostDb([id = identity->primaryId, storedName = nameChanged ? name : std::string(), now](DatabaseManager& db) {
        (void)db.UpdateTrackedAccountLastSeen(id, storedName, now);
    });
}

void AccountManagerService::PostResolveNames(std::vector<std::string> primaryIds, bool refreshAfter) {
    m_worker.Enqueue([this, ids = std::move(primaryIds), refreshAfter]() { ResolveNamesOnWorker(ids, refreshAfter); }, true);
}

void AccountManagerService::ResolveNamesOnWorker(const std::vector<std::string>& primaryIds, bool refreshAfter) {
    std::vector<std::pair<std::string, std::string>> found;
    if (m_db) {
        for (const auto& id : primaryIds) {
            const auto name = m_db->FindLatestPlayerName(id);
            if (!name) continue;
            std::string normalized = TrackedAccounts::NormalizeLabel(*name, TrackedAccounts::kMaxDisplayNameLength);
            if (!normalized.empty()) found.emplace_back(id, std::move(normalized));
        }
    }

    std::vector<std::pair<std::string, std::string>> persist;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        for (const auto& [id, name] : found) {
            m_resolvedNames[id] = name;
            AccountView* account = FindLocked(id);
            if (!account || !account->record.lastKnownName.empty()) continue;
            account->record.lastKnownName = name;
            persist.emplace_back(id, name);
        }
        if (refreshAfter) {
            const int64_t now = NowUnix();
            for (const auto& id : primaryIds) {
                if (AccountView* account = FindLocked(id)) RefreshLocked(*account, now, false);
            }
        }
    }
    for (const auto& [id, name] : persist)
        (void)m_db->UpdateTrackedAccountLastKnownName(id, name);
    if (!found.empty() || refreshAfter) Publish();
}

void AccountManagerService::ApplyFetchEvent(const TrackedAccountFetchEvent& event) {
    using Status = TrackedAccountFetchStatus;
    {
        std::lock_guard<std::mutex> lock(m_mutex);
        AccountView* account = FindLocked(event.primaryId);
        if (!account) return;
        const int64_t now = NowUnix();
        const bool retryNotice = event.retryAt != std::chrono::system_clock::time_point{};

        switch (event.status) {
        case Status::Fetching:
            account->refreshState = RefreshState::Fetching;
            account->refreshDetail.clear();
            account->retryAtUnix = 0;
            account->record.lastRefreshAttemptAtUnix = now;
            PostDb([id = event.primaryId, now](DatabaseManager& db) { (void)db.UpdateTrackedAccountRefreshAttempt(id, now); });
            break;
        case Status::Retrying:
        case Status::RateLimited:
            account->refreshState = event.status == Status::RateLimited ? RefreshState::RateLimited : RefreshState::Queued;
            account->refreshDetail = event.detail;
            account->retryAtUnix = retryNotice ? ToUnix(event.retryAt) : 0;
            if (!retryNotice) m_pending.erase(event.primaryId);
            break;
        case Status::Success: {
            std::vector<PlaylistRank> ranks;
            const std::string source = TrackedAccounts::RankSourceLabel(event.profile.rankVerificationSource);
            for (const auto& [key, mmr] : event.profile.playlistMMRs) {
                if (mmr <= 0 || !TrackedAccounts::IsSupportedPlaylistKey(key)) continue;
                PlaylistRank rank;
                rank.playlistKey = key;
                rank.mmr = mmr;
                if (const auto tier = event.profile.playlistTiers.find(key); tier != event.profile.playlistTiers.end()) {
                    rank.rankTier = tier->second;
                }
                if (const auto matches = event.profile.playlistMatches.find(key);
                    matches != event.profile.playlistMatches.end() && matches->second >= 0) {
                    rank.matchesPlayed = matches->second;
                }
                rank.source = source;
                rank.fetchedAtUnix = now;
                ranks.push_back(std::move(rank));
            }
            for (const auto& rank : ranks)
                account->record.ranks[rank.playlistKey] = rank;
            if (account->record.lastKnownName.empty()) {
                const std::string name = TrackedAccounts::NormalizeLabel(event.profile.displayName, TrackedAccounts::kMaxDisplayNameLength);
                if (!name.empty()) {
                    account->record.lastKnownName = name;
                    PostDb([id = event.primaryId, name](DatabaseManager& db) { (void)db.UpdateTrackedAccountLastKnownName(id, name); });
                }
            }
            account->record.lastSuccessfulRefreshAtUnix = now;
            account->record.lastRefreshAttemptAtUnix = now;
            account->refreshState = RefreshState::Idle;
            account->refreshDetail.clear();
            account->retryAtUnix = 0;
            m_pending.erase(event.primaryId);
            PostDb([id = event.primaryId, ranks = std::move(ranks), now](DatabaseManager& db) {
                (void)db.SaveTrackedAccountRanks(id, ranks, now);
            });
            break;
        }
        case Status::NotFound:
        case Status::NoRankedData:
        case Status::AuthRequired:
        case Status::ProviderUnavailable:
        case Status::Unsupported:
        case Status::Failed:
            account->refreshState = event.status == Status::NotFound              ? RefreshState::NotFound
                                    : event.status == Status::NoRankedData        ? RefreshState::NoRankedData
                                    : event.status == Status::AuthRequired        ? RefreshState::AuthRequired
                                    : event.status == Status::ProviderUnavailable ? RefreshState::ProviderUnavailable
                                    : event.status == Status::Unsupported         ? RefreshState::Unsupported
                                                                                  : RefreshState::Failed;
            account->refreshDetail = event.detail;
            account->retryAtUnix = 0;
            account->record.lastRefreshAttemptAtUnix = now;
            m_pending.erase(event.primaryId);
            PostDb([id = event.primaryId, now](DatabaseManager& db) { (void)db.UpdateTrackedAccountRefreshAttempt(id, now); });
            break;
        }
    }
    Publish();
}

#ifdef OMNISTATS_TEST_ENVIRONMENT
void AccountManagerService::SetClockForTests(std::function<int64_t()> nowUnix) {
    std::lock_guard<std::mutex> lock(m_mutex);
    m_clock = std::move(nowUnix);
}

void AccountManagerService::FlushForTests() {
    auto done = std::make_shared<std::promise<void>>();
    auto future = done->get_future();
    if (!m_worker.Enqueue([done]() { done->set_value(); }, true)) return;
    future.wait();
}
#endif

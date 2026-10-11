#include "ui/rml/views/AccountsView.hpp"

#include <RmlUi/Core/Context.h>

#include <algorithm>
#include <chrono>
#include <ctime>

#include "ui/Formatting.hpp"
#include "ui/rml/RmlUiController.hpp"

namespace {
    std::string FormatDateTime(int64_t unixSeconds) {
        if (unixSeconds <= 0) return "Never";
        const std::time_t time = static_cast<std::time_t>(unixSeconds);
        std::tm local{};
        if (localtime_s(&local, &time) != 0) return "Never";
        char buffer[64]{};
        std::strftime(buffer, sizeof(buffer), "%a %d %b %Y, %H:%M", &local);
        return buffer;
    }

    std::string FormatRelativeTime(int64_t unixSeconds, int64_t nowUnix) {
        if (unixSeconds <= 0) return "Never";
        int64_t delta = nowUnix - unixSeconds;
        if (delta < 0) delta = 0;
        if (delta < 60) return "just now";
        if (delta < 3600) return std::to_string(delta / 60) + "m ago";
        if (delta < 86400) return std::to_string(delta / 3600) + "h ago";
        return std::to_string(delta / 86400) + "d ago";
    }

    std::string PlatformCssClass(TrackedAccounts::Platform platform) {
        switch (platform) {
        case TrackedAccounts::Platform::Epic:
            return "badge platform-epic";
        case TrackedAccounts::Platform::Steam:
            return "badge platform-steam";
        case TrackedAccounts::Platform::PlayStation:
            return "badge platform-psn";
        case TrackedAccounts::Platform::Xbox:
            return "badge platform-xbox";
        case TrackedAccounts::Platform::Switch:
            return "badge platform-switch";
        }
        return "badge";
    }

    std::string DisplayTier(const std::string& playlistKey, const TrackedAccounts::PlaylistRank& rank) {
        const std::string& tier = rank.rankTier;
        if ((tier.empty() || tier.rfind("Unranked", 0) == 0) && rank.mmr.value_or(0) > 0) {
            return MMRFetcher::GetRankTierForPlaylistMmr(playlistKey, *rank.mmr);
        }
        return tier;
    }

    struct DerivedStatus {
        std::string text;
        std::string cssClass;
        bool staleOrFailed = false;
        bool isUpdating = false;
    };

    DerivedStatus DeriveRowStatus(AccountManagerService::RefreshState state,
                                  const TrackedAccounts::AccountRecord& record,
                                  const std::string& detail,
                                  int64_t retryAtUnix,
                                  int64_t now) {
        switch (state) {
        case AccountManagerService::RefreshState::Queued:
            return {"Queued", "accounts-status-badge status-queued", false, true};
        case AccountManagerService::RefreshState::Fetching:
            return {"Updating\xE2\x80\xA6", "accounts-status-badge status-fetching", false, true};
        case AccountManagerService::RefreshState::RateLimited: {
            if (retryAtUnix > now) {
                int64_t sec = retryAtUnix - now;
                return {"Rate limited (" + std::to_string(sec) + "s)", "accounts-status-badge status-ratelimited", true, false};
            }
            return {"Rate limited", "accounts-status-badge status-ratelimited", true, false};
        }
        case AccountManagerService::RefreshState::AuthRequired:
            return {"Sign-in required", "accounts-status-badge status-warning", true, false};
        case AccountManagerService::RefreshState::NotFound:
            return {"Not found", "accounts-status-badge status-warning", true, false};
        case AccountManagerService::RefreshState::NoRankedData:
            return {"No ranked data", "accounts-status-badge status-warning", false, false};
        case AccountManagerService::RefreshState::ProviderUnavailable:
            return {"No rank source", "accounts-status-badge status-warning", true, false};
        case AccountManagerService::RefreshState::Unsupported:
            return {detail.empty() ? "Unsupported" : detail, "accounts-status-badge status-warning", true, false};
        case AccountManagerService::RefreshState::Failed:
            return {"Refresh failed", "accounts-status-badge status-failed", true, false};
        case AccountManagerService::RefreshState::Idle:
            if (record.lastSuccessfulRefreshAtUnix <= 0) {
                return {"Never fetched", "accounts-status-badge status-idle", false, false};
            }
            if (now - record.lastSuccessfulRefreshAtUnix > AccountManagerService::kStaleAfterSeconds) {
                return {"Stale", "accounts-status-badge status-stale", true, false};
            }
            return {"Updated", "accounts-status-badge status-updated", false, false};
        }
        return {"Idle", "accounts-status-badge status-idle", false, false};
    }
}

bool AccountsView::Create(Rml::Context* context) {
    Reset();
    if (!context) return false;

    Rml::DataModelConstructor constructor = context->CreateDataModel("accounts");
    if (!constructor) return false;

    if (auto col = constructor.RegisterStruct<AccountsPlaylistColumn>()) {
        col.RegisterMember("key", &AccountsPlaylistColumn::key);
        col.RegisterMember("label", &AccountsPlaylistColumn::label);
        col.RegisterMember("short_label", &AccountsPlaylistColumn::short_label);
    }
    constructor.RegisterArray<std::vector<AccountsPlaylistColumn>>();

    if (auto cell = constructor.RegisterStruct<AccountsPlaylistRankCell>()) {
        cell.RegisterMember("playlist_key", &AccountsPlaylistRankCell::playlist_key);
        cell.RegisterMember("mmr", &AccountsPlaylistRankCell::mmr);
        cell.RegisterMember("tier", &AccountsPlaylistRankCell::tier);
        cell.RegisterMember("tier_color", &AccountsPlaylistRankCell::tier_color);
        cell.RegisterMember("is_stale", &AccountsPlaylistRankCell::is_stale);
        cell.RegisterMember("has_mmr", &AccountsPlaylistRankCell::has_mmr);
    }
    constructor.RegisterArray<std::vector<AccountsPlaylistRankCell>>();

    if (auto row = constructor.RegisterStruct<AccountRow>()) {
        row.RegisterMember("primary_id", &AccountRow::primary_id);
        row.RegisterMember("display_label", &AccountRow::display_label);
        row.RegisterMember("sub_label", &AccountRow::sub_label);
        row.RegisterMember("platform", &AccountRow::platform);
        row.RegisterMember("platform_class", &AccountRow::platform_class);
        row.RegisterMember("is_active", &AccountRow::is_active);
        row.RegisterMember("ranks", &AccountRow::ranks);
        row.RegisterMember("updated_relative", &AccountRow::updated_relative);
        row.RegisterMember("status_text", &AccountRow::status_text);
        row.RegisterMember("status_class", &AccountRow::status_class);
        row.RegisterMember("is_stale_or_failed", &AccountRow::is_stale_or_failed);
        row.RegisterMember("is_updating", &AccountRow::is_updating);
    }
    constructor.RegisterArray<std::vector<AccountRow>>();

    if (auto drank = constructor.RegisterStruct<AccountDetailRankRow>()) {
        drank.RegisterMember("playlist_key", &AccountDetailRankRow::playlist_key);
        drank.RegisterMember("playlist_label", &AccountDetailRankRow::playlist_label);
        drank.RegisterMember("mmr", &AccountDetailRankRow::mmr);
        drank.RegisterMember("tier", &AccountDetailRankRow::tier);
        drank.RegisterMember("tier_color", &AccountDetailRankRow::tier_color);
        drank.RegisterMember("matches_played", &AccountDetailRankRow::matches_played);
        drank.RegisterMember("source", &AccountDetailRankRow::source);
    }
    constructor.RegisterArray<std::vector<AccountDetailRankRow>>();

    if (auto cand = constructor.RegisterStruct<AccountImportCandidateRow>()) {
        cand.RegisterMember("platform", &AccountImportCandidateRow::platform);
        cand.RegisterMember("name", &AccountImportCandidateRow::name);
        cand.RegisterMember("masked_id", &AccountImportCandidateRow::masked_id);
        cand.RegisterMember("primary_id", &AccountImportCandidateRow::primary_id);
    }
    constructor.RegisterArray<std::vector<AccountImportCandidateRow>>();

    constructor.Bind("column_set", &m_columnSet);
    constructor.Bind("sort_col", &m_sortCol);
    constructor.Bind("sort_dir_arrow", &m_sortDirArrow);
    constructor.Bind("playlist_columns", &m_playlistColumns);
    constructor.Bind("rows", &m_rows);
    constructor.Bind("loaded", &m_loaded);
    constructor.Bind("has_service", &m_hasService);
    constructor.Bind("provider_unavailable", &m_providerUnavailable);
    constructor.Bind("provider_banner_text", &m_providerBannerText);
    constructor.Bind("has_import_candidates", &m_hasImportCandidates);
    constructor.Bind("import_candidates_count", &m_importCandidatesCount);
    constructor.Bind("import_button_label", &m_importButtonLabel);
    constructor.Bind("row_count", &m_rowCount);
    constructor.Bind("empty_message", &m_emptyMessage);
    constructor.Bind("subtitle", &m_subtitle);

    constructor.Bind("detail_open", &m_detailOpen);
    constructor.Bind("detail_primary_id", &m_detailPrimaryId);
    constructor.Bind("detail_nickname", &m_detailNickname);
    constructor.Bind("detail_platform", &m_detailPlatform);
    constructor.Bind("detail_platform_class", &m_detailPlatformClass);
    constructor.Bind("detail_last_known_name", &m_detailLastKnownName);
    constructor.Bind("detail_canonical_id", &m_detailCanonicalId);
    constructor.Bind("detail_ranks", &m_detailRanks);
    constructor.Bind("detail_last_refresh", &m_detailLastRefresh);
    constructor.Bind("detail_rank_source", &m_detailRankSource);
    constructor.Bind("detail_refresh_state", &m_detailRefreshState);
    constructor.Bind("detail_discovery_source", &m_detailDiscoverySource);
    constructor.Bind("detail_renaming", &m_detailRenaming);
    constructor.Bind("detail_rename_value", &m_detailRenameValue);
    constructor.Bind("detail_confirm_remove", &m_detailConfirmRemove);

    constructor.Bind("add_open", &m_addOpen);
    constructor.Bind("add_platform", &m_addPlatform);
    constructor.Bind("add_id_value", &m_addAccountId);
    constructor.Bind("add_nickname_value", &m_addNickname);
    constructor.Bind("add_id_help", &m_addIdHelp);
    constructor.Bind("add_error", &m_addError);
    constructor.Bind("add_already_tracked", &m_addAlreadyTracked);
    constructor.Bind("add_existing_id", &m_addExistingId);

    constructor.Bind("import_open", &m_importOpen);
    constructor.Bind("import_candidates", &m_importCandidates);
    constructor.Bind("import_submit_label", &m_importSubmitLabel);

    m_handle = constructor.GetModelHandle();
    m_bound = true;
    RebuildPlaylistColumns();
    return true;
}

void AccountsView::Reset() {
    m_handle = {};
    m_bound = false;
    m_lastSnapshot = {};
    m_activePrimaryId.clear();
    m_lastNowUnix = 0;
    m_columnSet = "competitive";
    m_sortCol = "order";
    m_sortAsc = true;
    m_sortDirArrow.clear();
    m_playlistColumns.clear();
    m_rows.clear();
    m_loaded = false;
    m_hasService = true;
    m_providerUnavailable = false;
    m_providerBannerText.clear();
    m_hasImportCandidates = false;
    m_importCandidatesCount = 0;
    m_importButtonLabel = "Import Known";
    m_rowCount = 0;
    m_emptyMessage = "No accounts tracked yet. Add an account to follow ranks and compare MMR across playlists.";
    m_subtitle = "Tracked Rocket League accounts";

    m_detailOpen = false;
    m_detailPrimaryId.clear();
    m_detailNickname.clear();
    m_detailPlatform.clear();
    m_detailPlatformClass.clear();
    m_detailLastKnownName.clear();
    m_detailCanonicalId.clear();
    m_detailRanks.clear();
    m_detailLastRefresh.clear();
    m_detailRankSource.clear();
    m_detailRefreshState.clear();
    m_detailDiscoverySource.clear();
    m_detailRenaming = false;
    m_detailRenameValue.clear();
    m_detailConfirmRemove = false;

    m_addOpen = false;
    m_addPlatform = "epic";
    m_addPlatformEnum = TrackedAccounts::Platform::Epic;
    m_addAccountId.clear();
    m_addNickname.clear();
    UpdateAddHelp();
    m_addError.clear();
    m_addAlreadyTracked = false;
    m_addExistingId.clear();

    m_importOpen = false;
    m_importCandidates.clear();
    m_importSubmitLabel = "Import 0 accounts";
}

void AccountsView::Dirty(const char* name) {
    if (m_bound) m_handle.DirtyVariable(name);
}

void AccountsView::SetUnavailable(bool unavailable) {
    m_hasService = !unavailable;
    m_loaded = true;
    if (unavailable) {
        m_emptyMessage = "Account tracking is unavailable.";
        m_rows.clear();
        m_rowCount = 0;
    }
    Dirty("has_service");
    Dirty("loaded");
    Dirty("empty_message");
    Dirty("rows");
    Dirty("row_count");
}

void AccountsView::UpdateSnapshot(const AccountManagerService::Snapshot& snapshot,
                                  const std::string& activePrimaryId,
                                  size_t importCandidatesCount,
                                  int64_t nowUnix) {
    m_lastSnapshot = snapshot;
    m_activePrimaryId = activePrimaryId;
    m_lastNowUnix = nowUnix > 0 ? nowUnix : static_cast<int64_t>(std::time(nullptr));
    m_hasService = true;
    m_loaded = snapshot.loaded;
    m_providerUnavailable = (snapshot.provider == RankProvider::Unavailable);

    m_importCandidatesCount = static_cast<int>(importCandidatesCount);
    m_hasImportCandidates = (m_importCandidatesCount > 0);
    m_importButtonLabel = m_hasImportCandidates ? "Import (" + std::to_string(m_importCandidatesCount) + ")" : "Import Known";

    RebuildRows();

    if (m_detailOpen) {
        RebuildDetail();
    }

    Dirty("loaded");
    Dirty("has_service");
    Dirty("provider_unavailable");
    Dirty("has_import_candidates");
    Dirty("import_candidates_count");
    Dirty("import_button_label");
    Dirty("rows");
    Dirty("row_count");
    Dirty("subtitle");
}

void AccountsView::SetColumnSet(const std::string& columnSet) {
    if (m_columnSet == columnSet) return;
    m_columnSet = columnSet;
    RebuildPlaylistColumns();
    RebuildRows();
    Dirty("column_set");
    Dirty("playlist_columns");
    Dirty("rows");
}

void AccountsView::SetSort(const std::string& col) {
    if (m_sortCol == col) {
        m_sortAsc = !m_sortAsc;
    } else {
        m_sortCol = col;
        if (col == "nickname") {
            m_sortAsc = true;
        } else if (col == "order") {
            m_sortAsc = true;
        } else if (col == "updated") {
            m_sortAsc = false;
        } else {
            m_sortAsc = false;
        }
    }
    m_sortDirArrow = m_sortAsc ? "\xE2\x96\xB2" : "\xE2\x96\xBC";
    RebuildRows();
    Dirty("sort_col");
    Dirty("sort_dir_arrow");
    Dirty("rows");
}

void AccountsView::RebuildPlaylistColumns() {
    m_playlistColumns.clear();
    const bool competitive = (m_columnSet == "competitive");
    for (const auto& col : TrackedAccounts::SupportedPlaylists()) {
        if (competitive == col.standard) {
            AccountsPlaylistColumn c;
            c.key = col.key;
            c.label = col.label;
            c.short_label = col.shortLabel;
            m_playlistColumns.push_back(std::move(c));
        }
    }
}

void AccountsView::RebuildRows() {
    const int64_t now = m_lastNowUnix > 0 ? m_lastNowUnix : static_cast<int64_t>(std::time(nullptr));

    std::vector<const AccountManagerService::AccountView*> views;
    views.reserve(m_lastSnapshot.accounts.size());
    for (const auto& acc : m_lastSnapshot.accounts) {
        views.push_back(&acc);
    }

    if (m_sortCol == "nickname") {
        std::stable_sort(views.begin(), views.end(), [this](const auto* a, const auto* b) {
            const std::string na = TrackedAccounts::DisplayLabel(a->record);
            const std::string nb = TrackedAccounts::DisplayLabel(b->record);
            return m_sortAsc ? na < nb : na > nb;
        });
    } else if (m_sortCol == "updated") {
        std::stable_sort(views.begin(), views.end(), [this](const auto* a, const auto* b) {
            const int64_t ta = a->record.lastSuccessfulRefreshAtUnix;
            const int64_t tb = b->record.lastSuccessfulRefreshAtUnix;
            return m_sortAsc ? ta < tb : ta > tb;
        });
    } else if (m_sortCol == "order") {
        std::stable_sort(views.begin(), views.end(), [this](const auto* a, const auto* b) {
            return m_sortAsc ? a->record.sortOrder < b->record.sortOrder : a->record.sortOrder > b->record.sortOrder;
        });
    } else {
        auto getMmr = [this](const AccountManagerService::AccountView* acc) -> std::optional<int> {
            auto it = acc->record.ranks.find(m_sortCol);
            if (it != acc->record.ranks.end()) return it->second.mmr;
            return std::nullopt;
        };

        std::stable_sort(views.begin(), views.end(), [this, &getMmr](const auto* a, const auto* b) {
            const auto ma = getMmr(a);
            const auto mb = getMmr(b);
            if (ma.has_value() && !mb.has_value()) return true;
            if (!ma.has_value() && mb.has_value()) return false;
            if (!ma.has_value() && !mb.has_value()) return false;
            return m_sortAsc ? *ma < *mb : *ma > *mb;
        });
    }

    m_rows.clear();
    m_rows.reserve(views.size());

    for (const auto* v : views) {
        AccountRow row;
        row.primary_id = v->record.primaryId;
        row.display_label = TrackedAccounts::DisplayLabel(v->record);

        std::string subLabel = TrackedAccounts::PlatformLabel(v->record.platform);
        if (!v->record.lastKnownName.empty() && v->record.lastKnownName != v->record.nickname) {
            subLabel += " \xE2\x80\xA2 " + v->record.lastKnownName;
        }
        row.sub_label = std::move(subLabel);
        row.platform = TrackedAccounts::PlatformLabel(v->record.platform);
        row.platform_class = PlatformCssClass(v->record.platform);
        row.is_active = (!m_activePrimaryId.empty() && v->record.primaryId == m_activePrimaryId);

        DerivedStatus status = DeriveRowStatus(v->refreshState, v->record, v->refreshDetail, v->retryAtUnix, now);
        row.status_text = status.text;
        row.status_class = status.cssClass;
        row.is_stale_or_failed = status.staleOrFailed;
        row.is_updating = status.isUpdating;
        row.updated_relative = FormatRelativeTime(v->record.lastSuccessfulRefreshAtUnix, now);

        row.ranks.reserve(m_playlistColumns.size());
        for (const auto& col : m_playlistColumns) {
            AccountsPlaylistRankCell cell;
            cell.playlist_key = col.key;
            auto it = v->record.ranks.find(std::string(col.key.c_str()));
            if (it != v->record.ranks.end() && it->second.mmr.has_value()) {
                cell.mmr = std::to_string(*it->second.mmr);
                cell.has_mmr = true;
                const std::string tier = DisplayTier(col.key, it->second);
                cell.tier = tier.empty() ? "" : tier.rfind("Unranked", 0) == 0 ? tier
                                                                               : Format::AbbreviateRank(tier);
                cell.tier_color = tier.empty() ? "" : RmlUiController::CssColor(Format::RankColor(tier));
            } else {
                cell.mmr = "\xE2\x80\x94";
                cell.has_mmr = false;
                cell.tier = "";
                cell.tier_color = "";
            }
            cell.is_stale = row.is_stale_or_failed;
            row.ranks.push_back(std::move(cell));
        }

        m_rows.push_back(std::move(row));
    }

    m_rowCount = static_cast<int>(m_rows.size());
    if (m_rowCount == 1) {
        m_subtitle = "1 account tracked";
    } else if (m_rowCount > 1) {
        m_subtitle = std::to_string(m_rowCount) + " accounts tracked";
    } else {
        m_subtitle = "Tracked Rocket League accounts";
    }
}

void AccountsView::OpenDetail(const std::string& primaryId, bool renameMode) {
    m_detailOpen = true;
    m_detailPrimaryId = primaryId;
    m_detailRenaming = renameMode;
    m_detailConfirmRemove = false;
    RebuildDetail();
    if (renameMode) {
        for (const auto& a : m_lastSnapshot.accounts) {
            if (a.record.primaryId == primaryId) {
                m_detailRenameValue = a.record.nickname;
                break;
            }
        }
    } else {
        m_detailRenameValue.clear();
    }
    Dirty("detail_open");
    Dirty("detail_renaming");
    Dirty("detail_confirm_remove");
    Dirty("detail_rename_value");
}

void AccountsView::CloseDetail() {
    m_detailOpen = false;
    m_detailPrimaryId.clear();
    m_detailRenaming = false;
    m_detailConfirmRemove = false;
    m_detailRenameValue.clear();
    Dirty("detail_open");
    Dirty("detail_renaming");
    Dirty("detail_confirm_remove");
}

void AccountsView::StartRename() {
    m_detailRenaming = true;
    for (const auto& a : m_lastSnapshot.accounts) {
        if (a.record.primaryId == m_detailPrimaryId) {
            m_detailRenameValue = a.record.nickname;
            break;
        }
    }
    Dirty("detail_renaming");
    Dirty("detail_rename_value");
}

void AccountsView::CancelRename() {
    m_detailRenaming = false;
    m_detailRenameValue.clear();
    Dirty("detail_renaming");
}

void AccountsView::FinishRename() {
    m_detailRenaming = false;
    m_detailRenameValue.clear();
    Dirty("detail_renaming");
}

void AccountsView::SetRenameValue(const std::string& val) {
    m_detailRenameValue = val;
}

void AccountsView::StartRemoveConfirm() {
    m_detailConfirmRemove = true;
    Dirty("detail_confirm_remove");
}

void AccountsView::CancelRemoveConfirm() {
    m_detailConfirmRemove = false;
    Dirty("detail_confirm_remove");
}

void AccountsView::RebuildDetail() {
    const auto* found = [&]() -> const AccountManagerService::AccountView* {
        for (const auto& a : m_lastSnapshot.accounts) {
            if (a.record.primaryId == m_detailPrimaryId) return &a;
        }
        return nullptr;
    }();

    if (!found) {
        CloseDetail();
        return;
    }

    const auto& rec = found->record;
    m_detailNickname = rec.nickname.empty() ? "\xE2\x80\x94" : rec.nickname;
    m_detailPlatform = TrackedAccounts::PlatformLabel(rec.platform);
    m_detailPlatformClass = PlatformCssClass(rec.platform);
    m_detailLastKnownName = rec.lastKnownName.empty() ? "\xE2\x80\x94" : rec.lastKnownName;
    m_detailCanonicalId = rec.primaryId;

    switch (rec.discoverySource) {
    case TrackedAccounts::DiscoverySource::Manual:
        m_detailDiscoverySource = "Manual";
        break;
    case TrackedAccounts::DiscoverySource::Auto:
        m_detailDiscoverySource = "Automatic";
        break;
    case TrackedAccounts::DiscoverySource::Import:
        m_detailDiscoverySource = "Imported";
        break;
    }

    const int64_t now = m_lastNowUnix > 0 ? m_lastNowUnix : static_cast<int64_t>(std::time(nullptr));
    if (rec.lastSuccessfulRefreshAtUnix <= 0) {
        m_detailLastRefresh = "Never";
    } else {
        m_detailLastRefresh = FormatDateTime(rec.lastSuccessfulRefreshAtUnix) + " (" +
                              FormatRelativeTime(rec.lastSuccessfulRefreshAtUnix, now) + ")";
    }

    DerivedStatus status = DeriveRowStatus(found->refreshState, rec, found->refreshDetail, found->retryAtUnix, now);
    m_detailRefreshState = status.text;

    m_detailRankSource.clear();
    m_detailRanks.clear();
    for (const auto& col : TrackedAccounts::SupportedPlaylists()) {
        AccountDetailRankRow dr;
        dr.playlist_key = col.key;
        dr.playlist_label = col.label;
        auto it = rec.ranks.find(col.key);
        if (it != rec.ranks.end() && it->second.mmr.has_value()) {
            dr.mmr = std::to_string(*it->second.mmr);
            const std::string tier = DisplayTier(col.key, it->second);
            dr.tier = tier.empty() ? "\xE2\x80\x94" : Format::RankTier(tier, false);
            dr.tier_color = tier.empty() ? "" : RmlUiController::CssColor(Format::RankColor(tier));
            dr.matches_played = it->second.matchesPlayed.has_value() ? std::to_string(*it->second.matchesPlayed) : "\xE2\x80\x94";
            dr.source = TrackedAccounts::RankSourceLabel(it->second.source);
            if (m_detailRankSource.empty() && !dr.source.empty()) {
                m_detailRankSource = dr.source;
            }
        } else {
            dr.mmr = "\xE2\x80\x94";
            dr.tier = "\xE2\x80\x94";
            dr.tier_color = "";
            dr.matches_played = "\xE2\x80\x94";
            dr.source = "\xE2\x80\x94";
        }
        m_detailRanks.push_back(std::move(dr));
    }

    Dirty("detail_nickname");
    Dirty("detail_platform");
    Dirty("detail_platform_class");
    Dirty("detail_last_known_name");
    Dirty("detail_canonical_id");
    Dirty("detail_ranks");
    Dirty("detail_last_refresh");
    Dirty("detail_rank_source");
    Dirty("detail_refresh_state");
    Dirty("detail_discovery_source");
}

void AccountsView::OpenAdd() {
    m_addOpen = true;
    m_addPlatform = "epic";
    m_addPlatformEnum = TrackedAccounts::Platform::Epic;
    m_addAccountId.clear();
    m_addNickname.clear();
    m_addError.clear();
    m_addAlreadyTracked = false;
    m_addExistingId.clear();
    UpdateAddHelp();
    Dirty("add_open");
    Dirty("add_platform");
    Dirty("add_id_value");
    Dirty("add_nickname_value");
    Dirty("add_id_help");
    Dirty("add_error");
    Dirty("add_already_tracked");
    Dirty("add_existing_id");
}

void AccountsView::CloseAdd() {
    m_addOpen = false;
    m_addError.clear();
    m_addAlreadyTracked = false;
    m_addExistingId.clear();
    Dirty("add_open");
}

void AccountsView::SetAddPlatform(const std::string& platformKey) {
    const auto platform = TrackedAccounts::ParsePlatform(platformKey);
    if (!platform) return;
    m_addPlatformEnum = *platform;
    m_addPlatform = TrackedAccounts::PlatformKey(*platform);
    UpdateAddHelp();
    Dirty("add_platform");
    Dirty("add_id_help");
}

void AccountsView::SetAddAccountId(const std::string& id) {
    m_addAccountId = id;
}

void AccountsView::SetAddNickname(const std::string& nick) {
    m_addNickname = nick;
}

void AccountsView::SetAddError(const std::string& err) {
    m_addError = err;
    m_addAlreadyTracked = false;
    Dirty("add_error");
    Dirty("add_already_tracked");
}

void AccountsView::SetAddAlreadyTracked(const std::string& existingId, const std::string& message) {
    m_addError = message;
    m_addAlreadyTracked = true;
    m_addExistingId = existingId;
    Dirty("add_error");
    Dirty("add_already_tracked");
    Dirty("add_existing_id");
}

void AccountsView::UpdateAddHelp() {
    switch (m_addPlatformEnum) {
    case TrackedAccounts::Platform::Epic:
        m_addIdHelp = "The 32-character account ID from your Epic Games account settings. Display names can't be used.";
        break;
    case TrackedAccounts::Platform::Steam:
        m_addIdHelp = "The 17-digit SteamID64, or a steamcommunity.com/profiles/ link.";
        break;
    case TrackedAccounts::Platform::PlayStation:
        m_addIdHelp = "The numeric PlayStation account ID. Online IDs can't be used.";
        break;
    case TrackedAccounts::Platform::Xbox:
        m_addIdHelp = "The numeric Xbox user ID (XUID). Gamertags can't be used.";
        break;
    case TrackedAccounts::Platform::Switch:
        m_addIdHelp = "The hexadecimal Nintendo account ID.";
        break;
    }
}

void AccountsView::OpenImport(const std::vector<AccountManagerService::ImportCandidate>& candidates) {
    m_importOpen = true;
    m_importCandidates.clear();
    m_importCandidates.reserve(candidates.size());
    for (const auto& cand : candidates) {
        AccountImportCandidateRow row;
        row.platform = TrackedAccounts::PlatformLabel(cand.identity.platform);
        row.name = cand.lastKnownName.empty() ? "Name not found in match history" : cand.lastKnownName;
        row.masked_id = TrackedAccounts::MaskedAccountId(cand.identity.accountId);
        row.primary_id = cand.identity.primaryId;
        m_importCandidates.push_back(std::move(row));
    }
    m_importSubmitLabel = "Import " + std::to_string(m_importCandidates.size()) + " accounts";
    Dirty("import_open");
    Dirty("import_candidates");
    Dirty("import_submit_label");
}

void AccountsView::CloseImport() {
    m_importOpen = false;
    Dirty("import_open");
}

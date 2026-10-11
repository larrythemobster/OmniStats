#pragma once

#include <RmlUi/Core/DataModelHandle.h>
#include <RmlUi/Core/Types.h>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "core/AccountManager.hpp"
#include "core/TrackedAccounts.hpp"

namespace Rml {
    class Context;
}

struct AccountsPlaylistColumn {
    Rml::String key;
    Rml::String label;
    Rml::String short_label;
};

struct AccountsPlaylistRankCell {
    Rml::String playlist_key;
    Rml::String mmr;
    Rml::String tier;
    Rml::String tier_color;
    bool is_stale = false;
    bool has_mmr = false;
};

struct AccountRow {
    Rml::String primary_id;
    Rml::String display_label;
    Rml::String sub_label;
    Rml::String platform;
    Rml::String platform_class;
    bool is_active = false;
    std::vector<AccountsPlaylistRankCell> ranks;
    Rml::String updated_relative;
    Rml::String status_text;
    Rml::String status_class;
    bool is_stale_or_failed = false;
    bool is_updating = false;
};

struct AccountDetailRankRow {
    Rml::String playlist_key;
    Rml::String playlist_label;
    Rml::String mmr;
    Rml::String tier;
    Rml::String tier_color;
    Rml::String matches_played;
    Rml::String source;
};

struct AccountImportCandidateRow {
    Rml::String platform;
    Rml::String name;
    Rml::String masked_id;
    Rml::String primary_id;
};

class AccountsView {
  public:
    bool Create(Rml::Context* context);
    void Reset();

    void UpdateSnapshot(const AccountManagerService::Snapshot& snapshot,
                        const std::string& activePrimaryId,
                        size_t importCandidatesCount,
                        int64_t nowUnix = 0);
    void SetUnavailable(bool unavailable);

    void SetColumnSet(const std::string& columnSet);
    const std::string& ColumnSet() const {
        return m_columnSet;
    }

    void SetSort(const std::string& col);
    const std::string& SortCol() const {
        return m_sortCol;
    }
    bool SortAsc() const {
        return m_sortAsc;
    }

    void OpenDetail(const std::string& primaryId, bool renameMode = false);
    void CloseDetail();
    bool IsDetailOpen() const {
        return m_detailOpen;
    }
    const std::string& DetailPrimaryId() const {
        return m_detailPrimaryId;
    }
    const std::string& DetailCanonicalId() const {
        return m_detailCanonicalId;
    }

    void StartRename();
    void CancelRename();
    void FinishRename();
    void SetRenameValue(const std::string& val);
    const std::string& RenameValue() const {
        return m_detailRenameValue;
    }

    void StartRemoveConfirm();
    void CancelRemoveConfirm();

    void OpenAdd();
    void CloseAdd();
    bool IsAddOpen() const {
        return m_addOpen;
    }
    void SetAddPlatform(const std::string& platformKey);
    TrackedAccounts::Platform AddPlatform() const {
        return m_addPlatformEnum;
    }
    void SetAddAccountId(const std::string& id);
    const std::string& AddAccountId() const {
        return m_addAccountId;
    }
    void SetAddNickname(const std::string& nick);
    const std::string& AddNickname() const {
        return m_addNickname;
    }
    void SetAddError(const std::string& err);
    void SetAddAlreadyTracked(const std::string& existingId, const std::string& message);
    const std::string& AddExistingId() const {
        return m_addExistingId;
    }

    void OpenImport(const std::vector<AccountManagerService::ImportCandidate>& candidates);
    void CloseImport();
    bool IsImportOpen() const {
        return m_importOpen;
    }

    const std::vector<AccountRow>& Rows() const {
        return m_rows;
    }
    bool IsLoaded() const {
        return m_loaded;
    }

  private:
    void Dirty(const char* name);
    void RebuildPlaylistColumns();
    void RebuildRows();
    void RebuildDetail();
    void UpdateAddHelp();

    Rml::DataModelHandle m_handle;
    bool m_bound = false;

    AccountManagerService::Snapshot m_lastSnapshot;
    std::string m_activePrimaryId;
    int64_t m_lastNowUnix = 0;

    std::string m_columnSet = "competitive";
    std::string m_sortCol = "order";
    bool m_sortAsc = true;
    Rml::String m_sortDirArrow = "";

    std::vector<AccountsPlaylistColumn> m_playlistColumns;
    std::vector<AccountRow> m_rows;

    bool m_loaded = false;
    bool m_hasService = true;
    bool m_providerUnavailable = false;
    Rml::String m_providerBannerText;
    bool m_hasImportCandidates = false;
    int m_importCandidatesCount = 0;
    Rml::String m_importButtonLabel = "Import Known";
    int m_rowCount = 0;
    Rml::String m_emptyMessage = "No accounts tracked yet. Add an account to follow ranks and compare MMR across playlists.";
    Rml::String m_subtitle = "Tracked Rocket League accounts";

    bool m_detailOpen = false;
    std::string m_detailPrimaryId;
    Rml::String m_detailNickname;
    Rml::String m_detailPlatform;
    Rml::String m_detailPlatformClass;
    Rml::String m_detailLastKnownName;
    Rml::String m_detailCanonicalId;
    std::vector<AccountDetailRankRow> m_detailRanks;
    Rml::String m_detailLastRefresh;
    Rml::String m_detailRankSource;
    Rml::String m_detailRefreshState;
    Rml::String m_detailDiscoverySource;
    bool m_detailRenaming = false;
    std::string m_detailRenameValue;
    bool m_detailConfirmRemove = false;

    bool m_addOpen = false;
    Rml::String m_addPlatform = "epic";
    TrackedAccounts::Platform m_addPlatformEnum = TrackedAccounts::Platform::Epic;
    std::string m_addAccountId;
    std::string m_addNickname;
    Rml::String m_addIdHelp = "The 32-character account ID from your Epic Games account settings. Display names can't be used.";
    Rml::String m_addError;
    bool m_addAlreadyTracked = false;
    Rml::String m_addExistingId;

    bool m_importOpen = false;
    std::vector<AccountImportCandidateRow> m_importCandidates;
    Rml::String m_importSubmitLabel = "Import 0 accounts";
};

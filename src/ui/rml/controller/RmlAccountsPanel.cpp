#include "ui/rml/controller/RmlUiController.hpp"
#include "core/state/LiveMatchStore.hpp"

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>

#include "database/DatabaseManager.hpp"
#include "network/api/AccountClient.hpp"
#include "ui/Formatting.hpp"
#include "ui/rml/controller/RmlUiHelpers.hpp"
#include "network/telemetry/TelemetryManager.hpp"

using namespace RmlUiDetail;

void RmlUiController::OpenAccounts() {
    ShowAccounts();
}

void RmlUiController::ShowAccounts() {
    if (!m_accounts.Document()) return;
    m_accounts.Show();
    m_accounts.Document()->PullToFront();
    m_onboarding.Raise();
    RefreshAccounts(true);
    m_renderDirty = true;
}

void RmlUiController::HideAccounts() {
    m_accounts.Hide();
    m_accounts.model.CloseDetail();
    m_accounts.model.CloseAdd();
    m_accounts.model.CloseImport();
    m_renderDirty = true;
}

void RmlUiController::RefreshAccounts(bool force) {
    if (!m_accountsService) {
        m_accounts.model.SetUnavailable(true);
        m_renderDirty = true;
        return;
    }
    const uint64_t v = m_accountsService->Version();
    const auto nowSteady = std::chrono::steady_clock::now();
    const bool timeToReRender = (nowSteady - m_lastAccountsRelativeRender) >= std::chrono::seconds(30);
    const std::string effectivePrimary = m_snap.myPrimaryId.empty() ? m_config.last_primary_id : m_snap.myPrimaryId;
    std::string canonicalActive = TrackedAccounts::CanonicalPrimaryId(effectivePrimary);
    if (!force && v == m_lastAccountsVersion && !timeToReRender && canonicalActive == m_lastAccountsActive) {
        return;
    }
    m_lastAccountsVersion = v;
    m_lastAccountsRelativeRender = nowSteady;
    m_lastAccountsActive = canonicalActive;

    auto snapshot = m_accountsService->GetSnapshot();
    auto candidates = m_accountsService->ImportCandidates();
    m_accounts.model.UpdateSnapshot(snapshot, canonicalActive, candidates.size());
    m_accountsService->RequestHistory(m_accounts.model.HistoryAccountIds(),
                                      m_accounts.model.HistoryPlaylist(), m_accounts.model.HistoryWindow());
    m_renderDirty = true;
}

void RmlUiController::RefreshAccountRow(const std::string& primaryId) {
    if (!m_accountsService) return;
    auto res = m_accountsService->Refresh(primaryId);
    switch (res) {
    case AccountManagerService::RefreshRequest::Queued:
        ShowToast("Rank refresh queued.");
        break;
    case AccountManagerService::RefreshRequest::AlreadyQueued:
        ShowToast("Rank refresh already in progress.");
        break;
    case AccountManagerService::RefreshRequest::CoolingDown:
        ShowToast("Refreshed recently; try again in a minute.");
        break;
    case AccountManagerService::RefreshRequest::ProviderUnavailable:
        ShowToast("Connect live ranks in Settings > Accounts & services to refresh ranks.");
        break;
    case AccountManagerService::RefreshRequest::Unsupported:
        ShowToast("Ranks can't be refreshed for this account.");
        break;
    case AccountManagerService::RefreshRequest::UnknownAccount:
        ShowToast("This account is no longer tracked.", true);
        break;
    }
    RefreshAccounts(true);
}

bool RmlUiController::HandleAccountsAction(const std::string& action, Rml::Element* target) {
    if (action == "accounts-open") {
        ShowAccounts();
        return true;
    } else if (action == "accounts-close") {
        HideAccounts();
        return true;
    } else if (action == "accounts-auto-detect") {
        SelectIdentity("");
        return true;
    } else if (action == "accounts-refresh-all") {
        if (m_accountsService) {
            size_t count = m_accountsService->RefreshAll();
            if (count > 0) {
                ShowToast("Queued refresh for " + std::to_string(count) + (count == 1 ? " account." : " accounts."));
            } else {
                ShowToast("Refreshed recently; try again in a minute.");
            }
            RefreshAccounts(true);
        }
        return true;
    } else if (action == "accounts-refresh-row") {
        const std::string id = Attribute(target, "data-id");
        if (!id.empty()) RefreshAccountRow(id);
        return true;
    } else if (action == "accounts-use") {
        const std::string id = Attribute(target, "data-id");
        if (!m_accountsService) return true;
        const auto snapshot = m_accountsService->GetSnapshot();
        const auto account = std::find_if(snapshot.accounts.begin(), snapshot.accounts.end(), [&](const auto& entry) {
            return entry.record.primaryId == id;
        });
        if (account == snapshot.accounts.end()) return true;
        std::string selectedId = id;
        for (const auto& known : m_config.known_primary_ids)
            if (TrackedAccounts::CanonicalPrimaryId(known) == id) {
                selectedId = known;
                break;
            }
        for (const auto& [rosterId, player] : m_snap.roster)
            if (TrackedAccounts::CanonicalPrimaryId(rosterId) == id) {
                selectedId = rosterId;
                break;
            }
        SelectIdentity(selectedId);
        ShowToast("Active account: " + TrackedAccounts::DisplayLabel(account->record));
        return true;
    } else if (action == "accounts-column-set") {
        const std::string set = Attribute(target, "data-set");
        if (!set.empty()) {
            m_accounts.model.SetColumnSet(set);
            m_renderDirty = true;
        }
        return true;
    } else if (action == "accounts-sort") {
        const std::string sort = Attribute(target, "data-sort");
        if (!sort.empty()) {
            m_accounts.model.SetSort(sort);
            m_renderDirty = true;
        }
        return true;
    } else if (action == "accounts-history-toggle") {
        const std::string id = Attribute(target, "data-id");
        if (m_accounts.model.ToggleHistoryAccount(id)) RefreshAccounts(true);
        return true;
    } else if (action == "accounts-history-playlist") {
        const std::string playlist = Attribute(target, "data-playlist");
        m_accounts.model.SetHistoryPlaylist(playlist);
        RefreshAccounts(true);
        return true;
    } else if (action == "accounts-history-range") {
        m_accounts.model.SetHistoryWindow(Attribute(target, "data-window"));
        RefreshAccounts(true);
        return true;
    } else if (action == "accounts-open-detail") {
        const std::string id = Attribute(target, "data-id");
        if (!id.empty()) {
            m_accounts.model.OpenDetail(id);
            m_renderDirty = true;
        }
        return true;
    } else if (action == "accounts-close-detail") {
        m_accounts.model.CloseDetail();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-detail-start-rename") {
        m_accounts.model.StartRename();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-detail-cancel-rename") {
        m_accounts.model.CancelRename();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-detail-save-rename") {
        if (m_accountsService) {
            m_accountsService->Rename(m_accounts.model.DetailPrimaryId(), m_accounts.model.RenameValue());
            m_accounts.model.FinishRename();
            RefreshAccounts(true);
        }
        return true;
    } else if (action == "accounts-detail-refresh") {
        if (!m_accounts.model.DetailPrimaryId().empty()) {
            RefreshAccountRow(m_accounts.model.DetailPrimaryId());
        }
        return true;
    } else if (action == "accounts-detail-copy-id") {
        if (!m_accounts.model.DetailCanonicalId().empty()) {
            m_systemInterface.SetClipboardText(m_accounts.model.DetailCanonicalId());
            ShowToast("Copied account ID to clipboard.");
        }
        return true;
    } else if (action == "accounts-detail-start-remove") {
        m_accounts.model.StartRemoveConfirm();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-detail-cancel-remove") {
        m_accounts.model.CancelRemoveConfirm();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-detail-confirm-remove") {
        if (m_accountsService) {
            m_accountsService->Remove(m_accounts.model.DetailPrimaryId());
            m_accounts.model.CloseDetail();
            ShowToast("Account removed.");
            RefreshAccounts(true);
        }
        return true;
    } else if (action == "accounts-open-add") {
        m_accounts.model.OpenAdd();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-add-close") {
        m_accounts.model.CloseAdd();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-add-platform") {
        const std::string plat = Attribute(target, "data-platform");
        if (!plat.empty()) {
            m_accounts.model.SetAddPlatform(plat);
            m_renderDirty = true;
        }
        return true;
    } else if (action == "accounts-add-submit") {
        if (m_accountsService) {
            auto res = m_accountsService->AddManual(m_accounts.model.AddPlatform(), m_accounts.model.AddAccountId(), m_accounts.model.AddNickname());
            if (res.status == AccountManagerService::AddResult::Status::Added) {
                m_accounts.model.CloseAdd();
                ShowToast("Account added.");
                RefreshAccounts(true);
            } else if (res.status == AccountManagerService::AddResult::Status::AlreadyTracked) {
                m_accounts.model.SetAddAlreadyTracked(res.primaryId, res.message);
                m_renderDirty = true;
            } else {
                m_accounts.model.SetAddError(res.message);
                m_renderDirty = true;
            }
        }
        return true;
    } else if (action == "accounts-add-rename-existing") {
        std::string existingId = Attribute(target, "data-id");
        if (existingId.empty()) existingId = m_accounts.model.AddExistingId();
        m_accounts.model.CloseAdd();
        m_accounts.model.OpenDetail(existingId, true);
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-open-import") {
        if (m_accountsService) {
            m_accounts.model.OpenImport(m_accountsService->ImportCandidates());
        }
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-import-close") {
        m_accounts.model.CloseImport();
        m_renderDirty = true;
        return true;
    } else if (action == "accounts-import-submit") {
        if (m_accountsService) {
            size_t count = m_accountsService->ImportKnown();
            m_accounts.model.CloseImport();
            ShowToast(count > 0 ? "Imported " + std::to_string(count) + (count == 1 ? " account." : " accounts.") : "No new accounts to import.");
            RefreshAccounts(true);
        }
        return true;
    } else {
        return false;
    }
    return true;
}

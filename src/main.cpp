#define WIN32_LEAN_AND_MEAN
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <ctime>
#include "core/Config.hpp"
#include "core/InputManager.hpp"
#include "core/SessionState.hpp"
#include "core/Storage.hpp"
#include "database/DatabaseManager.hpp"
#include "network/DiscordManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/MMRFetcher.hpp"
#include "network/ReplayUploader.hpp"
#include "network/StatsClient.hpp"
#include "network/TelemetryManager.hpp"
#include "ui/Overlay.hpp"
#include "core/StatsApiConfig.hpp"
#include <curl/curl.h>
#include <dbghelp.h>
#include <commctrl.h>
#include <windows.h>
using TaskDialogIndirect_t = HRESULT(WINAPI*)(const TASKDIALOGCONFIG*, int*, int*, BOOL*);
#pragma comment(linker, "/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

namespace {
    // RmlUi measures `dp` against the process DPI context. Make the process
    // per-monitor-v2 aware before any HWND (including startup dialogs) is created,
    // otherwise Windows can virtualize coordinates and RmlUi may rasterize small
    // text at the wrong effective pixel size. Resolve dynamically so the client
    // still starts on older Windows 10 builds that do not expose the v2 API.
    void EnablePerMonitorDpiAwareness() {
        HMODULE user32 = GetModuleHandleW(L"user32.dll");
        if (!user32) return;

        using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
        const auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
            GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
        if (setContext) {
            // DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2 is the pseudo-handle -4.
            const HANDLE perMonitorV2 = reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4));
            if (setContext(perMonitorV2)) return;

            // ERROR_ACCESS_DENIED means awareness was already established (for
            // example by a manifest). Do not downgrade it with the legacy API.
            if (GetLastError() == ERROR_ACCESS_DENIED) return;
        }

        using SetProcessDPIAwareFn = BOOL(WINAPI*)();
        const auto setLegacyAware = reinterpret_cast<SetProcessDPIAwareFn>(
            GetProcAddress(user32, "SetProcessDPIAware"));
        if (setLegacyAware) setLegacyAware();
    }
} // namespace

static wchar_t g_crashDumpPath[MAX_PATH] = {0};
static HRESULT CALLBACK PrivacyTaskDialogCallback(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, LONG_PTR lpRefData) {
    (void)hwnd;
    (void)wParam;
    (void)lpRefData;
    if (msg == TDN_HYPERLINK_CLICKED && lParam) {
        ShellExecuteW(NULL, L"open", reinterpret_cast<LPCWSTR>(lParam), NULL, NULL, SW_SHOWNORMAL);
    }
    return S_OK;
}
static bool ShowRequiredPrivacyDialog() {
    const TASKDIALOG_BUTTON buttons[] = {
        {IDYES, L"Accept"},
        {IDNO, L"Exit"}};
    TASKDIALOGCONFIG config = {};
    config.cbSize = sizeof(config);
    config.dwFlags = TDF_ENABLE_HYPERLINKS | TDF_ALLOW_DIALOG_CANCELLATION;
    config.pszWindowTitle = L"OmniStats Privacy Notice";
    config.pszMainIcon = TD_INFORMATION_ICON;
    config.pszMainInstruction = L"Accept the Privacy Policy and Terms of Use to continue.";
    config.pszContent =
        L"Required startup diagnostics: app version, a pseudonymous installation ID, and feature-toggle status. Match data and player names are not included.\n\n"
        L"Update checks are always enabled. Tracker rank lookup, Discord, Ballchasing, crash reports, and automatic update installation stay off unless enabled.\n\n"
        L"<a href=\"https://omnistats.org/privacy\">Privacy Policy</a>\n"
        L"<a href=\"https://omnistats.org/terms\">Terms of Use</a>\n\n"
        L"Accept to continue, or Exit to close OmniStats.";
    config.cButtons = ARRAYSIZE(buttons);
    config.pButtons = buttons;
    config.nDefaultButton = IDNO;
    config.pfCallback = PrivacyTaskDialogCallback;
    HMODULE comctl32 = LoadLibraryW(L"comctl32.dll");
    if (comctl32) {
        auto taskDialogIndirect = reinterpret_cast<TaskDialogIndirect_t>(GetProcAddress(comctl32, "TaskDialogIndirect"));
        if (taskDialogIndirect) {
            int selectedButton = IDNO;
            HRESULT hr = taskDialogIndirect(&config, &selectedButton, nullptr, nullptr);
            if (SUCCEEDED(hr)) {
                return selectedButton == IDYES;
            }
        }
    }
    std::string message =
        "Accept the Privacy Policy and Terms of Use to continue.\n\n"
        "Required startup diagnostics: app version, a pseudonymous installation ID, and feature-toggle status. Match data and player names are not included.\n\n"
        "Update checks are always enabled. Tracker rank lookup, Discord, Ballchasing, crash reports, and automatic update installation stay off unless enabled.\n\n"
        "Privacy Policy: https://omnistats.org/privacy\n"
        "Terms of Use: https://omnistats.org/terms\n\n"
        "Choose Yes to accept, or No to exit.";
    int accepted = MessageBoxA(NULL, message.c_str(), "OmniStats Privacy Notice", MB_YESNO | MB_ICONINFORMATION | MB_DEFBUTTON2 | MB_SETFOREGROUND);
    return accepted == IDYES;
}
static bool EnsureRequiredPrivacyAcceptance() {
    ConfigData conf = Config::Read();
    const bool acceptedCurrent = conf.privacy_policy_accepted_version == Config::CurrentPrivacyPolicyVersion &&
                                 conf.terms_accepted_version == Config::CurrentTermsVersion;
    if (acceptedCurrent) {
        return true;
    }
    if (!ShowRequiredPrivacyDialog()) {
        return false;
    }
    // Optional integrations (Tracker, Discord, crash reports) are offered by the
    // in-app first-run wizard, which runs until onboarding_completed is set.
    std::string acceptedAt = std::to_string(std::time(nullptr));
    Config::Update([&](ConfigData& c) {
        c.privacy_policy_accepted_version = Config::CurrentPrivacyPolicyVersion;
        c.terms_accepted_version = Config::CurrentTermsVersion;
        c.privacy_accepted_at = acceptedAt;
    },
                   false);
    Config::Save();
    return true;
}
LONG WINAPI
GlobalUnhandledExceptionFilter(struct _EXCEPTION_POINTERS* exceptionInfo) {
    HANDLE hFile = CreateFileW(g_crashDumpPath, GENERIC_WRITE, 0, NULL,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mdei;
        mdei.ThreadId = GetCurrentThreadId();
        mdei.ExceptionPointers = exceptionInfo;
        mdei.ClientPointers = FALSE;
        MINIDUMP_TYPE dumpType = MiniDumpNormal;
#ifndef NDEBUG
        dumpType = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
#endif
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                          dumpType, &mdei, NULL, NULL);
        CloseHandle(hFile);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#ifdef NDEBUG
#pragma comment(linker, "/SUBSYSTEM:windows /ENTRY:mainCRTStartup")
#endif
int main(int argc, char* argv[]) {
    EnablePerMonitorDpiAwareness();
    SetUnhandledExceptionFilter(GlobalUnhandledExceptionFilter);
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--apply-update") {
            MessageBoxA(NULL,
                        "OmniStats is updating to a new major version, which requires re-running the installer.\n\n"
                        "Please download and run the latest installer from the official website: https://omnistats.org",
                        "OmniStats Update Required",
                        MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
            return 0;
        }
    }
    (void)argc;
    (void)argv;
    // Initialize Curl globally for static linking
    curl_global_init(CURL_GLOBAL_ALL);
    Storage::InitializeEnvironment();
    std::string crashFile = Storage::GetDataDirectory() + "crash_pending.dmp";
    MultiByteToWideChar(CP_UTF8, 0, crashFile.c_str(), -1, g_crashDumpPath, MAX_PATH);
    // Redirect stdout to a log file in APPDATA
    std::string logPath =
        Storage::GetDataDirectory() + Storage::APP_NAME + "_log.txt";
    FILE* outStream = nullptr;
    freopen_s(&outStream, logPath.c_str(), "w", stdout);
    if (outStream)
        setvbuf(outStream, NULL, _IONBF, 0);
    std::cout << "OmniStats Starting...\n";
    Config::Load();
    if (!EnsureRequiredPrivacyAcceptance()) {
        std::cout << "[Privacy] Required privacy notice was declined. Exiting.\n";
        curl_global_cleanup();
        return 0;
    }
    ConfigData startupUpdateConf = Config::Read();
    if (startupUpdateConf.enable_auto_updates && ExternalUpdaterLauncher::RunStartupUpdateCheck()) {
        std::cout << "[Main] External updater launched for startup update.\n";
        curl_global_cleanup();
        return 0;
    }
    Config::InitSaver();
    auto g_state = std::make_shared<SessionState>();
    // Remember the selected MMR category from previous session
    ConfigData startupConf = Config::Read();
    MmrCategory liveCategory = StringToMmrCategory(startupConf.mmr_category);
    if (!startupConf.show_extra_playlists && IsExtraMmrCategory(liveCategory)) {
        liveCategory = MmrCategory::Best;
    }
    g_state->ui.rosterMmrCategory.store(liveCategory);

    MmrCategory graphCategory =
        StringToMmrCategory(startupConf.graph_mmr_category);
    if (graphCategory == MmrCategory::Best ||
        (!startupConf.show_extra_playlists &&
         IsExtraMmrCategory(graphCategory))) {
        graphCategory = MmrCategory::TwoVTwo;
    }
    g_state->ui.graphMmrCategory.store(graphCategory);
    // Stats API config check
    std::string apiPath = startupConf.rocket_league_stats_api_config_path;
    if (apiPath.empty()) {
        apiPath = StatsApiConfig::DetectConfigPath();
    }
    StatsApiConfig::CheckResult checkRes = StatsApiConfig::VerifyConfig(apiPath, startupConf.port);
    // Until the first-run wizard is finished it owns the Stats API fix.
    if (startupConf.check_stats_api_config_on_startup && startupConf.onboarding_completed &&
        checkRes.status != StatsApiConfig::Status::Valid) {
        std::wstring title = L"OmniStats Stats API Setup";
        std::wstring text = L"Rocket League Stats API is disabled or misconfigured. OmniStats needs PacketSendRate=30 and Port=49123 to read live game data. Fix it now?";
        if (checkRes.rlRunning) {
            text += L"\n\nRocket League must be restarted after fixing this.";
        }
        int msgRes = MessageBoxW(NULL, text.c_str(), title.c_str(), MB_YESNO | MB_ICONWARNING | MB_SYSTEMMODAL);
        if (msgRes == IDYES) {
            const bool repaired = ExternalUpdaterLauncher::RepairStatsApiConfig(apiPath, startupConf.port);
            if (repaired) {
                checkRes = StatsApiConfig::VerifyConfig(apiPath, startupConf.port);
                MessageBoxW(NULL, L"Rocket League Stats API configuration successfully updated!", title.c_str(), MB_OK | MB_ICONINFORMATION | MB_SYSTEMMODAL);
            } else {
                MessageBoxW(NULL, L"Failed to update Stats API configuration. Please check permissions or edit DefaultStatsAPI.ini manually.", title.c_str(), MB_OK | MB_ICONERROR | MB_SYSTEMMODAL);
            }
        }
    }
    {
        std::lock_guard<std::mutex> lock(g_state->ui.statsApiMutex);
        g_state->ui.statsApiResult = checkRes;
        g_state->ui.statsApiChecked.store(true);
    }
    if (checkRes.status == StatsApiConfig::Status::Valid) {
        std::cout << "[StatsApiConfig] Rocket League Stats API config is valid.\n";
    } else {
        std::cout << "[StatsApiConfig] Rocket League Stats API config check failed: " << StatsApiConfig::GetStatusMessage(checkRes.status) << "\n";
    }
    ConfigData updateConf = Config::Read();
    if (!updateConf.enable_auto_updates) {
        ExternalUpdaterLauncher::StartBackgroundUpdateCheck(
            g_state, ExternalUpdaterLauncher::BackgroundUpdateCheckReason::Initial);
    }
    auto dbManager = std::make_shared<DatabaseManager>(g_state);
    (void)dbManager->Initialize(Storage::GetDataDirectory() + Storage::APP_NAME +
                                ".db");
    TelemetryManager::Initialize(dbManager);
    // Start Backend Threads
    std::shared_ptr<MMRFetcher> mmrFetcher =
        std::make_shared<MMRFetcher>(g_state, dbManager);
    mmrFetcher->Start();
    StatsClient statsClient(g_state, mmrFetcher, dbManager);
    statsClient.Start();
    std::shared_ptr<DiscordManager> discordManager;
    if (Config::Read().discord_rpc_enabled) {
        discordManager = std::make_shared<DiscordManager>(g_state);
        discordManager->Initialize();
        statsClient.SetDiscordManager(discordManager);
    }
    InputManager inputManager(g_state);
    inputManager.Start();
    std::unique_ptr<ReplayUploader> uploader;
    ConfigData mainConf = Config::Read();
    if (mainConf.auto_upload_replays || mainConf.auto_save_replays) {
        uploader = std::make_unique<ReplayUploader>(g_state);
        uploader->Start();
    }
    Overlay overlay(g_state, dbManager);
    if (!overlay.Initialize()) {
        Config::ShutdownSaver();
        curl_global_cleanup();
        return 1;
    }
    overlay.RunLoop();
    std::cout << "[Main] Shutting down components...\n";
    inputManager.Stop();
    std::cout << "[Main] InputManager stopped.\n";
    statsClient.Stop();
    std::cout << "[Main] StatsClient stopped.\n";
    mmrFetcher->Stop();
    std::cout << "[Main] MMRFetcher stopped.\n";
    if (discordManager) {
        discordManager->Shutdown();
        std::cout << "[Main] DiscordManager stopped.\n";
    }
    if (uploader) {
        uploader->Stop();
        std::cout << "[Main] ReplayUploader stopped.\n";
    }
    TelemetryManager::Shutdown();
    std::cout << "[Main] TelemetryManager stopped.\n";
    Config::ShutdownSaver();
    std::cout << "[Main] Config Saver stopped.\n";
    ExternalUpdaterLauncher::ShutdownBackgroundTasks();
    curl_global_cleanup();
    std::cout << "[Main] Cleanup complete. Exiting.\n";
    return 0;
}

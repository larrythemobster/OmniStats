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
        L"Tracker rank lookup, Discord, Ballchasing, crash reports, and update checks stay off unless enabled.\n\n"
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
        "Tracker rank lookup, Discord, Ballchasing, crash reports, and update checks stay off unless enabled.\n\n"
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
    const bool firstAcceptance = conf.privacy_policy_accepted_version.empty() || conf.terms_accepted_version.empty();
    if (!ShowRequiredPrivacyDialog()) {
        return false;
    }
    bool enableMmrTracking = conf.enable_mmr_tracking;
    bool enableDiscordRpc = conf.discord_rpc_enabled;
    bool enableCrashReports = conf.crash_reports_enabled;

    if (firstAcceptance) {
        int mmr = MessageBoxA(
            NULL,
            "Enable live MMR tracking with Tracker Network?\n\nThis sends lobby player names and platform identifiers to Tracker Network to retrieve public rank information. The integration is optional and may stop working if the third-party service changes.",
            "Optional Tracker Rank Lookup",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND);
        enableMmrTracking = (mmr == IDYES);

        int discord = MessageBoxA(
            NULL,
            "Enable Discord Rich Presence?\n\nThis shares your current match/session status with your local Discord client for display on your Discord profile.",
            "Optional Discord Rich Presence",
            MB_YESNO | MB_ICONQUESTION | MB_DEFBUTTON2 | MB_SETFOREGROUND);
        enableDiscordRpc = (discord == IDYES);
        int crashReports = MessageBoxA(
            NULL,
            "Enable automatic crash report uploads?\n\nIf enabled, OmniStats uploads pending Windows minidump files on next startup to help diagnose native crashes. Minidumps may contain sensitive process memory, so leave this off unless you are comfortable sending them.",
            "Optional Crash Reports",
            MB_YESNO | MB_ICONWARNING | MB_DEFBUTTON2 | MB_SETFOREGROUND);
        enableCrashReports = (crashReports == IDYES);
    }
    std::string acceptedAt = std::to_string(std::time(nullptr));
    Config::Update([&](ConfigData& c) {
        c.privacy_policy_accepted_version = Config::CurrentPrivacyPolicyVersion;
        c.terms_accepted_version = Config::CurrentTermsVersion;
        c.privacy_accepted_at = acceptedAt;
        c.enable_mmr_tracking = enableMmrTracking;
        c.discord_rpc_enabled = enableDiscordRpc;
        c.crash_reports_enabled = enableCrashReports;
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
        if (arg == "--repair-stats-api" && i + 1 < argc) {
            std::string path = argv[i + 1];
            int port = 49123;
            if (i + 2 < argc) {
                try {
                    port = std::stoi(argv[i + 2]);
                } catch (...) {
                }
            }
            return StatsApiConfig::FixConfigStrictHeadless(path, port);
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
    if (startupUpdateConf.check_for_updates && startupUpdateConf.enable_auto_updates &&
        ExternalUpdaterLauncher::RunStartupUpdateCheck()) {
        std::cout << "[Main] External updater launched for startup update.\n";
        curl_global_cleanup();
        return 0;
    }
    Config::InitSaver();
    auto g_state = std::make_shared<SessionState>();
    // Remember the selected MMR category from previous session
    ConfigData startupConf = Config::Read();
    MmrCategory savedCat = StringToMmrCategory(startupConf.mmr_category);
    if (!startupConf.show_extra_playlists && IsExtraMmrCategory(savedCat)) {
        savedCat = MmrCategory::Best;
    }
    g_state->ui.rosterMmrCategory.store(savedCat);
    if (savedCat != MmrCategory::Best) {
        g_state->ui.graphMmrCategory.store(savedCat);
    }
    // Stats API config check
    std::string apiPath = startupConf.rocket_league_stats_api_config_path;
    if (apiPath.empty()) {
        apiPath = StatsApiConfig::DetectConfigPath();
    }
    StatsApiConfig::CheckResult checkRes = StatsApiConfig::VerifyConfig(apiPath, startupConf.port);
    if (startupConf.check_stats_api_config_on_startup && checkRes.status != StatsApiConfig::Status::Valid) {
        std::wstring title = L"OmniStats Stats API Setup";
        std::wstring text = L"Rocket League Stats API is disabled or misconfigured. OmniStats needs PacketSendRate=30 and Port=49123 to read live game data. Fix it now?";
        if (checkRes.rlRunning) {
            text += L"\n\nRocket League must be restarted after fixing this.";
        }
        int msgRes = MessageBoxW(NULL, text.c_str(), title.c_str(), MB_YESNO | MB_ICONWARNING | MB_SYSTEMMODAL);
        if (msgRes == IDYES) {
            StatsApiConfig::Status fixStatus = StatsApiConfig::FixConfig(apiPath, startupConf.port);
            if (fixStatus == StatsApiConfig::Status::Valid) {
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
    if (updateConf.check_for_updates && !updateConf.enable_auto_updates) {
        ExternalUpdaterLauncher::StartBackgroundUpdateCheck(g_state);
    }
    auto dbManager = std::make_shared<DatabaseManager>(g_state);
    (void)dbManager->Initialize(Storage::GetDataDirectory() + Storage::APP_NAME +
                                ".db");
    TelemetryManager::Initialize(dbManager);
    // Start Backend Threads
    std::shared_ptr<MMRFetcher> mmrFetcher =
        std::make_shared<MMRFetcher>(g_state);
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

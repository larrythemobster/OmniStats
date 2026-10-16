#include "core/tournaments/TournamentSchedule.hpp"
#define WIN32_LEAN_AND_MEAN
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iostream>
#include <memory>
#include <thread>
#include <cstdlib>
#include <exception>
#include <io.h>
#include "core/config/Config.hpp"
#include "core/accounts/AccountManager.hpp"
#include "platform/InputManager.hpp"
#include "core/state/SessionState.hpp"
#include "utils/Storage.hpp"
#include "utils/LogRotation.hpp"
#include "database/DatabaseManager.hpp"
#include "network/DiscordManager.hpp"
#include "network/api/AccountClient.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/mmr/MMRFetcher.hpp"
#include "network/api/ReplayUploader.hpp"
#include "network/api/StatsClient.hpp"
#include "network/telemetry/TelemetryManager.hpp"
#include "network/api/RemoteConfig.hpp"
#include "ui/Overlay.hpp"
#include "core/config/StatsApiConfig.hpp"
#include <curl/curl.h>
#include <dbghelp.h>
#include <windows.h>

namespace {
    struct ScopedHandleDeleter {
        using pointer = HANDLE;
        void operator()(HANDLE h) const noexcept {
            if (h != nullptr && h != INVALID_HANDLE_VALUE) {
                CloseHandle(h);
            }
        }
    };
    using ScopedWinHandle = std::unique_ptr<HANDLE, ScopedHandleDeleter>;
} // namespace

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
static HANDLE g_fatalLogHandle = INVALID_HANDLE_VALUE;
LONG WINAPI
GlobalUnhandledExceptionFilter(struct _EXCEPTION_POINTERS* exceptionInfo) {
    ScopedWinHandle hFile(CreateFileW(g_crashDumpPath, GENERIC_WRITE, 0, NULL,
                                      CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL));
    if (hFile) {
        MINIDUMP_EXCEPTION_INFORMATION mdei;
        mdei.ThreadId = GetCurrentThreadId();
        mdei.ExceptionPointers = exceptionInfo;
        mdei.ClientPointers = FALSE;
        MINIDUMP_TYPE dumpType = MiniDumpNormal;
#ifndef NDEBUG
        dumpType = static_cast<MINIDUMP_TYPE>(MiniDumpNormal | MiniDumpWithThreadInfo | MiniDumpWithUnloadedModules);
#endif
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile.get(),
                          dumpType, &mdei, NULL, NULL);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

[[noreturn]] static void RecordFatalTermination(const char* reason) {
    static volatile LONG handling = 0;
    if (InterlockedExchange(&handling, 1) == 0) {
        DWORD written = 0;
        WriteFile(g_fatalLogHandle, "[Fatal] ", 8, &written, nullptr);
        WriteFile(g_fatalLogHandle, reason, static_cast<DWORD>(std::strlen(reason)), &written, nullptr);
        WriteFile(g_fatalLogHandle, "\n", 1, &written, nullptr);
        CONTEXT context{};
        RtlCaptureContext(&context);
        EXCEPTION_RECORD record{};
        record.ExceptionCode = 0x40000015;
        record.ExceptionFlags = EXCEPTION_NONCONTINUABLE;
        EXCEPTION_POINTERS pointers{&record, &context};
        GlobalUnhandledExceptionFilter(&pointers);
    }
    TerminateProcess(GetCurrentProcess(), EXIT_FAILURE);
    std::_Exit(EXIT_FAILURE);
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
    AccountClient::EnsureSodiumInitialized();
    Storage::InitializeEnvironment();
    std::string crashFile = Storage::GetDataDirectory() + "crash_pending.dmp";
    MultiByteToWideChar(CP_UTF8, 0, crashFile.c_str(), -1, g_crashDumpPath, MAX_PATH);
    LogRotation::RotateLogs(Storage::GetDataDirectory(), Storage::APP_NAME + std::string("_log.txt"), 5);
    // Redirect stdout to a log file in APPDATA
    std::string logPath =
        Storage::GetDataDirectory() + Storage::APP_NAME + "_log.txt";
    // freopen_s opens without sharing: a second process could not open the log, lost stdout, and failed fast on its first write.
#pragma warning(suppress : 4996)
    FILE* outStream = std::freopen(logPath.c_str(), "w", stdout);
    if (outStream) {
        setvbuf(outStream, NULL, _IONBF, 0);
    } else {
#pragma warning(suppress : 4996)
        (void)std::freopen("NUL", "w", stdout);
    }
    if (outStream) g_fatalLogHandle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
    FILE* errStream = nullptr;
    freopen_s(&errStream, "NUL", "w", stderr);
    if (errStream) {
        if (outStream) _dup2(_fileno(stdout), _fileno(stderr));
        setvbuf(errStream, nullptr, _IONBF, 0);
    }
    std::set_terminate([] { RecordFatalTermination("Unhandled C++ termination"); });
    _set_purecall_handler([] { RecordFatalTermination("Pure virtual function call"); });
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned int, uintptr_t) {
        RecordFatalTermination("Invalid CRT parameter");
    });
    std::cout << "OmniStats Starting...\n";
    Config::Load();
    // Network services wait for the Privacy Policy and Terms, which the first-run window collects.
    const bool consentedAtStartup = Config::HasCurrentConsent(Config::Read());
    if (consentedAtStartup) AccountClient::Instance().SyncFromConfig();
    ConfigData startupUpdateConf = Config::Read();
    if (consentedAtStartup && startupUpdateConf.enable_auto_updates && ExternalUpdaterLauncher::RunStartupUpdateCheck()) {
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
    auto dbManager = std::make_shared<DatabaseManager>(g_state);
    if (!dbManager->Initialize(Storage::GetDataDirectory() + Storage::APP_NAME + ".db")) {
        std::cerr << "[Database] Initialization failed. Match recording is unavailable.\n";
        MessageBoxW(nullptr,
                    L"OmniStats could not open or upgrade your local database and cannot save matches.\n\n"
                    L"Check disk space and access to your OmniStats data folder, then restart the app.",
                    L"OmniStats database error", MB_OK | MB_ICONERROR);
        Config::ShutdownSaver();
        curl_global_cleanup();
        return 1;
    }
    const auto startConsentedServices = [&g_state, &dbManager](bool syncAccount) {
        if (syncAccount) AccountClient::Instance().SyncFromConfig();
        if (!Config::Read().enable_auto_updates) {
            ExternalUpdaterLauncher::StartBackgroundUpdateCheck(
                g_state, ExternalUpdaterLauncher::BackgroundUpdateCheckReason::Initial);
        }
        TelemetryManager::Initialize(dbManager);
        RemoteConfig::Instance().Start();
        Tournaments::TournamentScheduleService::Instance().Start();
    };
    if (consentedAtStartup) startConsentedServices(false);
    // Start Backend Threads
    std::shared_ptr<MMRFetcher> mmrFetcher =
        std::make_shared<MMRFetcher>(g_state, dbManager);
    mmrFetcher->Start();
    auto accountManager = std::make_shared<AccountManagerService>(dbManager, mmrFetcher);
    accountManager->Start();
    accountManager->QueueStartupRefresh();
    StatsClient statsClient(g_state, mmrFetcher, dbManager);
    statsClient.SetAccountManager(accountManager);
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
        uploader = std::make_unique<ReplayUploader>(g_state, dbManager);
        uploader->Start();
    }
    Overlay overlay(g_state, dbManager, accountManager);
    if (!overlay.Initialize()) {
        Tournaments::TournamentScheduleService::Instance().Stop();
        RemoteConfig::Instance().Shutdown();
        TelemetryManager::Shutdown();
        Config::ShutdownSaver();
        curl_global_cleanup();
        return 1;
    }
    if (!consentedAtStartup) overlay.SetConsentHandler([&startConsentedServices] { startConsentedServices(true); });
    overlay.RunLoop();
    std::cout << "[Main] Shutting down components...\n";
    inputManager.Stop();
    std::cout << "[Main] InputManager stopped.\n";
    statsClient.Stop();
    std::cout << "[Main] StatsClient stopped.\n";
    mmrFetcher->Stop();
    std::cout << "[Main] MMRFetcher stopped.\n";
    accountManager->Stop();
    std::cout << "[Main] AccountManager stopped.\n";
    if (discordManager) {
        discordManager->Shutdown();
        std::cout << "[Main] DiscordManager stopped.\n";
    }
    if (uploader) {
        uploader->Stop();
        std::cout << "[Main] ReplayUploader stopped.\n";
    }
    Tournaments::TournamentScheduleService::Instance().Stop();
    AccountClient::Instance().Shutdown();
    RemoteConfig::Instance().Shutdown();
    TelemetryManager::Shutdown();
    std::cout << "[Main] TelemetryManager stopped.\n";
    Config::ShutdownSaver();
    std::cout << "[Main] Config Saver stopped.\n";
    ExternalUpdaterLauncher::ShutdownBackgroundTasks();
    curl_global_cleanup();
    std::cout << "[Main] Cleanup complete. Exiting.\n";
    return 0;
}

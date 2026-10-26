#include "app/ProcessSetup.hpp"
#include "network/api/AccountClient.hpp"
#include "utils/LogRotation.hpp"
#include "utils/Storage.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <curl/curl.h>
#include <dbghelp.h>
#include <exception>
#include <io.h>
#include <memory>
#include <string>

#pragma comment(lib, "dbghelp.lib")

namespace AppShell {

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

        void EnablePerMonitorDpiAwareness() {
            HMODULE user32 = GetModuleHandleW(L"user32.dll");
            if (!user32) return;

            using SetProcessDpiAwarenessContextFn = BOOL(WINAPI*)(HANDLE);
            const auto setContext = reinterpret_cast<SetProcessDpiAwarenessContextFn>(
                GetProcAddress(user32, "SetProcessDpiAwarenessContext"));
            if (setContext) {
                const HANDLE perMonitorV2 = reinterpret_cast<HANDLE>(static_cast<INT_PTR>(-4));
                if (setContext(perMonitorV2)) return;
                if (GetLastError() == ERROR_ACCESS_DENIED) return;
            }

            using SetProcessDPIAwareFn = BOOL(WINAPI*)();
            const auto setLegacyAware = reinterpret_cast<SetProcessDPIAwareFn>(
                GetProcAddress(user32, "SetProcessDPIAware"));
            if (setLegacyAware) setLegacyAware();
        }

        wchar_t g_crashDumpPath[MAX_PATH] = {0};
        HANDLE g_fatalLogHandle = INVALID_HANDLE_VALUE;

        LONG WINAPI GlobalUnhandledExceptionFilter(struct _EXCEPTION_POINTERS* exceptionInfo) {
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

        [[noreturn]] void RecordFatalTermination(const char* reason) {
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
    } // namespace

    void ProcessSetup::InitializeEarly() {
        EnablePerMonitorDpiAwareness();
        SetUnhandledExceptionFilter(GlobalUnhandledExceptionFilter);
    }

    bool ProcessSetup::HandleCommandLine(int argc, char* argv[]) {
        for (int i = 1; i < argc; ++i) {
            std::string arg = argv[i];
            if (arg == "--apply-update") {
                MessageBoxA(NULL,
                            "OmniStats is updating to a new major version, which requires re-running the installer.\n\n"
                            "Please download and run the latest installer from the official website: https://omnistats.org",
                            "OmniStats Update Required",
                            MB_OK | MB_ICONINFORMATION | MB_SETFOREGROUND);
                return false;
            }
        }
        return true;
    }

    ProcessSetup::ProcessSetup() {

        // Initialize Curl globally for static linking
        curl_global_init(CURL_GLOBAL_ALL);
        AccountClient::EnsureSodiumInitialized();
        Storage::InitializeEnvironment();

        std::string crashFile = Storage::GetDataDirectory() + "crash_pending.dmp";
        MultiByteToWideChar(CP_UTF8, 0, crashFile.c_str(), -1, g_crashDumpPath, MAX_PATH);

        LogRotation::RotateLogs(Storage::GetDataDirectory(), Storage::APP_NAME + std::string("_log.txt"), 5);

        // Redirect stdout to a log file in APPDATA
        std::string logPath = Storage::GetDataDirectory() + Storage::APP_NAME + "_log.txt";
#pragma warning(suppress : 4996)
        FILE* outStream = std::freopen(logPath.c_str(), "w", stdout);
        if (outStream) {
            setvbuf(outStream, NULL, _IONBF, 0);
            g_fatalLogHandle = reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(stdout)));
        } else {
#pragma warning(suppress : 4996)
            (void)std::freopen("NUL", "w", stdout);
        }

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

        m_initialized = true;
    }

    ProcessSetup::~ProcessSetup() {
        if (m_initialized) {
            curl_global_cleanup();
        }
    }

} // namespace AppShell

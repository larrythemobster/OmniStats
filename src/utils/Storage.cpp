#include "utils/Storage.hpp"
#include "utils/Env.hpp"
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#include <algorithm>
#include <filesystem>
#include <iostream>
#include <system_error>
#include <utility>

namespace fs = std::filesystem;

namespace Storage {
    namespace {
        std::string dataDirectoryOverride;
    }

    void SetDataDirectoryOverride(std::string path) {
        dataDirectoryOverride = std::move(path);
    }

    std::string GetDataDirectory() {
        if (!dataDirectoryOverride.empty()) {
            return dataDirectoryOverride;
        }
        if (const std::optional<std::string> appData = Env::Get("APPDATA")) {
            return *appData + "\\" + APP_NAME + "\\";
        }
        return "data\\"; // Fallback
    }

    void InitializeEnvironment() {
        const std::string dir = GetDataDirectory();
        std::error_code ec;
        if (!fs::exists(dir, ec)) {
            if (fs::create_directories(dir, ec)) {
                std::cout << "[Storage] Created data directory." << std::endl;
            } else if (ec) {
                std::cerr << "[Storage] Failed to create data directory: " << ec.message() << std::endl;
            }
        }
    }

    std::string GetRocketLeagueTAGameDirectory() {
        PWSTR path = nullptr;
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &path)) && path) {
            const int sizeNeeded = WideCharToMultiByte(CP_UTF8, 0, path, -1, nullptr, 0, nullptr, nullptr);
            std::string documents;
            if (sizeNeeded > 1) {
                documents.resize(static_cast<size_t>(sizeNeeded));
                WideCharToMultiByte(CP_UTF8, 0, path, -1, documents.data(), sizeNeeded, nullptr, nullptr);
                documents.pop_back();
            }
            CoTaskMemFree(path);
            if (!documents.empty()) return documents + "\\My Games\\Rocket League\\TAGame\\";
        }
        if (const std::optional<std::string> userProfile = Env::Get("USERPROFILE")) {
            return *userProfile + "\\Documents\\My Games\\Rocket League\\TAGame\\";
        }
        return {};
    }

    bool WriteFileAtomically(const fs::path& path, std::string_view contents) {
        fs::path tempPath = path;
        tempPath += L".tmp";
        HANDLE file = CreateFileW(tempPath.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) return false;

        bool ok = true;
        size_t offset = 0;
        while (ok && offset < contents.size()) {
            const DWORD chunk = static_cast<DWORD>(std::min<size_t>(contents.size() - offset, 1u << 30));
            DWORD written = 0;
            ok = WriteFile(file, contents.data() + offset, chunk, &written, nullptr) && written == chunk;
            offset += written;
        }
        // MOVEFILE_WRITE_THROUGH only flushes the rename; the data must reach disk first.
        ok = ok && FlushFileBuffers(file);
        CloseHandle(file);

        ok = ok && MoveFileExW(tempPath.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
        if (!ok) {
            const DWORD error = GetLastError();
            DeleteFileW(tempPath.c_str());
            SetLastError(error);
        }
        return ok;
    }

}

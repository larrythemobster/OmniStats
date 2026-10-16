#include "utils/Storage.hpp"
#include "utils/Env.hpp"
#include <windows.h>
#include <shlobj.h>
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "shell32.lib")
#include <filesystem>
#include <iostream>
#include <system_error>

namespace fs = std::filesystem;

namespace Storage {
    std::string GetDataDirectory() {
#ifdef OMNISTATS_TEST_ENVIRONMENT
        const std::optional<std::string> temp = Env::Get("TEMP") ? Env::Get("TEMP") : Env::Get("TMP");
        return temp.value_or(".") + "\\omnistats_test\\";
#else
        if (const std::optional<std::string> appData = Env::Get("APPDATA")) {
            return *appData + "\\" + APP_NAME + "\\";
        }
        return "data\\"; // Fallback
#endif
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

}

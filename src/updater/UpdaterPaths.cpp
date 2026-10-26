#include "updater/UpdaterPaths.hpp"
#include "utils/Env.hpp"
#include <filesystem>
#include <windows.h>
#include <utility>

namespace fs = std::filesystem;

namespace UpdaterCommon {
    namespace {
        std::string appDataDirOverride;
    }

    void SetAppDataDirOverride(std::string path) {
        appDataDirOverride = std::move(path);
    }

    std::string GetAppDataDir() {
        if (!appDataDirOverride.empty()) {
            return appDataDirOverride;
        }
        if (const std::optional<std::string> appData = Env::Get("APPDATA")) {
            return *appData + "\\omnistats\\";
        }
        return "data\\";
    }

    std::string GetLocalAppDataDir() {
        if (const std::optional<std::string> localAppData = Env::Get("LOCALAPPDATA")) {
            return *localAppData + "\\OmniStats\\";
        }
        if (const std::optional<std::string> userProfile = Env::Get("USERPROFILE")) {
            return *userProfile + "\\AppData\\Local\\OmniStats\\";
        }
        return "C:\\OmniStats\\";
    }

    bool EnsureDirExists(const std::string& path) {
        try {
            if (!fs::exists(path)) {
                return fs::create_directories(path);
            }
            return true;
        } catch (...) {
            return false;
        }
    }

} // namespace UpdaterCommon

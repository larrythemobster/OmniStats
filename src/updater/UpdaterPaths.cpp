#include "updater/UpdaterPaths.hpp"
#include "utils/Env.hpp"
#include <filesystem>
#include <windows.h>

namespace fs = std::filesystem;

namespace UpdaterCommon {

    std::string GetAppDataDir() {
#ifdef OMNISTATS_TEST_ENVIRONMENT
        const std::optional<std::string> temp = Env::Get("TEMP") ? Env::Get("TEMP") : Env::Get("TMP");
        return temp.value_or(".") + "\\omnistats_test\\";
#else
        if (const std::optional<std::string> appData = Env::Get("APPDATA")) {
            return *appData + "\\omnistats\\";
        }
        return "data\\";
#endif
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

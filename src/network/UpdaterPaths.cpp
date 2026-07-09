#include "network/UpdaterPaths.hpp"
#include <cstdlib>
#include <filesystem>
#include <windows.h>

namespace fs = std::filesystem;

namespace UpdaterCommon {

std::string GetAppDataDir() {
#ifdef OMNISTATS_TEST_ENVIRONMENT
    const char* temp = std::getenv("TEMP");
    if (!temp) {
        temp = std::getenv("TMP");
    }
    return std::string(temp ? temp : ".") + "\\omnistats_test\\";
#else
    const char* appData = std::getenv("APPDATA");
    if (appData) {
        return std::string(appData) + "\\omnistats\\";
    }
    return "data\\";
#endif
}

std::string GetLocalAppDataDir() {
    const char* localAppData = std::getenv("LOCALAPPDATA");
    if (localAppData) {
        return std::string(localAppData) + "\\OmniStats\\";
    }
    const char* userProfile = std::getenv("USERPROFILE");
    if (userProfile) {
        return std::string(userProfile) + "\\AppData\\Local\\OmniStats\\";
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

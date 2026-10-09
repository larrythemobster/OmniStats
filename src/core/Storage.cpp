#include "Storage.hpp"
#include <cstdlib>
#include <filesystem>
#include <iostream>

namespace fs = std::filesystem;

namespace Storage {
    std::string GetDataDirectory() {
#ifdef OMNISTATS_TEST_ENVIRONMENT
        const char* temp = std::getenv("TEMP");
        if (!temp) {
            temp = std::getenv("TMP");
        }
        return std::string(temp ? temp : ".") + "\\omnistats_test\\";
#else
        const char* appData = std::getenv("APPDATA");
        if (appData) {
            return std::string(appData) + "\\" + APP_NAME + "\\";
        }
        return "data\\"; // Fallback
#endif
    }

    void InitializeEnvironment() {
        std::string dir = GetDataDirectory();
        if (!fs::exists(dir)) {
            fs::create_directories(dir);
            std::cout << "[Storage] Created data directory." << std::endl;
        }
    }

}

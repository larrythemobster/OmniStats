#include "utils/Storage.hpp"
#include "utils/Env.hpp"
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

}

#include "Storage.hpp"
#include <cstdlib>
#include <fstream>
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

    void AppendLineToFile(const std::string& filename, const nlohmann::json& data) {
        // Open file in append mode (std::ios::app)
        std::string filepath = GetDataDirectory() + filename;
        std::ofstream file(filepath, std::ios::app);
        if (file.is_open()) {
            file << data.dump() << "\n";
            file.flush();
        } else {
            std::cout << "[Storage] Failed to open data file for writing: " << filename << "\n";
        }
    }

    void AppendMatchSync(const nlohmann::json& record) {
        AppendLineToFile("matches.jsonl", record);
    }

    void AppendMMRHistory(const nlohmann::json& entry) {
        AppendLineToFile("mmr_history.jsonl", entry);
    }
}

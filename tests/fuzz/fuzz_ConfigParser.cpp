#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <fstream>
#include <string>

#include "core/config/Config.hpp"
#include "utils/Storage.hpp"

extern "C" int LLVMFuzzerInitialize(int*, char***) {
    Storage::InitializeEnvironment();
    return 0;
}

// Feeds arbitrary bytes through the real config.json load path, including corrupt-file recovery.
extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string path = Storage::GetDataDirectory() + "config.json";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out.write(reinterpret_cast<const char*>(data), static_cast<std::streamsize>(size));
    }
    Config::Load();
    const ConfigData config = Config::Read();
    if (config.match_summary_seconds < 5 || config.match_summary_seconds > 60) std::abort();
    return 0;
}

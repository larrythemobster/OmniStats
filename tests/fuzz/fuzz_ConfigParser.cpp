#include <stdint.h>
#include <stddef.h>
#include <string>
#include <nlohmann/json.hpp>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    if (size == 0) return 0;

    std::string input(reinterpret_cast<const char*>(data), size);

    // Invalid JSON is expected; this target only checks for crashes.
    try {
        auto parsed = nlohmann::json::parse(input);
    } catch (const nlohmann::json::exception&) {
    } catch (...) {
    }

    return 0;
}

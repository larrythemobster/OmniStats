#pragma once

#include <cstdint>
#include <filesystem>
#include <initializer_list>
#include <string>
#include <string_view>
#include <vector>

// Installed Windows fonts, enumerated through DirectWrite so per-user fonts and
// variable fonts resolve to real family names and files.
namespace SystemFonts {
    struct Face {
        std::filesystem::path file;
        uint32_t faceIndex = 0;
        int weight = 400;
    };

    // Upright text families that can draw Latin letters, sorted case-insensitively.
    std::vector<std::string> ListFamilies();

    // Upright, non-simulated local faces of `family` nearest to each requested
    // weight, deduplicated. Empty when the family is not installed.
    std::vector<Face> FindFaces(std::string_view family, std::initializer_list<int> weights);
}

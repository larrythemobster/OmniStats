#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace Core {

    struct ZipEntryInfo {
        std::string filename;
        uint32_t uncompressedSize = 0;
        uint32_t compressedSize = 0;
        uint32_t crc32 = 0;
        std::string data;
    };

    class ZipWriter {
      public:
        ZipWriter() = default;
        ~ZipWriter() = default;

        bool AddFile(const std::string& filenameInZip, const std::string& content);
        std::vector<uint8_t> Finalize();
        bool WriteToFile(const std::filesystem::path& destinationPath);

      private:
        struct Entry {
            std::string filename;
            uint32_t crc32 = 0;
            uint32_t compressedSize = 0;
            uint32_t uncompressedSize = 0;
            uint32_t localHeaderOffset = 0;
            uint16_t dosTime = 0;
            uint16_t dosDate = 0;
            uint16_t method = 8;
            std::vector<uint8_t> compressedData;
        };

        std::vector<Entry> m_entries;
    };

    // Test helper: reads back and inflates all entries.
    bool ReadZipEntries(const std::vector<uint8_t>& zipData, std::vector<ZipEntryInfo>& outEntries);

} // namespace Core

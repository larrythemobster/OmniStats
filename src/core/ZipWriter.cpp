#include "core/ZipWriter.hpp"

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <zlib.h>
#include <chrono>
#include <ctime>
#include <fstream>
#include <system_error>

namespace Core {

    namespace {

        inline void WriteU16(std::vector<uint8_t>& buf, uint16_t v) {
            buf.push_back(static_cast<uint8_t>(v & 0xFF));
            buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
        }

        inline void WriteU32(std::vector<uint8_t>& buf, uint32_t v) {
            buf.push_back(static_cast<uint8_t>(v & 0xFF));
            buf.push_back(static_cast<uint8_t>((v >> 8) & 0xFF));
            buf.push_back(static_cast<uint8_t>((v >> 16) & 0xFF));
            buf.push_back(static_cast<uint8_t>((v >> 24) & 0xFF));
        }

        inline uint16_t ReadU16(const uint8_t* p) {
            return static_cast<uint16_t>(p[0]) | (static_cast<uint16_t>(p[1]) << 8);
        }

        inline uint32_t ReadU32(const uint8_t* p) {
            return static_cast<uint32_t>(p[0]) |
                   (static_cast<uint32_t>(p[1]) << 8) |
                   (static_cast<uint32_t>(p[2]) << 16) |
                   (static_cast<uint32_t>(p[3]) << 24);
        }

        void GetCurrentDosDateTime(uint16_t& dosDate, uint16_t& dosTime) {
            const auto now = std::chrono::system_clock::now();
            const std::time_t tt = std::chrono::system_clock::to_time_t(now);
            std::tm tm{};
#if defined(_WIN32)
            localtime_s(&tm, &tt);
#else
            localtime_r(&tt, &tm);
#endif
            int year = tm.tm_year + 1900;
            if (year < 1980) year = 1980;
            dosDate = static_cast<uint16_t>(((year - 1980) << 9) | ((tm.tm_mon + 1) << 5) | tm.tm_mday);
            dosTime = static_cast<uint16_t>((tm.tm_hour << 11) | (tm.tm_min << 5) | (tm.tm_sec / 2));
        }

    } // namespace

    bool ZipWriter::AddFile(const std::string& filenameInZip, const std::string& content) {
        Entry entry;
        entry.filename = filenameInZip;
        entry.uncompressedSize = static_cast<uint32_t>(content.size());
        GetCurrentDosDateTime(entry.dosDate, entry.dosTime);

        if (content.empty()) {
            entry.method = 0; // Store
            entry.crc32 = 0;
            entry.compressedSize = 0;
            entry.compressedData.clear();
            m_entries.push_back(std::move(entry));
            return true;
        }

        uLong crc = crc32(0L, Z_NULL, 0);
        crc = crc32(crc, reinterpret_cast<const Bytef*>(content.data()), static_cast<uInt>(content.size()));
        entry.crc32 = static_cast<uint32_t>(crc);

        z_stream strm{};
        // -MAX_WBITS produces a raw deflate stream without zlib header/trailer
        if (deflateInit2(&strm, Z_DEFAULT_COMPRESSION, Z_DEFLATED, -MAX_WBITS, 8, Z_DEFAULT_STRATEGY) != Z_OK) {
            return false;
        }

        strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(content.data()));
        strm.avail_in = static_cast<uInt>(content.size());

        const uLong bound = deflateBound(&strm, strm.avail_in);
        entry.compressedData.resize(bound);
        strm.next_out = entry.compressedData.data();
        strm.avail_out = static_cast<uInt>(entry.compressedData.size());

        const int ret = deflate(&strm, Z_FINISH);
        if (ret != Z_STREAM_END) {
            deflateEnd(&strm);
            return false;
        }
        entry.compressedData.resize(strm.total_out);
        entry.compressedSize = static_cast<uint32_t>(strm.total_out);
        entry.method = 8; // Deflate
        deflateEnd(&strm);

        m_entries.push_back(std::move(entry));
        return true;
    }

    std::vector<uint8_t> ZipWriter::Finalize() {
        std::vector<uint8_t> buffer;

        for (auto& entry : m_entries) {
            entry.localHeaderOffset = static_cast<uint32_t>(buffer.size());
            WriteU32(buffer, 0x04034b50); // Local header signature
            WriteU16(buffer, 20);         // Version needed to extract (2.0)
            WriteU16(buffer, 0x0800);     // Flags (UTF-8 filename)
            WriteU16(buffer, entry.method);
            WriteU16(buffer, entry.dosTime);
            WriteU16(buffer, entry.dosDate);
            WriteU32(buffer, entry.crc32);
            WriteU32(buffer, entry.compressedSize);
            WriteU32(buffer, entry.uncompressedSize);
            WriteU16(buffer, static_cast<uint16_t>(entry.filename.size()));
            WriteU16(buffer, 0); // Extra field length
            buffer.insert(buffer.end(), entry.filename.begin(), entry.filename.end());
            buffer.insert(buffer.end(), entry.compressedData.begin(), entry.compressedData.end());
        }

        const uint32_t cdOffset = static_cast<uint32_t>(buffer.size());
        for (const auto& entry : m_entries) {
            WriteU32(buffer, 0x02014b50); // Central directory header signature
            WriteU16(buffer, 20);         // Version made by
            WriteU16(buffer, 20);         // Version needed to extract
            WriteU16(buffer, 0x0800);     // Flags (UTF-8 filename)
            WriteU16(buffer, entry.method);
            WriteU16(buffer, entry.dosTime);
            WriteU16(buffer, entry.dosDate);
            WriteU32(buffer, entry.crc32);
            WriteU32(buffer, entry.compressedSize);
            WriteU32(buffer, entry.uncompressedSize);
            WriteU16(buffer, static_cast<uint16_t>(entry.filename.size()));
            WriteU16(buffer, 0); // Extra field length
            WriteU16(buffer, 0); // Comment length
            WriteU16(buffer, 0); // Disk number start
            WriteU16(buffer, 0); // Internal file attributes
            WriteU32(buffer, 0); // External file attributes
            WriteU32(buffer, entry.localHeaderOffset);
            buffer.insert(buffer.end(), entry.filename.begin(), entry.filename.end());
        }
        const uint32_t cdSize = static_cast<uint32_t>(buffer.size() - cdOffset);

        WriteU32(buffer, 0x06054b50);                              // EOCD signature
        WriteU16(buffer, 0);                                       // Disk number
        WriteU16(buffer, 0);                                       // Disk with CD
        WriteU16(buffer, static_cast<uint16_t>(m_entries.size())); // CD entries on this disk
        WriteU16(buffer, static_cast<uint16_t>(m_entries.size())); // Total CD entries
        WriteU32(buffer, cdSize);
        WriteU32(buffer, cdOffset);
        WriteU16(buffer, 0); // Comment length

        return buffer;
    }

    bool ZipWriter::WriteToFile(const std::filesystem::path& destinationPath) {
        const std::vector<uint8_t> data = Finalize();
        const std::filesystem::path tempPath = destinationPath.string() + ".tmp." + std::to_string(GetCurrentProcessId());

        std::ofstream out(tempPath, std::ios::binary);
        if (!out.is_open()) {
            return false;
        }
        out.write(reinterpret_cast<const char*>(data.data()), static_cast<std::streamsize>(data.size()));
        out.close();
        if (!out.good()) {
            std::error_code ec;
            std::filesystem::remove(tempPath, ec);
            return false;
        }

#if defined(_WIN32)
        if (!MoveFileExW(tempPath.c_str(), destinationPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            std::error_code ec;
            std::filesystem::remove(tempPath, ec);
            return false;
        }
#else
        std::error_code ec;
        std::filesystem::rename(tempPath, destinationPath, ec);
        if (ec) {
            std::filesystem::remove(tempPath, ec);
            return false;
        }
#endif
        return true;
    }

    bool ReadZipEntries(const std::vector<uint8_t>& zipData, std::vector<ZipEntryInfo>& outEntries) {
        outEntries.clear();
        if (zipData.size() < 22) return false;

        size_t eocdPos = 0;
        bool foundEocd = false;
        const size_t maxScan = std::min<size_t>(zipData.size(), 65557);
        for (size_t i = zipData.size() - 22; i + maxScan >= zipData.size(); --i) {
            if (ReadU32(&zipData[i]) == 0x06054b50) {
                eocdPos = i;
                foundEocd = true;
                break;
            }
            if (i == 0) break;
        }
        if (!foundEocd) return false;

        const uint16_t totalEntries = ReadU16(&zipData[eocdPos + 10]);
        const uint32_t cdSize = ReadU32(&zipData[eocdPos + 12]);
        const uint32_t cdOffset = ReadU32(&zipData[eocdPos + 16]);

        if (cdOffset + cdSize > zipData.size()) return false;

        size_t cursor = cdOffset;
        for (uint16_t e = 0; e < totalEntries; ++e) {
            if (cursor + 46 > zipData.size()) return false;
            if (ReadU32(&zipData[cursor]) != 0x02014b50) return false;

            const uint16_t method = ReadU16(&zipData[cursor + 10]);
            const uint32_t expectedCrc = ReadU32(&zipData[cursor + 16]);
            const uint32_t compSize = ReadU32(&zipData[cursor + 20]);
            const uint32_t uncompSize = ReadU32(&zipData[cursor + 24]);
            const uint16_t nameLen = ReadU16(&zipData[cursor + 28]);
            const uint16_t extraLen = ReadU16(&zipData[cursor + 30]);
            const uint16_t commentLen = ReadU16(&zipData[cursor + 32]);
            const uint32_t localOffset = ReadU32(&zipData[cursor + 42]);

            cursor += 46;
            if (cursor + nameLen > zipData.size()) return false;
            std::string filename(reinterpret_cast<const char*>(&zipData[cursor]), nameLen);
            cursor += nameLen + extraLen + commentLen;

            if (localOffset + 30 > zipData.size()) return false;
            if (ReadU32(&zipData[localOffset]) != 0x04034b50) return false;
            const uint16_t localNameLen = ReadU16(&zipData[localOffset + 26]);
            const uint16_t localExtraLen = ReadU16(&zipData[localOffset + 28]);
            const size_t dataOffset = localOffset + 30 + localNameLen + localExtraLen;

            if (dataOffset + compSize > zipData.size()) return false;

            std::string decompressed;
            if (compSize == 0 && uncompSize == 0) {
                decompressed.clear();
            } else if (method == 0) { // Store
                decompressed.assign(reinterpret_cast<const char*>(&zipData[dataOffset]), compSize);
            } else if (method == 8) { // Deflate
                z_stream strm{};
                if (inflateInit2(&strm, -MAX_WBITS) != Z_OK) return false;

                strm.next_in = const_cast<Bytef*>(&zipData[dataOffset]);
                strm.avail_in = compSize;

                decompressed.resize(uncompSize);
                strm.next_out = reinterpret_cast<Bytef*>(decompressed.data());
                strm.avail_out = uncompSize;

                const int ret = inflate(&strm, Z_FINISH);
                inflateEnd(&strm);
                if (ret != Z_STREAM_END && ret != Z_OK) return false;
            } else {
                return false;
            }

            uLong actualCrc = crc32(0L, Z_NULL, 0);
            if (!decompressed.empty()) {
                actualCrc = crc32(actualCrc, reinterpret_cast<const Bytef*>(decompressed.data()), static_cast<uInt>(decompressed.size()));
            }
            if (static_cast<uint32_t>(actualCrc) != expectedCrc) return false;

            ZipEntryInfo info;
            info.filename = std::move(filename);
            info.uncompressedSize = uncompSize;
            info.compressedSize = compSize;
            info.crc32 = expectedCrc;
            info.data = std::move(decompressed);
            outEntries.push_back(std::move(info));
        }

        return true;
    }

} // namespace Core

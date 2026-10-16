#include "core/tournaments/RegionDetector.hpp"
#include "utils/Storage.hpp"
#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <vector>

namespace Tournaments {
    namespace {
        class Reader {
          public:
            explicit Reader(std::span<const uint8_t> bytes) : data(bytes) {}
            template <class T>
            bool Value(T& value) {
                if (sizeof(T) > data.size() - pos) return false;
                std::memcpy(&value, data.data() + pos, sizeof(T));
                pos += sizeof(T);
                return true;
            }
            bool String(std::string& value) {
                int32_t length = 0;
                if (!Value(length) || length <= 0 || length > 256 || static_cast<size_t>(length) > data.size() - pos) return false;
                if (data[pos + static_cast<size_t>(length) - 1] != 0) return false;
                value.assign(reinterpret_cast<const char*>(data.data() + pos), static_cast<size_t>(length) - 1);
                pos += static_cast<size_t>(length);
                return true;
            }
            bool Slice(uint64_t size, std::span<const uint8_t>& out) {
                if (size > data.size() - pos) return false;
                out = data.subspan(pos, static_cast<size_t>(size));
                pos += static_cast<size_t>(size);
                return true;
            }
            bool Property(std::string& name, std::string& type, std::span<const uint8_t>& value) {
                if (!String(name)) return false;
                if (name == "None") return true;
                uint64_t size = 0;
                return String(type) && Value(size) && Slice(size, value);
            }
            std::span<const uint8_t> data;
            size_t pos = 0;
        };
        std::vector<uint8_t> ReadBytes(const std::filesystem::path& path, size_t maxBytes, bool last = false) {
            std::ifstream stream(path, std::ios::binary | std::ios::ate);
            if (!stream) return {};
            const auto end = stream.tellg();
            if (end <= 0) return {};
            const auto length = static_cast<uint64_t>(end);
            if (!last && length > maxBytes) return {};
            const size_t count = static_cast<size_t>(std::min<uint64_t>(length, maxBytes));
            stream.seekg(static_cast<std::streamoff>(length - count));
            std::vector<uint8_t> result(count);
            if (!stream.read(reinterpret_cast<char*>(result.data()), static_cast<std::streamsize>(count))) return {};
            return result;
        }
    }

    std::string RegionGroup(std::string_view region) {
        if (region == "US" || region == "USE" || region == "USW" || region == "USC") return "US";
        if (region == "ASIA" || region == "ASC" || region == "ASM" || region == "JPN" || region == "IND") return "ASIA";
        for (auto code : {"EU", "OCE", "SAM", "ME", "SAF"})
            if (region == code) return code;
        return {};
    }

    std::optional<DetectedRegion> ParseRegionCache(std::span<const uint8_t> bytes) {
        // RegionCache has a 16-byte cache header followed by UE3 property tags.
        if (bytes.size() < 16 || bytes.size() > 1024 * 1024) return std::nullopt;
        Reader root(bytes.subspan(16));
        std::optional<DetectedRegion> best;
        float bestPing = std::numeric_limits<float>::max();
        for (;;) {
            std::string name, type;
            std::span<const uint8_t> value;
            if (!root.Property(name, type, value)) return std::nullopt;
            if (name == "None") break;
            if (name != "CachedRegionPings" || type != "ArrayProperty") continue;
            Reader array(value);
            int32_t count = 0;
            if (!array.Value(count) || count < 0 || count > 256) return std::nullopt;
            for (int32_t i = 0; i < count; ++i) {
                std::string server;
                float ping = -1;
                for (;;) {
                    std::string field, fieldType;
                    std::span<const uint8_t> fieldValue;
                    if (!array.Property(field, fieldType, fieldValue)) return std::nullopt;
                    if (field == "None") break;
                    Reader item(fieldValue);
                    if (field == "Name" && fieldType == "NameProperty" && !item.String(server)) return std::nullopt;
                    if (field == "Ping" && fieldType == "FloatProperty" && (fieldValue.size() != sizeof(float) || !item.Value(ping))) return std::nullopt;
                }
                std::string code = server;
                while (!code.empty() && code.back() >= '0' && code.back() <= '9')
                    code.pop_back();
                if (!RegionGroup(code).empty() && std::isfinite(ping) && ping > 0 && ping < 1 && ping < bestPing) {
                    bestPing = ping;
                    best = DetectedRegion{code, "ping", server, static_cast<int>(std::lround(ping * 1000))};
                }
            }
        }
        return best;
    }

    std::optional<DetectedRegion> ParseMatchRegions(std::string_view log) {
        std::map<std::string, int> counts;
        size_t position = 0;
        std::string winner;
        int highest = 0;
        while (position < log.size()) {
            const size_t end = log.find('\n', position);
            const auto line = log.substr(position, end == std::string_view::npos ? log.size() - position : end - position);
            position = end == std::string_view::npos ? log.size() : end + 1;
            if (line.find("Reservation=") == std::string_view::npos) continue;
            const auto start = line.find("Region=\"");
            if (start == std::string_view::npos) continue;
            const auto close = line.find('"', start + 8);
            if (close == std::string_view::npos) continue;
            std::string region(line.substr(start + 8, close - start - 8));
            if (RegionGroup(region).empty()) continue;
            const int count = ++counts[region];
            if (count >= highest) {
                highest = count;
                winner = std::move(region);
            }
        }
        if (winner.empty()) return std::nullopt;
        return DetectedRegion{winner, "matches", {}, 0};
    }

    std::optional<DetectedRegion> DetectRegion() {
        const auto utf8 = Storage::GetRocketLeagueTAGameDirectory();
        const std::filesystem::path directory(std::u8string_view(reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
        if (directory.empty()) return std::nullopt;
        const auto cache = ReadBytes(directory / "Cache" / "RegionCache.cache", 1024 * 1024);
        if (auto region = ParseRegionCache(cache)) return region;
        const auto log = ReadBytes(directory / "Logs" / "Launch.log", 4 * 1024 * 1024, true);
        if (log.empty()) return std::nullopt;
        return ParseMatchRegions(std::string_view(reinterpret_cast<const char*>(log.data()), log.size()));
    }
}

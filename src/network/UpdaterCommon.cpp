#include "network/UpdaterCommon.hpp"
#include "core/FileHash.hpp"
#include <vector>
#include <sstream>
#include <algorithm>
#include <windows.h>

namespace UpdaterCommon {

    std::string Trim(const std::string& str) {
        size_t first = str.find_first_not_of(" \t\r\n");
        if (first == std::string::npos) {
            return "";
        }
        size_t last = str.find_last_not_of(" \t\r\n");
        return str.substr(first, (last - first + 1));
    }

    bool IsNewerVersion(const std::string& current, const std::string& latest) {
        std::vector<int> curParts, latParts;
        std::stringstream curSS(current), latSS(latest);
        std::string item;

        while (std::getline(curSS, item, '.')) {
            try {
                curParts.push_back(std::stoi(item));
            } catch (...) {
                curParts.push_back(0);
            }
        }
        while (std::getline(latSS, item, '.')) {
            try {
                latParts.push_back(std::stoi(item));
            } catch (...) {
                latParts.push_back(0);
            }
        }

        while (curParts.size() < 3) {
            curParts.push_back(0);
        }
        while (latParts.size() < 3) {
            latParts.push_back(0);
        }

        for (size_t i = 0; i < 3; ++i) {
            if (latParts[i] > curParts[i]) {
                return true;
            }
            if (latParts[i] < curParts[i]) {
                return false;
            }
        }
        return false;
    }

    std::string GetFileVersion(const std::string& filePath) {
        DWORD dummy = 0;
        DWORD size = GetFileVersionInfoSizeA(filePath.c_str(), &dummy);
        if (size == 0) {
            return "";
        }
        std::vector<char> buffer(size);
        if (!GetFileVersionInfoA(filePath.c_str(), 0, size, buffer.data())) {
            return "";
        }
        VS_FIXEDFILEINFO* fileInfo = nullptr;
        UINT len = 0;
        if (!VerQueryValueA(buffer.data(), "\\", reinterpret_cast<LPVOID*>(&fileInfo), &len) || len == 0) {
            return "";
        }
        std::stringstream ss;
        ss << ((fileInfo->dwFileVersionMS >> 16) & 0xffff) << "."
           << (fileInfo->dwFileVersionMS & 0xffff) << "."
           << ((fileInfo->dwFileVersionLS >> 16) & 0xffff);
        return ss.str();
    }

    bool VerifyFileSHA256(const std::string& filePath, const std::string& expectedHash) {
        std::string localHash = CalculateSHA256(filePath);
        if (localHash.empty()) {
            return false;
        }
        std::string cleanExpected = Trim(expectedHash);
        if (cleanExpected.length() >= 64) {
            cleanExpected = cleanExpected.substr(0, 64);
        }
        std::transform(cleanExpected.begin(), cleanExpected.end(), cleanExpected.begin(), ::tolower);
        std::transform(localHash.begin(), localHash.end(), localHash.begin(), ::tolower);
        return localHash == cleanExpected;
    }

} // namespace UpdaterCommon

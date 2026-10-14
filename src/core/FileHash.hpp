#pragma once

#include <fstream>
#include <iomanip>
#include <memory>
#include <sstream>
#include <string>
#include <vector>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

namespace omnistats::utils {

    struct BcryptAlgDeleter {
        using pointer = BCRYPT_ALG_HANDLE;
        void operator()(BCRYPT_ALG_HANDLE h) const noexcept {
            if (h != nullptr) {
                BCryptCloseAlgorithmProvider(h, 0);
            }
        }
    };

    struct BcryptHashDeleter {
        using pointer = BCRYPT_HASH_HANDLE;
        void operator()(BCRYPT_HASH_HANDLE h) const noexcept {
            if (h != nullptr) {
                BCryptDestroyHash(h);
            }
        }
    };

    inline std::string CalculateSHA256(const std::string& filePath) {
        std::ifstream file(filePath, std::ios::binary);
        if (!file) return "";

        BCRYPT_ALG_HANDLE rawAlg = nullptr;
        if (BCryptOpenAlgorithmProvider(&rawAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) {
            return "";
        }
        std::unique_ptr<BCRYPT_ALG_HANDLE, BcryptAlgDeleter> algHandle(rawAlg);

        DWORD cbHashObject = 0;
        DWORD cbHash = 0;
        DWORD cbData = 0;
        if (BCryptGetProperty(algHandle.get(), BCRYPT_OBJECT_LENGTH, reinterpret_cast<PBYTE>(&cbHashObject), sizeof(DWORD), &cbData, 0) < 0 ||
            BCryptGetProperty(algHandle.get(), BCRYPT_HASH_LENGTH, reinterpret_cast<PBYTE>(&cbHash), sizeof(DWORD), &cbData, 0) < 0) {
            return "";
        }

        std::vector<BYTE> hashObject(cbHashObject);
        std::vector<BYTE> hashValue(cbHash);
        BCRYPT_HASH_HANDLE rawHash = nullptr;
        if (BCryptCreateHash(algHandle.get(), &rawHash, hashObject.data(), cbHashObject, nullptr, 0, 0) < 0) {
            return "";
        }
        std::unique_ptr<BCRYPT_HASH_HANDLE, BcryptHashDeleter> hashHandle(rawHash);

        char buffer[4096];
        while (file.read(buffer, sizeof(buffer))) {
            if (BCryptHashData(hashHandle.get(), reinterpret_cast<PBYTE>(buffer), static_cast<ULONG>(file.gcount()), 0) < 0) {
                return "";
            }
        }
        if (file.gcount() > 0) {
            if (BCryptHashData(hashHandle.get(), reinterpret_cast<PBYTE>(buffer), static_cast<ULONG>(file.gcount()), 0) < 0) {
                return "";
            }
        }
        if (BCryptFinishHash(hashHandle.get(), hashValue.data(), cbHash, 0) < 0) {
            return "";
        }

        std::ostringstream ss;
        ss << std::hex << std::setfill('0');
        for (DWORD i = 0; i < cbHash; ++i) {
            ss << std::setw(2) << static_cast<int>(hashValue[i]);
        }
        return ss.str();
    }

} // namespace omnistats::utils

using omnistats::utils::CalculateSHA256;

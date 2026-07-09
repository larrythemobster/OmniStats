#pragma once
#include <string>
#include <fstream>
#include <sstream>
#include <iomanip>
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <bcrypt.h>

inline std::string CalculateSHA256(const std::string& filePath) {
    std::ifstream file(filePath, std::ios::binary);
    if (!file) return "";

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    BCRYPT_HASH_HANDLE hHash = nullptr;
    DWORD cbHashObject = 0;
    DWORD cbHash = 0;
    DWORD cbData = 0;
    PBYTE pbHashObject = nullptr;
    PBYTE pbHash = nullptr;
    std::string hashStr = "";

    if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0) {
        if (BCryptGetProperty(hAlg, BCRYPT_OBJECT_LENGTH, (PBYTE)&cbHashObject, sizeof(DWORD), &cbData, 0) >= 0) {
            pbHashObject = (PBYTE)HeapAlloc(GetProcessHeap(), 0, cbHashObject);
            if (BCryptGetProperty(hAlg, BCRYPT_HASH_LENGTH, (PBYTE)&cbHash, sizeof(DWORD), &cbData, 0) >= 0) {
                pbHash = (PBYTE)HeapAlloc(GetProcessHeap(), 0, cbHash);
                if (BCryptCreateHash(hAlg, &hHash, pbHashObject, cbHashObject, nullptr, 0, 0) >= 0) {
                    char buffer[4096];
                    while (file.read(buffer, sizeof(buffer))) {
                        BCryptHashData(hHash, (PBYTE)buffer, (ULONG)file.gcount(), 0);
                    }
                    if (file.gcount() > 0) {
                        BCryptHashData(hHash, (PBYTE)buffer, (ULONG)file.gcount(), 0);
                    }
                    if (BCryptFinishHash(hHash, pbHash, cbHash, 0) >= 0) {
                        std::stringstream ss;
                        ss << std::hex << std::setfill('0');
                        for (DWORD i = 0; i < cbHash; i++) {
                            ss << std::setw(2) << (int)pbHash[i];
                        }
                        hashStr = ss.str();
                    }
                }
            }
        }
    }

    if (hHash) BCryptDestroyHash(hHash);
    if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
    if (pbHashObject) HeapFree(GetProcessHeap(), 0, pbHashObject);
    if (pbHash) HeapFree(GetProcessHeap(), 0, pbHash);

    return hashStr;
}

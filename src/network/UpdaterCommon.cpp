#include "network/UpdaterCommon.hpp"
#include "core/FileHash.hpp"
#include <vector>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <bcrypt.h>

#pragma comment(lib, "wintrust.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
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

    static std::string CalculateBufferSha256(const BYTE* data, DWORD len) {
        if (!data || len == 0) {
            return "";
        }
        BCRYPT_ALG_HANDLE hAlg = nullptr;
        BCRYPT_HASH_HANDLE hHash = nullptr;
        DWORD cbHash = 32;
        BYTE hash[32] = {0};
        std::string result = "";

        if (BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0) {
            if (BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0) >= 0) {
                BCryptHashData(hHash, const_cast<PBYTE>(data), len, 0);
                if (BCryptFinishHash(hHash, hash, cbHash, 0) >= 0) {
                    std::stringstream ss;
                    ss << std::hex << std::setfill('0');
                    for (DWORD i = 0; i < cbHash; ++i) {
                        ss << std::setw(2) << static_cast<int>(hash[i]);
                    }
                    result = ss.str();
                }
            }
        }
        if (hHash) BCryptDestroyHash(hHash);
        if (hAlg) BCryptCloseAlgorithmProvider(hAlg, 0);
        return result;
    }

    SignatureVerificationResult VerifyAuthenticodeSignature(const std::string& filePath) {
        SignatureVerificationResult result;

        WINTRUST_FILE_INFO fileInfo = {};
        fileInfo.cbStruct = sizeof(fileInfo);

        const int wideLength = MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, nullptr, 0);
        if (wideLength <= 0) {
            result.statusMessage = "Failed to convert file path to wide string.";
            return result;
        }
        std::vector<wchar_t> widePath(static_cast<size_t>(wideLength));
        if (MultiByteToWideChar(CP_UTF8, 0, filePath.c_str(), -1, widePath.data(), wideLength) <= 0) {
            result.statusMessage = "Failed to convert file path to wide string.";
            return result;
        }
        fileInfo.pcwszFilePath = widePath.data();

        WINTRUST_DATA trustData = {};
        trustData.cbStruct = sizeof(trustData);
        trustData.dwUIChoice = WTD_UI_NONE;
        trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
        trustData.dwUnionChoice = WTD_CHOICE_FILE;
        trustData.pFile = &fileInfo;
        trustData.dwStateAction = WTD_STATEACTION_VERIFY;
        trustData.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;

        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        const LONG status = WinVerifyTrust(nullptr, &action, &trustData);

        // Separate cryptographic Authenticode signature validity from Windows trust-chain status:
        // - ERROR_SUCCESS: signature intact, root trusted by Windows CA store
        // - CERT_E_UNTRUSTEDROOT: signature intact, root untrusted (expected for pinned self-signed cert)
        // All other codes (TRUST_E_NOSIGNATURE, TRUST_E_BAD_DIGEST, 0x800b0003, etc.) indicate unsigned,
        // modified, corrupted, or structurally invalid signatures.
        if (status == ERROR_SUCCESS || status == static_cast<LONG>(CERT_E_UNTRUSTEDROOT)) {
            CRYPT_PROVIDER_DATA* providerData = WTHelperProvDataFromStateData(trustData.hWVTStateData);
            if (providerData) {
                CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(providerData, 0, FALSE, 0);
                if (signer && signer->csCertChain > 0 && signer->pasCertChain && signer->pasCertChain[0].pCert) {
                    PCCERT_CONTEXT cert = signer->pasCertChain[0].pCert;
                    result.digestValid = true;
                    result.trustedRoot = (status == ERROR_SUCCESS);

                    const DWORD chars = CertGetNameStringA(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, nullptr, 0);
                    if (chars > 1) {
                        std::vector<char> name(chars);
                        if (CertGetNameStringA(cert, CERT_NAME_SIMPLE_DISPLAY_TYPE, 0, nullptr, name.data(), chars) > 1) {
                            result.signer.assign(name.data());
                        }
                    }

                    // Compute only the SHA-256 fingerprint of the DER-encoded signing certificate
                    result.certSha256 = CalculateBufferSha256(cert->pbCertEncoded, cert->cbCertEncoded);
                    result.statusMessage = result.trustedRoot
                                               ? "Authenticode signature valid (trusted root)"
                                               : "Authenticode signature valid (untrusted root / self-signed)";
                } else {
                    result.statusMessage = "Signer certificate not found in signature chain.";
                }
            } else {
                result.statusMessage = "Cryptographic provider state data unavailable.";
            }
        } else {
            std::stringstream ss;
            ss << "WinVerifyTrust status 0x" << std::hex << status;
            if (status == static_cast<LONG>(TRUST_E_NOSIGNATURE)) {
                ss << " (TRUST_E_NOSIGNATURE: file is unsigned)";
            } else if (status == static_cast<LONG>(TRUST_E_BAD_DIGEST)) {
                ss << " (TRUST_E_BAD_DIGEST: signature digest mismatch - file modified/tampered)";
            }
            result.statusMessage = ss.str();
        }

        trustData.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &action, &trustData);
        return result;
    }

    bool VerifyMsiPackage(const std::string& msiPath,
                          const std::string& expectedFileSha,
                          const std::string& currentPinnedCertSha,
                          const std::string& nextPinnedCertSha) {
        // 1. Keep the existing updater file SHA-256 check
        if (!VerifyFileSHA256(msiPath, expectedFileSha)) {
            std::cout << "[Updater] MSI file SHA-256 verification failed.\n";
            return false;
        }
        std::cout << "[Updater] MSI file SHA-256 verification passed.\n";

        // Resolve certificate pins, defaulting to SigningConfig if not passed explicitly
        std::string currentPin = currentPinnedCertSha.empty() ? SigningConfig::CURRENT_CERT_SHA256 : currentPinnedCertSha;
        std::string nextPin = nextPinnedCertSha.empty() ? SigningConfig::NEXT_CERT_SHA256 : nextPinnedCertSha;

        currentPin = Trim(currentPin);
        nextPin = Trim(nextPin);

        if (currentPin.empty() && nextPin.empty()) {
            std::cout << "[Updater] Authenticode certificate pin verification is not configured for this build.\n";
            return true;
        }

        // 2. Extra updater check: verify Authenticode signature against pinned certificate hash
        const SignatureVerificationResult signature = VerifyAuthenticodeSignature(msiPath);
        if (!signature.digestValid) {
            std::cout << "[Updater] MSI Authenticode verification failed: " << signature.statusMessage << "\n";
            return false;
        }

        // Display publisher name for information only; do not reject if publisher string changes
        if (!signature.signer.empty()) {
            std::cout << "[Updater] MSI Authenticode signer: '" << signature.signer << "' ("
                      << (signature.trustedRoot ? "trusted root" : "untrusted root / self-signed") << ").\n";
        }

        std::string actualCertSha = signature.certSha256;
        std::transform(actualCertSha.begin(), actualCertSha.end(), actualCertSha.begin(), ::tolower);

        // Accept either current pinned cert or next rotated cert
        bool certMatches = false;
        if (!currentPin.empty()) {
            std::string cleanCurrent = currentPin;
            std::transform(cleanCurrent.begin(), cleanCurrent.end(), cleanCurrent.begin(), ::tolower);
            if (actualCertSha == cleanCurrent) {
                certMatches = true;
                std::cout << "[Updater] MSI verified against current pinned certificate.\n";
            }
        }

        if (!certMatches && !nextPin.empty()) {
            std::string cleanNext = nextPin;
            std::transform(cleanNext.begin(), cleanNext.end(), cleanNext.begin(), ::tolower);
            if (actualCertSha == cleanNext) {
                certMatches = true;
                std::cout << "[Updater] MSI verified against next (rotated) pinned certificate.\n";
            }
        }

        if (!certMatches) {
            std::cout << "[Updater] MSI certificate hash mismatch.\n"
                      << "          Expected: " << currentPin;
            if (!nextPin.empty()) {
                std::cout << " (or next: " << nextPin << ")";
            }
            std::cout << "\n          Actual:   " << actualCertSha << "\n";
            return false;
        }

        std::cout << "[Updater] MSI Authenticode certificate verification passed.\n";
        return true;
    }

} // namespace UpdaterCommon

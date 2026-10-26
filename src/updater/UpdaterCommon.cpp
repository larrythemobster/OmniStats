#include "updater/UpdaterCommon.hpp"
#include "updater/AuthenticodeStatus.hpp"
#include "utils/FileHash.hpp"
#include "updater/UpdaterEventLog.hpp"
#include <vector>
#include <sstream>
#include <fstream>
#include <algorithm>
#include <iostream>
#include <iomanip>
#include <windows.h>
#include <wintrust.h>
#include <softpub.h>
#include <bcrypt.h>

#ifndef WTD_CACHE_ONLY_URL_RETRIEVAL
#define WTD_CACHE_ONLY_URL_RETRIEVAL 0x00001000
#endif
#ifndef CERT_E_REVOKED
#define CERT_E_REVOKED 0x800B010CL
#endif
#ifndef CERT_E_REVOCATION_FAILURE
#define CERT_E_REVOCATION_FAILURE 0x800B010EL
#endif
#ifndef CRYPT_E_REVOKED
#define CRYPT_E_REVOKED 0x80092010L
#endif
#ifndef CRYPT_E_NO_REVOCATION_CHECK
#define CRYPT_E_NO_REVOCATION_CHECK 0x80092012L
#endif
#ifndef CRYPT_E_REVOCATION_OFFLINE
#define CRYPT_E_REVOCATION_OFFLINE 0x80092013L
#endif
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

    static DWORD NonRootRevocationErrors(HANDLE state) {
        CRYPT_PROVIDER_DATA* provider = WTHelperProvDataFromStateData(state);
        if (!provider) return 0;
        const CRYPT_PROVIDER_SGNR* signer = WTHelperGetProvSignerFromChain(provider, 0, FALSE, 0);
        if (!signer || !signer->pChainContext) return 0;
        DWORD errors = 0;
        const CERT_CHAIN_CONTEXT* context = signer->pChainContext;
        for (DWORD i = 0; i < context->cChain; ++i) {
            const CERT_SIMPLE_CHAIN* chain = context->rgpChain[i];
            DWORD checkedElements = chain->cElement;
            if (checkedElements > 0) {
                CERT_INFO* last = chain->rgpElement[checkedElements - 1]->pCertContext->pCertInfo;
                if (CertCompareCertificateName(X509_ASN_ENCODING, &last->Subject, &last->Issuer)) --checkedElements;
            }
            for (DWORD j = 0; j < checkedElements; ++j) {
                errors |= chain->rgpElement[j]->TrustStatus.dwErrorStatus;
            }
        }
        return errors;
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
        trustData.fdwRevocationChecks = WTD_REVOKE_WHOLECHAIN;
        trustData.dwUnionChoice = WTD_CHOICE_FILE;
        trustData.pFile = &fileInfo;
        trustData.dwStateAction = WTD_STATEACTION_VERIFY;
        trustData.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT;

        GUID action = WINTRUST_ACTION_GENERIC_VERIFY_V2;
        LONG status = WinVerifyTrust(nullptr, &action, &trustData);

        AuthenticodeStatus classification = ClassifyAuthenticodeStatus(status, NonRootRevocationErrors(trustData.hWVTStateData));
        // Retry only an unknown revocation result. Never discard an observed revocation.
        if (classification == AuthenticodeStatus::RevocationUnknown) {
            trustData.dwStateAction = WTD_STATEACTION_CLOSE;
            WinVerifyTrust(nullptr, &action, &trustData);

            trustData.hWVTStateData = nullptr;
            trustData.dwStateAction = WTD_STATEACTION_VERIFY;
            trustData.dwProvFlags = WTD_REVOCATION_CHECK_CHAIN_EXCLUDE_ROOT | WTD_CACHE_ONLY_URL_RETRIEVAL;
            status = WinVerifyTrust(nullptr, &action, &trustData);
            classification = ClassifyAuthenticodeStatus(status, NonRootRevocationErrors(trustData.hWVTStateData));
        }
        if (classification == AuthenticodeStatus::RevocationUnknown) {
            trustData.dwStateAction = WTD_STATEACTION_CLOSE;
            WinVerifyTrust(nullptr, &action, &trustData);

            trustData.hWVTStateData = nullptr;
            trustData.dwStateAction = WTD_STATEACTION_VERIFY;
            trustData.fdwRevocationChecks = WTD_REVOKE_NONE;
            trustData.dwProvFlags = WTD_CACHE_ONLY_URL_RETRIEVAL;
            status = WinVerifyTrust(nullptr, &action, &trustData);
            classification = ClassifyAuthenticodeStatus(status, NonRootRevocationErrors(trustData.hWVTStateData), false);
            result.revocationUnknown = true;
        }
        // Unknown revocation is permitted only after a separate signature check.
        // VerifyMsiPackage still requires the signer certificate to match a pin.
        if (classification == AuthenticodeStatus::Valid || classification == AuthenticodeStatus::UntrustedRoot) {
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
            } else if (classification == AuthenticodeStatus::Revoked) {
                ss << " (certificate chain is revoked)";
            } else if (classification == AuthenticodeStatus::RevocationUnknown) {
                ss << " (certificate revocation status is unknown)";
            }
            result.statusMessage = ss.str();
        }

        trustData.dwStateAction = WTD_STATEACTION_CLOSE;
        WinVerifyTrust(nullptr, &action, &trustData);
        return result;
    }

    MsiVerifyResult VerifyMsiPackage(const std::string& msiPath,
                                     const std::string& expectedFileSha,
                                     const std::string& currentPinnedCertSha,
                                     const std::string& nextPinnedCertSha) {
        // 1. Keep the existing updater file SHA-256 check
        if (!VerifyFileSHA256(msiPath, expectedFileSha)) {
            std::cout << "[Updater] MSI file SHA-256 verification failed.\n";
            return MsiVerifyResult::ChecksumMismatch;
        }
        std::cout << "[Updater] MSI file SHA-256 verification passed.\n";

        // Resolve certificate pins, defaulting to SigningConfig if not passed explicitly
        std::string currentPin = currentPinnedCertSha.empty() ? SigningConfig::CURRENT_CERT_SHA256 : currentPinnedCertSha;
        std::string nextPin = nextPinnedCertSha.empty() ? SigningConfig::NEXT_CERT_SHA256 : nextPinnedCertSha;

        currentPin = Trim(currentPin);
        nextPin = Trim(nextPin);

        if (currentPin.empty() && nextPin.empty()) {
            std::cout << "[Updater] Authenticode certificate pin verification is not configured for this build.\n";
            return MsiVerifyResult::Ok;
        }

        // 2. Extra updater check: verify Authenticode signature against pinned certificate hash
        const SignatureVerificationResult signature = VerifyAuthenticodeSignature(msiPath);
        if (!signature.digestValid) {
            std::cout << "[Updater] MSI Authenticode verification failed: " << signature.statusMessage << "\n";
            return MsiVerifyResult::SignatureInvalid;
        }

        // Display publisher name for information only; do not reject if publisher string changes
        if (!signature.signer.empty()) {
            std::cout << "[Updater] MSI Authenticode signer: '" << signature.signer << "' ("
                      << (signature.trustedRoot ? "trusted root" : "untrusted root / self-signed") << ").\n";
        }
        if (signature.revocationUnknown) {
            std::cout << "[Updater] Certificate revocation status is unknown; signature and certificate pin are still required.\n";
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
            return MsiVerifyResult::SignatureInvalid;
        }

        std::cout << "[Updater] MSI Authenticode certificate verification passed.\n";
        return MsiVerifyResult::Ok;
    }

    std::string GetInstalledAppVersion() {
        char updaterPath[MAX_PATH] = {0};
        GetModuleFileNameA(NULL, updaterPath, MAX_PATH);
        std::string updaterDir;
        const std::string updaterPathStr(updaterPath);
        const size_t lastSlash = updaterPathStr.find_last_of("\\/");
        if (lastSlash != std::string::npos) {
            updaterDir = updaterPathStr.substr(0, lastSlash + 1);
        }

        const std::string appNextToUpdater = updaterDir + "OmniStats.exe";
        const std::string appInLocalAppData = GetLocalAppDataDir() + "OmniStats.exe";

        std::string localVersion;
        if (GetFileAttributesA(appNextToUpdater.c_str()) != INVALID_FILE_ATTRIBUTES) {
            localVersion = GetFileVersion(appNextToUpdater);
        } else if (GetFileAttributesA(appInLocalAppData.c_str()) != INVALID_FILE_ATTRIBUTES) {
            localVersion = GetFileVersion(appInLocalAppData);
        }

        if (localVersion.empty()) {
#ifdef OMNISTATS_VERSION
            localVersion = OMNISTATS_VERSION;
#else
            localVersion = "2.0.0";
#endif
        }
        return localVersion;
    }

    void RecordUpdaterEvent(const std::string& event,
                            const std::string& targetVersion,
                            const std::string& errorCategory,
                            const std::string& version) {
        UpdaterEventLog::Append(GetAppDataDir(), event, targetVersion, errorCategory, version);
    }

} // namespace UpdaterCommon

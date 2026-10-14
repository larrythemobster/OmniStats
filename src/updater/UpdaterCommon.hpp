#pragma once
#include <string>
#include <curl/curl.h>

namespace UpdaterCommon {

    std::string Trim(const std::string& str);
    bool IsNewerVersion(const std::string& current, const std::string& latest);
    std::string GetFileVersion(const std::string& filePath);
    bool VerifyFileSHA256(const std::string& filePath, const std::string& expectedHash);

    struct SignatureVerificationResult {
        bool digestValid = false;  // Cryptographic signature is intact and untampered
        bool trustedRoot = false;  // Whether root cert is in Windows Trusted Root store
        std::string signer;        // Subject display name (for logging/display only)
        std::string certSha256;    // SHA-256 fingerprint of DER-encoded certificate
        std::string statusMessage; // Diagnostic message
    };

    SignatureVerificationResult VerifyAuthenticodeSignature(const std::string& filePath);
    bool VerifyMsiPackage(const std::string& msiPath,
                          const std::string& expectedFileSha,
                          const std::string& currentPinnedCertSha = "",
                          const std::string& nextPinnedCertSha = "");

} // namespace UpdaterCommon

// Include split headers for backward compatibility
#include "updater/UpdaterPaths.hpp"
#include "updater/UpdaterDownload.hpp"
#include "updater/UpdaterProcess.hpp"
#include "updater/ShortcutUtils.hpp"
#include "network/http/SigningConfig.hpp"

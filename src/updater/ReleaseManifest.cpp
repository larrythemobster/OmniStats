#include "updater/ReleaseManifest.hpp"
#include "updater/UpdaterCommon.hpp"
#include "updater/UpdaterDownload.hpp"
#include "network/http/SigningConfig.hpp"
#include <nlohmann/json.hpp>
#include <sodium.h>
#include <ctime>
#include <algorithm>
#include <cstring>
#include <iostream>

bool ReleaseManifestVerifier::ParseManifest(const std::string& jsonStr, ReleaseManifest& outManifest) {
    if (jsonStr.empty()) {
        return false;
    }
    try {
        auto j = nlohmann::json::parse(jsonStr);
        if (!j.is_object()) {
            return false;
        }

        if (!j.contains("version") || !j["version"].is_string() ||
            !j.contains("msi_sha256") || !j["msi_sha256"].is_string() ||
            !j.contains("msi_url") || !j["msi_url"].is_string()) {
            return false;
        }

        outManifest.version = UpdaterCommon::Trim(j["version"].get<std::string>());
        outManifest.msi_sha256 = UpdaterCommon::Trim(j["msi_sha256"].get<std::string>());
        std::transform(outManifest.msi_sha256.begin(), outManifest.msi_sha256.end(),
                       outManifest.msi_sha256.begin(), ::tolower);
        outManifest.msi_url = UpdaterCommon::Trim(j["msi_url"].get<std::string>());

        if (outManifest.version.empty() || outManifest.msi_sha256.length() != 64 || outManifest.msi_url.empty()) {
            return false;
        }

        if (j.contains("min_version") && j["min_version"].is_string()) {
            outManifest.min_version = UpdaterCommon::Trim(j["min_version"].get<std::string>());
        } else {
            outManifest.min_version.clear();
        }

        if (j.contains("published_at")) {
            if (j["published_at"].is_string()) {
                outManifest.published_at = UpdaterCommon::Trim(j["published_at"].get<std::string>());
            } else if (j["published_at"].is_number()) {
                outManifest.published_at = std::to_string(j["published_at"].get<long long>());
            }
        } else {
            outManifest.published_at.clear();
        }

        return true;
    } catch (...) {
        return false;
    }
}

std::string ReleaseManifestVerifier::SerializeManifest(const ReleaseManifest& manifest) {
    nlohmann::ordered_json j;
    j["version"] = manifest.version;
    j["msi_sha256"] = manifest.msi_sha256;
    j["msi_url"] = manifest.msi_url;
    j["min_version"] = manifest.min_version;
    j["published_at"] = manifest.published_at;
    return j.dump();
}

bool ReleaseManifestVerifier::VerifySignature(const std::string& manifestData,
                                              const std::string& signatureBytes,
                                              const std::string& publicKeyHex) {
    if (manifestData.empty() || signatureBytes.empty() || publicKeyHex.empty()) {
        return false;
    }
    if (sodium_init() < 0) {
        return false;
    }

    const std::string cleanPkHex = UpdaterCommon::Trim(publicKeyHex);
    if (cleanPkHex.length() != crypto_sign_PUBLICKEYBYTES * 2) {
        return false;
    }

    std::vector<unsigned char> pk(crypto_sign_PUBLICKEYBYTES);
    size_t binLen = 0;
    if (sodium_hex2bin(pk.data(), pk.size(), cleanPkHex.data(), cleanPkHex.length(),
                       nullptr, &binLen, nullptr) != 0 ||
        binLen != crypto_sign_PUBLICKEYBYTES) {
        return false;
    }

    std::vector<unsigned char> sig(crypto_sign_BYTES);
    if (signatureBytes.size() == crypto_sign_BYTES) {
        std::memcpy(sig.data(), signatureBytes.data(), crypto_sign_BYTES);
    } else {
        const std::string cleanSigHex = UpdaterCommon::Trim(signatureBytes);
        if (cleanSigHex.length() == crypto_sign_BYTES * 2) {
            if (sodium_hex2bin(sig.data(), sig.size(), cleanSigHex.data(), cleanSigHex.length(),
                               nullptr, &binLen, nullptr) != 0 ||
                binLen != crypto_sign_BYTES) {
                return false;
            }
        } else {
            return false;
        }
    }

    const int ret = crypto_sign_verify_detached(
        sig.data(),
        reinterpret_cast<const unsigned char*>(manifestData.data()),
        manifestData.size(),
        pk.data());
    return (ret == 0);
}

bool ReleaseManifestVerifier::VerifySignatureWithRotation(const std::string& manifestData,
                                                          const std::string& signatureBytes,
                                                          const std::string& currentKeyHex,
                                                          const std::string& nextKeyHex) {
    if (!currentKeyHex.empty()) {
        if (VerifySignature(manifestData, signatureBytes, currentKeyHex)) {
            return true;
        }
    }
    if (!nextKeyHex.empty()) {
        if (VerifySignature(manifestData, signatureBytes, nextKeyHex)) {
            return true;
        }
    }
    return false;
}

ManifestVerifyResult ReleaseManifestVerifier::VerifyManifest(const std::string& manifestJson,
                                                             const std::string& signatureBytes,
                                                             const std::string& installedVersion,
                                                             const std::string& currentKeyHex,
                                                             const std::string& nextKeyHex,
                                                             ReleaseManifest* outManifest) {
    if (manifestJson.empty()) {
        return ManifestVerifyResult::ManifestMissing;
    }

    if (!VerifySignatureWithRotation(manifestJson, signatureBytes, currentKeyHex, nextKeyHex)) {
        return ManifestVerifyResult::SignatureInvalid;
    }

    ReleaseManifest manifest;
    if (!ParseManifest(manifestJson, manifest)) {
        return ManifestVerifyResult::ParseError;
    }

    if (!installedVersion.empty() && UpdaterCommon::IsNewerVersion(manifest.version, installedVersion)) {
        return ManifestVerifyResult::DowngradeRefused;
    }

    if (outManifest) {
        *outManifest = std::move(manifest);
    }
    return ManifestVerifyResult::Ok;
}

bool ReleaseManifestVerifier::IsUpdateForced(const std::string& installedVersion, const std::string& minVersion) {
    if (installedVersion.empty() || minVersion.empty()) {
        return false;
    }
    return UpdaterCommon::IsNewerVersion(installedVersion, minVersion);
}

std::string ReleaseManifestVerifier::ResolveMsiUrl(const std::string& serverUrl, const std::string& msiUrl) {
    if (msiUrl.rfind("http://", 0) == 0 || msiUrl.rfind("https://", 0) == 0) {
        return msiUrl;
    }
    std::string cleanServer = serverUrl;
    while (!cleanServer.empty() && (cleanServer.back() == '/' || cleanServer.back() == '\\')) {
        cleanServer.pop_back();
    }
    if (!msiUrl.empty() && (msiUrl.front() == '/' || msiUrl.front() == '\\')) {
        return cleanServer + msiUrl;
    }
    return cleanServer + "/" + msiUrl;
}

bool ReleaseManifestVerifier::FetchAndVerify(const std::string& serverUrl,
                                             const std::string& installedVersion,
                                             ReleaseManifest& outManifest,
                                             ManifestVerifyResult& outResult,
                                             const std::string& currentKeyHex,
                                             const std::string& nextKeyHex) {
    const std::string curKey = currentKeyHex.empty() ? SigningConfig::CURRENT_RELEASE_PUBLIC_KEY_HEX : currentKeyHex;
    const std::string nxtKey = nextKeyHex.empty() ? SigningConfig::NEXT_RELEASE_PUBLIC_KEY_HEX : nextKeyHex;

    const std::string manifestUrl = serverUrl + "/release-manifest.json?t=" + std::to_string(std::time(nullptr));
    const std::string sigUrl = serverUrl + "/release-manifest.json.sig?t=" + std::to_string(std::time(nullptr));

    long httpCode = 0;
    CURLcode curlCode = CURLE_OK;
    std::string manifestJson = UpdaterCommon::DownloadString(manifestUrl, 15, &httpCode, &curlCode);
    if (manifestJson.empty() || httpCode != 200 || curlCode != CURLE_OK) {
        outResult = ManifestVerifyResult::ManifestMissing;
        return false;
    }

    httpCode = 0;
    curlCode = CURLE_OK;
    std::string sigData = UpdaterCommon::DownloadString(sigUrl, 15, &httpCode, &curlCode);
    if (sigData.empty() || httpCode != 200 || curlCode != CURLE_OK) {
        outResult = ManifestVerifyResult::SignatureInvalid;
        return false;
    }

    outResult = VerifyManifest(manifestJson, sigData, installedVersion, curKey, nxtKey, &outManifest);
    return (outResult == ManifestVerifyResult::Ok);
}

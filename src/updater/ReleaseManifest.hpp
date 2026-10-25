#pragma once
#include <string>
#include <vector>
#include <cstdint>

struct ReleaseManifest {
    std::string version;
    std::string msi_sha256;
    std::string msi_url;
    std::string min_version;
    std::string published_at;
};

enum class ManifestVerifyResult {
    Ok,
    ParseError,
    SignatureInvalid,
    DowngradeRefused,
    ManifestMissing
};

namespace ReleaseManifestVerifier {

    bool ParseManifest(const std::string& jsonStr, ReleaseManifest& outManifest);

    std::string SerializeManifest(const ReleaseManifest& manifest);

    bool VerifySignature(const std::string& manifestData,
                         const std::string& signatureBytes,
                         const std::string& publicKeyHex);

    bool VerifySignatureWithRotation(const std::string& manifestData,
                                     const std::string& signatureBytes,
                                     const std::string& currentKeyHex,
                                     const std::string& nextKeyHex = "");

    ManifestVerifyResult VerifyManifest(const std::string& manifestJson,
                                        const std::string& signatureBytes,
                                        const std::string& installedVersion,
                                        const std::string& currentKeyHex,
                                        const std::string& nextKeyHex = "",
                                        ReleaseManifest* outManifest = nullptr);

    bool IsUpdateForced(const std::string& installedVersion, const std::string& minVersion);

    std::string ResolveMsiUrl(const std::string& serverUrl, const std::string& msiUrl);

    bool FetchAndVerify(const std::string& serverUrl,
                        const std::string& installedVersion,
                        ReleaseManifest& outManifest,
                        ManifestVerifyResult& outResult,
                        const std::string& currentKeyHex = "",
                        const std::string& nextKeyHex = "");

} // namespace ReleaseManifestVerifier

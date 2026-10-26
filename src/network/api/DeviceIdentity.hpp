#pragma once

#include <array>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

class DeviceIdentity {
  public:
    struct DeviceKeyPair {
        std::string publicKeyBase64Url;
        std::string secretKeyBase64Url;
        std::string publicId;
    };

    struct DeviceProof {
        int64_t timestamp = 0;
        std::string nonce;
        std::string signature;
        std::string canonical;
    };

    static bool EnsureSodiumInitialized();

    static std::string Base64UrlEncode(const uint8_t* data, size_t len);
    static bool Base64UrlDecode(std::string_view encoded, std::vector<uint8_t>& out);
    static std::string Sha256Hex(std::string_view input);
    static std::string DerivePublicIdForPublicKey(std::string_view publicKeyBase64Url);
    static DeviceKeyPair DeriveKeyPairFromSeed(const std::array<uint8_t, 32>& seed);
    static bool DeriveKeyPairFromSecret(std::string_view secretKeyBase64Url, DeviceKeyPair& out);
    static std::string BuildProofCanonical(std::string_view operation,
                                           std::string_view publicId,
                                           int64_t timestamp,
                                           std::string_view nonceBase64Url,
                                           std::string_view credential);
    static DeviceProof SignProofDeterministic(std::string_view secretKeyBase64Url,
                                              std::string_view operation,
                                              std::string_view publicId,
                                              int64_t timestamp,
                                              std::string_view nonceBase64Url,
                                              std::string_view credential);
    static DeviceProof CreateProof(std::string_view secretKeyBase64Url,
                                   std::string_view operation,
                                   std::string_view publicId,
                                   std::string_view credential,
                                   int64_t timestamp);

    static std::string GenerateNonce();
    static std::string LocalDeviceName();

    static bool EnsureDeviceKey(DeviceKeyPair* outKey = nullptr);
};

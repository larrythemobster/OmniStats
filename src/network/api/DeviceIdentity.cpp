#include "network/api/DeviceIdentity.hpp"
#include "core/config/Config.hpp"

#include <sodium.h>
#include <windows.h>

#include <cctype>
#include <cstring>
#include <mutex>

namespace {

    std::string TrimWhitespace(std::string_view value) {
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.front()))) {
            value.remove_prefix(1);
        }
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back()))) {
            value.remove_suffix(1);
        }
        return std::string(value);
    }

} // namespace

bool DeviceIdentity::EnsureSodiumInitialized() {
    static std::once_flag once;
    static bool initialized = false;
    std::call_once(once, []() {
        initialized = (sodium_init() >= 0);
    });
    return initialized;
}

std::string DeviceIdentity::Base64UrlEncode(const uint8_t* data, size_t len) {
    EnsureSodiumInitialized();
    if (!data || len == 0) return {};
    const size_t maxLen = sodium_base64_ENCODED_LEN(len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    std::string encoded(maxLen, '\0');
    sodium_bin2base64(encoded.data(), encoded.size(), data, len, sodium_base64_VARIANT_URLSAFE_NO_PADDING);
    encoded.resize(std::strlen(encoded.c_str()));
    return encoded;
}

bool DeviceIdentity::Base64UrlDecode(std::string_view encoded, std::vector<uint8_t>& out) {
    EnsureSodiumInitialized();
    if (encoded.empty()) {
        out.clear();
        return false;
    }
    out.assign(encoded.size(), 0);
    size_t binLen = 0;
    if (sodium_base642bin(out.data(),
                          out.size(),
                          encoded.data(),
                          encoded.size(),
                          nullptr,
                          &binLen,
                          nullptr,
                          sodium_base64_VARIANT_URLSAFE_NO_PADDING) != 0) {
        out.clear();
        return false;
    }
    out.resize(binLen);
    return true;
}

std::string DeviceIdentity::Sha256Hex(std::string_view input) {
    EnsureSodiumInitialized();
    std::array<uint8_t, crypto_hash_sha256_BYTES> digest{};
    crypto_hash_sha256(digest.data(),
                       reinterpret_cast<const unsigned char*>(input.data()),
                       input.size());
    std::array<char, crypto_hash_sha256_BYTES * 2 + 1> hex{};
    sodium_bin2hex(hex.data(), hex.size(), digest.data(), digest.size());
    return std::string(hex.data(), crypto_hash_sha256_BYTES * 2);
}

std::string DeviceIdentity::DerivePublicIdForPublicKey(std::string_view publicKeyBase64Url) {
    std::vector<uint8_t> raw;
    if (!Base64UrlDecode(publicKeyBase64Url, raw) || raw.size() != crypto_sign_ed25519_PUBLICKEYBYTES) {
        return {};
    }
    std::array<uint8_t, crypto_hash_sha256_BYTES> sum{};
    crypto_hash_sha256(sum.data(), raw.data(), raw.size());
    return Base64UrlEncode(sum.data(), sum.size());
}

DeviceIdentity::DeviceKeyPair DeviceIdentity::DeriveKeyPairFromSeed(const std::array<uint8_t, 32>& seed) {
    EnsureSodiumInitialized();
    std::array<uint8_t, crypto_sign_ed25519_PUBLICKEYBYTES> pk{};
    std::array<uint8_t, crypto_sign_ed25519_SECRETKEYBYTES> sk{};
    crypto_sign_ed25519_seed_keypair(pk.data(), sk.data(), seed.data());

    DeviceKeyPair pair;
    pair.publicKeyBase64Url = Base64UrlEncode(pk.data(), pk.size());
    pair.secretKeyBase64Url = Base64UrlEncode(seed.data(), seed.size());
    pair.publicId = DerivePublicIdForPublicKey(pair.publicKeyBase64Url);
    sodium_memzero(sk.data(), sk.size());
    return pair;
}

bool DeviceIdentity::DeriveKeyPairFromSecret(std::string_view secretKeyBase64Url, DeviceKeyPair& out) {
    std::vector<uint8_t> raw;
    if (!Base64UrlDecode(secretKeyBase64Url, raw)) {
        return false;
    }
    if (raw.size() != crypto_sign_ed25519_SEEDBYTES && raw.size() != crypto_sign_ed25519_SECRETKEYBYTES) {
        sodium_memzero(raw.data(), raw.size());
        return false;
    }
    std::array<uint8_t, 32> seed{};
    std::memcpy(seed.data(), raw.data(), seed.size());
    sodium_memzero(raw.data(), raw.size());
    out = DeriveKeyPairFromSeed(seed);
    sodium_memzero(seed.data(), seed.size());
    return !out.publicId.empty();
}

std::string DeviceIdentity::BuildProofCanonical(std::string_view operation,
                                                std::string_view publicId,
                                                int64_t timestamp,
                                                std::string_view nonceBase64Url,
                                                std::string_view credential) {
    std::string out;
    out.reserve(160 + publicId.size() + nonceBase64Url.size());
    out.append("OMNISTATS-DEVICE-PROOF-v1\n");
    out.append(operation);
    out.push_back('\n');
    out.append(publicId);
    out.push_back('\n');
    out.append(std::to_string(timestamp));
    out.push_back('\n');
    out.append(nonceBase64Url);
    out.push_back('\n');
    out.append(Sha256Hex(credential));
    return out;
}

DeviceIdentity::DeviceProof DeviceIdentity::SignProofDeterministic(std::string_view secretKeyBase64Url,
                                                                   std::string_view operation,
                                                                   std::string_view publicId,
                                                                   int64_t timestamp,
                                                                   std::string_view nonceBase64Url,
                                                                   std::string_view credential) {
    EnsureSodiumInitialized();
    DeviceProof proof;
    std::vector<uint8_t> rawSecret;
    if (!Base64UrlDecode(secretKeyBase64Url, rawSecret)) {
        return proof;
    }
    if (rawSecret.size() != crypto_sign_ed25519_SEEDBYTES && rawSecret.size() != crypto_sign_ed25519_SECRETKEYBYTES) {
        sodium_memzero(rawSecret.data(), rawSecret.size());
        return proof;
    }

    std::array<uint8_t, crypto_sign_ed25519_PUBLICKEYBYTES> pk{};
    std::array<uint8_t, crypto_sign_ed25519_SECRETKEYBYTES> sk{};
    crypto_sign_ed25519_seed_keypair(pk.data(), sk.data(), rawSecret.data());
    sodium_memzero(rawSecret.data(), rawSecret.size());

    proof.timestamp = timestamp;
    proof.nonce = std::string(nonceBase64Url);
    proof.canonical = BuildProofCanonical(operation, publicId, timestamp, nonceBase64Url, credential);

    std::array<uint8_t, crypto_sign_ed25519_BYTES> sig{};
    crypto_sign_ed25519_detached(sig.data(),
                                 nullptr,
                                 reinterpret_cast<const unsigned char*>(proof.canonical.data()),
                                 proof.canonical.size(),
                                 sk.data());
    sodium_memzero(sk.data(), sk.size());
    proof.signature = Base64UrlEncode(sig.data(), sig.size());
    return proof;
}

std::string DeviceIdentity::GenerateNonce() {
    EnsureSodiumInitialized();
    std::array<uint8_t, 32> nonceBytes{};
    randombytes_buf(nonceBytes.data(), nonceBytes.size());
    return Base64UrlEncode(nonceBytes.data(), nonceBytes.size());
}

DeviceIdentity::DeviceProof DeviceIdentity::CreateProof(std::string_view secretKeyBase64Url,
                                                        std::string_view operation,
                                                        std::string_view publicId,
                                                        std::string_view credential,
                                                        int64_t timestamp) {
    return SignProofDeterministic(secretKeyBase64Url, operation, publicId, timestamp, GenerateNonce(), credential);
}

std::string DeviceIdentity::LocalDeviceName() {
    wchar_t buffer[256] = {};
    DWORD size = static_cast<DWORD>(std::size(buffer));
    if (!GetComputerNameExW(ComputerNamePhysicalDnsHostname, buffer, &size) || size == 0) {
        size = static_cast<DWORD>(std::size(buffer));
        if (!GetComputerNameW(buffer, &size) || size == 0) {
            return "Windows PC";
        }
    }
    const int needed = WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(size), nullptr, 0, nullptr, nullptr);
    if (needed <= 0) return "Windows PC";
    std::string utf8(static_cast<size_t>(needed), '\0');
    WideCharToMultiByte(CP_UTF8, 0, buffer, static_cast<int>(size), utf8.data(), needed, nullptr, nullptr);
    utf8 = TrimWhitespace(utf8);
    if (utf8.empty()) return "Windows PC";
    if (utf8.size() > 100) utf8.resize(100);
    return utf8;
}

bool DeviceIdentity::EnsureDeviceKey(DeviceKeyPair* outKey) {
    EnsureSodiumInitialized();
    const ConfigData conf = Config::Read();
    DeviceKeyPair pair;
    if (!conf.account_device_key.empty() && DeriveKeyPairFromSecret(conf.account_device_key, pair)) {
        if (conf.account_device_public_id != pair.publicId) {
            Config::Update([&](ConfigData& c) {
                c.account_device_public_id = pair.publicId;
            });
            Config::Save();
        }
        if (outKey) *outKey = pair;
        return true;
    }

    std::array<uint8_t, 32> seed{};
    randombytes_buf(seed.data(), seed.size());
    pair = DeriveKeyPairFromSeed(seed);
    sodium_memzero(seed.data(), seed.size());
    if (pair.publicId.empty()) return false;

    Config::Update([&](ConfigData& c) {
        c.account_device_key = pair.secretKeyBase64Url;
        c.account_device_public_id = pair.publicId;
    });
    Config::Save();

    if (outKey) *outKey = pair;
    return true;
}

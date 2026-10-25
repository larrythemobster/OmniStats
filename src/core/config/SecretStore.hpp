#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#ifdef small
#undef small
#endif

#include <functional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <utility>
#include <vector>

class SecretStore {
  public:
    struct CryptResult {
        bool success = false;
        std::string value;
        DWORD errorCode = 0;
    };

    struct DecryptResult {
        bool success = false;
        std::string value;
        bool needsReencryption = false;
        DWORD errorCode = 0;
    };

    using CryptProtectHook = std::function<bool(const std::string&, std::string&, DWORD&)>;
    using CryptUnprotectHook = std::function<bool(const std::string&, std::string&, DWORD&)>;

    SecretStore() = default;

    static std::string StringToHex(std::string_view input);
    static bool IsHexString(std::string_view s);
    static std::string HexToString(std::string_view input);
    static bool HasDpapiSignature(std::string_view bytes);

    CryptResult EncryptSecret(const std::string& plainText) const;
    DecryptResult DecryptSecret(const std::string& raw) const;

    void LoadSecretField(const std::string& fieldName, const std::string& rawValue,
                         std::string& targetValue, bool& outNeedsLegacyMigration,
                         bool& outCredentialsDecryptionFailed);

    void SaveSecretField(const std::string& fieldName, const std::string& currentPlainVal,
                         std::string& outDiskValue);

    bool MigrateLegacySecrets(const std::vector<std::pair<std::string, std::string>>& secretsToMigrate);

    void Reset();

    static void SetCryptProtectHookForTests(CryptProtectHook hook);
    static void SetCryptUnprotectHookForTests(CryptUnprotectHook hook);
    static void ResetCryptHooksForTests();

  private:
    std::unordered_map<std::string, std::string> m_savedCiphertexts;
    std::unordered_map<std::string, std::string> m_encryptedPlaintexts;
};

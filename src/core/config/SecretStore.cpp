#include "core/config/SecretStore.hpp"
#include <cctype>
#include <cstring>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <wincrypt.h>

#ifdef OMNISTATS_TEST_ENVIRONMENT
static SecretStore::CryptProtectHook s_protectHook = nullptr;
static SecretStore::CryptUnprotectHook s_unprotectHook = nullptr;

void SecretStore::SetCryptProtectHookForTests(CryptProtectHook hook) {
    s_protectHook = std::move(hook);
}

void SecretStore::SetCryptUnprotectHookForTests(CryptUnprotectHook hook) {
    s_unprotectHook = std::move(hook);
}

void SecretStore::ResetCryptHooksForTests() {
    s_protectHook = nullptr;
    s_unprotectHook = nullptr;
}
#else
void SecretStore::SetCryptProtectHookForTests(CryptProtectHook) {}
void SecretStore::SetCryptUnprotectHookForTests(CryptUnprotectHook) {}
void SecretStore::ResetCryptHooksForTests() {}
#endif

std::string SecretStore::StringToHex(std::string_view input) {
    std::stringstream ss;
    ss << std::hex << std::setfill('0');
    for (unsigned char c : input) {
        ss << std::setw(2) << static_cast<int>(c);
    }
    return ss.str();
}

bool SecretStore::IsHexString(std::string_view s) {
    if (s.empty() || s.length() % 2 != 0) return false;
    for (char c : s) {
        if (!std::isxdigit(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

std::string SecretStore::HexToString(std::string_view input) {
    if (!IsHexString(input)) return "";
    std::string output;
    output.reserve(input.length() / 2);
    try {
        for (size_t i = 0; i < input.length(); i += 2) {
            std::string part(input.substr(i, 2));
            char chr = static_cast<char>(std::stoul(part, nullptr, 16));
            output.push_back(chr);
        }
    } catch (...) {
        return "";
    }
    return output;
}

bool SecretStore::HasDpapiSignature(std::string_view bytes) {
    if (bytes.size() < 20) return false;
    static const unsigned char kDpapiHeader[8] = {0x01, 0x00, 0x00, 0x00, 0xd0, 0x02, 0x9a, 0xdf};
    return std::memcmp(bytes.data(), kDpapiHeader, sizeof(kDpapiHeader)) == 0;
}

SecretStore::CryptResult SecretStore::EncryptSecret(const std::string& plainText) const {
    if (plainText.empty()) {
        return {true, "", 0};
    }
#ifdef OMNISTATS_TEST_ENVIRONMENT
    if (s_protectHook) {
        std::string cipher;
        DWORD err = 0;
        if (s_protectHook(plainText, cipher, err)) {
            return {true, cipher, 0};
        }
        return {false, "", err ? err : static_cast<DWORD>(ERROR_ENCRYPTION_FAILED)};
    }
#endif
    DATA_BLOB inputBlob;
    inputBlob.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(plainText.data()));
    inputBlob.cbData = static_cast<DWORD>(plainText.size());

    DATA_BLOB outputBlob{};
    if (CryptProtectData(&inputBlob, L"OmniStats Token", nullptr, nullptr, nullptr,
                         CRYPTPROTECT_UI_FORBIDDEN, &outputBlob)) {
        std::string encryptedBytes(reinterpret_cast<char*>(outputBlob.pbData), outputBlob.cbData);
        LocalFree(outputBlob.pbData);
        return {true, StringToHex(encryptedBytes), 0};
    }
    const DWORD err = GetLastError();
    std::cerr << "[Config] DPAPI encryption failed with error: " << err << "\n";
    return {false, "", err};
}

SecretStore::DecryptResult SecretStore::DecryptSecret(const std::string& raw) const {
    if (raw.empty()) {
        return {true, "", false, 0};
    }
#ifdef OMNISTATS_TEST_ENVIRONMENT
    if (s_unprotectHook) {
        std::string plain;
        DWORD err = 0;
        if (s_unprotectHook(raw, plain, err)) {
            return {true, plain, false, 0};
        }
        return {false, "", false, err ? err : static_cast<DWORD>(ERROR_DECRYPTION_FAILED)};
    }
#endif
    if (IsHexString(raw)) {
        std::string cipherBytes = HexToString(raw);
        if (!cipherBytes.empty()) {
            DATA_BLOB inputBlob;
            inputBlob.pbData = reinterpret_cast<BYTE*>(cipherBytes.data());
            inputBlob.cbData = static_cast<DWORD>(cipherBytes.size());

            DATA_BLOB outputBlob{};
            if (CryptUnprotectData(&inputBlob, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN, &outputBlob)) {
                std::string decryptedBytes(reinterpret_cast<char*>(outputBlob.pbData), outputBlob.cbData);
                LocalFree(outputBlob.pbData);
                DWORD flags = 0;
                if (cipherBytes.size() >= 44) std::memcpy(&flags, cipherBytes.data() + 40, sizeof(flags));
                return {true, decryptedBytes, (flags & CRYPTPROTECT_LOCAL_MACHINE) != 0, 0};
            }
            const DWORD err = GetLastError();
            if (HasDpapiSignature(cipherBytes)) {
                std::cerr << "[Config] DPAPI decryption failed for ciphertext with error: " << err << "\n";
                return {false, "", false, err};
            }
        }
    }
    return {true, raw, true, 0};
}

void SecretStore::LoadSecretField(const std::string& fieldName, const std::string& rawValue,
                                  std::string& targetValue, bool& outNeedsLegacyMigration,
                                  bool& outCredentialsDecryptionFailed) {
    if (!rawValue.empty()) {
        const auto dec = DecryptSecret(rawValue);
        if (dec.success) {
            targetValue = dec.value;
            if (dec.needsReencryption) {
                outNeedsLegacyMigration = true;
                m_encryptedPlaintexts.erase(fieldName);
                if (HasDpapiSignature(HexToString(rawValue))) {
                    m_savedCiphertexts[fieldName] = rawValue;
                } else {
                    m_savedCiphertexts.erase(fieldName);
                }
            } else {
                m_savedCiphertexts[fieldName] = rawValue;
                m_encryptedPlaintexts[fieldName] = dec.value;
            }
        } else {
            targetValue.clear();
            m_savedCiphertexts[fieldName] = rawValue;
            m_encryptedPlaintexts.erase(fieldName);
            outCredentialsDecryptionFailed = true;
            std::cerr << "[Config] Failed to decrypt " << fieldName << ". Preserving ciphertext.\n";
        }
    } else {
        targetValue.clear();
        m_savedCiphertexts.erase(fieldName);
        m_encryptedPlaintexts.erase(fieldName);
    }
}

void SecretStore::SaveSecretField(const std::string& fieldName, const std::string& currentPlainVal,
                                  std::string& outDiskValue) {
    if (currentPlainVal.empty()) {
        if (m_savedCiphertexts.count(fieldName) && !m_savedCiphertexts[fieldName].empty() &&
            m_encryptedPlaintexts.find(fieldName) == m_encryptedPlaintexts.end()) {
            outDiskValue = m_savedCiphertexts[fieldName];
            return;
        }
        outDiskValue = "";
        m_savedCiphertexts.erase(fieldName);
        m_encryptedPlaintexts.erase(fieldName);
        return;
    }
    if (m_encryptedPlaintexts.count(fieldName) && m_encryptedPlaintexts[fieldName] == currentPlainVal &&
        m_savedCiphertexts.count(fieldName) && !m_savedCiphertexts[fieldName].empty()) {
        outDiskValue = m_savedCiphertexts[fieldName];
        return;
    }
    const auto enc = EncryptSecret(currentPlainVal);
    if (enc.success) {
        m_savedCiphertexts[fieldName] = enc.value;
        m_encryptedPlaintexts[fieldName] = currentPlainVal;
        outDiskValue = enc.value;
    } else {
        std::cerr << "[Config] Encryption failed for " << fieldName << ". Secret will not be written to disk in plaintext.\n";
        if (m_savedCiphertexts.count(fieldName) && !m_savedCiphertexts[fieldName].empty()) {
            outDiskValue = m_savedCiphertexts[fieldName];
        } else {
            outDiskValue = "";
        }
    }
}

bool SecretStore::MigrateLegacySecrets(const std::vector<std::pair<std::string, std::string>>& secretsToMigrate) {
    bool ok = true;
    for (const auto& [name, plain] : secretsToMigrate) {
        if (!plain.empty() &&
            (m_encryptedPlaintexts.find(name) == m_encryptedPlaintexts.end() ||
             m_encryptedPlaintexts[name] != plain)) {
            const auto enc = EncryptSecret(plain);
            if (enc.success) {
                m_savedCiphertexts[name] = enc.value;
                m_encryptedPlaintexts[name] = plain;
            } else {
                ok = false;
            }
        }
    }
    return ok;
}

void SecretStore::Reset() {
    m_savedCiphertexts.clear();
    m_encryptedPlaintexts.clear();
}

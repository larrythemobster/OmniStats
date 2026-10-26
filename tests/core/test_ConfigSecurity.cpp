#include <gtest/gtest.h>
#include "core/config/Config.hpp"
#include "core/config/SecretStore.hpp"
#include "utils/Storage.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>
#include <wincrypt.h>
#include <iomanip>
#include <sstream>
#include <cstring>

class ConfigSecurityTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Storage::InitializeEnvironment();
        m_originalConfig = Config::Read();
    }

    void TearDown() override {
        Config::Update([this](ConfigData& c) { c = m_originalConfig; });
        Config::Save();
    }

    std::string ReadRawConfigFile() {
        const std::string path = Storage::GetDataDirectory() + "config.json";
        std::ifstream file(path);
        if (!file.is_open()) return "";
        return std::string(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    void WriteRawConfigFile(const std::string& content) {
        const std::string path = Storage::GetDataDirectory() + "config.json";
        std::ofstream file(path, std::ios::trunc);
        file << content;
        file.flush();
    }

    ConfigData m_originalConfig;
};

TEST_F(ConfigSecurityTest, EncryptionFailureNeverWritesPlaintextToDisk) {
    const std::string testNewSecret = "test_val_alpha_001";

    SecretStore secrets([](const std::string&, std::string&, DWORD& err) {
        err = 0x8009000B;
        return false;
    });

    Config::Update([&](ConfigData& c) {
        c.custom_api_key = testNewSecret;
        c.port = 55555; // Unrelated setting
    });
    Config::Save(secrets);

    // Verify plaintext is NEVER written to the config file on disk
    const std::string raw = ReadRawConfigFile();
    EXPECT_EQ(raw.find(testNewSecret), std::string::npos);

    // Verify unrelated settings are still saved properly
    const auto j = nlohmann::json::parse(raw);
    EXPECT_EQ(j.value("port", 0), 55555);
}

TEST_F(ConfigSecurityTest, EncryptionFailurePreservesPreviouslySavedEncryptedValue) {
    const std::string testInitialSecret = "test_val_bravo_111";

    // Step 1: Save with normal encryption working
    Config::Update([&](ConfigData& c) {
        c.custom_api_key = testInitialSecret;
    });
    Config::Save();

    const std::string initialRaw = ReadRawConfigFile();
    const auto initialJson = nlohmann::json::parse(initialRaw);
    const std::string savedCiphertext = initialJson.value("custom_api_key", "");
    EXPECT_FALSE(savedCiphertext.empty());
    EXPECT_NE(savedCiphertext, testInitialSecret);

    // Step 2: User tries to change key, but DPAPI encryption fails
    SecretStore secrets([](const std::string&, std::string&, DWORD& err) {
        err = 0x80090005;
        return false;
    });
    Config::Load(secrets);
    const std::string testFailedSecret = "test_val_charlie_failed";

    Config::Update([&](ConfigData& c) {
        c.custom_api_key = testFailedSecret;
    });
    Config::Save(secrets);

    // Verify the previously saved valid encrypted ciphertext was preserved on disk!
    const std::string updatedRaw = ReadRawConfigFile();
    EXPECT_EQ(updatedRaw.find(testFailedSecret), std::string::npos);
    const auto updatedJson = nlohmann::json::parse(updatedRaw);
    EXPECT_EQ(updatedJson.value("custom_api_key", ""), savedCiphertext);
}

TEST_F(ConfigSecurityTest, MigratesLegacyPlaintextCredentialsToEncryptedStorage) {
    // Write a legacy config file containing plaintext secrets
    const std::string legacyPlainSecret = "legacy_test_string_445566";
    const std::string legacyPlainBcValue = "legacy_test_string_778899";
    nlohmann::json legacyJson;
    legacyJson["custom_api_key"] = legacyPlainSecret;
    legacyJson["ballchasing_token"] = legacyPlainBcValue;
    legacyJson["port"] = 49123;
    WriteRawConfigFile(legacyJson.dump(4));

    // Load config
    Config::Load();

    // Verify credentials were read into memory correctly
    const auto loaded = Config::Read();
    EXPECT_EQ(loaded.custom_api_key, legacyPlainSecret);
    EXPECT_EQ(loaded.ballchasing_token, legacyPlainBcValue);

    // Verify disk now contains encrypted ciphertexts, NOT plaintext
    const std::string rawAfterLoad = ReadRawConfigFile();
    EXPECT_EQ(rawAfterLoad.find(legacyPlainSecret), std::string::npos);
    EXPECT_EQ(rawAfterLoad.find(legacyPlainBcValue), std::string::npos);

    const auto jsonAfterLoad = nlohmann::json::parse(rawAfterLoad);
    EXPECT_NE(jsonAfterLoad.value("custom_api_key", ""), legacyPlainSecret);
    EXPECT_FALSE(jsonAfterLoad.value("custom_api_key", "").empty());
}

TEST_F(ConfigSecurityTest, FailedLegacyMigrationNeverRewritesPlaintext) {
    const std::string legacyFailingSecret = "legacy_fail_string_998877";
    nlohmann::json legacyJson;
    legacyJson["custom_api_key"] = legacyFailingSecret;
    legacyJson["port"] = 49123;
    WriteRawConfigFile(legacyJson.dump(4));

    SecretStore secrets([](const std::string&, std::string&, DWORD& err) {
        err = 0x8009000B;
        return false;
    });

    Config::Load(secrets);

    // Since migration failed, SaveInternal must not write plaintext
    Config::Save(secrets);
    const std::string raw = ReadRawConfigFile();
    EXPECT_EQ(raw.find(legacyFailingSecret), std::string::npos);
}

TEST_F(ConfigSecurityTest, CorruptedCiphertextPreservesDiskValueWithoutCorruptingUnrelatedSettings) {
    // Write a config with corrupted DPAPI hex (DPAPI header followed by invalid ciphertext)
    // Header: 01000000d0029adf0e1c6443969482a170fb26fb followed by corrupted bytes
    const std::string corruptedCipherHex = "01000000d0029adf0e1c6443969482a170fb26fbdeadbeefcafebabe";
    nlohmann::json corruptJson;
    corruptJson["custom_api_key"] = corruptedCipherHex;
    corruptJson["port"] = 33333;
    WriteRawConfigFile(corruptJson.dump(4));

    Config::Load();
    const auto loaded = Config::Read();
    EXPECT_TRUE(loaded.custom_api_key.empty()); // In-memory secret is cleared, not populated with corrupted garbage
    EXPECT_EQ(loaded.port, 33333);

    // Save and verify disk preserves the ciphertext without corrupting config
    Config::Save();
    const std::string raw = ReadRawConfigFile();
    const auto afterJson = nlohmann::json::parse(raw);
    EXPECT_EQ(afterJson.value("custom_api_key", ""), corruptedCipherHex);
    EXPECT_EQ(afterJson.value("port", 0), 33333);
}

TEST_F(ConfigSecurityTest, InjectedDecryptionFailurePreservesCiphertext) {
    const std::string ciphertext = "01000000d0029adf0e1c6443969482a170fb26fbdeadbeef";
    WriteRawConfigFile(nlohmann::json{{"custom_api_key", ciphertext}}.dump());
    SecretStore secrets({}, [](const std::string&, std::string&, DWORD& error) {
        error = ERROR_DECRYPTION_FAILED;
        return false;
    });

    Config::Load(secrets);
    EXPECT_TRUE(Config::Read().credentials_decryption_failed);
    EXPECT_TRUE(Config::Read().custom_api_key.empty());
    Config::Save(secrets);
    EXPECT_EQ(nlohmann::json::parse(ReadRawConfigFile()).value("custom_api_key", ""), ciphertext);
}

TEST_F(ConfigSecurityTest, MachineScopedCredentialsMigrateWithoutLosingTheSecret) {
    const std::string secret = "synthetic_machine_scope_migration";
    DATA_BLOB input{static_cast<DWORD>(secret.size()),
                    reinterpret_cast<BYTE*>(const_cast<char*>(secret.data()))};
    DATA_BLOB output{};
    ASSERT_TRUE(CryptProtectData(&input, L"OmniStats Token", nullptr, nullptr, nullptr,
                                 CRYPTPROTECT_UI_FORBIDDEN | CRYPTPROTECT_LOCAL_MACHINE, &output));
    std::ostringstream hex;
    hex << std::hex << std::setfill('0');
    for (DWORD i = 0; i < output.cbData; ++i)
        hex << std::setw(2) << static_cast<unsigned>(output.pbData[i]);
    LocalFree(output.pbData);
    WriteRawConfigFile(nlohmann::json{{"custom_api_key", hex.str()}}.dump());

    Config::Load();
    EXPECT_EQ(Config::Read().custom_api_key, secret);
    const auto stored = nlohmann::json::parse(ReadRawConfigFile()).at("custom_api_key").get<std::string>();
    ASSERT_GE(stored.size(), 88u);
    std::string bytes;
    for (size_t i = 0; i < stored.size(); i += 2)
        bytes.push_back(static_cast<char>(std::stoul(stored.substr(i, 2), nullptr, 16)));
    DWORD flags = 0;
    std::memcpy(&flags, bytes.data() + 40, sizeof(flags));
    EXPECT_EQ(flags & CRYPTPROTECT_LOCAL_MACHINE, 0u);
    DATA_BLOB encrypted{static_cast<DWORD>(bytes.size()), reinterpret_cast<BYTE*>(bytes.data())};
    ASSERT_TRUE(CryptUnprotectData(&encrypted, nullptr, nullptr, nullptr, nullptr,
                                   CRYPTPROTECT_UI_FORBIDDEN, &output));
    EXPECT_EQ(std::string(reinterpret_cast<char*>(output.pbData), output.cbData), secret);
    LocalFree(output.pbData);
}
TEST(SecretStoreInstanceTest, EncryptDecryptAndPreserveCiphertextOnFailure) {
    SecretStore store;

    const std::string plain = "super_secret_api_token_12345";
    auto enc = store.EncryptSecret(plain);
    ASSERT_TRUE(enc.success);
    EXPECT_FALSE(enc.value.empty());
    EXPECT_NE(enc.value, plain);
    EXPECT_TRUE(SecretStore::IsHexString(enc.value));

    auto dec = store.DecryptSecret(enc.value);
    EXPECT_TRUE(dec.success);
    EXPECT_EQ(dec.value, plain);

    std::string loaded;
    bool needsLegacyMigration = false;
    bool decryptionFailed = false;
    store.LoadSecretField("custom_api_key", enc.value, loaded, needsLegacyMigration, decryptionFailed);
    EXPECT_EQ(loaded, plain);
    EXPECT_FALSE(needsLegacyMigration);
    EXPECT_FALSE(decryptionFailed);

    std::string diskVal;
    store.SaveSecretField("custom_api_key", loaded, diskVal);
    EXPECT_EQ(diskVal, enc.value);

    SecretStore failingStore([](const std::string&, std::string&, DWORD& err) {
        err = 0x8009000B;
        return false;
    });
    failingStore.LoadSecretField("custom_api_key", enc.value, loaded, needsLegacyMigration, decryptionFailed);

    std::string diskValAfterFail;
    failingStore.SaveSecretField("custom_api_key", "new_secret_attempt", diskValAfterFail);
    EXPECT_EQ(diskValAfterFail, enc.value);
}

TEST(SecretStoreInstanceTest, InjectedFunctionsAreInstanceScoped) {
    SecretStore first(
        [](const std::string& plain, std::string& cipher, DWORD&) {
            cipher = "first:" + plain;
            return true;
        },
        [](const std::string& cipher, std::string& plain, DWORD&) {
            plain = cipher.substr(6);
            return true;
        });
    SecretStore second(
        [](const std::string&, std::string&, DWORD& error) {
            error = 123;
            return false;
        },
        [](const std::string&, std::string&, DWORD& error) {
            error = 456;
            return false;
        });

    EXPECT_EQ(first.EncryptSecret("secret").value, "first:secret");
    EXPECT_EQ(first.DecryptSecret("first:secret").value, "secret");
    EXPECT_EQ(second.EncryptSecret("secret").errorCode, 123u);
    EXPECT_EQ(second.DecryptSecret("first:secret").errorCode, 456u);
    EXPECT_EQ(first.DecryptSecret("first:secret").value, "secret");
}

TEST(SecretStoreInstanceTest, LegacyMigrationAndDecryptionErrorHandling) {
    SecretStore store;

    std::string loaded;
    bool needsLegacyMigration = false;
    bool decryptionFailed = false;
    const std::string legacyPlain = "unencrypted_legacy_credential";
    store.LoadSecretField("custom_api_key", legacyPlain, loaded, needsLegacyMigration, decryptionFailed);
    EXPECT_EQ(loaded, legacyPlain);
    EXPECT_TRUE(needsLegacyMigration);
    EXPECT_FALSE(decryptionFailed);

    const std::vector<std::pair<std::string, std::string>> toMigrate = {
        {"custom_api_key", legacyPlain}};
    EXPECT_TRUE(store.MigrateLegacySecrets(toMigrate));

    std::string diskVal;
    store.SaveSecretField("custom_api_key", legacyPlain, diskVal);
    EXPECT_NE(diskVal, legacyPlain);
    EXPECT_TRUE(SecretStore::IsHexString(diskVal));

    std::string corruptedLoaded;
    needsLegacyMigration = false;
    decryptionFailed = false;
    const std::string corruptedHex = "01000000d0029adf0e1c6443969482a170fb26fbdeadbeefcafebabe";
    store.LoadSecretField("custom_api_key", corruptedHex, corruptedLoaded, needsLegacyMigration, decryptionFailed);
    EXPECT_TRUE(corruptedLoaded.empty());
    EXPECT_TRUE(decryptionFailed);

    std::string corruptedDiskVal;
    store.SaveSecretField("custom_api_key", corruptedLoaded, corruptedDiskVal);
    EXPECT_EQ(corruptedDiskVal, corruptedHex);
}

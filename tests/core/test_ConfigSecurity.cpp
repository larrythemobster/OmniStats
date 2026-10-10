#include <gtest/gtest.h>
#include "core/Config.hpp"
#include "core/Storage.hpp"

#include <filesystem>
#include <fstream>
#include <nlohmann/json.hpp>

class ConfigSecurityTest : public ::testing::Test {
  protected:
    void SetUp() override {
        Storage::InitializeEnvironment();
        m_originalConfig = Config::Read();
        Config::ResetCryptHooksForTests();
    }

    void TearDown() override {
        Config::ResetCryptHooksForTests();
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

    // Inject encryption failure
    Config::SetCryptProtectHookForTests([](const std::string&, std::string&, DWORD& err) {
        err = 0x8009000B; // NTE_BAD_KEY_STATE or simulated DPAPI error
        return false;
    });

    Config::Update([&](ConfigData& c) {
        c.custom_api_key = testNewSecret;
        c.port = 55555; // Unrelated setting
    });
    Config::Save();

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
    Config::ResetCryptHooksForTests();
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
    const std::string testFailedSecret = "test_val_charlie_failed";
    Config::SetCryptProtectHookForTests([](const std::string&, std::string&, DWORD& err) {
        err = 0x80090005;
        return false;
    });

    Config::Update([&](ConfigData& c) {
        c.custom_api_key = testFailedSecret;
    });
    Config::Save();

    // Verify the previously saved valid encrypted ciphertext was preserved on disk!
    const std::string updatedRaw = ReadRawConfigFile();
    EXPECT_EQ(updatedRaw.find(testFailedSecret), std::string::npos);
    const auto updatedJson = nlohmann::json::parse(updatedRaw);
    EXPECT_EQ(updatedJson.value("custom_api_key", ""), savedCiphertext);
}

TEST_F(ConfigSecurityTest, MigratesLegacyPlaintextCredentialsToEncryptedStorage) {
    Config::ResetCryptHooksForTests();

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

    // Force encryption failure during migration
    Config::SetCryptProtectHookForTests([](const std::string&, std::string&, DWORD& err) {
        err = 0x8009000B;
        return false;
    });

    Config::Load();

    // Since migration failed, SaveInternal must not write plaintext
    Config::Save();
    const std::string raw = ReadRawConfigFile();
    EXPECT_EQ(raw.find(legacyFailingSecret), std::string::npos);
}

TEST_F(ConfigSecurityTest, CorruptedCiphertextPreservesDiskValueWithoutCorruptingUnrelatedSettings) {
    Config::ResetCryptHooksForTests();

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

#include <gtest/gtest.h>
#include "core/Storage.hpp"
#include <nlohmann/json.hpp>
#include <filesystem>
#include <fstream>

TEST(StorageTest, DataDirectoryIsValid) {
    std::string path = Storage::GetDataDirectory();
    EXPECT_FALSE(path.empty());
    // Path should end with backslash
    EXPECT_EQ(path.back(), '\\');
}

TEST(StorageTest, AppendMatchDoesNotCrash) {
    Storage::InitializeEnvironment();
    nlohmann::json dummy = {{"test", "match"}};
    EXPECT_NO_THROW(Storage::AppendMatchSync(dummy));
}

TEST(StorageTest, AppendMMRDoesNotCrash) {
    Storage::InitializeEnvironment();
    nlohmann::json dummy = {{"test", "mmr"}};
    EXPECT_NO_THROW(Storage::AppendMMRHistory(dummy));
}

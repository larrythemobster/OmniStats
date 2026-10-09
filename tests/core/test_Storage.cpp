#include <gtest/gtest.h>
#include "core/Storage.hpp"
#include <filesystem>

TEST(StorageTest, DataDirectoryIsValid) {
    std::string path = Storage::GetDataDirectory();
    EXPECT_FALSE(path.empty());
    // Path should end with backslash
    EXPECT_EQ(path.back(), '\\');
}

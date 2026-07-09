#include <gtest/gtest.h>
#include "network/ReplayUploader.hpp"
#include "core/SessionState.hpp"
#include <memory>

TEST(ReplayUploaderTest, Lifecycle) {
    auto state = std::make_shared<SessionState>();
    ReplayUploader uploader(state);
    
    EXPECT_NO_THROW(uploader.Start());
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    EXPECT_NO_THROW(uploader.Stop());
}

#include "network/omninet/OmniNet.hpp"
#include "updater/UpdaterPaths.hpp"
#include "utils/Env.hpp"
#include "utils/Storage.hpp"

#include <gtest/gtest.h>
#include <optional>
#include <string>

namespace {
    class TestEnvironment final : public ::testing::Environment {
      public:
        void SetUp() override {
            const std::optional<std::string> temp = Env::Get("TEMP") ? Env::Get("TEMP") : Env::Get("TMP");
            const std::string directory = temp.value_or(".") + "\\omnistats_test\\";
            Storage::SetDataDirectoryOverride(directory);
            UpdaterCommon::SetAppDataDirOverride(directory);
            OmniNet::SetDefaultTransportForTesting([](const OmniNet::Request&) {
                OmniNet::Response response;
                response.transportError = -1;
                return response;
            });
        }
    };

    [[maybe_unused]] const auto* environment = ::testing::AddGlobalTestEnvironment(new TestEnvironment);
}

#include "ui/rml/presenters/ReleaseBanners.hpp"
#include <gtest/gtest.h>
#include "core/state/SessionState.hpp"

TEST(ReleaseBanners, DatabaseUpgradeBannerFollowsSessionFlag) {
    auto state = std::make_shared<SessionState>();
    EXPECT_EQ(RenderReleaseBanners(state, nullptr).find("db-upgrade-banner"), std::string::npos);

    state->ui.databaseUpgradeIncomplete.store(true);
    const std::string html = RenderReleaseBanners(state, nullptr);
    EXPECT_NE(html.find("db-upgrade-banner"), std::string::npos);
    EXPECT_NE(html.find("data-action='help-discord'"), std::string::npos);
}

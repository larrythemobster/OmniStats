#include <gtest/gtest.h>
#include "ui/widgets/MmrGraphWidget.hpp"
#include <vector>

TEST(MmrGraphWidgetStateTest, GraphParamsInitialization) {
    std::vector<float> history = {1000.0f, 1010.0f, 1005.0f};
    Widgets::MmrGraphParams params{
        history,
        1005,
        1000,
        150.0f};

    EXPECT_EQ(params.history.size(), 3);
    EXPECT_EQ(params.currentMmr, 1005);
    EXPECT_EQ(params.initialMmr, 1000);
    EXPECT_FLOAT_EQ(params.plotHeight, 150.0f);
}

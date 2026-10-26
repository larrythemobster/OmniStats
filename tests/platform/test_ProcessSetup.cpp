#include <gtest/gtest.h>
#include "app/ProcessSetup.hpp"

TEST(ProcessSetupTest, SingleInstanceMutexEnforcesSingleProcess) {
    AppShell::ProcessSetup::ReleaseSingleInstance();

    HANDLE hExistingInstance = CreateMutexW(nullptr, TRUE, L"Local\\OmniStats_SingleInstance_Mutex");
    ASSERT_NE(hExistingInstance, nullptr);

    EXPECT_FALSE(AppShell::ProcessSetup::EnsureSingleInstance());

    CloseHandle(hExistingInstance);

    EXPECT_TRUE(AppShell::ProcessSetup::EnsureSingleInstance());
    AppShell::ProcessSetup::ReleaseSingleInstance();
}

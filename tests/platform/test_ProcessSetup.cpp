#include <gtest/gtest.h>
#include "app/ProcessSetup.hpp"

TEST(ProcessSetupTest, SingleInstanceMutexEnforcesSingleProcess) {
    AppShell::ProcessSetup::ReleaseSingleInstance();

    HANDLE hExistingInstance = CreateMutexW(nullptr, TRUE, L"Local\\OmniStats_SingleInstance_Mutex");
    const DWORD error = GetLastError();
    ASSERT_NE(hExistingInstance, nullptr);
    if (error == ERROR_ALREADY_EXISTS) {
        CloseHandle(hExistingInstance);
        GTEST_SKIP() << "Another OmniStats process owns the session mutex.";
    }

    EXPECT_FALSE(AppShell::ProcessSetup::EnsureSingleInstance());

    CloseHandle(hExistingInstance);

    EXPECT_TRUE(AppShell::ProcessSetup::EnsureSingleInstance());
    AppShell::ProcessSetup::ReleaseSingleInstance();
}

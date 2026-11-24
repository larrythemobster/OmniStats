#pragma once
#include <windows.h>

namespace UpdaterCommon {

    bool WaitForProcessExit(DWORD processId, DWORD timeoutMs);

} // namespace UpdaterCommon

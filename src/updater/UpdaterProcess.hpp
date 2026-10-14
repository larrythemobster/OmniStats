#pragma once
#include <windows.h>
#include <string>

namespace UpdaterCommon {

    bool ProcessImageMatches(DWORD processId, const std::string& targetPath);
    bool WaitForProcessExit(DWORD processId, DWORD timeoutMs);
    bool TerminateProcessById(DWORD processId);
    bool TerminateProcessesRunningFromPath(const std::string& targetPath);

} // namespace UpdaterCommon

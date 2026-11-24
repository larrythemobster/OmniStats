#include "updater/UpdaterProcess.hpp"

namespace UpdaterCommon {

    bool WaitForProcessExit(DWORD processId, DWORD timeoutMs) {
        if (processId == 0 || processId == GetCurrentProcessId()) {
            return true;
        }
        HANDLE process = OpenProcess(SYNCHRONIZE, FALSE, processId);
        if (!process) {
            return true;
        }
        DWORD waitResult = WaitForSingleObject(process, timeoutMs);
        CloseHandle(process);
        return waitResult == WAIT_OBJECT_0;
    }

} // namespace UpdaterCommon

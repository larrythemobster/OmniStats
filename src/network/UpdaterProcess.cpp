#include "network/UpdaterProcess.hpp"
#include <tlhelp32.h>
#include <iostream>

namespace UpdaterCommon {

bool ProcessImageMatches(DWORD processId, const std::string& targetPath) {
    if (processId == 0 || processId == GetCurrentProcessId()) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, processId);
    if (!process) {
        return false;
    }
    char processPath[MAX_PATH] = {0};
    DWORD processPathLen = MAX_PATH;
    bool matches = false;
    if (QueryFullProcessImageNameA(process, 0, processPath, &processPathLen)) {
        if (processPathLen >= MAX_PATH) {
            processPathLen = MAX_PATH - 1;
        }
        processPath[processPathLen] = '\0';
        matches = _stricmp(processPath, targetPath.c_str()) == 0;
    }
    CloseHandle(process);
    return matches;
}

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

bool TerminateProcessById(DWORD processId) {
    if (processId == 0 || processId == GetCurrentProcessId()) {
        return false;
    }
    HANDLE process = OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, processId);
    if (!process) {
        return false;
    }
    bool terminated = TerminateProcess(process, 0) != FALSE;
    if (terminated) {
        WaitForSingleObject(process, 5000);
    }
    CloseHandle(process);
    return terminated;
}

bool TerminateProcessesRunningFromPath(const std::string& targetPath) {
    HANDLE snapshot = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (snapshot == INVALID_HANDLE_VALUE) {
        return false;
    }
    bool terminatedAny = false;
    PROCESSENTRY32 entry = {};
    entry.dwSize = sizeof(entry);
    if (Process32First(snapshot, &entry)) {
        do {
            if (ProcessImageMatches(entry.th32ProcessID, targetPath)) {
                terminatedAny = TerminateProcessById(entry.th32ProcessID) || terminatedAny;
            }
        } while (Process32Next(snapshot, &entry));
    }
    CloseHandle(snapshot);
    return terminatedAny;
}

} // namespace UpdaterCommon

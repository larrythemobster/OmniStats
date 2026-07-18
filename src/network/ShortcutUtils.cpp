#include "network/ShortcutUtils.hpp"
#include <windows.h>
#include <shlobj.h>
#include <vector>
#include <cstdlib>

#pragma comment(lib, "shell32.lib")

namespace UpdaterCommon {

    bool CreateShortcut(const std::string& exePath, const std::string& shortcutPath, const std::string& description) {
        HRESULT hr = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
        bool needUninit = SUCCEEDED(hr);

        IShellLinkA* pShellLink = nullptr;
        hr = CoCreateInstance(CLSID_ShellLink, NULL, CLSCTX_INPROC_SERVER, IID_IShellLinkA, (LPVOID*)&pShellLink);
        if (SUCCEEDED(hr)) {
            pShellLink->SetPath(exePath.c_str());
            pShellLink->SetDescription(description.c_str());
            size_t lastSlash = exePath.find_last_of("\\/");
            if (lastSlash != std::string::npos) {
                pShellLink->SetWorkingDirectory(exePath.substr(0, lastSlash).c_str());
            }

            IPersistFile* pPersistFile = nullptr;
            hr = pShellLink->QueryInterface(IID_IPersistFile, (LPVOID*)&pPersistFile);
            if (SUCCEEDED(hr)) {
                int size_needed = MultiByteToWideChar(CP_UTF8, 0, shortcutPath.c_str(), -1, NULL, 0);
                std::vector<wchar_t> wstr(size_needed);
                MultiByteToWideChar(CP_UTF8, 0, shortcutPath.c_str(), -1, wstr.data(), size_needed);

                hr = pPersistFile->Save(wstr.data(), TRUE);
                pPersistFile->Release();
            }
            pShellLink->Release();
        }

        if (needUninit) {
            CoUninitialize();
        }
        return SUCCEEDED(hr);
    }

    bool RepairShortcutIfExists(const std::string& exePath,
                                const std::string& shortcutPath,
                                const std::string& description) {
        DWORD attributes = GetFileAttributesA(shortcutPath.c_str());
        if (attributes == INVALID_FILE_ATTRIBUTES || (attributes & FILE_ATTRIBUTE_DIRECTORY)) {
            return true;
        }

        return CreateShortcut(exePath, shortcutPath, description);
    }

    bool RepairExistingOmniStatsShortcuts(const std::string& exePath) {
        const std::string description = "OmniStats Rocket League live telemetry companion";
        bool ok = true;

        char szPath[MAX_PATH];
        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_PROGRAMS, NULL, SHGFP_TYPE_CURRENT, szPath))) {
            ok = RepairShortcutIfExists(
                     exePath,
                     std::string(szPath) + "\\OmniStats.lnk",
                     description) &&
                 ok;
        }

        if (SUCCEEDED(SHGetFolderPathA(NULL, CSIDL_DESKTOPDIRECTORY, NULL, SHGFP_TYPE_CURRENT, szPath))) {
            ok = RepairShortcutIfExists(
                     exePath,
                     std::string(szPath) + "\\OmniStats.lnk",
                     description) &&
                 ok;
        }

        return ok;
    }

} // namespace UpdaterCommon

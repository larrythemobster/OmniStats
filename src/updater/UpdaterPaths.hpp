#pragma once
#include <string>

namespace UpdaterCommon {

    std::string GetAppDataDir();
    void SetAppDataDirOverride(std::string path);
    std::string GetLocalAppDataDir();
    bool EnsureDirExists(const std::string& path);

} // namespace UpdaterCommon

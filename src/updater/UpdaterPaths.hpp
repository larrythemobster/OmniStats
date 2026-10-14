#pragma once
#include <string>

namespace UpdaterCommon {

    std::string GetAppDataDir();
    std::string GetLocalAppDataDir();
    bool EnsureDirExists(const std::string& path);

} // namespace UpdaterCommon

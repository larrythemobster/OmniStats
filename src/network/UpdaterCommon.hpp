#pragma once
#include <string>
#include <curl/curl.h>

namespace UpdaterCommon {

    std::string Trim(const std::string& str);
    bool IsNewerVersion(const std::string& current, const std::string& latest);
    std::string GetFileVersion(const std::string& filePath);
    bool VerifyFileSHA256(const std::string& filePath, const std::string& expectedHash);

} // namespace UpdaterCommon

// Include split headers for backward compatibility
#include "network/UpdaterPaths.hpp"
#include "network/UpdaterDownload.hpp"
#include "network/UpdaterProcess.hpp"
#include "network/ShortcutUtils.hpp"

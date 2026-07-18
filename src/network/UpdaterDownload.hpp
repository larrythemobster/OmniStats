#pragma once
#include <string>

namespace UpdaterCommon {

    bool DownloadFile(const std::string& url, const std::string& outputPath, long timeoutSecs = 120);
    std::string DownloadString(const std::string& url, long timeoutSecs = 15);

} // namespace UpdaterCommon

#pragma once
#include <string>
#include <curl/curl.h>

namespace UpdaterCommon {

    bool DownloadFile(const std::string& url, const std::string& outputPath, long timeoutSecs = 120, long* outHttpCode = nullptr, CURLcode* outCurlCode = nullptr);
    std::string DownloadString(const std::string& url, long timeoutSecs = 15, long* outHttpCode = nullptr, CURLcode* outCurlCode = nullptr);

} // namespace UpdaterCommon

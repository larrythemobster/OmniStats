#include "network/UpdaterDownload.hpp"
#include "network/UpdaterPaths.hpp"
#include <curl/curl.h>
#include <fstream>
#include <iostream>
#include <windows.h>

namespace UpdaterCommon {

static size_t WriteStringCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::string* s = static_cast<std::string*>(userp);
    s->append(static_cast<char*>(contents), total);
    return total;
}

static size_t WriteFileCallback(void* contents, size_t size, size_t nmemb, void* userp) {
    size_t total = size * nmemb;
    std::ofstream* out = static_cast<std::ofstream*>(userp);
    out->write(static_cast<char*>(contents), total);
    return total;
}

static std::string TrimInternal(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) {
        return "";
    }
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

bool DownloadFile(const std::string& url, const std::string& outputPath, long timeoutSecs) {
    size_t lastSlash = outputPath.find_last_of("\\/");
    if (lastSlash != std::string::npos) {
        EnsureDirExists(outputPath.substr(0, lastSlash));
    }

    std::ofstream outFile(outputPath, std::ios::binary);
    if (!outFile.is_open()) {
        std::cout << "[UpdaterCommon] Failed to open local file for writing: " << outputPath << "\n";
        return false;
    }

    CURL* curl = curl_easy_init();
    if (!curl) {
        outFile.close();
        return false;
    }

    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteFileCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &outFile);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSecs);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    outFile.close();
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        std::cout << "[UpdaterCommon] Download failed (CURL: " << res << ", HTTP: " << http_code << ") for URL: " << url << "\n";
        DeleteFileA(outputPath.c_str());
        return false;
    }
    return true;
}

std::string DownloadString(const std::string& url, long timeoutSecs) {
    CURL* curl = curl_easy_init();
    if (!curl) {
        return "";
    }

    std::string response;
    curl_easy_setopt(curl, CURLOPT_URL, url.c_str());
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteStringCallback);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, timeoutSecs);
    curl_easy_setopt(curl, CURLOPT_SSL_OPTIONS, CURLSSLOPT_NATIVE_CA);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, 1L);
    curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, 2L);

    CURLcode res = curl_easy_perform(curl);
    long http_code = 0;
    curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &http_code);
    curl_easy_cleanup(curl);

    if (res != CURLE_OK || http_code != 200) {
        return "";
    }
    return TrimInternal(response);
}

} // namespace UpdaterCommon

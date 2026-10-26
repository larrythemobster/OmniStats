#include "network/http/CurlHttpClient.hpp"
#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <string_view>

namespace Http {

    namespace {

        struct HeaderParseState {
            HttpResponse* response = nullptr;
        };

        size_t WriteCallback(void* contents, size_t size, size_t nmemb, void* userp) {
            auto* str = static_cast<std::string*>(userp);
            str->append(static_cast<const char*>(contents), size * nmemb);
            return size * nmemb;
        }

        size_t HeaderCallback(char* buffer, size_t size, size_t nitems, void* userdata) {
            const size_t total = size * nitems;
            if (!userdata || !buffer) return total;
            auto* state = static_cast<HeaderParseState*>(userdata);

            std::string_view line(buffer, total);
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) {
                line.remove_suffix(1);
            }

            const size_t colon = line.find(':');
            if (colon != std::string_view::npos) {
                std::string key(line.substr(0, colon));
                std::string_view val = line.substr(colon + 1);
                while (!val.empty() && (val.front() == ' ' || val.front() == '\t')) {
                    val.remove_prefix(1);
                }
                std::string lowerKey = key;
                std::transform(lowerKey.begin(), lowerKey.end(), lowerKey.begin(),
                               [](unsigned char c) { return static_cast<char>(std::tolower(c)); });

                if (lowerKey == "retry-after") {
                    try {
                        state->response->retryAfterSeconds = std::stol(std::string(val));
                    } catch (...) {
                    }
                } else if (lowerKey == "location") {
                    state->response->location = std::string(val);
                }
                state->response->headers.emplace(std::move(lowerKey), std::string(val));
            }

            return total;
        }

        struct ProgressState {
            const std::function<bool()>* shouldAbort = nullptr;
            bool wasCancelled = false;
        };

        int ProgressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
            auto* state = static_cast<ProgressState*>(clientp);
            if (state && state->shouldAbort && (*state->shouldAbort)()) {
                state->wasCancelled = true;
                return 1; // Non-zero aborts curl transfer
            }
            return 0;
        }

    } // namespace

    bool CurlHttpClient::IsAvailable() const {
        return true;
    }

    HttpResponse CurlHttpClient::Request(const HttpRequest& request) {
        HttpResponse response;
        if (request.shouldAbort && request.shouldAbort()) {
            response.cancelled = true;
            return response;
        }
        CURL* curl = curl_easy_init();
        if (!curl) {
            response.transportError = -1;
            response.errorMessage = "Failed to initialize curl handle";
            return response;
        }

        curl_easy_setopt(curl, CURLOPT_URL, request.url.c_str());
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, WriteCallback);
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body);

        HeaderParseState headerState{&response};
        curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, HeaderCallback);
        curl_easy_setopt(curl, CURLOPT_HEADERDATA, &headerState);

        curl_easy_setopt(curl, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
        curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, request.followLocation ? 1L : 0L);
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "");

        switch (request.method) {
        case Method::Post:
            curl_easy_setopt(curl, CURLOPT_POST, 1L);
            if (!request.body.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
            }
            break;
        case Method::Put:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT");
            if (!request.body.empty()) {
                curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body.c_str());
                curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
            }
            break;
        case Method::Delete:
            curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "DELETE");
            break;
        case Method::Head:
            curl_easy_setopt(curl, CURLOPT_NOBODY, 1L);
            break;
        case Method::Get:
        default:
            curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L);
            break;
        }

        struct curl_slist* headers = nullptr;
        for (const auto& h : request.headers) {
            headers = curl_slist_append(headers, h.c_str());
        }
        if (headers) {
            curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headers);
        }

        ProgressState progState{&request.shouldAbort, false};
        if (request.shouldAbort) {
            curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION, ProgressCallback);
            curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progState);
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L);
        } else {
            curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 1L);
        }

        CURLcode res = curl_easy_perform(curl);
        long httpCode = 0;
        curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &httpCode);

        response.statusCode = static_cast<int>(httpCode);
        response.transportError = static_cast<int>(res);
        response.cancelled = progState.wasCancelled || (res == CURLE_ABORTED_BY_CALLBACK);
        if (res != CURLE_OK) {
            response.errorMessage = curl_easy_strerror(res);
        }

        if (headers) {
            curl_slist_free_all(headers);
        }
        curl_easy_cleanup(curl);

        return response;
    }

} // namespace Http

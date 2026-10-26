#include "network/http/CurlImpersonateHttpClient.hpp"
#include "network/http/CurlImpersonate.hpp"

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
                        const long parsed = std::stol(std::string(val));
                        if (parsed > 0) state->response->retryAfterSeconds = parsed;
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

        int CIProgressCallback(void* clientp, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
            auto* state = static_cast<ProgressState*>(clientp);
            if (state && state->shouldAbort && (*state->shouldAbort)()) {
                state->wasCancelled = true;
                return 1;
            }
            return 0;
        }

    } // namespace

    CurlImpersonateHttpClient::CurlImpersonateHttpClient(std::shared_ptr<CurlImpersonate> backend)
        : m_backend(backend ? std::move(backend) : std::make_shared<CurlImpersonate>()) {}

    bool CurlImpersonateHttpClient::IsAvailable() const {
        return m_backend && (m_backend->IsReady() || m_backend->EnsureAvailable());
    }

    HttpResponse CurlImpersonateHttpClient::Request(const HttpRequest& request) {
        HttpResponse response;
        if (!IsAvailable()) {
            response.transportError = -1;
            response.errorMessage = "curl-impersonate runtime not available";
            return response;
        }

        CURL* handle = m_backend->easy_init();
        if (!handle) {
            response.transportError = -1;
            response.errorMessage = "Failed to initialize curl-impersonate handle";
            return response;
        }

        if (!request.impersonateTarget.empty() && m_backend->easy_impersonate) {
            m_backend->easy_impersonate(handle, request.impersonateTarget.c_str(), 0);
        }

        m_backend->easy_setopt(handle, CURLOPT_URL, request.url.c_str());
        m_backend->easy_setopt(handle, CURLOPT_WRITEFUNCTION, WriteCallback);
        m_backend->easy_setopt(handle, CURLOPT_WRITEDATA, &response.body);

        HeaderParseState headerState{&response};
        m_backend->easy_setopt(handle, CURLOPT_HEADERFUNCTION, HeaderCallback);
        m_backend->easy_setopt(handle, CURLOPT_HEADERDATA, &headerState);

        const long timeoutSec = static_cast<long>(
            (std::max)(1LL, std::chrono::duration_cast<std::chrono::seconds>(request.timeout).count()));
        m_backend->easy_setopt(handle, CURLOPT_TIMEOUT, timeoutSec);
        m_backend->easy_setopt(handle, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
        m_backend->easy_setopt(handle, CURLOPT_FOLLOWLOCATION, request.followLocation ? 1L : 0L);
        m_backend->easy_setopt(handle, CURLOPT_ACCEPT_ENCODING, "");

        switch (request.method) {
        case Method::Post:
            m_backend->easy_setopt(handle, CURLOPT_POST, 1L);
            if (!request.body.empty()) {
                m_backend->easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
                m_backend->easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
            }
            break;
        case Method::Put:
            m_backend->easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "PUT");
            if (!request.body.empty()) {
                m_backend->easy_setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
                m_backend->easy_setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
            }
            break;
        case Method::Delete:
            m_backend->easy_setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
            break;
        case Method::Head:
            m_backend->easy_setopt(handle, CURLOPT_NOBODY, 1L);
            break;
        case Method::Get:
        default:
            m_backend->easy_setopt(handle, CURLOPT_HTTPGET, 1L);
            break;
        }

        struct curl_slist* headers = nullptr;
        for (const auto& h : request.headers) {
            headers = m_backend->slist_append(headers, h.c_str());
        }
        if (headers) {
            m_backend->easy_setopt(handle, CURLOPT_HTTPHEADER, headers);
        }

        ProgressState progState{&request.shouldAbort, false};
        if (request.shouldAbort) {
            m_backend->easy_setopt(handle, CURLOPT_XFERINFOFUNCTION, CIProgressCallback);
            m_backend->easy_setopt(handle, CURLOPT_XFERINFODATA, &progState);
            m_backend->easy_setopt(handle, CURLOPT_NOPROGRESS, 0L);
        } else {
            m_backend->easy_setopt(handle, CURLOPT_NOPROGRESS, 1L);
        }

        CURLcode res = m_backend->easy_perform(handle);
        long httpCode = 0;
        m_backend->easy_getinfo(handle, CURLINFO_RESPONSE_CODE, &httpCode);

        response.statusCode = static_cast<int>(httpCode);
        response.transportError = static_cast<int>(res);
        response.cancelled = progState.wasCancelled || (res == CURLE_ABORTED_BY_CALLBACK);
        if (res != CURLE_OK) {
            response.errorMessage = "Curl perform failed with code " + std::to_string(res);
        }

        if (headers) {
            m_backend->slist_free_all(headers);
        }
        m_backend->easy_cleanup(handle);

        return response;
    }

} // namespace Http

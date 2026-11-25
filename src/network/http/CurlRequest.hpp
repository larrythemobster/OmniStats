#pragma once

#include "network/http/IHttpClient.hpp"
#include "network/http/CurlImpersonate.hpp"

#include <algorithm>
#include <cctype>
#include <string_view>

namespace Http::Detail {
    struct CurlOperations {
        pfn_curl_easy_init init;
        pfn_curl_easy_cleanup cleanup;
        pfn_curl_easy_setopt setopt;
        pfn_curl_easy_perform perform;
        pfn_curl_easy_getinfo getinfo;
        pfn_curl_slist_append append;
        pfn_curl_slist_free_all freeHeaders;
        pfn_curl_easy_impersonate impersonate = nullptr;
        bool dynamic = false;
        decltype(&curl_mime_init) mimeInit = nullptr;
        decltype(&curl_mime_addpart) mimeAddPart = nullptr;
        decltype(&curl_mime_name) mimeName = nullptr;
        decltype(&curl_mime_filedata) mimeFileData = nullptr;
        decltype(&curl_mime_free) mimeFree = nullptr;
        decltype(&curl_easy_strerror) strerror = nullptr;
    };

    struct ResponseState {
        HttpResponse* response;
        size_t maxBytes;
        bool positiveRetryOnly;
    };

    inline size_t WriteResponse(void* data, size_t size, size_t count, void* context) {
        auto& state = *static_cast<ResponseState*>(context);
        const size_t total = size * count;
        if (total > state.maxBytes - (std::min)(state.maxBytes, state.response->body.size())) return 0;
        state.response->body.append(static_cast<const char*>(data), total);
        return total;
    }

    inline size_t ReadHeader(char* data, size_t size, size_t count, void* context) {
        const size_t total = size * count;
        if (!context || !data) return total;
        auto& state = *static_cast<ResponseState*>(context);
        std::string_view line(data, total);
        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
            line.remove_suffix(1);
        const size_t colon = line.find(':');
        if (colon == std::string_view::npos) return total;
        std::string key(line.substr(0, colon));
        std::transform(key.begin(), key.end(), key.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        std::string_view value = line.substr(colon + 1);
        while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
            value.remove_prefix(1);
        if (key == "retry-after") {
            try {
                const long parsed = std::stol(std::string(value));
                if (!state.positiveRetryOnly || parsed > 0) state.response->retryAfterSeconds = parsed;
            } catch (...) {
            }
        } else if (key == "location") {
            state.response->location = std::string(value);
        }
        state.response->headers.emplace(std::move(key), std::string(value));
        return total;
    }

    struct ProgressState {
        const std::function<bool()>* shouldAbort;
        bool cancelled = false;
    };

    inline int TransferProgress(void* context, curl_off_t, curl_off_t, curl_off_t, curl_off_t) {
        auto& state = *static_cast<ProgressState*>(context);
        if ((*state.shouldAbort)()) {
            state.cancelled = true;
            return 1;
        }
        return 0;
    }
    inline HttpResponse ExecuteCurl(const HttpRequest& request, const CurlOperations& ops) {
        HttpResponse response;
        const bool multipart = request.multipartFile;
        if (multipart && !ops.mimeInit) {
            response.transportError = -1;
            response.errorMessage = "Multipart uploads are unavailable";
            return response;
        }
        if (!ops.dynamic && !multipart && request.shouldAbort && request.shouldAbort()) {
            response.cancelled = true;
            return response;
        }
        CURL* handle = ops.init();
        if (!handle) {
            response.transportError = -1;
            response.errorMessage = ops.dynamic ? "Failed to initialize curl-impersonate handle" : "Failed to initialize curl handle";
            return response;
        }
        if (!request.impersonateTarget.empty() && ops.impersonate)
            ops.impersonate(handle, request.impersonateTarget.c_str(), 0);

        ResponseState state{&response, request.maxResponseBytes, ops.dynamic};
        ops.setopt(handle, CURLOPT_URL, request.url.c_str());
        ops.setopt(handle, CURLOPT_WRITEFUNCTION, WriteResponse);
        ops.setopt(handle, CURLOPT_WRITEDATA, &state);
        ops.setopt(handle, CURLOPT_HEADERFUNCTION, ReadHeader);
        ops.setopt(handle, CURLOPT_HEADERDATA, &state);
        if (ops.dynamic) {
            const long seconds = static_cast<long>((std::max)(1LL, std::chrono::duration_cast<std::chrono::seconds>(request.timeout).count()));
            ops.setopt(handle, CURLOPT_TIMEOUT, seconds);
        } else {
            ops.setopt(handle, CURLOPT_TIMEOUT_MS, static_cast<long>(request.timeout.count()));
        }
        if (ops.dynamic || request.nativeCa) ops.setopt(handle, CURLOPT_SSL_OPTIONS, static_cast<long>(CURLSSLOPT_NATIVE_CA));
        if (request.nativeCa) {
            ops.setopt(handle, CURLOPT_SSL_VERIFYPEER, 1L);
            ops.setopt(handle, CURLOPT_SSL_VERIFYHOST, 2L);
        }
        if (request.lowSpeedLimit) ops.setopt(handle, CURLOPT_LOW_SPEED_LIMIT, request.lowSpeedLimit);
        if (request.lowSpeedTime) ops.setopt(handle, CURLOPT_LOW_SPEED_TIME, request.lowSpeedTime);
        ops.setopt(handle, CURLOPT_FOLLOWLOCATION, request.followLocation ? 1L : 0L);
        if (!multipart) ops.setopt(handle, CURLOPT_MAXREDIRS, kMaxRedirects);
        ops.setopt(handle, CURLOPT_PROTOCOLS_STR, request.httpsOnly ? "https" : "http,https");
        ops.setopt(handle, CURLOPT_REDIR_PROTOCOLS_STR, "https");
        if (!multipart) ops.setopt(handle, CURLOPT_ACCEPT_ENCODING, "");

        switch (request.method) {
        case Method::Post:
            ops.setopt(handle, CURLOPT_POST, 1L);
            if (!request.body.empty()) {
                ops.setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
                ops.setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
            }
            break;
        case Method::Put:
            ops.setopt(handle, CURLOPT_CUSTOMREQUEST, "PUT");
            if (!request.body.empty()) {
                ops.setopt(handle, CURLOPT_POSTFIELDS, request.body.c_str());
                ops.setopt(handle, CURLOPT_POSTFIELDSIZE, static_cast<long>(request.body.size()));
            }
            break;
        case Method::Delete:
            ops.setopt(handle, CURLOPT_CUSTOMREQUEST, "DELETE");
            break;
        case Method::Head:
            ops.setopt(handle, CURLOPT_NOBODY, 1L);
            break;
        case Method::Get:
        default:
            if (!multipart) ops.setopt(handle, CURLOPT_HTTPGET, 1L);
            break;
        }
        curl_slist* headers = nullptr;
        for (const auto& header : request.headers)
            headers = ops.append(headers, header.c_str());
        curl_mime* mime = nullptr;
        if (multipart && ops.mimeInit) {
            mime = ops.mimeInit(handle);
            curl_mimepart* part = mime ? ops.mimeAddPart(mime) : nullptr;
            if (!part || !headers) {
                if (mime) ops.mimeFree(mime);
                if (headers) ops.freeHeaders(headers);
                ops.cleanup(handle);
                response.transportError = -1;
                return response;
            }
            ops.mimeName(part, "file");
            ops.mimeFileData(part, request.multipartFilePath.c_str());
            ops.setopt(handle, CURLOPT_MIMEPOST, mime);
        }
        if (headers) ops.setopt(handle, CURLOPT_HTTPHEADER, headers);
        ProgressState progress{&request.shouldAbort};
        if (request.shouldAbort) {
            ops.setopt(handle, CURLOPT_XFERINFOFUNCTION, TransferProgress);
            ops.setopt(handle, CURLOPT_XFERINFODATA, &progress);
            ops.setopt(handle, CURLOPT_NOPROGRESS, 0L);
        } else {
            ops.setopt(handle, CURLOPT_NOPROGRESS, 1L);
        }
        const CURLcode result = ops.perform(handle);
        long status = 0;
        ops.getinfo(handle, CURLINFO_RESPONSE_CODE, &status);
        response.statusCode = static_cast<int>(status);
        response.transportError = static_cast<int>(result);
        response.cancelled = progress.cancelled || result == CURLE_ABORTED_BY_CALLBACK;
        if (result != CURLE_OK)
            response.errorMessage = ops.dynamic ? "Curl perform failed with code " + std::to_string(result) : ops.strerror(result);
        if (headers) ops.freeHeaders(headers);
        if (mime) ops.mimeFree(mime);
        ops.cleanup(handle);
        return response;
    }
}

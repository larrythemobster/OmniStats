#pragma once

#include <chrono>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace Http {

    enum class Method {
        Get,
        Post,
        Put,
        Delete,
        Head
    };

    struct HttpRequest {
        std::string url;
        Method method = Method::Get;
        std::vector<std::string> headers;
        std::string body;
        std::chrono::milliseconds timeout{10000};
        bool followLocation = true;
        std::string impersonateTarget;
        std::function<bool()> shouldAbort;
    };

    struct HttpResponse {
        int statusCode = 0;
        std::string body;
        std::map<std::string, std::string> headers;
        long retryAfterSeconds = 0;
        std::string location;
        int transportError = 0;
        std::string errorMessage;
        bool cancelled = false;
    };

    class IHttpClient {
      public:
        virtual ~IHttpClient() = default;
        virtual bool IsAvailable() const = 0;
        virtual HttpResponse Request(const HttpRequest& request) = 0;
    };

} // namespace Http

#pragma once

#include <string>
#include <string_view>

namespace HttpSecurity {

    struct ParsedUrl {
        std::string scheme;
        std::string host;
        int port = 0;
        std::string path;
        std::string query;
        bool valid = false;

        [[nodiscard]] int EffectivePort() const {
            if (port > 0) return port;
            if (scheme == "https") return 443;
            if (scheme == "http") return 80;
            return 0;
        }
    };

    ParsedUrl ParseUrl(std::string_view url);

    bool IsLoopbackHost(std::string_view host);

    bool IsAllowedSensitiveUrl(const ParsedUrl& url, std::string* outError = nullptr);

    bool IsSameOrigin(const ParsedUrl& a, const ParsedUrl& b);

    std::string ResolveRedirectUrl(std::string_view currentUrl, std::string_view location);

    struct RedirectValidationResult {
        bool allowed = false;
        std::string resolvedUrl;
        std::string reason;
    };

    RedirectValidationResult ValidateRedirect(const std::string& currentUrl,
                                              const std::string& locationHeader,
                                              bool isSensitiveRequest);

} // namespace HttpSecurity

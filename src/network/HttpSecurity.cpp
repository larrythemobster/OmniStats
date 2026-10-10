#include "network/HttpSecurity.hpp"

#include <algorithm>
#include <cctype>

namespace HttpSecurity {

    static std::string ToLower(std::string_view s) {
        std::string out;
        out.reserve(s.size());
        for (char c : s) {
            out.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
        }
        return out;
    }

    static std::string_view Trim(std::string_view s) {
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.front()))) {
            s.remove_prefix(1);
        }
        while (!s.empty() && std::isspace(static_cast<unsigned char>(s.back()))) {
            s.remove_suffix(1);
        }
        return s;
    }

    ParsedUrl ParseUrl(std::string_view rawUrl) {
        ParsedUrl result;
        const std::string_view trimmed = Trim(rawUrl);
        if (trimmed.empty()) return result;

        const size_t schemeDelim = trimmed.find("://");
        if (schemeDelim == std::string_view::npos || schemeDelim == 0) {
            return result;
        }

        result.scheme = ToLower(trimmed.substr(0, schemeDelim));
        const std::string_view rest = trimmed.substr(schemeDelim + 3);

        const size_t pathStart = rest.find_first_of("/?#");
        std::string_view authority = (pathStart == std::string_view::npos) ? rest : rest.substr(0, pathStart);
        std::string_view pathAndQuery = (pathStart == std::string_view::npos) ? std::string_view{} : rest.substr(pathStart);

        const size_t userinfoDelim = authority.find('@');
        if (userinfoDelim != std::string_view::npos) {
            authority = authority.substr(userinfoDelim + 1);
        }
        if (authority.empty()) return result;

        if (authority.front() == '[') {
            const size_t bracketEnd = authority.find(']');
            if (bracketEnd == std::string_view::npos) return result;
            result.host = ToLower(authority.substr(0, bracketEnd + 1));
            std::string_view afterBracket = authority.substr(bracketEnd + 1);
            if (!afterBracket.empty() && afterBracket.front() == ':') {
                try {
                    result.port = std::stoi(std::string(afterBracket.substr(1)));
                } catch (...) {
                    return result;
                }
            }
        } else {
            const size_t portDelim = authority.find(':');
            if (portDelim != std::string_view::npos) {
                result.host = ToLower(authority.substr(0, portDelim));
                try {
                    result.port = std::stoi(std::string(authority.substr(portDelim + 1)));
                } catch (...) {
                    return result;
                }
            } else {
                result.host = ToLower(authority);
            }
        }

        if (result.host.empty()) return result;

        if (pathAndQuery.empty()) {
            result.path = "/";
        } else {
            const size_t queryDelim = pathAndQuery.find('?');
            const size_t fragmentDelim = pathAndQuery.find('#');
            const size_t endOfPath = std::min(queryDelim, fragmentDelim);
            if (endOfPath == std::string_view::npos) {
                result.path = std::string(pathAndQuery);
            } else {
                result.path = std::string(pathAndQuery.substr(0, endOfPath));
                if (queryDelim != std::string_view::npos) {
                    const size_t endOfQuery = (fragmentDelim != std::string_view::npos && fragmentDelim > queryDelim)
                                                  ? fragmentDelim - queryDelim - 1
                                                  : std::string_view::npos;
                    result.query = std::string(pathAndQuery.substr(queryDelim + 1, endOfQuery));
                }
            }
        }

        if (result.path.empty() || result.path.front() != '/') {
            result.path = "/" + result.path;
        }

        result.valid = true;
        return result;
    }

    bool IsLoopbackHost(std::string_view host) {
        const std::string lower = ToLower(Trim(host));
        if (lower == "localhost" || lower == "127.0.0.1" || lower == "::1" || lower == "[::1]") {
            return true;
        }
        if (lower.rfind("127.", 0) == 0) {
            return true;
        }
        return false;
    }

    bool IsAllowedSensitiveUrl(const ParsedUrl& url, std::string* outError) {
        if (!url.valid) {
            if (outError) *outError = "Malformed or invalid URL";
            return false;
        }
        if (url.scheme == "https") {
            return true;
        }
        if (url.scheme == "http") {
            if (IsLoopbackHost(url.host)) {
                return true;
            }
            if (outError) *outError = "Plain HTTP is rejected for authenticated requests; HTTPS is required for remote endpoints";
            return false;
        }
        if (outError) *outError = "Unsupported URL scheme: must be HTTPS";
        return false;
    }

    bool IsSameOrigin(const ParsedUrl& a, const ParsedUrl& b) {
        if (!a.valid || !b.valid) return false;
        return a.scheme == b.scheme && a.host == b.host && a.EffectivePort() == b.EffectivePort();
    }

    std::string ResolveRedirectUrl(std::string_view currentUrl, std::string_view location) {
        const std::string_view loc = Trim(location);
        if (loc.empty()) return std::string(currentUrl);

        if (loc.find("://") != std::string_view::npos) {
            return std::string(loc);
        }

        const ParsedUrl base = ParseUrl(currentUrl);
        if (!base.valid) return std::string(loc);

        std::string hostPort = base.host;
        if (base.port > 0 && base.port != base.EffectivePort()) {
            hostPort += ":" + std::to_string(base.port);
        } else if (base.port > 0) {
            if ((base.scheme == "https" && base.port != 443) || (base.scheme == "http" && base.port != 80)) {
                hostPort += ":" + std::to_string(base.port);
            }
        }

        if (loc.rfind("//", 0) == 0) {
            return base.scheme + ":" + std::string(loc);
        }

        if (loc.front() == '/') {
            return base.scheme + "://" + hostPort + std::string(loc);
        }

        std::string basePath = base.path;
        const size_t lastSlash = basePath.rfind('/');
        if (lastSlash != std::string::npos) {
            basePath = basePath.substr(0, lastSlash + 1);
        } else {
            basePath = "/";
        }
        return base.scheme + "://" + hostPort + basePath + std::string(loc);
    }

    RedirectValidationResult ValidateRedirect(const std::string& currentUrl,
                                              const std::string& locationHeader,
                                              bool isSensitiveRequest) {
        RedirectValidationResult result;
        result.resolvedUrl = ResolveRedirectUrl(currentUrl, locationHeader);

        const ParsedUrl current = ParseUrl(currentUrl);
        const ParsedUrl resolved = ParseUrl(result.resolvedUrl);

        if (!resolved.valid) {
            result.allowed = false;
            result.reason = "Malformed redirect Location destination";
            return result;
        }

        if (current.valid && current.scheme == "https" && resolved.scheme == "http") {
            result.allowed = false;
            result.reason = "HTTPS-to-HTTP downgrade is prohibited";
            return result;
        }

        if (isSensitiveRequest) {
            if (!IsSameOrigin(current, resolved)) {
                result.allowed = false;
                result.reason = "Cross-origin redirect rejected for authenticated request";
                return result;
            }
            std::string err;
            if (!IsAllowedSensitiveUrl(resolved, &err)) {
                result.allowed = false;
                result.reason = err;
                return result;
            }
        }

        result.allowed = true;
        return result;
    }

} // namespace HttpSecurity

#pragma once

#include "network/api/DeviceIdentity.hpp"
#include <nlohmann/json.hpp>
#include <cctype>
#include <string>
#include <string_view>

namespace AuthJson {
    inline nlohmann::json Proof(const DeviceIdentity::DeviceProof& proof) {
        return {{"timestamp", proof.timestamp}, {"nonce", proof.nonce}, {"signature", proof.signature}};
    }

    inline std::string DisplayName(const nlohmann::json& json, const std::string& fallback, bool trim) {
        if (json.is_object() && json.contains("account") && json["account"].is_object()) {
            const auto& account = json["account"];
            for (const char* field : {"display_name", "email"}) {
                if (!account.contains(field) || !account[field].is_string()) continue;
                std::string value = account[field].get<std::string>();
                if (trim) {
                    std::string_view view(value);
                    while (!view.empty() && std::isspace(static_cast<unsigned char>(view.front())))
                        view.remove_prefix(1);
                    while (!view.empty() && std::isspace(static_cast<unsigned char>(view.back())))
                        view.remove_suffix(1);
                    value = std::string(view);
                }
                if (!value.empty()) return value;
            }
        }
        return fallback.empty() ? "OmniStats User" : fallback;
    }

    struct Tokens {
        std::string access;
        std::string refresh;
        int expiresIn;
    };
    inline std::string ErrorCode(const std::string& body) {
        try {
            const auto json = nlohmann::json::parse(body);
            if (json.is_object() && json.contains("error") && json["error"].is_object())
                return json["error"].value("code", "");
        } catch (...) {
        }
        return {};
    }

    inline Tokens ParseTokens(const nlohmann::json& json, int defaultExpiry) {
        return {json.value("access_token", ""), json.value("refresh_token", ""), json.value("expires_in", defaultExpiry)};
    }
}

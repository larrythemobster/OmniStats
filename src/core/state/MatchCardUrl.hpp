#pragma once

#include <nlohmann/json.hpp>
#include <optional>
#include <string>
#include <string_view>

#include "core/state/SessionState.hpp"

namespace OmniStats::MatchCard {

    std::string Base64UrlEncode(std::string_view input);
    std::string Base64UrlDecode(std::string_view input);

    nlohmann::json BuildMatchCardJson(const MatchDetail& detail);
    std::string BuildMatchCardUrl(const MatchDetail& detail);
    std::string BuildMatchCardUrl(const nlohmann::json& cardJson);

    std::optional<nlohmann::json> ParseMatchCardUrl(std::string_view urlOrFragment);

} // namespace OmniStats::MatchCard

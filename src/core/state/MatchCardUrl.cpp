#include "core/state/MatchCardUrl.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>

#include "core/game/MatchAnalytics.hpp"

namespace OmniStats::MatchCard {

    namespace {

        constexpr char kBase64UrlTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

    } // namespace

    std::string Base64UrlEncode(std::string_view in) {
        std::string out;
        const size_t len = in.size();
        out.reserve(((len + 2) / 3) * 4);

        size_t i = 0;
        while (i + 3 <= len) {
            uint32_t n = (static_cast<uint8_t>(in[i]) << 16) |
                         (static_cast<uint8_t>(in[i + 1]) << 8) |
                         static_cast<uint8_t>(in[i + 2]);
            out.push_back(kBase64UrlTable[(n >> 18) & 0x3F]);
            out.push_back(kBase64UrlTable[(n >> 12) & 0x3F]);
            out.push_back(kBase64UrlTable[(n >> 6) & 0x3F]);
            out.push_back(kBase64UrlTable[n & 0x3F]);
            i += 3;
        }

        if (len - i == 1) {
            uint32_t n = static_cast<uint8_t>(in[i]) << 16;
            out.push_back(kBase64UrlTable[(n >> 18) & 0x3F]);
            out.push_back(kBase64UrlTable[(n >> 12) & 0x3F]);
        } else if (len - i == 2) {
            uint32_t n = (static_cast<uint8_t>(in[i]) << 16) |
                         (static_cast<uint8_t>(in[i + 1]) << 8);
            out.push_back(kBase64UrlTable[(n >> 18) & 0x3F]);
            out.push_back(kBase64UrlTable[(n >> 12) & 0x3F]);
            out.push_back(kBase64UrlTable[(n >> 6) & 0x3F]);
        }

        return out;
    }

    std::string Base64UrlDecode(std::string_view in) {
        auto decodeChar = [](char c) -> int {
            if (c >= 'A' && c <= 'Z') return c - 'A';
            if (c >= 'a' && c <= 'z') return c - 'a' + 26;
            if (c >= '0' && c <= '9') return c - '0' + 52;
            if (c == '-' || c == '+') return 62;
            if (c == '_' || c == '/') return 63;
            return -1;
        };

        std::string out;
        out.reserve((in.size() * 3) / 4);

        uint32_t buf = 0;
        int bits = 0;
        for (char c : in) {
            if (c == '=') break;
            int val = decodeChar(c);
            if (val < 0) continue;
            buf = (buf << 6) | static_cast<uint32_t>(val);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                out.push_back(static_cast<char>((buf >> bits) & 0xFF));
            }
        }
        return out;
    }

    nlohmann::json BuildMatchCardJson(const MatchDetail& detail) {
        nlohmann::json j = nlohmann::json::object();
        j["v"] = 1;

        if (detail.ourScore > detail.theirScore) {
            j["res"] = "W";
        } else if (detail.ourScore < detail.theirScore) {
            j["res"] = "L";
        } else {
            j["res"] = detail.win ? "W" : "D";
        }

        j["sc"] = nlohmann::json::array({detail.ourScore, detail.theirScore});

        if (!detail.playlist.empty()) {
            j["pl"] = detail.playlist;
        }

        if (detail.durationSeconds.has_value() && *detail.durationSeconds > 0.0f) {
            j["dur"] = static_cast<int>(std::lround(*detail.durationSeconds));
        } else if (detail.analytics.has_value() && detail.analytics->playSeconds > 0.0f) {
            j["dur"] = static_cast<int>(std::lround(detail.analytics->playSeconds));
        }

        const bool isOt = (detail.overtimeSeconds.has_value() && *detail.overtimeSeconds > 0.0f) ||
                          (detail.analytics.has_value() && detail.analytics->endedInOvertime);
        if (isOt) {
            j["ot"] = true;
        }

        if (detail.timestampUnix > 0) {
            j["ts"] = detail.timestampUnix;
        }

        const MatchDetailPlayer* mePlayer = nullptr;
        for (const auto& p : detail.ourTeam) {
            if (p.isMe) {
                mePlayer = &p;
                break;
            }
        }
        if (!mePlayer && !detail.accountPrimaryId.empty()) {
            for (const auto& p : detail.ourTeam) {
                if (p.primaryId == detail.accountPrimaryId) {
                    mePlayer = &p;
                    break;
                }
            }
        }
        if (!mePlayer && !detail.ourTeam.empty()) {
            mePlayer = &detail.ourTeam.front();
        }

        if (mePlayer && !mePlayer->name.empty()) {
            j["name"] = mePlayer->name;
        }

        if (mePlayer && mePlayer->hasStats) {
            nlohmann::json st = nlohmann::json::object();
            if (mePlayer->goals.has_value()) st["g"] = *mePlayer->goals;
            if (mePlayer->assists.has_value()) st["a"] = *mePlayer->assists;
            if (mePlayer->saves.has_value()) st["sv"] = *mePlayer->saves;
            if (mePlayer->shots.has_value()) st["sh"] = *mePlayer->shots;
            if (mePlayer->score.has_value()) st["sc"] = *mePlayer->score;
            if (mePlayer->demos.has_value()) st["dm"] = *mePlayer->demos;
            if (!st.empty()) {
                j["st"] = std::move(st);
            }
        }

        nlohmann::json mmr = nlohmann::json::object();
        if (detail.mmrBefore.has_value()) mmr["b"] = *detail.mmrBefore;
        if (detail.mmrAfter.has_value()) mmr["a"] = *detail.mmrAfter;
        if (mePlayer && !mePlayer->tier.empty() && mePlayer->tier != "Unranked") {
            mmr["rank"] = mePlayer->tier;
        }
        if (!mmr.empty()) {
            j["mmr"] = std::move(mmr);
        }

        if (detail.analytics.has_value()) {
            const PlayerAnalytics* meAnalytics = nullptr;
            if (mePlayer && !mePlayer->primaryId.empty()) {
                meAnalytics = detail.analytics->FindPlayer(mePlayer->primaryId);
            }
            if (!meAnalytics && !detail.accountPrimaryId.empty()) {
                meAnalytics = detail.analytics->FindPlayer(detail.accountPrimaryId);
            }
            if (meAnalytics && meAnalytics->car.has_value() && meAnalytics->car->sampledSeconds > 0.0f) {
                const auto& c = *meAnalytics->car;
                j["bpm"] = static_cast<int>(std::lround(c.BoostPerMinute()));
                j["ss"] = std::clamp(static_cast<int>(std::lround(c.Share(c.supersonicSeconds) * 100.0f)), 0, 100);
                j["air"] = std::clamp(static_cast<int>(std::lround(c.Share(c.airSeconds) * 100.0f)), 0, 100);
            }
        }

        return j;
    }

    std::string BuildMatchCardUrl(const nlohmann::json& cardJson) {
        const std::string payload = cardJson.dump();
        return "https://omnistats.org/card#1." + Base64UrlEncode(payload);
    }

    std::string BuildMatchCardUrl(const MatchDetail& detail) {
        return BuildMatchCardUrl(BuildMatchCardJson(detail));
    }

    std::optional<nlohmann::json> ParseMatchCardUrl(std::string_view urlOrFragment) {
        size_t hashPos = urlOrFragment.find('#');
        std::string_view fragment = (hashPos != std::string_view::npos)
                                        ? urlOrFragment.substr(hashPos + 1)
                                        : urlOrFragment;

        if (!fragment.starts_with("1.")) {
            return std::nullopt;
        }

        std::string_view b64 = fragment.substr(2);
        if (b64.empty()) {
            return std::nullopt;
        }

        std::string decoded = Base64UrlDecode(b64);
        if (decoded.empty()) {
            return std::nullopt;
        }

        nlohmann::json j = nlohmann::json::parse(decoded, nullptr, false);
        if (j.is_discarded() || !j.is_object()) {
            return std::nullopt;
        }

        if (!j.contains("v") || j["v"] != 1) return std::nullopt;
        if (!j.contains("res") || !j["res"].is_string()) return std::nullopt;
        const std::string res = j["res"];
        if (res != "W" && res != "L" && res != "D") return std::nullopt;
        if (!j.contains("sc") || !j["sc"].is_array() || j["sc"].size() != 2) return std::nullopt;

        return j;
    }

} // namespace OmniStats::MatchCard

#include "Formatting.hpp"
#include <cstdio>
#include <algorithm>
#include <cmath>

namespace Format {

    std::string SimpleUrlEncode(const std::string& str) {
        std::string encoded;
        for (char c : str) {
            if (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.' || c == '~') {
                encoded += c;
            } else if (c == ' ') {
                encoded += "%20";
            } else {
                char buf[4];
                snprintf(buf, sizeof(buf), "%%%02X", (unsigned char)c);
                encoded += buf;
            }
        }
        return encoded;
    }

    std::string PairCount(int scope_v, int self_v, bool always_pair) {
        if (scope_v <= 0) return "-";
        char buf[64];
        if (!always_pair && self_v >= scope_v) {
            snprintf(buf, sizeof(buf), "%d", scope_v);
        } else if (self_v > 0) {
            snprintf(buf, sizeof(buf), "%d | %d", scope_v, self_v);
        } else {
            snprintf(buf, sizeof(buf), "%d | -", scope_v);
        }
        return buf;
    }

    std::string PairSpeed(float scope_v, float self_v, bool always_pair, const std::string& suffix, bool useImperialUnits) {
        if (scope_v <= 0.0f) return "-";

        float display_scope = scope_v;
        float display_self = self_v;
        std::string display_suffix = suffix;

        bool is_speed = (suffix.find("kph") != std::string::npos || suffix.find("KPH") != std::string::npos);
        if (useImperialUnits && is_speed) {
            display_scope = scope_v * 0.621371f;
            if (self_v > 0.0f) display_self = self_v * 0.621371f;

            if (display_suffix == " kph")
                display_suffix = " mph";
            else if (display_suffix == " KPH")
                display_suffix = " MPH";
            else if (display_suffix == "kph")
                display_suffix = "mph";
            else if (display_suffix == "KPH")
                display_suffix = "MPH";
        }

        char buf[64];
        const char* suf = display_suffix.c_str();
        if (!always_pair && display_self >= display_scope) {
            snprintf(buf, sizeof(buf), "%d%s", (int)std::round(display_scope), suf);
        } else if (display_self > 0.0f) {
            snprintf(buf, sizeof(buf), "%d%s | %d%s", (int)std::round(display_scope), suf, (int)std::round(display_self), suf);
        } else {
            snprintf(buf, sizeof(buf), "%d%s | -", (int)std::round(display_scope), suf);
        }
        return buf;
    }

    std::string PairFastest(float scope_v, float self_v, bool always_pair) {
        if (scope_v <= 0.0f) return "-";
        char buf[64];
        if (!always_pair && self_v > 0.0f && self_v <= scope_v) {
            snprintf(buf, sizeof(buf), "%.1fs", scope_v);
        } else if (self_v > 0.0f) {
            snprintf(buf, sizeof(buf), "%.1fs | %.1fs", scope_v, self_v);
        } else {
            snprintf(buf, sizeof(buf), "%.1fs | -", scope_v);
        }
        return buf;
    }

    ImVec4 RankColor(const std::string& tier) {
        auto starts = [&](const std::string& prefix) -> bool {
            return tier.rfind(prefix, 0) == 0;
        };

        if (starts("Supersonic")) return ImVec4(1.00f, 0.35f, 0.78f, 1.0f);     // Bright pink/magenta
        if (starts("Grand Champion")) return ImVec4(0.80f, 0.21f, 0.13f, 1.0f); // GC red
        if (starts("Champion")) return ImVec4(0.67f, 0.31f, 0.86f, 1.0f);       // Champ purple
        if (starts("Diamond")) return ImVec4(0.16f, 0.51f, 0.86f, 1.0f);        // Diamond blue
        if (starts("Platinum")) return ImVec4(0.20f, 0.71f, 0.71f, 1.0f);       // Plat cyan/teal
        if (starts("Gold")) return ImVec4(0.95f, 0.77f, 0.06f, 1.0f);           // Gold yellow
        if (starts("Silver")) return ImVec4(0.74f, 0.76f, 0.78f, 1.0f);         // Silver gray
        if (starts("Bronze")) return ImVec4(0.89f, 0.51f, 0.07f, 1.0f);         // Bronze orange/brown
        return ImVec4(0.43f, 0.45f, 0.50f, 1.0f);                               // Muted gray for unranked
    }

    std::string RankTier(const std::string& tier, bool useRomanNumerals) {
        if (useRomanNumerals) {
            return tier;
        }
        std::string res = tier;
        size_t pos;
        if ((pos = res.find("Div IV")) != std::string::npos)
            res.replace(pos, 6, "Div 4");
        else if ((pos = res.find("Div III")) != std::string::npos)
            res.replace(pos, 7, "Div 3");
        else if ((pos = res.find("Div II")) != std::string::npos)
            res.replace(pos, 6, "Div 2");
        else if ((pos = res.find("Div I")) != std::string::npos)
            res.replace(pos, 5, "Div 1");

        auto replaceNumeral = [&](const std::string& roman, const std::string& arabic) {
            size_t p = res.find(roman + " Div");
            if (p != std::string::npos) {
                res.replace(p, roman.length(), arabic);
            } else if (res.length() >= roman.length() && res.substr(res.length() - roman.length()) == roman) {
                res.replace(res.length() - roman.length(), roman.length(), arabic);
            }
        };

        replaceNumeral(" III", " 3");
        replaceNumeral(" II", " 2");
        replaceNumeral(" I", " 1");

        return res;
    }

    std::string AbbreviateRank(const std::string& tier) {
        if (tier.empty() || tier == "Unranked" || tier == "-") {
            return "-";
        }
        if (tier.rfind("Supersonic Legend", 0) == 0) {
            return "SSL";
        }

        std::string prefix = "";
        size_t prefixLen = 0;
        if (tier.rfind("Grand Champion", 0) == 0) {
            prefix = "GC";
            prefixLen = 14;
        } else if (tier.rfind("Champion", 0) == 0) {
            prefix = "C";
            prefixLen = 8;
        } else if (tier.rfind("Platinum", 0) == 0) {
            prefix = "P";
            prefixLen = 8;
        } else if (tier.rfind("Diamond", 0) == 0) {
            prefix = "D";
            prefixLen = 7;
        } else if (tier.rfind("Gold", 0) == 0) {
            prefix = "G";
            prefixLen = 4;
        } else if (tier.rfind("Silver", 0) == 0) {
            prefix = "S";
            prefixLen = 6;
        } else if (tier.rfind("Bronze", 0) == 0) {
            prefix = "B";
            prefixLen = 6;
        } else {
            return tier; // Fallback
        }

        std::string tierNum = "";
        std::string rest = tier.substr(prefixLen);

        std::string tierPart = rest;
        size_t divPos = tierPart.find("Div");
        if (divPos != std::string::npos) {
            tierPart = tierPart.substr(0, divPos);
        }

        if (tierPart.find(" III") != std::string::npos || tierPart.find(" 3") != std::string::npos) {
            tierNum = "3";
        } else if (tierPart.find(" II") != std::string::npos || tierPart.find(" 2") != std::string::npos) {
            tierNum = "2";
        } else if (tierPart.find(" I") != std::string::npos || tierPart.find(" 1") != std::string::npos) {
            tierNum = "1";
        }

        std::string divSuffix = "";
        if (tier.find("Div IV") != std::string::npos || tier.find("Div 4") != std::string::npos) {
            divSuffix = ".D4";
        } else if (tier.find("Div III") != std::string::npos || tier.find("Div 3") != std::string::npos) {
            divSuffix = ".D3";
        } else if (tier.find("Div II") != std::string::npos || tier.find("Div 2") != std::string::npos) {
            divSuffix = ".D2";
        } else if (tier.find("Div I") != std::string::npos || tier.find("Div 1") != std::string::npos) {
            divSuffix = ".D1";
        }

        return prefix + tierNum + divSuffix;
    }

    std::string FriendlyVoidReason(const std::string& reason) {
        if (reason == "round_never_started") return "round never started";
        if (reason == "local_player_spectator_bug") return "spectator bug";
        if (reason == "local_player_never_active") return "local player never active";
        if (reason == "local_player_team_unknown") return "unknown team";
        if (reason == "invalid_winner" || reason == "invalid_winner_team") return "invalid winner";
        if (reason == "lobby_never_full") return "lobby was never full";
        return reason;
    }

} // namespace Format

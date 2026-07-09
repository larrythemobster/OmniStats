#pragma once
#include <string>
#include <imgui.h>
#include "core/Config.hpp"

namespace Format {

std::string SimpleUrlEncode(const std::string& str);

std::string PairCount(int scope_v, int self_v, bool always_pair = true);

std::string PairSpeed(float scope_v, float self_v, bool always_pair, const std::string& suffix, bool useImperialUnits);

std::string PairFastest(float scope_v, float self_v, bool always_pair = true);

ImVec4 RankColor(const std::string& tier);

std::string RankTier(const std::string& tier, bool useRomanNumerals);

std::string AbbreviateRank(const std::string& tier);

std::string FriendlyVoidReason(const std::string& reason);

inline ImVec4 FromColor(const ColorRGBA& c) {
    return ImVec4(c.r, c.g, c.b, c.a);
}

// Short alias matching the old Overlay::C() convention
inline ImVec4 C(const ColorRGBA& c) { return FromColor(c); }

} // namespace Format

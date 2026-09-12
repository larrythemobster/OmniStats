#pragma once
#include <string>
#include "core/Config.hpp"

namespace Format {

    std::string SimpleUrlEncode(const std::string& str);

    std::string PairCount(int scope_v, int self_v, bool always_pair = true);

    std::string PairSpeed(float scope_v, float self_v, bool always_pair, const std::string& suffix, bool useImperialUnits);

    std::string PairFastest(float scope_v, float self_v, bool always_pair = true);

    ColorRGBA RankColor(const std::string& tier);

    std::string RankTier(const std::string& tier, bool useRomanNumerals);

    std::string AbbreviateRank(const std::string& tier);

    std::string FriendlyVoidReason(const std::string& reason);

} // namespace Format

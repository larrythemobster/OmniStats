#pragma once

#include "core/Config.hpp"

#include <string>

namespace PrivacyLog {
    inline bool DebugEnabled() {
        return Config::Read().debug_logging;
    }

    inline std::string Sensitive(const std::string& value, const char* label) {
        if (value.empty()) return "";
        if (DebugEnabled()) return value;
        return std::string("[") + label + " redacted]";
    }
}

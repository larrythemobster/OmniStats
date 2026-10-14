#pragma once

#include <cstdlib>
#include <memory>
#include <optional>
#include <string>

namespace Env {
    inline std::optional<std::string> Get(const char* name) {
        char* raw = nullptr;
        size_t length = 0;
        if (_dupenv_s(&raw, &length, name) != 0 || raw == nullptr) {
            return std::nullopt;
        }
        const std::unique_ptr<char, decltype(&std::free)> owned(raw, &std::free);
        return std::string(owned.get());
    }
} // namespace Env

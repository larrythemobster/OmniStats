#pragma once
#include <string>

namespace UpdaterCommon {

    bool CreateShortcut(const std::string& exePath, const std::string& shortcutPath, const std::string& description);
    bool RepairShortcutIfExists(const std::string& exePath, const std::string& shortcutPath, const std::string& description);
    bool RepairExistingOmniStatsShortcuts(const std::string& exePath);

} // namespace UpdaterCommon

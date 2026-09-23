#include "ui/rml/RmlUiHelpers.hpp"
#include <shobjidl.h>
#include <commdlg.h>

#include <RmlUi/Core.h>
#include <RmlUi/Core/Context.h>
#include <RmlUi/Core/Element.h>
#include <RmlUi/Core/ElementDocument.h>
#include <RmlUi/Core/ElementInstancer.h>
#include <RmlUi/Core/ElementText.h>
#include <RmlUi/Core/Factory.h>
#include <RmlUi/Core/StyleSheetContainer.h>
#include <RmlUi/Core/Elements/ElementFormControl.h>
#include <RmlUi/Core/Event.h>
#include <SDL2/SDL_gamecontroller.h>
#include <algorithm>
#include <array>
#include <bit>
#include <chrono>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <set>
#include <shared_mutex>
#include <sstream>
#include <unordered_set>

#include "core/AppVersion.hpp"
#include "core/Storage.hpp"
#include "core/StatsApiConfig.hpp"
#include "database/DatabaseManager.hpp"
#include "network/ExternalUpdaterLauncher.hpp"
#include "network/MMRFetcher.hpp"
#include "ui/Formatting.hpp"
#include "ui/KeyNames.hpp"
#include "ui/rml/RmlInputWin32.hpp"
#include "ui/rml/RmlMmrGraphLines.hpp"

namespace RmlUiDetail {
    std::string PromptForDatabaseFile(HWND parentHwnd) {
        std::string result;
        HRESULT hr = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
        const bool mustUninit = (hr == S_OK || hr == S_FALSE);

        IFileOpenDialog* fileOpen = nullptr;
        hr = CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&fileOpen));
        if (SUCCEEDED(hr) && fileOpen) {
            COMDLG_FILTERSPEC filters[] = {
                {L"SQLite Database (*.db)", L"*.db"},
                {L"All Files (*.*)", L"*.*"}};
            fileOpen->SetFileTypes(static_cast<UINT>(std::size(filters)), filters);
            fileOpen->SetDefaultExtension(L"db");
            fileOpen->SetTitle(L"Select OmniStats Database to Merge");

            FILEOPENDIALOGOPTIONS options{};
            if (SUCCEEDED(fileOpen->GetOptions(&options))) {
                fileOpen->SetOptions(options | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | FOS_PATHMUSTEXIST);
            }

            hr = fileOpen->Show(parentHwnd);
            if (SUCCEEDED(hr)) {
                IShellItem* item = nullptr;
                if (SUCCEEDED(fileOpen->GetResult(&item)) && item) {
                    PWSTR filePath = nullptr;
                    if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &filePath)) && filePath) {
                        int len = WideCharToMultiByte(CP_UTF8, 0, filePath, -1, nullptr, 0, nullptr, nullptr);
                        if (len > 0) {
                            result.resize(static_cast<size_t>(len - 1));
                            WideCharToMultiByte(CP_UTF8, 0, filePath, -1, result.data(), len, nullptr, nullptr);
                        }
                        CoTaskMemFree(filePath);
                    }
                    item->Release();
                }
            }
            fileOpen->Release();
        } else {
            wchar_t szFile[MAX_PATH] = {0};
            OPENFILENAMEW ofn{};
            ofn.lStructSize = sizeof(ofn);
            ofn.hwndOwner = parentHwnd;
            ofn.lpstrFile = szFile;
            ofn.nMaxFile = sizeof(szFile) / sizeof(wchar_t);
            ofn.lpstrFilter = L"SQLite Database (*.db)\0*.db\0All Files (*.*)\0*.*\0";
            ofn.nFilterIndex = 1;
            ofn.lpstrTitle = L"Select OmniStats Database to Merge";
            ofn.Flags = OFN_PATHMUSTEXIST | OFN_FILEMUSTEXIST | OFN_NOCHANGEDIR;
            if (GetOpenFileNameW(&ofn)) {
                int len = WideCharToMultiByte(CP_UTF8, 0, szFile, -1, nullptr, 0, nullptr, nullptr);
                if (len > 0) {
                    result.resize(static_cast<size_t>(len - 1));
                    WideCharToMultiByte(CP_UTF8, 0, szFile, -1, result.data(), len, nullptr, nullptr);
                }
            }
        }

        if (mustUninit) {
            CoUninitialize();
        }
        return result;
    }

    int64_t SteadyNowMs() {
        return std::chrono::duration_cast<std::chrono::milliseconds>(
                   std::chrono::steady_clock::now().time_since_epoch())
            .count();
    }

    float SanitizedScale(float value, float fallback) {
        if (!std::isfinite(value) || value < 0.5f) return fallback;
        return value;
    }

    float SanitizedUiScale(float value) {
        if (!std::isfinite(value)) return 1.0f;
        return std::clamp(value, 0.5f, 2.0f);
    }

    std::string RmlDevDirectory() {
        char buffer[MAX_PATH]{};
        const DWORD length = GetEnvironmentVariableA("OMNISTATS_RML_DIR", buffer, MAX_PATH);
        if (length > 0 && length < MAX_PATH) return std::string(buffer, length);
#ifdef OMNISTATS_RML_DEV_DIR
        return OMNISTATS_RML_DEV_DIR;
#else
        return {};
#endif
    }

    std::string ToLower(std::string value) {
        std::transform(value.begin(), value.end(), value.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        return value;
    }

    void HashAppend(uint64_t& hash, std::string_view value) {
        for (unsigned char c : value) {
            hash ^= c;
            hash *= kFnvPrime;
        }
        hash ^= 0xffu;
        hash *= kFnvPrime;
    }

    void HashAppend(uint64_t& hash, uint64_t value) {
        for (int i = 0; i < 8; ++i) {
            hash ^= static_cast<unsigned char>((value >> (i * 8)) & 0xffu);
            hash *= kFnvPrime;
        }
    }

    uint64_t AvalancheHash(uint64_t value) {
        value ^= value >> 33;
        value *= 0xff51afd7ed558ccdull;
        value ^= value >> 33;
        value *= 0xc4ceb9fe1a85ec53ull;
        value ^= value >> 33;
        return value;
    }

    // Platform identity arrives as the `primaryId` prefix Rocket League reports,
    // which varies in casing and spelling ("Epic", "PS4", "XboxOne", "Unknown").
    PlatformKind PlatformKindFor(const std::string& platform) {
        const std::string lower = ToLower(platform);
        if (lower.empty()) return PlatformKind::Unknown;
        if (lower == "epic" || lower == "epicgames") return PlatformKind::Epic;
        if (lower == "steam") return PlatformKind::Steam;
        if (lower.rfind("ps", 0) == 0 || lower == "playstation") return PlatformKind::PlayStation;
        if (lower.rfind("xb", 0) == 0) return PlatformKind::Xbox;
        if (lower == "switch" || lower == "nintendo") return PlatformKind::Nintendo;
        if (lower == "unknown" || lower == "bot") return PlatformKind::Bot;
        return PlatformKind::Unknown;
    }

    std::string PlatformBadgeLabel(PlatformKind kind, const std::string& platform) {
        switch (kind) {
        case PlatformKind::Epic:
            return "EPIC";
        case PlatformKind::Steam:
            return "STEAM";
        case PlatformKind::PlayStation:
            return "PSN";
        case PlatformKind::Xbox:
            return "XBOX";
        case PlatformKind::Nintendo:
            return "SWITCH";
        case PlatformKind::Bot:
            return "BOT";
        case PlatformKind::Unknown:
            break;
        }

        std::string upper = platform;
        std::transform(upper.begin(), upper.end(), upper.begin(), [](unsigned char c) {
            return static_cast<char>(std::toupper(c));
        });
        return upper;
    }

    std::string PlatformDisplayName(PlatformKind kind, const std::string& platform) {
        switch (kind) {
        case PlatformKind::Epic:
            return "Epic";
        case PlatformKind::Steam:
            return "Steam";
        case PlatformKind::PlayStation:
            return "PlayStation";
        case PlatformKind::Xbox:
            return "Xbox";
        case PlatformKind::Nintendo:
            return "Nintendo";
        case PlatformKind::Bot:
            return "Bot";
        case PlatformKind::Unknown:
            break;
        }
        return platform;
    }

    // Class names only; the palette lives in kPlatformPalette so the theme pass
    // can reapply platform colors after it overwrites `.badge`/`.label` colors.
    const char* PlatformClass(PlatformKind kind) {
        switch (kind) {
        case PlatformKind::Epic:
            return "platform platform-epic";
        case PlatformKind::Steam:
            return "platform platform-steam";
        case PlatformKind::PlayStation:
            return "platform platform-psn";
        case PlatformKind::Xbox:
            return "platform platform-xbox";
        case PlatformKind::Nintendo:
            return "platform platform-switch";
        case PlatformKind::Bot:
            return "platform platform-bot";
        case PlatformKind::Unknown:
            break;
        }
        return "platform platform-unknown";
    }

    Rml::Span<const Rml::byte> EmbeddedResource(const char* name) {
        HMODULE module = GetModuleHandleW(nullptr);
        HRSRC resource = FindResourceA(module, name, RT_RCDATA);
        if (!resource) return {};
        HGLOBAL loaded = LoadResource(module, resource);
        if (!loaded) return {};
        const DWORD size = SizeofResource(module, resource);
        const auto* bytes = static_cast<const Rml::byte*>(LockResource(loaded));
        if (!bytes || size == 0) return {};
        return {bytes, static_cast<size_t>(size)};
    }

    std::vector<Rml::byte> ReadFontFile(const char* fileName) {
        const std::string relative = std::string("resources/fonts/") + fileName;
        std::vector<std::string> candidates = {relative};
#ifdef OMNISTATS_SOURCE_DIR
        candidates.push_back(std::string(OMNISTATS_SOURCE_DIR) + "/" + relative);
#endif
        for (const auto& candidate : candidates) {
            std::error_code ec;
            const auto size = std::filesystem::file_size(candidate, ec);
            if (ec || size == 0) continue;
            std::ifstream file(candidate, std::ios::binary);
            if (!file) continue;
            std::vector<Rml::byte> bytes(static_cast<size_t>(size));
            file.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(size));
            if (file.gcount() != static_cast<std::streamsize>(size)) continue;
            return bytes;
        }
        return {};
    }

    std::string TrackerUrlForPlayer(const PlayerData& player) {
        const size_t delimiter = player.primaryId.find('|');
        if (delimiter == std::string::npos) return {};

        const std::string rawPlatform = ToLower(player.primaryId.substr(0, delimiter));
        std::string trackerPlatform;
        if (rawPlatform == "epic" || rawPlatform == "epicgames")
            trackerPlatform = "epic";
        else if (rawPlatform == "steam")
            trackerPlatform = "steam";
        else if (rawPlatform == "ps4" || rawPlatform == "psn" || rawPlatform == "playstation")
            trackerPlatform = "psn";
        else if (rawPlatform == "xboxone" || rawPlatform == "xbox" || rawPlatform == "xbl")
            trackerPlatform = "xbl";
        else if (rawPlatform == "switch" || rawPlatform == "nintendo")
            trackerPlatform = "switch";
        else
            return {};

        std::string identifier = player.name;
        if (trackerPlatform == "steam") {
            const size_t idStart = delimiter + 1;
            const size_t secondDelimiter = player.primaryId.find('|', idStart);
            identifier = player.primaryId.substr(idStart, secondDelimiter == std::string::npos ? std::string::npos : secondDelimiter - idStart);
        }
        if (identifier.empty()) return {};

        return "https://rocketleague.tracker.network/rocket-league/profile/" + trackerPlatform + "/" +
               Format::SimpleUrlEncode(identifier) + "/overview";
    }

    std::string Attribute(Rml::Element* element, const char* name) {
        if (!element) return {};
        return element->GetAttribute<Rml::String>(name, "").c_str();
    }

    const char* GamepadButtonName(int button) {
        switch (button) {
        case SDL_CONTROLLER_BUTTON_A:
            return "A";
        case SDL_CONTROLLER_BUTTON_B:
            return "B";
        case SDL_CONTROLLER_BUTTON_X:
            return "X";
        case SDL_CONTROLLER_BUTTON_Y:
            return "Y";
        case SDL_CONTROLLER_BUTTON_BACK:
            return "Select/Back";
        case SDL_CONTROLLER_BUTTON_GUIDE:
            return "Guide";
        case SDL_CONTROLLER_BUTTON_START:
            return "Start";
        case SDL_CONTROLLER_BUTTON_LEFTSTICK:
            return "Left Thumb";
        case SDL_CONTROLLER_BUTTON_RIGHTSTICK:
            return "Right Thumb";
        case SDL_CONTROLLER_BUTTON_LEFTSHOULDER:
            return "Left Bumper";
        case SDL_CONTROLLER_BUTTON_RIGHTSHOULDER:
            return "Right Bumper";
        case SDL_CONTROLLER_BUTTON_DPAD_UP:
            return "D-Pad Up";
        case SDL_CONTROLLER_BUTTON_DPAD_DOWN:
            return "D-Pad Down";
        case SDL_CONTROLLER_BUTTON_DPAD_LEFT:
            return "D-Pad Left";
        case SDL_CONTROLLER_BUTTON_DPAD_RIGHT:
            return "D-Pad Right";
        case 22:
            return "Left Stick Up";
        case 23:
            return "Left Stick Down";
        case 24:
            return "Left Stick Left";
        case 25:
            return "Left Stick Right";
        case 26:
            return "Right Stick Up";
        case 27:
            return "Right Stick Down";
        case 28:
            return "Right Stick Left";
        case 29:
            return "Right Stick Right";
        default:
            return nullptr;
        }
    }

    std::string GamepadBindName(int mappedButton, bool raw, int rawButton) {
        if (raw && rawButton >= 0) return "Raw Button " + std::to_string(rawButton);
        if (!raw) {
            if (const char* name = GamepadButtonName(mappedButton)) return name;
            if (mappedButton >= 0) return "Controller Button " + std::to_string(mappedButton);
        }
        return "None";
    }

    DashboardLayout::WidgetId WidgetFromDom(const std::string& value) {
        if (value == "live-roster") return DashboardLayout::WidgetId::LiveRoster;
        if (value == "live-match") return DashboardLayout::WidgetId::LiveMatchStats;
        if (value == "session") return DashboardLayout::WidgetId::SessionStats;
        if (value == "mmr-graph") return DashboardLayout::WidgetId::MmrGraph;
        if (value == "streaks") return DashboardLayout::WidgetId::StreaksStats;
        if (value == "gamemodes") return DashboardLayout::WidgetId::GamemodeBreakdown;
        if (value == "lobby-ranks") return DashboardLayout::WidgetId::LobbyRanks;
        if (value == "demos") return DashboardLayout::WidgetId::DemoTracker;
        if (value == "previous-games") return DashboardLayout::WidgetId::PreviousGames;
        return DashboardLayout::WidgetId::LiveRoster;
    }

    DashboardLayout::Zone ZoneFromDom(const std::string& value) {
        if (value == "right") return DashboardLayout::Zone::Right;
        if (value == "bottom") return DashboardLayout::Zone::Bottom;
        if (value == "hidden") return DashboardLayout::Zone::Hidden;
        if (value == "top") return DashboardLayout::Zone::Top;
        return DashboardLayout::Zone::Left;
    }

    std::string MmrLabel(MmrCategory category) {
        switch (category) {
        case MmrCategory::Best:
            return "Best Rank";
        case MmrCategory::OneVOne:
            return "1v1";
        case MmrCategory::TwoVTwo:
            return "2v2";
        case MmrCategory::ThreeVThree:
            return "3v3";
        case MmrCategory::Casual:
            return "Casual";
        case MmrCategory::Tourny:
            return "Tournament";
        case MmrCategory::Hoops:
            return "Hoops";
        case MmrCategory::Rumble:
            return "Rumble";
        case MmrCategory::Dropshot:
            return "Dropshot";
        case MmrCategory::SnowDay:
            return "Snow Day";
        case MmrCategory::Heatseeker:
            return "Heatseeker";
        }
        return "Best Rank";
    }

    std::vector<MmrCategory> MmrCategories(bool includeBest, bool extras) {
        std::vector<MmrCategory> result;
        if (includeBest) result.push_back(MmrCategory::Best);
        result.insert(result.end(), {MmrCategory::OneVOne, MmrCategory::TwoVTwo, MmrCategory::ThreeVThree});
        if (extras) result.insert(result.end(), {MmrCategory::Hoops, MmrCategory::Rumble, MmrCategory::Dropshot, MmrCategory::SnowDay, MmrCategory::Heatseeker});
        result.insert(result.end(), {MmrCategory::Casual, MmrCategory::Tourny});
        return result;
    }

    std::string RankPart(const std::string& tier) {
        std::string value = ToLower(tier);
        if (const size_t divPos = value.find(" div"); divPos != std::string::npos) value.resize(divPos);
        while (!value.empty() && std::isspace(static_cast<unsigned char>(value.back())))
            value.pop_back();
        return value;
    }

    int TierResourceIndex(const std::string& tier) {
        const std::string t = RankPart(tier);
        int base = 0;
        if (t.empty() || t == "-" || t.find("unranked") != std::string::npos) return 0;
        if (t.find("supersonic") == 0) return 22;
        if (t.find("bronze") == 0)
            base = 1;
        else if (t.find("silver") == 0)
            base = 4;
        else if (t.find("gold") == 0)
            base = 7;
        else if (t.find("platinum") == 0)
            base = 10;
        else if (t.find("diamond") == 0)
            base = 13;
        else if (t.find("grand champion") == 0)
            base = 19;
        else if (t.find("champion") == 0)
            base = 16;
        else
            return 0;

        if (t.find(" iii") != std::string::npos || t.find(" 3") != std::string::npos) return base + 2;
        if (t.find(" ii") != std::string::npos || t.find(" 2") != std::string::npos) return base + 1;
        return base;
    }

    int DivisionLevel(const std::string& tier) {
        const int tierIndex = TierResourceIndex(tier);
        if (tierIndex <= 0 || tierIndex >= 22) return 0;
        const std::string t = ToLower(tier);
        if (t.find("div iv") != std::string::npos || t.find("division iv") != std::string::npos || t.find("div 4") != std::string::npos || t.find("division 4") != std::string::npos) return 4;
        if (t.find("div iii") != std::string::npos || t.find("division iii") != std::string::npos || t.find("div 3") != std::string::npos || t.find("division 3") != std::string::npos) return 3;
        if (t.find("div ii") != std::string::npos || t.find("division ii") != std::string::npos || t.find("div 2") != std::string::npos || t.find("division 2") != std::string::npos) return 2;
        if (t.find("div i") != std::string::npos || t.find("division i") != std::string::npos || t.find("div 1") != std::string::npos || t.find("division 1") != std::string::npos) return 1;
        return 0;
    }

    int DivisionColorResourceIndex(const std::string& tier) {
        const int index = TierResourceIndex(tier);
        if (index >= 1 && index <= 3) return 1;
        if (index >= 4 && index <= 6) return 2;
        if (index >= 7 && index <= 9) return 3;
        if (index >= 10 && index <= 12) return 4;
        if (index >= 13 && index <= 15) return 5;
        if (index >= 16 && index <= 18) return 6;
        if (index >= 19 && index <= 22) return 7;
        return 0;
    }

    int PlaylistResourceIndex(const std::string& playlist) {
        const std::string p = ToLower(playlist);
        if (p == "1v1" || p == "duel") return 0;
        if (p == "2v2" || p == "doubles") return 1;
        if (p == "3v3" || p == "standard") return 2;
        if (p == "hoops") return 3;
        if (p == "rumble") return 4;
        if (p == "dropshot") return 5;
        if (p == "snowday" || p == "snow day") return 6;
        if (p == "t" || p == "tourny" || p == "tournament" || p == "tournaments") return 7;
        if (p == "4v4") return 8;
        if (p == "heatseeker") return 9;
        return -1;
    }

    std::string Escape(std::string_view text) {
        std::string out;
        out.reserve(text.size() + 16);

        const auto appendAscii = [&out](unsigned char c) {
            switch (c) {
            case '&':
                out += "&amp;";
                break;
            case '<':
                out += "&lt;";
                break;
            case '>':
                out += "&gt;";
                break;
            case '\"':
                out += "&quot;";
                break;
            case '\'':
                out += "&#39;";
                break;
            default:
                out.push_back(static_cast<char>(c));
                break;
            }
        };
        const auto isContinuation = [](unsigned char c) { return (c & 0xC0u) == 0x80u; };
        const auto appendReplacement = [&out]() { out += "\xEF\xBF\xBD"; };

        for (size_t i = 0; i < text.size();) {
            const auto lead = static_cast<unsigned char>(text[i]);
            if (lead < 0x80u) {
                appendAscii(lead);
                ++i;
                continue;
            }

            size_t length = 0;
            bool valid = false;
            if (lead >= 0xC2u && lead <= 0xDFu) {
                length = 2;
                valid = i + length <= text.size() &&
                        isContinuation(static_cast<unsigned char>(text[i + 1]));
            } else if (lead >= 0xE0u && lead <= 0xEFu) {
                length = 3;
                if (i + length <= text.size()) {
                    const auto b1 = static_cast<unsigned char>(text[i + 1]);
                    const auto b2 = static_cast<unsigned char>(text[i + 2]);
                    const bool secondValid =
                        (lead == 0xE0u) ? (b1 >= 0xA0u && b1 <= 0xBFu) : (lead == 0xEDu) ? (b1 >= 0x80u && b1 <= 0x9Fu)
                                                                                         : isContinuation(b1);
                    valid = secondValid && isContinuation(b2);
                }
            } else if (lead >= 0xF0u && lead <= 0xF4u) {
                length = 4;
                if (i + length <= text.size()) {
                    const auto b1 = static_cast<unsigned char>(text[i + 1]);
                    const auto b2 = static_cast<unsigned char>(text[i + 2]);
                    const auto b3 = static_cast<unsigned char>(text[i + 3]);
                    const bool secondValid =
                        (lead == 0xF0u) ? (b1 >= 0x90u && b1 <= 0xBFu) : (lead == 0xF4u) ? (b1 >= 0x80u && b1 <= 0x8Fu)
                                                                                         : isContinuation(b1);
                    valid = secondValid && isContinuation(b2) && isContinuation(b3);
                }
            }

            if (!valid) {
                appendReplacement();
                ++i;
                continue;
            }

            out.append(text.substr(i, length));
            i += length;
        }
        return out;
    }

    std::string FormatNumber(float value, int precision) {
        std::ostringstream out;
        out << std::fixed << std::setprecision(precision) << value;
        return out.str();
    }

    std::string FormatRecord(int wins, int losses) {
        return std::to_string(wins) + "-" + std::to_string(losses);
    }

    std::string FormatClock(int64_t unixSeconds) {
        if (unixSeconds <= 0) return "--";
        int64_t now = static_cast<int64_t>(std::time(nullptr));
        int64_t diff = now - unixSeconds;
        if (diff < 0) diff = 0;
        if (diff < 60) return "now";
        if (diff < 3600) return std::to_string(diff / 60) + "m ago";
        if (diff < 86400) return std::to_string(diff / 3600) + "h ago";
        return std::to_string(diff / 86400) + "d ago";
    }

    std::string ToggleControl(std::string_view key, std::string_view label, std::string_view help, bool checked, bool disabled) {
        std::string out = "<div class='setting-row'><div class='setting-info'><div class='setting-name'>" + Escape(label) + "</div>";
        if (!help.empty()) out += "<div class='setting-help'>" + Escape(help) + "</div>";
        out += "</div><div class='toggle-switch'><input type='checkbox' class='checkbox' data-setting='" + Escape(key) + "' ";
        if (checked) out += "checked='checked' ";
        if (disabled) out += "disabled='disabled' ";
        out += "/><span class='toggle-thumb'></span></div></div>";
        return out;
    }

    std::string SelectControl(std::string_view key, const std::vector<SelectOption>& options, std::string_view current, const char* klass, bool disabled) {
        std::string out = "<select";
        if (klass && *klass) {
            out += " class='";
            out += klass;
            out += '\'';
        }
        out += " data-setting='" + Escape(key) + '\'';
        if (disabled) out += " disabled='disabled'";
        out += '>';
        for (const auto& option : options) {
            out += "<option value='" + Escape(option.value) + '\'';
            if (option.value == current) out += " selected='selected'";
            out += '>' + Escape(option.label) + "</option>";
        }
        out += "</select>";
        return out;
    }

    std::string SelectRow(std::string_view key, std::string_view label, std::string_view help, const std::vector<SelectOption>& options, std::string_view current, bool disabled) {
        std::string out = "<div class='setting-row'><div class='setting-info'><div class='setting-name'>" + Escape(label) + "</div>";
        if (!help.empty()) out += "<div class='setting-help'>" + Escape(help) + "</div>";
        out += "</div>" + SelectControl(key, options, current, "", disabled) + "</div>";
        return out;
    }

    std::vector<SelectOption> MmrCategoryOptions(bool includeBest, bool extras) {
        std::vector<SelectOption> options;
        for (auto category : MmrCategories(includeBest, extras))
            options.push_back({MmrCategoryToString(category), MmrLabel(category)});
        return options;
    }

    std::vector<SelectOption> GamemodeScopeOptions() {
        return {{"current_session", "Current Session"}, {"all_time", "All-Time"}};
    }

    std::string GamemodeScopeValue(const ConfigData& config) {
        return config.gamemode_breakdown_scope == "all_time" ? "all_time" : "current_session";
    }

    std::string StatCell(std::string_view label, std::string_view value, std::string_view valueClass) {
        std::string out = "<div class='stat-cell'><div class='label'>" + Escape(label) + "</div><div class='value mono";
        if (!valueClass.empty()) {
            out += ' ';
            out += valueClass;
        }
        out += "'>" + Escape(value) + "</div></div>";
        return out;
    }

    std::string StatGrid(std::string_view title, const std::vector<std::pair<std::string, std::string>>& rows) {
        if (rows.empty()) return {};
        std::string out = "<div class='stat-section-title' style='margin-top:7dp'>" + Escape(title) + "</div><div class='stat-grid cols-2 compact-stats'>";
        for (const auto& [label, value] : rows)
            out += StatCell(label, value);
        out += "</div>";
        return out;
    }

    std::string SectionStart(std::string_view title) {
        return "<div class='setting-section'><div class='setting-title'>" + Escape(title) + "</div>";
    }

    std::string SectionEnd() {
        return "</div>";
    }

    std::string Button(std::string_view action, std::string_view label, const char* klass) {
        std::string out = "<button data-action='" + Escape(action) + '\'';
        if (klass && *klass) {
            out += " class='";
            out += klass;
            out += '\'';
        }
        out += '>' + Escape(label) + "</button>";
        return out;
    }

    ColorRGBA* ThemeColorForKey(ConfigData& config, std::string_view key) {
        if (key == "theme_bg") return &config.themeBg;
        if (key == "theme_panel") return &config.themeSettingsPanel;
        if (key == "theme_topbar") return &config.themeTopbar;
        if (key == "theme_graph_panel") return &config.themeGraphPanel;
        if (key == "theme_roster_card") return &config.themeRosterCard;
        if (key == "theme_roster_card_self") return &config.themeRosterCardSelf;
        if (key == "theme_stat_box") return &config.themeStatBox;
        if (key == "theme_match_row") return &config.themeMatchRow;
        if (key == "theme_match_row_alt") return &config.themeMatchRowAlt;
        if (key == "theme_text") return &config.themeText;
        if (key == "theme_accent") return &config.themeAccent;
        if (key == "theme_win") return &config.themeWin;
        if (key == "theme_loss") return &config.themeLoss;
        if (key == "theme_dim") return &config.themeDim;
        if (key == "theme_muted") return &config.themeMuted;
        if (key == "theme_graph") return &config.themeGraphLine;
        if (key == "theme_baseline") return &config.themeGraphBaseline;
        return nullptr;
    }

    const ColorRGBA* ThemeColorForKey(const ConfigData& config, std::string_view key) {
        if (key == "theme_bg") return &config.themeBg;
        if (key == "theme_panel") return &config.themeSettingsPanel;
        if (key == "theme_topbar") return &config.themeTopbar;
        if (key == "theme_graph_panel") return &config.themeGraphPanel;
        if (key == "theme_roster_card") return &config.themeRosterCard;
        if (key == "theme_roster_card_self") return &config.themeRosterCardSelf;
        if (key == "theme_stat_box") return &config.themeStatBox;
        if (key == "theme_match_row") return &config.themeMatchRow;
        if (key == "theme_match_row_alt") return &config.themeMatchRowAlt;
        if (key == "theme_text") return &config.themeText;
        if (key == "theme_accent") return &config.themeAccent;
        if (key == "theme_win") return &config.themeWin;
        if (key == "theme_loss") return &config.themeLoss;
        if (key == "theme_dim") return &config.themeDim;
        if (key == "theme_muted") return &config.themeMuted;
        if (key == "theme_graph") return &config.themeGraphLine;
        if (key == "theme_baseline") return &config.themeGraphBaseline;
        return nullptr;
    }

    const char* ThemeColorLabel(std::string_view key) {
        if (key == "theme_bg") return "Overlay background";
        if (key == "theme_panel") return "Settings panels";
        if (key == "theme_topbar") return "Top bar";
        if (key == "theme_graph_panel") return "Graph panel";
        if (key == "theme_roster_card") return "Roster card";
        if (key == "theme_roster_card_self") return "Roster card (you)";
        if (key == "theme_stat_box") return "Stat boxes / K/D";
        if (key == "theme_match_row") return "Previous games row";
        if (key == "theme_match_row_alt") return "Previous games row (alt)";
        if (key == "theme_text") return "Text";
        if (key == "theme_accent") return "Accent";
        if (key == "theme_win") return "Win";
        if (key == "theme_loss") return "Loss";
        if (key == "theme_dim") return "Dim text";
        if (key == "theme_muted") return "Muted info";
        if (key == "theme_graph") return "Graph line";
        if (key == "theme_baseline") return "Graph baseline";
        return "Color";
    }

    bool ParseThemeComponentKey(std::string_view key, std::string_view& colorKey, char& component) {
        const size_t separator = key.rfind(':');
        if (separator == std::string_view::npos || separator + 2 != key.size()) return false;
        colorKey = key.substr(0, separator);
        component = key[separator + 1];
        return component == 'r' || component == 'g' || component == 'b' || component == 'a';
    }
    bool SetThemeComponent(ConfigData& config, std::string_view key, const std::string& value) {
        std::string_view colorKey;
        char component = 0;
        if (!ParseThemeComponentKey(key, colorKey, component)) return false;
        ColorRGBA* color = ThemeColorForKey(config, colorKey);
        if (!color) return false;

        char* end = nullptr;
        const long parsed = std::strtol(value.c_str(), &end, 10);
        if (!end || end == value.c_str() || *end != '\0') return false;
        const float normalized = static_cast<float>(std::clamp(parsed, 0L, 255L)) / 255.0f;
        switch (component) {
        case 'r':
            color->r = normalized;
            break;
        case 'g':
            color->g = normalized;
            break;
        case 'b':
            color->b = normalized;
            break;
        case 'a':
            color->a = normalized;
            break;
        default:
            return false;
        }
        return true;
    }

    Hsv RgbToHsv(const ColorRGBA& color) {
        const float r = std::clamp(color.r, 0.0f, 1.0f);
        const float g = std::clamp(color.g, 0.0f, 1.0f);
        const float b = std::clamp(color.b, 0.0f, 1.0f);
        const float max = std::max({r, g, b});
        const float min = std::min({r, g, b});
        const float span = max - min;

        Hsv hsv;
        hsv.v = max;
        hsv.s = max > 0.0f ? span / max : 0.0f;
        if (span <= 0.0f) return hsv;

        if (max == r)
            hsv.h = 60.0f * std::fmod((g - b) / span, 6.0f);
        else if (max == g)
            hsv.h = 60.0f * ((b - r) / span + 2.0f);
        else
            hsv.h = 60.0f * ((r - g) / span + 4.0f);
        if (hsv.h < 0.0f) hsv.h += 360.0f;
        return hsv;
    }

    ColorRGBA HsvToRgb(const Hsv& hsv, float alpha) {
        const float h = std::fmod(std::fmod(hsv.h, 360.0f) + 360.0f, 360.0f) / 60.0f;
        const float s = std::clamp(hsv.s, 0.0f, 1.0f);
        const float v = std::clamp(hsv.v, 0.0f, 1.0f);
        const float c = v * s;
        const float x = c * (1.0f - std::fabs(std::fmod(h, 2.0f) - 1.0f));
        const float m = v - c;

        float r = m, g = m, b = m;
        switch (static_cast<int>(h)) {
        case 0:
            r += c;
            g += x;
            break;
        case 1:
            r += x;
            g += c;
            break;
        case 2:
            g += c;
            b += x;
            break;
        case 3:
            g += x;
            b += c;
            break;
        case 4:
            r += x;
            b += c;
            break;
        default:
            r += c;
            b += x;
            break;
        }
        return ColorRGBA{r, g, b, std::clamp(alpha, 0.0f, 1.0f)};
    }

    int QuantizedPickerHue(float hue) {
        constexpr int kHueStep = 6;
        int gradientHue = static_cast<int>(std::lround(hue / static_cast<float>(kHueStep))) * kHueStep;
        gradientHue %= 360;
        if (gradientHue < 0) gradientHue += 360;
        return gradientHue;
    }

    int EnabledLobbyRankColumns(const ConfigData& config) {
        int rankColumns = 0;
        rankColumns += config.show_lobby_rank_1v1 ? 1 : 0;
        rankColumns += config.show_lobby_rank_2v2 ? 1 : 0;
        rankColumns += config.show_lobby_rank_3v3 ? 1 : 0;
        rankColumns += config.show_lobby_rank_casual ? 1 : 0;
        rankColumns += config.show_lobby_rank_tourny ? 1 : 0;
        if (config.show_extra_playlists) {
            rankColumns += config.show_lobby_rank_hoops ? 1 : 0;
            rankColumns += config.show_lobby_rank_rumble ? 1 : 0;
            rankColumns += config.show_lobby_rank_dropshot ? 1 : 0;
            rankColumns += config.show_lobby_rank_snowday ? 1 : 0;
            rankColumns += config.show_lobby_rank_heatseeker ? 1 : 0;
        }
        return std::max(rankColumns, 1);
    }

    float LobbyRanksContentMinDp(const ConfigData& config) {
        const int columns = EnabledLobbyRankColumns(config);
        // Keep a protected identity column plus one column per enabled playlist,
        // matching the compact .lobby-rank-* metrics in the stylesheet:
        // 135dp identity column + 4dp row padding + 22dp card padding/border + 4dp tolerance
        // = 165dp base, plus 68dp per enabled playlist column (rank/rating pair
        // plus the games-played gutter).
        return 165.0f + 68.0f * static_cast<float>(columns);
    }

    std::pair<float, float> OverlayWidgetDefaultSize(DashboardLayout::WidgetId widget, float dpiScale) {
        const float dpi = SanitizedScale(dpiScale);
        std::pair<float, float> size;
        switch (widget) {
        case DashboardLayout::WidgetId::LiveRoster:
            size = {440.0f, 260.0f};
            break;
        case DashboardLayout::WidgetId::LiveMatchStats:
            size = {340.0f, 210.0f};
            break;
        case DashboardLayout::WidgetId::SessionStats:
            size = {280.0f, 220.0f};
            break;
        case DashboardLayout::WidgetId::MmrGraph:
            size = {430.0f, 260.0f};
            break;
        case DashboardLayout::WidgetId::StreaksStats:
            size = {300.0f, 115.0f};
            break;
        case DashboardLayout::WidgetId::GamemodeBreakdown:
            size = {340.0f, 210.0f};
            break;
        case DashboardLayout::WidgetId::LobbyRanks:
            // Auto width comes from the per-column minimum below, so this only
            // has to cover the identity column and margins.
            size = {165.0f, 220.0f};
            break;
        case DashboardLayout::WidgetId::DemoTracker:
            size = {280.0f, 84.0f};
            break;
        case DashboardLayout::WidgetId::PreviousGames:
            size = {560.0f, 260.0f};
            break;
        default:
            size = {320.0f, 200.0f};
            break;
        }
        return {size.first * dpi, size.second * dpi};
    }

    std::pair<float, float> OverlayWidgetMinSize(DashboardLayout::WidgetId widget, float dpiScale) {
        const float dpi = SanitizedScale(dpiScale);
        if (widget == DashboardLayout::WidgetId::LiveRoster) return {430.0f * dpi, 120.0f * dpi};
        return {220.0f * dpi, 120.0f * dpi};
    }

    // Overlay container geometry is persisted in design pixels at 100% app text
    // size. The drawn box has to grow and shrink with ui_scale exactly like the
    // text inside it: otherwise low scales leave dead space (and high scales
    // overflow), and every drag/resize clamp is computed against a box that is
    // not what the user sees.
    float OverlayUiScale(const ConfigData* config) {
        return config ? SanitizedUiScale(config->ui_scale) : 1.0f;
    }

    std::pair<float, float> OverlayContainerSize(const OverlayLayout::ContainerConfig& container, float dpiScale, const ConfigData* config) {
        const float ui = OverlayUiScale(config);
        float width = container.w * ui;
        float height = container.h * ui;
        if (container.w <= 1.0f) {
            width = 0.0f;
            for (auto widget : container.widgets)
                width = std::max(width, OverlayWidgetDefaultSize(widget, dpiScale).first * ui);
        }
        if (container.h <= 1.0f) {
            height = 0.0f;
            for (auto widget : container.widgets)
                height += OverlayWidgetDefaultSize(widget, dpiScale).second * ui;
            height += std::max<int>(0, static_cast<int>(container.widgets.size()) - 1) * 42.0f * SanitizedScale(dpiScale) * ui;
        }
        const auto [minWidth, minHeight] = OverlayContainerMinSize(container, dpiScale, config);
        return {std::max(width, minWidth), std::max(height, minHeight)};
    }

    std::pair<float, float> OverlayContainerMinSize(const OverlayLayout::ContainerConfig& container, float dpiScale, const ConfigData* config) {
        const float dpi = SanitizedScale(dpiScale);
        const float ui = OverlayUiScale(config);
        const float scale = dpi * ui;
        float width = 220.0f * scale;
        float height = 120.0f * scale;
        if (!container.widgets.empty()) {
            width = 0.0f;
            height = 0.0f;
            for (auto widget : container.widgets) {
                float widgetWidth = 0.0f;
                float widgetHeight = 0.0f;
                if (widget == DashboardLayout::WidgetId::LobbyRanks && config) {
                    widgetWidth = LobbyRanksContentMinDp(*config) * scale;
                    widgetHeight = 120.0f * scale;
                } else {
                    const auto [ww, wh] = OverlayWidgetMinSize(widget, dpi);
                    widgetWidth = ww * ui;
                    widgetHeight = wh * ui;
                }
                width = std::max(width, widgetWidth);
                height += widgetHeight;
            }
            height += std::max<int>(0, static_cast<int>(container.widgets.size()) - 1) * 42.0f * scale;
        }
        return {std::max(width, 220.0f * scale), std::max(height, 120.0f * scale)};
    }
}

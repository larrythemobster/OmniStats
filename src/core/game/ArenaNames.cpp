#include "core/game/ArenaNames.hpp"
#include <cctype>
#include <unordered_map>

namespace ArenaNames {
    namespace {
        std::string Lower(std::string_view value) {
            std::string out(value);
            for (char& c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        bool IsPackageName(std::string_view value) {
            if (value.size() < 3 || value.find(' ') != std::string_view::npos) return false;
            const char p = value[value.size() - 1];
            return value[value.size() - 2] == '_' && (p == 'p' || p == 'P');
        }

        // Display names follow the in-game map selector; the asset list matches rlstats.net/gameinfo/maps.
        const std::unordered_map<std::string, std::string>& Maps() {
            static const std::unordered_map<std::string, std::string> maps = {
                {"underwater", "AquaDome"},
                {"underwater_grs", "AquaDome (Salty Shallows)"},
                {"park", "Beckwith Park"},
                {"park_night", "Beckwith Park (Midnight)"},
                {"park_snowy", "Beckwith Park (Snowy)"},
                {"park_rainy", "Beckwith Park (Stormy)"},
                {"park_bman", "Beckwith Park (Gotham Night)"},
                {"mall_day", "Boostfield Mall"},
                {"cs", "Champions Field"},
                {"cs_day", "Champions Field (Day)"},
                {"swoosh", "Champions Field (Nike FC)"},
                {"bb", "Champions Field (NFL)"},
                {"outlaw", "Deadeye Canyon"},
                {"outlaw_oasis", "Deadeye Canyon (Oasis)"},
                {"stadium", "DFH Stadium"},
                {"stadium_10a", "DFH Stadium (10th Anniversary)"},
                {"stadium_race_day", "DFH Stadium (Circuit)"},
                {"stadium_day", "DFH Stadium (Day)"},
                {"stadium_winter", "DFH Stadium (Snowy)"},
                {"stadium_foggy", "DFH Stadium (Stormy)"},
                {"woods", "Drift Woods"},
                {"woods_night", "Drift Woods (Night)"},
                {"ff_dusk", "Estadio Vida"},
                {"farm", "Farmstead"},
                {"farm_night", "Farmstead (Night)"},
                {"farm_grs", "Farmstead (Pitched)"},
                {"farm_hw", "Farmstead (Spooky)"},
                {"farm_upsidedown", "Farmstead (The Upside Down)"},
                {"chn_stadium", "Forbidden Temple"},
                {"chn_stadium_day", "Forbidden Temple (Day)"},
                {"fni_stadium", "Forbidden Temple (Fire & Ice)"},
                {"uf_day", "Futura Garden"},
                {"uf_night", "Futura Garden (Night)"},
                {"eurostadium", "Mannfield"},
                {"eurostadium_dusk", "Mannfield (Dusk)"},
                {"eurostadium_night", "Mannfield (Night)"},
                {"eurostadium_snownight", "Mannfield (Snowy)"},
                {"eurostadium_rainy", "Mannfield (Stormy)"},
                {"neotokyo_standard", "Neo Tokyo"},
                {"neotokyo_arcade", "Neo Tokyo (Arcade)"},
                {"neotokyo_toon", "Neo Tokyo (Comic)"},
                {"neotokyo_hax", "Neo Tokyo (Hacked)"},
                {"music", "Neon Fields"},
                {"paname_dusk", "Parc de Paris"},
                {"cs_hw", "Rivals Arena"},
                {"beach", "Salty Shores"},
                {"beach_night", "Salty Shores (Night)"},
                {"beach_night_grs", "Salty Shores (Salty Fest)"},
                {"street", "Sovereign Heights"},
                {"arc_standard", "Starbase ARC"},
                {"arc_darc", "Starbase ARC (Aftermath)"},
                {"trainstation", "Urban Central"},
                {"trainstation_dawn", "Urban Central (Dawn)"},
                {"trainstation_night", "Urban Central (Night)"},
                {"trainstation_spooky", "Urban Central (Haunted)"},
                {"haunted_trainstation", "Urban Central (Haunted)"},
                {"utopiastadium", "Utopia Coliseum"},
                {"utopiastadium_dusk", "Utopia Coliseum (Dusk)"},
                {"utopiastadium_lux", "Utopia Coliseum (Gilded)"},
                {"utopiastadium_snow", "Utopia Coliseum (Snowy)"},
                {"wasteland_s", "Wasteland"},
                {"wasteland_night_s", "Wasteland (Night)"},
                {"wasteland_grs", "Wasteland (Pitched)"},
                {"arc", "Arctagon"},
                {"wasteland", "Badlands"},
                {"wasteland_night", "Badlands (Night)"},
                {"labs_pillarheat", "Barricade"},
                {"labs_basin", "Basin"},
                {"ko_calavera", "Calavera"},
                {"ko_carbon", "Carbon"},
                {"labs_pillarwings", "Colossus"},
                {"shattershot", "Core 707"},
                {"labs_corridor", "Corridor"},
                {"labs_cosmic", "Cosmic"},
                {"labs_cosmic_v4", "Cosmic"},
                {"labs_doublegoal", "Double Goal"},
                {"labs_doublegoal_v2", "Double Goal"},
                {"hoopsstadium", "Dunk House"},
                {"hoops_dunkhouse", "Dunk House"},
                {"labs_holyfield_space", "Force Field"},
                {"labs_galleon", "Galleon"},
                {"labs_galleon_mast", "Galleon Retro"},
                {"labs_pillarglass", "Hourglass"},
                {"labs_holyfield", "Loophole"},
                {"labs_4v4_arena15_eurostadium_night", "Mannfield (Quads)"},
                {"labs_4v4_arena15_blackout", "Midnight Metro"},
                {"labs_4v4_arena15_retro", "Sunset Dunes"},
                {"labs_octagon", "Octagon"},
                {"labs_octagon_02", "Octagon"},
                {"labs_circlepillars", "Pillars"},
                {"ko_quadron", "Quadron"},
                {"labs_octagon_b2b_02", "Roadblock"},
                {"hoopsstreet", "The Block"},
                {"throwbackstadium", "Throwback Stadium"},
                {"throwbackhockey", "Throwback Stadium (Snowy)"},
                {"neotokyo", "Tokyo Underpass"},
                {"labs_underpass", "Underpass"},
                {"labs_underpass_v0", "Underpass"},
                {"labs_utopia", "Utopia Retro"},
                {"labs_octagon_vent", "Vent"},
            };
            return maps;
        }

        // Names the previous asset table produced that are wrong for the asset that produced them.
        // "Neo Tokyo" is excluded: it came from both neotokyo_standard_p and neotokyo_p.
        const std::unordered_map<std::string, std::string>& LegacyRenames() {
            static const std::unordered_map<std::string, std::string> renames = {
                {"AquaDome (Grs)", "AquaDome (Salty Shallows)"},
                {"Beckwith Park (Bman)", "Beckwith Park (Gotham Night)"},
                {"Beckwith Park (Night)", "Beckwith Park (Midnight)"},
                {"Champions Field", "Mannfield"},
                {"Champions Field (Dusk)", "Mannfield (Dusk)"},
                {"Champions Field (Night)", "Mannfield (Night)"},
                {"Champions Field (Snownight)", "Mannfield (Snowy)"},
                {"Champions Field (Stormy)", "Mannfield (Stormy)"},
                {"Cosmic (V4)", "Cosmic"},
                {"DFH Stadium (10a)", "DFH Stadium (10th Anniversary)"},
                {"DFH Stadium (Foggy)", "DFH Stadium (Stormy)"},
                {"DFH Stadium (Winter)", "DFH Stadium (Snowy)"},
                {"Deadeye Canyon", "Deadeye Canyon (Oasis)"},
                {"DunkHouse", "Dunk House"},
                {"Estadio Vida", "Neon Fields"},
                {"Farmstead (Grs)", "Farmstead (Pitched)"},
                {"Farmstead (Hw)", "Farmstead (Spooky)"},
                {"Farmstead (Upsidedown)", "Farmstead (The Upside Down)"},
                {"Neo Tokyo (Hax)", "Neo Tokyo (Hacked)"},
                {"Neo Tokyo (Toon)", "Neo Tokyo (Comic)"},
                {"Octagon (02)", "Octagon"},
                {"Octagon (Vent)", "Vent"},
                {"Underpass (V0)", "Underpass"},
                {"Urban Central (Spooky)", "Urban Central (Haunted)"},
                {"Wasteland", "Badlands"},
                {"Wasteland (Grs)", "Wasteland (Pitched)"},
                {"Wasteland (Night)", "Badlands (Night)"},
                {"Wasteland (S)", "Wasteland"},
            };
            return renames;
        }
    }

    std::string DisplayName(std::string_view asset) {
        if (asset.empty()) return {};
        std::string key = Lower(asset);
        if (IsPackageName(key)) key.resize(key.size() - 2);
        if (auto it = Maps().find(key); it != Maps().end()) return it->second;

        std::string readable;
        bool startWord = true;
        for (char c : key) {
            if (c == '_') {
                if (!readable.empty() && readable.back() != ' ') readable.push_back(' ');
                startWord = true;
                continue;
            }
            readable.push_back(startWord ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c);
            startWord = false;
        }
        while (!readable.empty() && readable.back() == ' ')
            readable.pop_back();
        return readable.empty() ? std::string(asset) : readable;
    }

    std::string RepairStoredName(std::string_view stored) {
        if (IsPackageName(stored)) return DisplayName(stored);
        if (auto it = LegacyRenames().find(std::string(stored)); it != LegacyRenames().end()) return it->second;
        return std::string(stored);
    }
}

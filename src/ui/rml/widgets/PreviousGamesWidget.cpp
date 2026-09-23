#include "ui/rml/RmlUiController.hpp"
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

#include "ui/rml/RmlUiHelpers.hpp"

using namespace RmlUiDetail;

std::string RmlUiController::RenderPreviousGames(bool includeHeading) {
    std::ostringstream out;
    const int configuredLimit = std::clamp(m_config.previous_games_limit, 10, kPreviousGamesMaxLimit);
    // The dashboard already labels the widget, so only the meta line is needed there.
    out << "<div class='row previous-games-header'>";
    if (includeHeading)
        out << "<div class='card-title grow'>PREVIOUS GAMES</div>";
    else
        out << "<div class='grow'></div>";
    out << "<div class='label'>last " << configuredLimit << " games</div></div>";
    if (!m_snap.recentSavedMatchesLoaded) {
        out << "<div class='muted'>Loading saved match history...</div>";
    } else if (m_snap.recentSavedMatches.empty()) {
        out << "<div class='muted'>No saved games in local history.</div>";
    } else {
        out << "<div class='match-row match-header'>"
            << "<div class='match-mode'>PLAYLIST</div><div class='match-score'>SCORE</div>"
            << "<div class='match-mmr'>MMR</div><div class='match-time'>TIME</div></div>"
            << "<div class='previous-games-list'>";
        const int limit = std::min<int>(configuredLimit, static_cast<int>(m_snap.recentSavedMatches.size()));
        for (int i = 0; i < limit; ++i) {
            const auto& match = m_snap.recentSavedMatches[static_cast<size_t>(i)];
            const std::string playlist = std::string(match.ranked ? "R " : "C ") + (match.mode.empty() ? "Unknown" : match.mode);
            out << "<div class='match-row match-data " << (match.win ? "match-win" : "match-loss")
                << (i % 2 == 0 ? " match-even" : " match-odd") << "'>"
                << "<div class='match-mode'>" << Escape(playlist) << "</div>"
                << "<div class='match-score'>" << match.ourScore << '-' << match.theirScore << "</div>"
                << "<div class='match-mmr'>" << (match.pendingTrackerConfirmation ? "***" : (match.mmrEstimated && match.mmr > 0 ? "~" + std::to_string(match.mmr) : (match.mmr > 0 ? std::to_string(match.mmr) : "--"))) << "</div>"
                << "<div class='match-time'>" << FormatClock(match.endedAtUnix) << "</div></div>";
        }
        out << "</div>";
    }
    out << "<div class='previous-games-footer'><div>Current session: <span class='win'>W:" << m_snap.sessionTotals.wins
        << "</span> <span class='loss'>L:" << m_snap.sessionTotals.losses << "</span></div>"
        << "<div class='dim'>Stored locally</div></div>";
    return out.str();
}

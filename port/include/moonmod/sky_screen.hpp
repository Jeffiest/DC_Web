#pragma once

#include <string>
#include <string_view>

#include "moonmod/title_modes.hpp"

// A mod's sky page in play (ui.sky_open), drawn and driven by src/moonmod/sky_screen_moonmod.cpp: the title mode's
// node sky (moonmod::TitleSky, the Gauntlet's Norune Eternal) over a paused dungeon, for an upgrade sky a save keeps
// (Vigil's, at a shrine). It pauses play as the map screen does. The pad works it as the title's sky page does: the
// pad moves between nodes, L2/R2 step through the constellations, Triangle shows the whole sky; Cross, Square and
// L1/R1 go to the mod as ui.sky_selected (buy, toggle, rank_down, rank_up), and Circle, Select or Start closes it
// (ui.sky_closed). The mod answers by opening the page again, which keeps the cursor where it was.
namespace dc::game {
// The page as the mod gave it: `mode` holds the heading (title), the button hint and the sky; `mode.local_id` is the
// mod's own id for it. False when another mod's page is open; the same mod's page is replaced, staying open.
bool MoonModSkyOpen(moonmod::TitleMode mode);
void MoonModSkyCloseOwner(std::string_view owner);
// A page is waiting for the dungeon to pause for it.
bool MoonModSkyWaiting();
bool MoonModSkyUp();
// The paused dungeon task's step: true once the page has closed.
bool MoonModSkyStep();
// The dungeon is paused for the page (it may have closed meanwhile; MoonModSkyStep then resumes play).
bool MoonModSkyPaused();
void MoonModSkyDraw();
void MoonModSkyReset();
// One press on the open page, as the pad's step reads it (for tests): true once it has closed.
bool MoonModSkyInput(moonmod::TitleModeInput input);
const moonmod::TitleMode *MoonModSkyPage();

// The title's sky page renderer (title/titleloop.cpp), shared with the page in play.
void MoonModDrawSkyPage(const moonmod::TitleMode &mode, int alpha);
} // namespace dc::game

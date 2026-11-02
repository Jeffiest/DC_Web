#pragma once

#include <string>
#include <string_view>
#include <vector>

// A mod's map screen (ui.map_open), drawn and driven by src/moonmod/map_screen_moonmod.cpp: a whole floor's map over
// a dimmed screen, the cells the player has seen, a mod's marks on it (shrines, a waypoint) and a cursor. In a dungeon
// it pauses play as the choice panel does; in a town it holds the town still. The map is the floor the player is on
// (live), or one the mod kept (dungeons.map_snapshot), so a town can show a dungeon's.
namespace dc::game {
class CDungeonMap;

// Each cell of a floor: 0 nothing; else 32, plus 16 for a room's floor (or a stair), plus the sides a player can walk
// out of it by (1 north, 2 east, 4 west, 8 south).
void DngMapCellShapes(const CDungeonMap &map, unsigned char *out);

enum MoonModMapMarkKind {
    MOONMOD_MAP_SHRINE = 0,   // a shrine not lit yet
    MOONMOD_MAP_SHRINE_LIT,   // a lit shrine
    MOONMOD_MAP_WAYPOINT,     // the player's waypoint
    MOONMOD_MAP_GATE,         // a zone gate
    MOONMOD_MAP_STAIR,        // the way down or out
    MOONMOD_MAP_MARK,         // anything else
};
struct MoonModMapMark {
    int         x = 0, y = 0;
    int         kind = MOONMOD_MAP_MARK;
    std::string label;
    bool        pick = false; // Cross can take it, and L1/R1 step between such marks
};
struct MoonModMapView {
    std::string                owner;
    std::string                id;
    std::string                title;
    std::string                subtitle;
    bool                       live = true;         // the floor the player is on
    std::vector<unsigned char> shapes;              // otherwise: DngMapCellShapes of the kept floor
    std::vector<unsigned char> seen;                // and its cells the player has seen (1)
    std::vector<MoonModMapMark> marks;
    int                        cursor_x = -1, cursor_y = -1;
    bool                       waypoints = false;   // Triangle puts the waypoint (one) under the cursor, or takes it away
    std::string                hint;                // what Cross does ("Travel")
    bool                       pages = false;       // L2/R2 ask the mod for another map (ui.map_page)
};
// False when another mod's map is open; the same mod's map is replaced (another page), staying open.
bool MoonModMapOpen(MoonModMapView view);
void MoonModMapCloseOwner(std::string_view owner);
// A map is waiting for the dungeon to pause for it.
bool MoonModMapWaiting();
bool MoonModMapUp();
// The paused dungeon task's step: true once the map has closed.
bool MoonModMapStep();
// The dungeon is paused for the map (it may have closed meanwhile; MoonModMapStep then resumes play).
bool MoonModMapPaused();
// A town's step, each frame of its walking loop: reads the pad for the map while one is open and holds the town's own
// controls meanwhile.
void MoonModMapStepTown(bool walking);
void MoonModMapDraw();
void MoonModMapReset();
} // namespace dc::game

namespace moonmod {
// The player's use of the map, to its owner: ui.map_choice (Cross on a mark), ui.map_waypoint (Triangle; set false
// when taken away), ui.map_page (L2/R2) and ui.map_closed. `mark` counts from 1 (0: none).
void ChronicleMapChoice(const std::string &owner, const std::string &id, int x, int y, int mark);
void ChronicleMapWaypoint(const std::string &owner, const std::string &id, int x, int y, bool set);
void ChronicleMapClosed(const std::string &owner, const std::string &id);
void ChronicleMapPage(const std::string &owner, const std::string &id, int way);
} // namespace moonmod

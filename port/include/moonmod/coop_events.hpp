#pragma once

// Co-op's shared doors (dungeons.shared_events_get / shared_event_run), from src/moonmod/coop_events_moonmod.cpp. When
// a player's system script changes the floor (a door or gate opens, a switch turns: an object moved, an event switched)
// without leaving it, the script is queued for the mod to send to the other players on that floor, whose games run the
// same script at the same place once they are free to (not in a menu, a fight's knockback or another script). A
// script run that way is never sent on again, and the key it asks for is taken as given: the door opens for everyone.
#include <vector>

namespace dc::game {

struct MoonModSharedEvent {
    int   script = -1;
    int   mode = 0; // how it was started: 0 by itself, 1 the action button, 2 the hold button
    int   ext = 0;  // ext_memory
    float pos[3] = {}, dir[3] = {};
};

// From the system script ops: 1 for one that changes the floor (an object placed, an event switched), 2 for one that
// leaves it or changes the party (a new floor, the dungeon's exit, the character), which is never shared.
void MoonModCoopScriptOp(int kind);
void MoonModCoopEventsPoll();         // each dungeon frame
// The script about to start is one every game runs by itself (a room cleared of monsters, a monster's death): not sent.
void MoonModCoopScriptNotShared();
std::vector<MoonModSharedEvent> MoonModCoopTakeEvents();
bool MoonModCoopRunEvent(const MoonModSharedEvent &event); // false when one is already waiting
// From the script's item window: true when a shared script asked, which takes the first key it offers.
bool MoonModCoopAutoItemSelect();

} // namespace dc::game

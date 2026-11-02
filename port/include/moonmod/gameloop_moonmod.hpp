#pragma once

// MoonMod input requests the dungeon loop serves (defined in port/src/dun/movechara.cpp).
// ps2/include/dun/gameloop.hpp includes this under PORT.

/** Queue the legacy first-person view transition from a MoonFactory input action. */
void MoonModRequestFirstPersonToggle();
/** Queue a native action-button press for the selected dungeon quick item. */
void MoonModRequestQuickItemUse();

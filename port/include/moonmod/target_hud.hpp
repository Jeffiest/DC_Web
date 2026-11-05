#pragma once

#include "common.h"

// The locked target's bar (hud.target_bar_set, hud.target_meter_set), drawn by src/moonmod/target_hud.cpp in place of
// the game's thin gauge under the target's name: a wider HP bar with a trail that shows what a blow took, and under it
// a meter a mod fills for that monster (Vigil's poise).
namespace dc::game {
// Whether the bar replaces the game's gauge.
void MoonModTargetBarSet(bool on);
bool MoonModTargetBarOn();

// The meter for the monster in slot: fill 0..1 in rgb (0xRRGGBB), gone after seconds without another set (0 keeps it).
// A full meter flashes. The adapter clears a slot's meter when another monster takes the slot.
void MoonModTargetMeterSet(int slot, float fill, u32 rgb, float seconds);
void MoonModTargetMeterClear(int slot);
void MoonModTargetMetersClear();

// Draws the bar for the locked slot from EnemyLifeGage (its place and life); false when it is off, so the game's
// gauge draws.
bool MoonModTargetBarDraw(int slot);
} // namespace dc::game

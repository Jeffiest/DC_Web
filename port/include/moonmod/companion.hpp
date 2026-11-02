#pragma once

// The Gauntlet's companion (party.companion_set), drawn and driven by src/moonmod/companion_moonmod.cpp.
namespace dc::game {
// The character to bring (-1 none; any playable one, a registered character too), the weapon in its hand, its stance
// (0 passive, 1 lock-on target, 2 aggressive) and a damage multiplier on its weapon's base attack. A shot cell (a
// basefx01 cell, 0-15) and size make it fight from range with that sprite; -1 keeps the character's own way.
void MoonModCompanionRequest(int chara, int weapon_item, int stance, float damage_scale, int shot_cell = -1,
                             float shot_size = 1.0f);
int  MoonModCompanionLoaded();
void MoonModCompanionStep();
void MoonModCompanionDraw(bool shadow);
void MoonModCompanionDrawShots();
} // namespace dc::game

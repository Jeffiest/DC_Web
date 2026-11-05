#pragma once

// The Gauntlet's companion (party.companion_set), drawn and driven by src/moonmod/companion_moonmod.cpp.
namespace dc::game {
// The character to bring (-1 none; any playable one, a registered character too), the weapon in its hand, its stance
// (0 passive, 1 lock-on target, 2 aggressive) and a damage multiplier on its weapon's base attack. A shot cell (a
// basefx01 cell, 0-15) and size make it fight from range with that sprite; -1 keeps the character's own way.
void MoonModCompanionRequest(int chara, int weapon_item, int stance, float damage_scale, int shot_cell = -1,
                             float shot_size = 1.0f);
// Hostile (party.companion_set hostile, an invader): it hunts the player instead of following. Its blows are native
// hits on the player (so a guard, a dodge's invincibility and poise all take them), the player's own weapon hits hurt
// it, and at no life left it falls and goes. hp 0 makes it a helper again; tint is the ambient it draws in (0..128,
// -1 for none).
void MoonModCompanionHostile(int hp, int damage, int defense, const float *tint);
struct MoonModCompanionLife {
    bool hostile = false;
    int  hp = 0, max_hp = 0;
};
MoonModCompanionLife MoonModCompanionLifeGet();
// Where a hostile companion (an invader) stands, while it's up to be locked on to (not falling); false otherwise.
bool MoonModCompanionInvaderAt(float *out);
int  MoonModCompanionLoaded();
void MoonModCompanionStep();
void MoonModCompanionDraw(bool shadow);
void MoonModCompanionDrawShots();
} // namespace dc::game

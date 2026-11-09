#pragma once

// Co-op's remote players (world.avatar_set / avatar_clear), drawn and driven by src/moonmod/coop_avatars_moonmod.cpp:
// up to three other players on the dungeon floor or walking in the town, each a hero of its own with the weapon in its hand. A mod sets
// where each one is as the network reports it, a few times a second; between reports it eases there, turns the
// shortest way and plays the motion it was told. Each has its own model and texture arenas, taken only while it is up.
#include <string>

namespace dc::game {

inline constexpr int kMoonModAvatars = 3;

struct MoonModAvatarState {
    int   character = 0;  // any playable character (a registered one too)
    int   weapon = -1;    // the weapon item in its hand, or -1
    float x = 0.0f, y = 0.0f, z = 0.0f;
    float heading = 0.0f; // radians, as the player's frame turns
    int   motion = 0;
    float tint[3] = {-1.0f, -1.0f, -1.0f}; // the ambient it draws in (0..128), or -1 for the scene's own
    std::string name;                        // shown over its head, or nothing
    bool  down = false;   // knocked out (co-op's down, not out): lies on its motion's last frame; monsters pass it by
};

// slot 0..kMoonModAvatars-1. A new character or weapon loads on the next dungeon frame; a far jump (a new floor, a
// warp) places it at once.
bool MoonModAvatarSet(int slot, const MoonModAvatarState &state);
void MoonModAvatarClear(int slot); // -1 clears every one
bool MoonModAvatarShown(int slot);
// Where a shown avatar stands on this floor now (co-op's monsters see it as a player); false when it isn't here.
bool MoonModAvatarAt(int slot, float out[3]);
void MoonModAvatarsStep();
void MoonModAvatarsDraw(bool shadow);
void MoonModAvatarsDrawNames(); // in the HUD pass: each name over its hero, fading with distance

} // namespace dc::game

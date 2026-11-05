#pragma once

#include <array>
#include <vector>

// The death ghost (world.ghost_get / world.ghost_play), drawn and driven by src/moonmod/ghost_moonmod.cpp: the player's
// last seconds on a dungeon floor are kept, and a mod can play a kept stretch back on a pale, see-through copy of a hero.
namespace dc::game {
// One kept frame: x, y, z, the heading in radians, and the motion number the hero played.
using MoonModGhostFrame = std::array<float, 5>;
constexpr int kMoonModGhostFrames = 150; // every other frame: five seconds

// The kept frames, oldest first, and the character they were the player's of (-1 when none).
std::vector<MoonModGhostFrame> MoonModGhostRecorded(int &character);
// Play these frames back on the character (-1 stops). False when the frames are empty or the character unknown.
bool MoonModGhostPlay(int character, const std::vector<MoonModGhostFrame> &frames);
bool MoonModGhostPlaying();
void MoonModGhostStep();
void MoonModGhostDraw(bool shadow);
} // namespace dc::game

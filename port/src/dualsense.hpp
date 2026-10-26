#pragma once

#include <cstdint>

// The lightbar colour for a party member's life: green when whole, through yellow to red when low.
// Pure, so a test can read it.
struct LightbarColour {
    std::uint8_t red = 0;
    std::uint8_t green = 0;
    std::uint8_t blue = 0;

    bool operator==(const LightbarColour &) const = default;
};

LightbarColour LightbarForLife(int life, int max_life);

// The colour a pad rests at where there is no party member's life to show: the title, the towns.
LightbarColour LightbarIdle();

// Once a tick, from the pad read: sets pad 0's lightbar to the active character's life in a dungeon.
void DualSenseUpdate();

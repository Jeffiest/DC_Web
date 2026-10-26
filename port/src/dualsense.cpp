#include "dualsense.hpp"

#include <algorithm>

#include "dun/gameloop.hpp"
#include "main.hpp"
#include "platform/input.hpp"
#include "userstatus.hpp"

extern s32 mode;

LightbarColour LightbarForLife(int life, int max_life) {
    if (max_life <= 0) {
        return LightbarIdle();
    }
    float fraction = std::clamp(static_cast<float>(life) / static_cast<float>(max_life), 0.0f, 1.0f);
    // Red to yellow over the lower half, yellow to green over the upper.
    float red = fraction < 0.5f ? 255.0f : 255.0f * (1.0f - fraction) * 2.0f;
    float green = fraction < 0.5f ? 255.0f * fraction * 2.0f : 255.0f;
    return {static_cast<std::uint8_t>(red), static_cast<std::uint8_t>(green), 0};
}

LightbarColour LightbarIdle() { return {0, 48, 160}; }

void DualSenseUpdate() {
    LightbarColour colour = LightbarIdle();
    if (mode == GAME_MODE_DUNGEON && UserStatus != nullptr) {
        int who = UserStatus->cur_chara;
        if (who >= 0 && who < 6) {
            colour = LightbarForLife(UserStatus->hp[who], UserStatus->max_hp[who]);
        }
    }
    InputSetLightbar(0, colour.red, colour.green, colour.blue);
}

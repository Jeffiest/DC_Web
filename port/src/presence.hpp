#pragma once

#include "platform/discord.hpp"

struct PresenceState {
    int  mode = -1;
    int  map_no = -1;
    int  edit_mode = -1;
    int  dungeon = -1;
    int  floor = -1;
    bool back_floor = false;
    bool choosing_floor = false;
    int  chara = 0;
};

DiscordActivity PresenceDescribe(const PresenceState &state);

void PresenceTick();

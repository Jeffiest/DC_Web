// MoonMod town villagers that used to live in ps2/src/editloop3.cpp: the registry, and the hooks
// EdSelectVillager and EdInitVilagerPosition call to fill free villager slots and place them.

#include "common.h"

#include <libvu0.h>

#include <cstdio>
#include <cstring>

#include "editground.hpp"
#include "editloop.hpp"
#include "editloop3.hpp"
#include "main.hpp"
#include "npcharacter.hpp"

namespace dc::game {

namespace {

struct ChronicleModVillager {
    int   town_id;
    char  name[32];
    float position[4];
    float rotation_y;
    int   talk_event;
    int   slot;
};

ChronicleModVillager g_mod_villagers[CHRONICLE_MOD_VILLAGER_COUNT];
int                  g_mod_villager_slots[10] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

} // namespace

bool SetChronicleModVillager(int index, int town_id, const char *name, const float *position, float rotation_y,
                             int talk_event) {
    if (index < 0 || index >= CHRONICLE_MOD_VILLAGER_COUNT || name == NULL || std::strlen(name) >= 32) {
        return false;
    }
    ChronicleModVillager &villager = g_mod_villagers[index];
    std::memset(&villager, 0, sizeof(villager));
    villager.slot = -1;
    if (name[0] == '\0') {
        return true;
    }
    if (position == NULL || town_id < 0 || talk_event < 0) {
        return false;
    }
    villager.town_id = town_id;
    std::strcpy(villager.name, name);
    std::memcpy(villager.position, position, sizeof(float) * 3);
    villager.position[3] = 1.0f;
    villager.rotation_y = rotation_y;
    villager.talk_event = talk_event;
    return true;
}

int ChronicleModVillagerInSlot(int slot) {
    return slot >= 0 && slot < 10 ? g_mod_villager_slots[slot] : -1;
}

int ChronicleModVillagerSlot(int index) {
    return index >= 0 && index < CHRONICLE_MOD_VILLAGER_COUNT && g_mod_villagers[index].name[0] != '\0'
               ? g_mod_villagers[index].slot
               : -1;
}

int ChronicleModVillagerTalkEvent(int index) {
    return index >= 0 && index < CHRONICLE_MOD_VILLAGER_COUNT ? g_mod_villagers[index].talk_event : 0;
}

void ResetChronicleModVillagersForTests() {
    std::memset(g_mod_villagers, 0, sizeof(g_mod_villagers));
    for (int &slot : g_mod_villager_slots) slot = -1;
}

void ChronicleAddModVillagers(VILLAGER_INFO *villagers, VILLAGER_INFO *restore) {
    for (int &slot : g_mod_villager_slots) slot = -1;
    for (int index = 0; index < CHRONICLE_MOD_VILLAGER_COUNT; index++) {
        ChronicleModVillager &mod = g_mod_villagers[index];
        mod.slot = -1;
        if (mod.name[0] == '\0' || mod.town_id != NowEditMap || MapNo > 10) {
            continue;
        }
        int slot = 0;
        while (slot < 10 && villagers[slot].name[0] != '\0') {
            slot++;
        }
        if (slot >= 10) {
            break;
        }
        VILLAGER_INFO &info = villagers[slot];
        std::memset(&info, 0, sizeof(info));
        std::strcpy(info.name, mod.name);
        info.index = -1;
        info.character_no = -1;
        info.talk_event_no = mod.talk_event;
        info.talk_event_level = 0; // only on the talk button, never on approach
        std::memcpy(info.position, mod.position, sizeof(info.position));
        info.rotation[1] = mod.rotation_y;
        if (restore != NULL) {
            restore[slot] = info;
        }
        mod.slot = slot;
        g_mod_villager_slots[slot] = index;
        if (slot + 1 < 10) {
            villagers[slot + 1].name[0] = '\0';
        }
    }
}

void ChroniclePlaceModVillagers(CNPCharacter *characters, VILLAGER_INFO *villagers) {
    for (int index = 0; index < CHRONICLE_MOD_VILLAGER_COUNT; index++) {
        const ChronicleModVillager &mod = g_mod_villagers[index];
        if (mod.slot < 0 || mod.slot >= 10 || std::strcmp(villagers[mod.slot].name, mod.name) != 0) {
            continue;
        }
        VILLAGER_INFO &info = villagers[mod.slot];
        CNPCharacter  &character = characters[mod.slot];
        info.placed = true;
        info.initial_motion = 0;
        std::memcpy(info.position, mod.position, sizeof(info.position));
        info.rotation[0] = 0.0f;
        info.rotation[1] = mod.rotation_y;
        info.rotation[2] = 0.0f;
        character.ClearSeq();
        character.event_status = 0;
        character.villager_id = -1;
        character.map_parts_no = -1;
        sceVu0FVECTOR position = {mod.position[0], mod.position[1], mod.position[2], 1.0f};
        character.SetPosition(position);
        character.SetRotation(0.0f, mod.rotation_y, 0.0f);
        character.draw_enabled = character.initialized;
        character.SetMotion(0, 0);
        std::printf("moonmod: villager %s stands in slot %d (%s)\n", mod.name, mod.slot,
                    character.initialized ? "loaded" : "model not loaded");
    }
}

} // namespace dc::game

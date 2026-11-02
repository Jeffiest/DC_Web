#pragma once

// MoonMod town villagers (port/src/moonmod/editloop3_moonmod.cpp). ps2/include/editloop.hpp
// includes this under PORT, after VILLAGER_INFO.

class CNPCharacter;
struct VILLAGER_INFO;

/* Port-only: villagers a mod adds to a town (characters.register kind = "npc"). Retail fills at most 6 of the 10
   villager slots, so up to 4 more stand in the free ones, at a fixed place, whenever the town picks its villagers.
   name is a model in the town's chara folder (gedit/<town>/chara/<name>.chr; a mod may add a new one). A talk_event
   above 0 runs that native event; 0 publishes game.npc.talk instead. An empty name clears the entry. */
constexpr int CHRONICLE_MOD_VILLAGER_COUNT = 4;
bool SetChronicleModVillager(int index, int town_id, const char *name, const float *position, float rotation_y,
                             int talk_event);
/* The mod villager entry standing in villager slot `slot` (0-9), or -1. */
int ChronicleModVillagerInSlot(int slot);
/* The villager slot mod villager entry `index` stands in, or -1 while it is not in the town. */
int ChronicleModVillagerSlot(int index);
int ChronicleModVillagerTalkEvent(int index);
void ResetChronicleModVillagersForTests();
/* editloop3.cpp hooks: fill free slots after the native pick, and put them in place after the native placement. */
void ChronicleAddModVillagers(VILLAGER_INFO *villagers, VILLAGER_INFO *restore);
void ChroniclePlaceModVillagers(CNPCharacter *characters, VILLAGER_INFO *villagers);

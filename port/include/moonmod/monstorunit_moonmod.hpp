#pragma once

// MoonMod additions to the monster unit (port/src/moonmod/monstorunit_moonmod.cpp).
// ps2/include/monstorunit.hpp includes this under PORT, after MONSTOR_MODEL.

class CDungeonMap;

/** Monsters awake at once; retail 4. */
extern int MonstorActiveLimit;

/** Resolves a retail or registered MoonFactory monster model definition. */
MONSTOR_MODEL *GetMonstorTable(int model_no);

/** Registers a variant that inherits a retail definition while optionally selecting other model and script assets. */
bool RegisterMoonModMonstorVariant(int template_model_no, int hp, int defense, int reward_exp, int reward_money,
                                   const char *model_name, const char *script_name, int *out_model_no);
bool UnregisterMoonModMonstorVariant(int model_no);
void ResetMoonModMonstorVariants();

/** The monster count ArrangementPos places on a random floor once a mod's population settings apply. */
int MoonModArrangementCount(CDungeonMap *map, int count, int model_no);

/** dungeons.set_zones: each locked zone gate's key goes to a monster in the zone before it (a chest there, failing one). */
// dungeons.set_zones source_floor: on the main floor built with zones of other floors, loads the monster models of every
// zone's floor (each once, up to the model table's size) and notes the zones each may stand in; false (nothing loaded)
// on any other floor.
bool MoonModLoadZoneMonsters(CMonstorUnit *unit, BT_ENEMY_FLOOR *floors, int cur_floor, CDataAlloc2<1> *alloc);
// Whether a monster of loaded model `model` may be placed at `position` (its zone's floor has it).
bool MoonModZoneModelAllowed(CDungeonMap *map, const float *position, int model);
void MoonModZoneKeyCarriers(CMonstorUnit *unit, CDungeonMap *map);

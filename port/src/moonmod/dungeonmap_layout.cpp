// The floor layout a mod asks for (moonmod/dungeonmap_layout.hpp) and the chest count it gives
// buildEventData.
#include "dungeonmap.hpp"

#include <libvu0.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>

#include "btitem.hpp"
#include "dun/gameloop.hpp"
#include "itemdata.hpp"
#include "mathutil.hpp"
#include "userstatus.hpp"

namespace dc::game {

// Floor layout a mod asked for (moonmod/dungeonmap_layout.hpp); 0 keeps the retail value.
int DngRoomBonusPercent = 0, DngMonsterBonusPercent = 0, DngChestBonusPercent = 0, DngChestValuePercent = 0;
double DngMonstersPerRoomMin = 0.0, DngMonstersPerRoomMax = 0.0;
double DngChestsPerRoomMin = 0.0, DngChestsPerRoomMax = 0.0;
int DngEnemySurvivalChance = 0;
int DngMiniMapTile=16, DngMiniMapOriginX=380, DngMiniMapOriginY=64;
int DngMiniMapClipX0 = 0, DngMiniMapClipY0 = 0, DngMiniMapClipX1 = -1, DngMiniMapClipY1 = -1;
int DngWaypointX = -1, DngWaypointY = -1, DngWaypointSeed = -1;
int DngLayoutRooms = 0;
int DngNextSeed = -1;
int DngLayoutArea = 0;
int DngLayoutRoomMin = 0;
int DngLayoutRoomMax = 0;
int DngExitRoomEnabled = 0;
int DngExitRoomValid = 0;
int DngExitRoomX = 0;
int DngExitRoomY = 0;
int DngShortcutDoors = 0;
int DngShortcutOpenKeys = 0;
int DngIllusoryWalls = 0;
int DngIllusoryOpen = 0;
int DngZoneCount = 0;
int DngZoneRooms[DNG_ZONE_MAX] = {};
int DngZoneOpenMask = 0;
int DngZoneSmokeMask = 0;
int DngZoneFloorCount = 0;
int DngZoneFloorSeed = -1;
DNG_ZONE_GATE DngZoneGateTable[DNG_ZONE_MAX - 1];
int DngZoneGateCount = 0;
unsigned char DngZoneOfCell[DNG_CELLS];
int DngZoneShrines = 0;
int DngZoneDescent = 0;
int DngHeightSeed = -1;
float DngCellY0[DNG_CELLS] = {};
float DngCellY1[DNG_CELLS] = {};
signed char DngCellRamp[DNG_CELLS] = {};
int DngZoneShrineHeal = 1;
int DngZoneSanctuary = 0;
DngNpcMark DngNpcMarks[DNG_NPC_MARK_MAX] = {};
int        DngNpcMarkCount = 0;
int        DngNpcMarkSeed = 0;
DngNpcModel DngNpcModels[DNG_NPC_MODEL_MAX] = {};
int         DngNpcModelCount = 0;
bool DngZoneFloorIs(int map_seed) { return DngZoneFloorCount > 0 && DngZoneFloorSeed == map_seed; }
int DngZoneSourceFloor[DNG_ZONE_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
int DngZoneFloorSource[DNG_ZONE_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
int DngZoneModelMask[DNG_ZONE_MODEL_MAX] = {};
int DngZoneQuietMask = 0, DngZoneHealMask = 0, DngZoneArenaMask = 0;
int DngZoneFloorQuietMask = 0, DngZoneFloorHealMask = 0;
int DngZoneVaults = 1;

int DngPutSpotLimit() { return DngZoneCount > 0 || DngZoneFloorCount > 0 ? 1 << 16 : 196; }
int DngZoneExitFloor = -1;
int DngZoneFloorExit = -1;

bool DngZoneSourcesShown(int map_seed) {
    if (DngZoneFloorCount <= 0 || DngZoneFloorSeed != map_seed) return false;
    for (int z = 0; z < DngZoneFloorCount; ++z) {
        if (DngZoneFloorSource[z] >= 0) return true;
    }
    return false;
}

int DngZoneSourceFloorAt(int map_seed, const float *pos, int own_floor) {
    if (!DngZoneSourcesShown(map_seed)) return own_floor;
    const int col = static_cast<int>((pos[0] + 80.0f) / 160.0f), row = static_cast<int>((pos[2] + 80.0f) / 160.0f);
    if (col < 0 || row < 0 || col >= DNG_GRID || row >= DNG_GRID) return own_floor;
    const int zone = DngZoneOfCell[col + row * DNG_GRID];
    if (zone == DNG_ZONE_NONE || zone >= DNG_ZONE_MAX) return own_floor;
    // a zone of another dungeon (source_dungeon): its source_floor is that dungeon's, for its monsters only
    if (DngZoneFloorDungeon[zone] >= 0) return own_floor;
    return DngZoneFloorSource[zone] >= 0 ? DngZoneFloorSource[zone] : own_floor;
}
int DngZoneShrineX[DNG_ZONE_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
int DngZoneShrineY[DNG_ZONE_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};

int DngZoneGateKey(int zone) {
    // A map of more than eight zones runs out of keys: from the eighth gate on the six keys come round again (a key
    // opens the nearest gate it fits, and the one it opened came first).
    if (zone > 7) zone = 1 + (zone - 1) % 6;
    int key = ITEM_SILVER_KEY + 1;
    for (int z = 1; z <= zone; ++z) {
        --key;
        if (key == ITEM_BONE_KEY + selectMapNo) --key; // pass over the key the shortcut doors skip
    }
    return key;
}
DNG_SHORTCUT_DOOR DngShortcutDoorTable[DNG_SHORTCUT_MAX];
int DngShortcutDoorCount = 0;
int DngShortcutDoorSeed = -1;
int DngWardenRoomEnabled = 0, DngWardenRoomLockIn = 0, DngWardenRoomState = 0, DngWardenRoomSeed = -1;
int DngWardenRoomX = 0, DngWardenRoomY = 0, DngWardenRoomW = 0, DngWardenRoomH = 0;
DNG_WARDEN_DOOR DngWardenDoorTable[DNG_WARDEN_DOORS];
int DngWardenDoorCount = 0;
DNG_SEAL_DOOR DngSealDoorTable[DNG_SEAL_DOORS];
int DngSealDoorCount = 0;
DNG_SEAL_ROOM DngSealRoomTable[DNG_ROOM_MAX];
int DngSealRoomCount = 0;
int DngSealRoomSeed = -1;
int DngRestRoomEnabled = 0, DngRestRoomSeed = -1, DngRestSpringX = 0, DngRestSpringY = 0, DngRestStairsX = 0, DngRestStairsY = 0;
int DngRestHallX = 0, DngRestHallY = 0, DngRestHallW = 0, DngRestHallH = 0;
int DngCharaBlockMask = -1;

int DngWeaponUnique = 0, DngWeaponRarePercent = 0, DngLargeChestPercent = -1;

namespace {
bool PartyHolds(int item_no) {
    if (UserStatus == nullptr) return false;
    for (int chara = 0; chara < CharaCount(); chara++) {
        for (const WEAPON_HAVE &weapon : UserStatus->chara_weapons[chara]) {
            if (weapon.item_no == item_no) return true;
        }
    }
    return false;
}
} // namespace

int DngChestWeapon(int item_no, const int *placed, int placed_count) {
    if (item_no < ITEM_WEAPON_START || item_no > ITEM_WEAPON_END) return item_no;
    if (DngWeaponUnique == 0 && DngWeaponRarePercent <= 0) return item_no;
    const WEAPON_DATA *rolled = GetWeaponData(item_no);
    if (rolled == nullptr) return item_no;
    const auto taken = [&](int item) {
        if (DngWeaponUnique == 0) return false;
        if (PartyHolds(item)) return true;
        for (int i = 0; i < placed_count; ++i) {
            if (placed[i] == item) return true;
        }
        return false;
    };
    const bool rare = DngWeaponRarePercent > 0 && static_cast<int>(100.0f * static_cast<float>(rand()) / 2147483648.0f) < DngWeaponRarePercent;
    if (!rare && !taken(item_no)) return item_no;
    // The same character's weapons: for a rare one, those stronger than the roll (up to about half again its attack),
    // one at random; otherwise the nearest in strength, a stronger one before a weaker.
    int stronger[64];
    int stronger_count = 0;
    int nearest = -1, nearest_gap = 0x7FFF;
    const int ceiling = rolled->attack + std::max(6, rolled->attack / 2);
    for (int item = ITEM_WEAPON_START; item <= ITEM_WEAPON_END; ++item) {
        const WEAPON_DATA *data = GetWeaponData(item);
        if (item == item_no || data == nullptr || data->owner != rolled->owner || data->attack <= 0 || taken(item) || !ItemModelAvailable(item)) {
            continue; // unused slots (326, 330...) have stats but no model; a chest showing one crashed
        }
        if (data->attack > rolled->attack && data->attack <= ceiling && stronger_count < 64) stronger[stronger_count++] = item;
        const int gap = data->attack >= rolled->attack ? data->attack - rolled->attack : (rolled->attack - data->attack) * 2;
        if (gap < nearest_gap) {
            nearest_gap = gap;
            nearest = item;
        }
    }
    if (rare && stronger_count > 0) {
        return stronger[std::clamp(static_cast<int>(static_cast<float>(stronger_count) * static_cast<float>(rand()) / 2147483648.0f), 0,
                                   stronger_count - 1)];
    }
    if (!taken(item_no)) return item_no;
    return nearest;
}

void DngShortcutDoorTint(int key_item, float *rgb) {
    // Bone, Moustache, Shipcabin, Stone, Handle, Pitchdark, Silver
    static const float kTint[DNG_SHORTCUT_MAX][3] = {{255, 240, 170}, {255, 140, 40}, {40, 200, 215}, {150, 165, 200},
                                                     {255, 210, 40},  {160, 60, 230}, {215, 230, 255}};
    const int k = std::clamp(key_item - ITEM_BONE_KEY, 0, DNG_SHORTCUT_MAX - 1);
    for (int c = 0; c < 3; ++c) rgb[c] = kTint[k][c];
}

// buildEventData's chest count: the retail 4-7 target, raised by a mod's chest bonus and per-room
// density, and capped below the box table so mimics, gate keys and event chests keep their slots.
int DngChestTarget(CDungeonMap *map, int retail_target) {
    int chest_target = retail_target;
    // A map whose zones are several floors (dungeons.set_zones source_floor) gets each floor's chests.
    if (DngZoneFloorCount > 1 && DngZoneSourcesShown(map->map_seed)) chest_target *= DngZoneFloorCount;
    // Reserve shared event capacity for mimics, gate keys and event chests.
    chest_target = std::clamp((chest_target * (100 + DngChestBonusPercent) + 99) / 100, 0, DNG_REGULAR_CHEST_MAX);
    // A floor built in zones (dungeons.set_zones) is its mod's: another mod's room density doesn't apply to it.
    const bool zoned = DngZoneFloorCount > 0 && DngZoneFloorSeed == map->map_seed;
    if (map->room_num > 0 && DngChestsPerRoomMax > 0.0 && !zoned) {
        // Keep the room-density roll stable for this floor seed, independently
        // from monster placement and other random generation decisions.
        const u32 seed = static_cast<u32>(map->map_seed);
        const u32 mixed = seed * 277803737u + 1013904223u;
        const double roll = static_cast<double>((mixed >> 8) & 0x00ffffffu) / 16777215.0;
        const double per_room = DngChestsPerRoomMin + (DngChestsPerRoomMax - DngChestsPerRoomMin) * roll;
        // Match the native chest bonus: scale the room-derived target once,
        // then keep whichever is higher between it and the modified baseline.
        const double bonus_scale = (100.0 + DngChestBonusPercent) / 100.0;
        const int room_target = static_cast<int>(std::ceil(map->room_num * per_room * bonus_scale));
        // Retain the unmodified random 4-7 chest target, then apply the
        // deterministic room-density target without lowering native content.
        chest_target = std::min(DNG_REGULAR_CHEST_MAX, std::max(chest_target, room_target));
    }
    return chest_target;
}

} // namespace dc::game

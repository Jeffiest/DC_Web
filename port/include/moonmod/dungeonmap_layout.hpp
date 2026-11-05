#pragma once

// PC-only floor layout knobs a mod sets (entities/dungeons API). ps2/include/dungeonmap.hpp includes
// this under PORT, after its capacity macros (DNG_GRID, DNG_ROOM_MAX, DNG_BOX_MAX, ...).

#define DNG_REGULAR_CHEST_MAX (DNG_BOX_MAX - 16)

/* Floor layout a mod asked for (entities/dungeons API); 0 keeps the retail value. */
extern int DngRoomBonusPercent, DngMonsterBonusPercent, DngChestBonusPercent, DngChestValuePercent;
/* Per-floor entity targets derived from the realized room count. Zero disables room scaling. */
extern double DngMonstersPerRoomMin, DngMonstersPerRoomMax;
extern double DngChestsPerRoomMin, DngChestsPerRoomMax;
extern int DngEnemySurvivalChance;
extern int DngLayoutRooms;   /**< Rooms to try to place, 2..DNG_ROOM_MAX. */
extern int DngNextSeed;      /**< dungeons.set_seed: the seed the next new floor is built from, once; -1 picks one. */
/* Shortcut doors (dungeons.set_shortcuts): a dead-end room gets a second way out that loops back to a far part of
 * the floor, locked from the far side; the key waits in a small chest in the dead end. Up to 7, one per key item
 * (Bone Key .. Silver Key), each door tinted the colour of its key. */
#define DNG_SHORTCUT_MAX 7
extern int DngShortcutDoors; /**< Shortcuts a mod asked for on each floor, 0..DNG_SHORTCUT_MAX. */
extern int DngShortcutOpenKeys; /**< dungeons.set_shortcuts open: bit k set builds the door Bone Key + k opens already open. */
/* Illusory walls (dungeons.set_shortcuts illusory): more shortcut doorways, each drawn as the room's plain wall with no
 * key (key_item 0 in the table). A blow struck at one opens it (ChronicleWallStruck). They share the 7 doors. */
extern int DngIllusoryWalls;     /**< Illusory walls a mod asked for on each floor, 0..3. */
extern int DngIllusoryOpen;      /**< set_shortcuts open_walls: bit n set builds the floor's illusory wall n already open. */
struct DNG_SHORTCUT_DOOR {
    int x, y;           /**< The door's cell. */
    int key_item;       /**< The key item that opens it; 0 for an illusory wall, which a blow opens. */
    int gate_part;      /**< The locked gate part the cell shows. */
    int open_part;      /**< The doorway part the cell had, put back when it opens. */
    int open_direction; /**< That doorway's direction. */
    int locked;         /**< 1 until the key opens it. */
};
extern DNG_SHORTCUT_DOOR DngShortcutDoorTable[DNG_SHORTCUT_MAX];
/* The Warden room (dungeons.set_warden_room): on a floor built while it is on, the room behind the floor's exit gate
 * has its other doorways closed by gates. They open once every monster outside is dead (DNG_WARDEN_OPEN); with
 * lock_in they close behind the player who walks in (DNG_WARDEN_SHUT) until the mod releases them. */
#define DNG_WARDEN_DOORS 12
enum { DNG_WARDEN_NONE = 0, DNG_WARDEN_SEALED = 1, DNG_WARDEN_OPEN = 2, DNG_WARDEN_SHUT = 3, DNG_WARDEN_RELEASED = 4 };
struct DNG_WARDEN_DOOR {
    int x, y;           /**< The doorway's cell. */
    int gate_part;      /**< The gate that closes it. */
    int open_part;      /**< The doorway part, put back when it opens. */
    int open_direction; /**< That doorway's direction. */
};
extern int DngWardenRoomEnabled;  /**< Seal the Warden room on floors built from now on. */
extern int DngWardenRoomLockIn;   /**< Close the doors behind the player who walks in. */
extern int DngWardenRoomState;    /**< DNG_WARDEN_* for the floor last built. */
extern int DngWardenRoomSeed;     /**< The map seed it was built for. */
extern int DngWardenRoomX, DngWardenRoomY, DngWardenRoomW, DngWardenRoomH; /**< The room, in cells. */
extern DNG_WARDEN_DOOR DngWardenDoorTable[DNG_WARDEN_DOORS];
extern int DngWardenDoorCount;
/* Rooms that shut (dungeons.room_seal): every room's doorways and the gate that would close each, recorded as a floor
 * is built, so a mod can shut the room the player stands in (an ambush) and open it again. */
#define DNG_SEAL_DOORS 1024
struct DNG_SEAL_DOOR {
    int room, x, y;     /**< The room and the doorway's cell. */
    int gate_part;      /**< The gate that closes it, facing the corridor. */
    int open_part;      /**< The doorway part, put back when it opens. */
    int open_direction; /**< That doorway's direction. */
};
struct DNG_SEAL_ROOM {
    int x, y, w, h; /**< The room, in cells. */
};
extern DNG_SEAL_DOOR DngSealDoorTable[DNG_SEAL_DOORS];
extern int DngSealDoorCount;
extern DNG_SEAL_ROOM DngSealRoomTable[DNG_ROOM_MAX];
extern int DngSealRoomCount;
extern int DngSealRoomSeed; /**< The map seed the tables were recorded for. */
/* Zones (dungeons.set_zones, Vigil World): a floor built while it is on is a chain of zones, each in its own part of the
 * grid (a snake of squares: east, then south, then west), its rooms joined only to each other. One corridor joins each
 * zone to the one before, and its doorway into the zone is a zone gate, locked until the player brings its key; a
 * monster in the zone before carries the key (a small chest holds it when no monster stands there). The stair down
 * stands in the first zone and the stair up in the last. Gates the mod names as opened are built open. */
#define DNG_ZONE_MAX 30 // a whole Gauntlet run on one map is 25 (masks are int: bit 29 at most)
#define DNG_ZONE_NONE 255
struct DNG_ZONE_GATE {
    int x, y;           /**< The gate's cell: the doorway of the zone it leads into. */
    int zone;           /**< The zone behind it, 1..zones-1. */
    int key_item;       /**< The key item that opens it; -1 for a smoke gate, which no key opens. */
    int gate_part;      /**< The locked gate part the cell shows. */
    int open_part;      /**< The doorway part the cell had, put back when it opens. */
    int open_direction; /**< That doorway's direction. */
    int locked;         /**< 1 until the key opens it. */
};
extern int DngZoneCount;                   /**< Zones a mod asked for on each floor (0: off, else 2..DNG_ZONE_MAX). */
extern int DngZoneRooms[DNG_ZONE_MAX];     /**< Rooms in each of them. */
extern int DngZoneOpenMask;                /**< Gates built open: bit z for the gate into zone z. */
/* Smoke gates (set_zones smoke): bit z makes the gate into zone z a smoke gate, a wall no key opens, drawn pale; the
 * player walking up to it is game.dungeon.smoke_touched (the mod takes them on, to a floor of the game's own). Built
 * open like any other gate once the mod lists it in open. */
extern int DngZoneSmokeMask;
/* Per-zone set_zones flags, bit z each, as asked and as the floor last built with zones has them:
 * quiet: no monster of the floor's own starts in the zone (and none counts towards the floor's number);
 * heal: its shrine's spring heals as a spring does even with shrine_heal = false;
 * arena: its rooms are built 6x6, room for a boss and its cast. */
extern int DngZoneQuietMask, DngZoneHealMask, DngZoneArenaMask;
extern int DngZoneFloorQuietMask, DngZoneFloorHealMask;
// set_zones vaults = false: no way down to the floor's back side (a map whose zones make a whole run).
extern int DngZoneVaults;
extern int DngZoneFloorCount;              /**< Zones on the main floor last built (0: none). */
extern int DngZoneFloorSeed;               /**< The map seed they were built for. */
extern DNG_ZONE_GATE DngZoneGateTable[DNG_ZONE_MAX - 1];
extern int DngZoneGateCount;
extern unsigned char DngZoneOfCell[DNG_CELLS]; /**< The zone each cell is in, or DNG_ZONE_NONE. */
/* The key a zone gate takes: Silver Key down, never the key a shortcut door or the floor's own gate key takes. */
int DngZoneGateKey(int zone);
// Shrines (dungeons.set_zones shrines = true): a spring in each zone's shrine room and no other; the cell of each
// zone's spring on the floor last built with zones, or -1.
extern int DngZoneShrines;
// set_zones shrine_heal = false: a shrine's spring refills water but leaves HP alone (the mod's rest heals).
extern int DngZoneShrineHeal;
// set_zones sanctuary = true: no monster steps within DNG_ZONE_SANCTUARY of the spot in front of a shrine's spring (the
// circle no monster starts in), so the party is safe there; chasers stop at its edge.
extern int DngZoneSanctuary;
// dungeons.set_npc_marks: where a mod stands one of the floor's own NPCs (its parts' people, such as Divine Beast
// Cave's Fairy King, NPC 0) on the floor it was asked for, drawn there each frame: {npc, cell x, cell y, facing 0-3}.
struct DngNpcMark {
    int npc;
    int x;
    int y;
    int facing;
};
constexpr int DNG_NPC_MARK_MAX = 16;
extern DngNpcMark DngNpcMarks[DNG_NPC_MARK_MAX];
extern int        DngNpcMarkCount;
extern int        DngNpcMarkSeed; // the floor (its map seed) the marks were set on
// dungeons.set_npc_models: a character pack (a scene's, such as dun/d01/event/e28c03c.chr, the Fairy King) loaded into
// a free NPC slot of a dungeon's map when that map loads, for set_npc_marks to stand (a dungeon's parts only load the
// people their floors place). cfg: the pack's config (<stem>.cfg, or base.cfg for a dun/mainchara pack).
struct DngNpcModel {
    int  dungeon;
    int  npc;
    int  motion;
    char pack[48];
    char cfg[16];
};
constexpr int DNG_NPC_MODEL_MAX = 3;
extern DngNpcModel DngNpcModels[DNG_NPC_MODEL_MAX];
extern int         DngNpcModelCount;
constexpr float DNG_ZONE_SANCTUARY = 400.0f;

/**
 * Whether the player's water is a shrine spring that only quenches thirst: the main floor of a zoned layout asked with
 * shrine_heal = false.
 */
bool ShrineWaterDry();

/* Height steps (set_zones descent): each zone of a zoned main floor lies `descent` units below the zone it is entered
 * from, and the corridor cell outside each gate is a ramp between the two (the nearest straight corridor cell within a
 * few of the gate; the cells between it and the gate lie at the lower height). A gate with no straight corridor near it
 * leaves its zone level with the one before. Off (0) the floor is built and drawn exactly as before. */
class CFrame;
class CBoxVu0;
class CDungeonMap;
extern int DngZoneDescent;              /**< set_zones descent: units each zone lies below the one before (0: flat). */
extern int DngHeightSeed;               /**< The main floor (its map seed) the heights were built for, or -1. */
extern float DngCellY0[DNG_CELLS];      /**< Each cell's floor height (a ramp's at its west or north edge). */
extern float DngCellY1[DNG_CELLS];      /**< A ramp's floor height at its east or south edge. */
extern signed char DngCellRamp[DNG_CELLS]; /**< 0 flat, 1 a ramp along x (west to east), 2 along z (north to south). */
/** True when map is the main floor the heights were built for. */
bool DngHeightsOn(const CDungeonMap *map);
/** The floor's height under a world position on map (0 off the heights). */
float DngFloorHeight(const CDungeonMap *map, float x, float z);
/** Moves a point the game placed on the floor (y about 0) onto the floor's height there. */
void DngLiftPoint(const CDungeonMap *map, float *pos);
/** A cell's part or collision frame, already turned and placed at the cell, raised to the cell's height (and sheared
 * along a ramp); box, when given, is grown to take in the ramp's slope for a collision query. */
void DngLiftCellFrame(const CDungeonMap *map, CFrame *frame, int col, int row, CBoxVu0 *box = nullptr);
/** Puts a frame DngLiftCellFrame sheared for a ramp back as it was (part frames are shared between cells). */
void DngRestoreCellFrame(CFrame *frame);
// True while the player is on the main floor last built with zones (the floor with this seed).
bool DngZoneFloorIs(int map_seed);
// Regions (dungeons.set_zones source_floor): the floor each zone takes its monsters, atla and chest items from (-1, the
// floor being built), as asked and as the floor last built with zones has them; the zones each loaded monster model
// may stand in (a bit per zone; 0 for a model every zone shares, or a floor without sources).
extern int DngZoneSourceFloor[DNG_ZONE_MAX];
extern int DngZoneFloorSource[DNG_ZONE_MAX];
constexpr int DNG_ZONE_MODEL_MAX = 128;
extern int DngZoneModelMask[DNG_ZONE_MODEL_MAX];
/* Part banks (dungeons.set_zones source_dungeon, the Gauntlet's Confluence): a zone may be built from another
 * dungeon's parts. When the floor's map loads, each such dungeon's main floor pack loads into a bank of its own
 * (its ground textures in a texture block of their own), and a cell of that zone draws, collides and takes its camera,
 * torches and water from the bank's corridors, room walls, doors, corners, floors, dividing walls, pillars and
 * healing springs (each dungeon's own shrine). Stairs, gates and the character doors stay the floor's own, so its
 * events and keys line up. With no
 * source_dungeon asked for, nothing loads and every cell is the map's own, as before. */
class CDungeonParts;
constexpr int DNG_PART_BANK_MAX = 6;
constexpr int DNG_PART_BANK_TEXTURE_BLOCK = 0x70; /**< Bank b's ground textures are in block 0x70 + b. */
extern int DngZoneSourceDungeon[DNG_ZONE_MAX];    /**< As asked: the dungeon each zone takes its parts from (-1, the floor's). */
extern int DngZoneFloorDungeon[DNG_ZONE_MAX];     /**< As the floor last built with zones has them. */
extern int DngPartBankCount;                      /**< Banks loaded with the map. */
extern int DngPartBankDungeon[DNG_PART_BANK_MAX]; /**< The dungeon each bank holds. */
/** The part a cell of map draws: a bank's, in a zone of another dungeon, else map->parts[its parts_no]. */
CDungeonParts *DngCellPart(CDungeonMap *map, int cell);
/** The dungeons the asked zones want banks of (a bit each), leaving out own_dungeon. */
int DngPartBanksWanted(int own_dungeon);
/** The dungeons the banks loaded with the map hold (a bit each). */
int DngPartBanksLoaded();
/** Loads the banks the asked zones want (BtMapJumpLoad, after the map's own parts). */
void DngLoadPartBanks(int own_dungeon);
/** As a floor is built (after game.dungeon.generating): zones that now want other banks than the map loaded with load
 * them in place of the old ones, before the monsters take what the map leaves of its buffer. */
void BtReloadPartBanks();
// True when the floor last built with zones (with this seed) has a zone of another dungeon.
bool DngZoneDungeonsShown(int map_seed);
/** Each frame before the dungeon's lights are set: on a floor with zones of other dungeons, the lights, ambient, fog and
 * background ease (over about a second) to those of the dungeon of the zone the player stands in, each bank's as its
 * map file has them, the floor's own elsewhere. Off such a floor it does nothing (it hands back the floor's own if it
 * had eased them). Another writer of the lights (a new floor, dungeons.lighting_set) becomes the floor's own. */
void DngZoneLightingStep();

// The floor the exit gate of a floor built with zones leads to (-1, the next), as asked and as that floor has it.
extern int DngZoneExitFloor;
extern int DngZoneFloorExit;
// The floor whose monsters, atla and chest items go at a world position of the map with this seed (own_floor outside
// a zone with a source).
int DngZoneSourceFloorAt(int map_seed, const float *pos, int own_floor);
// True when the floor last built with zones (with this seed) has a zone of another floor.
bool DngZoneSourcesShown(int map_seed);
// The most spots a search for a free place on a floor gathers: the game's 196, or with zones asked for (a map that
// can fill the whole grid) every spot on it.
int DngPutSpotLimit();
extern int DngZoneShrineX[DNG_ZONE_MAX];
extern int DngZoneShrineY[DNG_ZONE_MAX];
/* Rest floor (dungeons.set_rest_room): a floor built while it is on is a hall and a small antechamber joined by a
 * short corridor that holds the way in and the exit gate. The hall has a healing spring against its top wall; the
 * back floor's stairs stand in the antechamber's top wall. No dividing walls, pillars, gated room or character
 * parts. */
extern int DngRestRoomEnabled; /**< Build rest floors from now on. */
extern int DngRestRoomSeed;    /**< The map seed of the rest floor last built (-1: none). */
extern int DngRestSpringX, DngRestSpringY; /**< Its spring's cell. */
extern int DngRestStairsX, DngRestStairsY; /**< Its back floor stairs' cell. */
extern int DngRestHallX, DngRestHallY, DngRestHallW, DngRestHallH; /**< Its hall, in cells. */
/* Character blocks (dungeons.set_character_blocks): the parts only one character opens (Xiao's doors and jumps,
 * Goro's, Ruby's, Ungaga's and Osmond's) go on a floor only for the characters in this mask (bit n = character n);
 * -1 keeps the retail rule, which counts the party. */
extern int DngCharaBlockMask;
extern int DngShortcutDoorCount; /**< Doors on the floor last built. */
extern int DngShortcutDoorSeed;  /**< The map seed they were built for. */
/* Weapons in chests (dungeons.set_weapon_drops): `unique` never puts a weapon the party already holds (or one
 * already in a chest on the floor) in a chest; `rare` is the percent chance a chest weapon steps up to a stronger
 * one for the same character; `large` is the percent of chests that are large (retail 12), where weapons are. */
extern int DngWeaponUnique, DngWeaponRarePercent, DngLargeChestPercent;
/* The weapon a chest holds instead of the one its table rolled, under those rules; -1 when there is none to give
 * (the chest then rolls again). `placed` lists the weapons already in chests on this floor. */
int DngChestWeapon(int item_no, const int *placed, int placed_count);
/* The tint a door of this key is drawn with (0..255 per channel). */
void DngShortcutDoorTint(int key_item, float *rgb);
extern int DngLayoutArea;    /**< Side of the corner rooms are placed in, 8..DNG_GRID. */
extern int DngLayoutRoomMin; /**< Smallest room side, 2..6. */
extern int DngLayoutRoomMax; /**< Largest room side, DngLayoutRoomMin..6. */
#define DNG_BUILD_AREA      (DngLayoutArea > 0 ? DngLayoutArea : 16)
#define DNG_BUILD_ROOM_MIN  (DngLayoutRoomMax > 0 ? DngLayoutRoomMin : 3)
#define DNG_BUILD_ROOM_SPAN (DngLayoutRoomMax > 0 ? DngLayoutRoomMax - DngLayoutRoomMin + 1 : 2)
#define DNG_BUILD_ROOM_SPAN_F ((float) DNG_BUILD_ROOM_SPAN)
/* Full-screen map tile: retail 16 px for its 20-cell grid; a larger layout shrinks it to fit. */
extern int DngMiniMapTile, DngMiniMapOriginX, DngMiniMapOriginY;
// A map too big for the corner (a whole stretch of a dungeon on one map) shows the part around the player, in this
// box of the screen; everything the mini map draws keeps inside it (DngMiniMapShows). x1 < 0: the whole map shows.
extern int DngMiniMapClipX0, DngMiniMapClipY0, DngMiniMapClipX1, DngMiniMapClipY1;
inline bool DngMiniMapShows(int x, int y, int w, int h) {
    return DngMiniMapClipX1 < 0 || (x >= DngMiniMapClipX0 && y >= DngMiniMapClipY0 && x + w <= DngMiniMapClipX1 && y + h <= DngMiniMapClipY1);
}
// dungeons.set_waypoint: the player's waypoint on the floor built from this seed (x < 0: none), shown on the mini map
// (at the box's edge, pointing the way, while it lies outside).
extern int DngWaypointX, DngWaypointY, DngWaypointSeed;
#define DNG_MAP_TILE DngMiniMapTile
#define DNG_MAP_X DngMiniMapOriginX
#define DNG_MAP_Y DngMiniMapOriginY
#define DNG_MAP_SCALE       (0.1f * (float) DNG_MAP_TILE / 16.0f)
/* Walk-through exit room (PC only): a 3x3 room with a stair back to town on its
 * north side and a stair to the next floor on its south side, built in a free
 * part of the grid. Using the floor's exit moves the player into it instead of
 * opening the continue/leave window. */
extern int DngExitRoomEnabled; /**< 1 to build and use the exit room (default 0: unfinished). */
extern int DngExitRoomValid;   /**< 1 when the current main floor has one. */
extern int DngExitRoomX;       /**< Column of the room's west wall. */
extern int DngExitRoomY;       /**< Row of the room's north wall. */
#define DNG_IN_EXIT_ROOM(col, row) \
    (DngExitRoomValid && (col) >= DngExitRoomX && (col) <= DngExitRoomX + 2 && (row) >= DngExitRoomY - 1 && \
     (row) <= DngExitRoomY + 3)

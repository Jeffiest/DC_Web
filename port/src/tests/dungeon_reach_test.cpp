#include <gtest/gtest.h>

#include <array>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <vector>

#include "dungeonmap.hpp"
#include "dun/gameloop.hpp"
#include "dungeonparts.hpp"
#include "moonmod/dungeonmap_layout.hpp"
#include "savedata.hpp"
#include "userstatus.hpp"

namespace dc::game {}
using namespace dc::game;

namespace {

// Which sides a cell opens: 1 north (row - 1), 2 east (col + 1), 4 west (col - 1), 8 south (row + 1), as the floor
// builder's chain tables count them.
constexpr std::array<std::array<int, 2>, 4> kStep = {{{0, -1}, {1, 0}, {-1, 0}, {0, 1}}};
constexpr std::array<int, 4> kSide = {1, 2, 4, 8};
constexpr int Opposite(int side) { return side == 1 ? 8 : side == 8 ? 1 : side == 2 ? 4 : 2; }

enum class Kind { None, Road, Room, Door, Stair, Blocked };

// A road's open sides: the chain table's mask names the closed ones.
int RoadOpen(int part, int direction) {
    static constexpr int kRoad[][3] = {{6, 0, 0}, {9, 0, 1}, {5, 1, 0}, {3, 1, 1}, {10, 1, 2}, {12, 1, 3}, {0, 2, 0}, {1, 3, 0},
                                       {2, 3, 1}, {8, 3, 2}, {4, 3, 3}, {7, 4, 0}, {11, 4, 1}, {14, 4, 2}, {13, 4, 3}};
    for (const auto &r : kRoad) {
        if (r[1] == part && r[2] == direction) return 15 & ~r[0];
    }
    return 0;
}

// The side a doorway opens to the corridor: north, east, south, west for each group of four.
int DoorOutward(int part) {
    static constexpr int kOut[4] = {1, 2, 8, 4};
    for (int first : {MAP_PARTS_ROOM_DOOR_NORTH, MAP_PARTS_ROOM_GATE_NORTH, MAP_PARTS_ROOM_DOOR_B_NORTH, MAP_PARTS_ROOM_DOOR_C_NORTH,
                      MAP_PARTS_KEY_DOOR_XIAO_NORTH, MAP_PARTS_KEY_DOOR_GORO_NORTH, MAP_PARTS_KEY_DOOR_RUBY_NORTH}) {
        if (part >= first && part < first + 4) return kOut[part - first];
    }
    return 0;
}

bool CharacterDoor(int part) {
    return (part >= MAP_PARTS_KEY_DOOR_XIAO_NORTH && part <= MAP_PARTS_KEY_DOOR_RUBY_WEST && part != MAP_PARTS_KEY_XIAO);
}

bool CharacterPart(int part) {
    return part == MAP_PARTS_KEY_XIAO || part == MAP_PARTS_KEY_UNGAGA || part == MAP_PARTS_KEY_OSMOND || CharacterDoor(part);
}

Kind KindOf(int part, bool obstacles_block) {
    if (part == MAP_PARTS_NONE || part == MAP_PARTS_URA_ROAD) return Kind::None;
    if (part <= MAP_PARTS_ROAD_DEAD_END) return Kind::Road;
    if (CharacterPart(part)) {
        if (obstacles_block) return Kind::Blocked;
        return part == MAP_PARTS_KEY_XIAO || part == MAP_PARTS_KEY_UNGAGA || part == MAP_PARTS_KEY_OSMOND ? Kind::Road : Kind::Door;
    }
    if (DoorOutward(part) != 0) return Kind::Door;
    if ((part >= MAP_PARTS_DIVIDE_END_NORTH && part <= MAP_PARTS_DIVIDE_END_WEST) ||
        (part >= MAP_PARTS_DIVIDE_WALL_NORTH && part <= MAP_PARTS_DIVIDE_WALL_WEST)) {
        return Kind::Blocked;
    }
    if (part == MAP_PARTS_STAIR_DOWN || part == MAP_PARTS_STAIR_UP || (part >= MAP_PARTS_URA_ENTRANCE_NORTH && part <= MAP_PARTS_URA_ENTRANCE_WEST)) {
        return Kind::Stair;
    }
    return Kind::Room; // walls, corners, floor, springs, pillars, dividing doors
}

struct Report {
    int unreachable_rooms = 0;  // room cells the start can't reach
    std::vector<int> cut;       // which ones
    bool exit_reachable = true;
    int open_into_nothing = 0;  // a road side that opens onto a cell that doesn't open back (a drop into the void)
    std::string where;
};

// Walks the finished floor from the stair down. obstacles_block: character parts stop the walk (no switching).
Report Walk(const CDungeonMap &map, bool obstacles_block) {
    Report report;
    auto part = [&](int c, int r) { return map.cells[c + r * DNG_GRID].parts_no; };
    auto opens = [&](int c, int r, int side, int nc, int nr) {
        const int p = part(c, r);
        const Kind k = KindOf(p, obstacles_block);
        const Kind nk = KindOf(part(nc, nr), obstacles_block);
        switch (k) {
            case Kind::Road: {
                const int key_part = p == MAP_PARTS_KEY_XIAO || p == MAP_PARTS_KEY_UNGAGA || p == MAP_PARTS_KEY_OSMOND;
                const int open = key_part ? 15 : RoadOpen(p, map.cells[c + r * DNG_GRID].direction);
                return (open & side) != 0;
            }
            case Kind::Room: return nk == Kind::Room || nk == Kind::Door || nk == Kind::Stair;
            case Kind::Door: return side == DoorOutward(p) || nk == Kind::Room || nk == Kind::Door;
            case Kind::Stair: return true;
            default: return false;
        }
    };
    int start = -1;
    for (int i = 0; i < DNG_GRID * DNG_GRID && start < 0; ++i) {
        if (map.cells[i].parts_no == MAP_PARTS_STAIR_DOWN) start = i;
    }
    if (start < 0) {
        report.exit_reachable = false;
        report.where = "no stair down";
        return report;
    }
    std::vector<char> seen(DNG_GRID * DNG_GRID, 0);
    std::vector<int> queue = {start};
    seen[start] = 1;
    while (!queue.empty()) {
        const int at = queue.back();
        queue.pop_back();
        const int c = at % DNG_GRID, r = at / DNG_GRID;
        for (int s = 0; s < 4; ++s) {
            const int nc = c + kStep[s][0], nr = r + kStep[s][1];
            if (nc < 0 || nr < 0 || nc >= DNG_GRID || nr >= DNG_GRID) continue;
            if (!opens(c, r, kSide[s], nc, nr) || !opens(nc, nr, Opposite(kSide[s]), c, r)) continue;
            if (!seen[nc + nr * DNG_GRID]) {
                seen[nc + nr * DNG_GRID] = 1;
                queue.push_back(nc + nr * DNG_GRID);
            }
        }
    }
    for (int r = 0; r < DNG_GRID; ++r) {
        for (int c = 0; c < DNG_GRID; ++c) {
            const int p = part(c, r);
            const Kind k = KindOf(p, obstacles_block);
            if (k == Kind::Road && p <= MAP_PARTS_ROAD_DEAD_END) {
                for (int s = 0; s < 4; ++s) {
                    const int nc = c + kStep[s][0], nr = r + kStep[s][1];
                    if (nc < 0 || nr < 0 || nc >= DNG_GRID || nr >= DNG_GRID) continue;
                    if (opens(c, r, kSide[s], nc, nr) && KindOf(part(nc, nr), false) != Kind::Blocked &&
                        !opens(nc, nr, Opposite(kSide[s]), c, r)) {
                        ++report.open_into_nothing;
                        report.where += " road(" + std::to_string(c) + "," + std::to_string(r) + ")->part" + std::to_string(part(nc, nr));
                    }
                }
            }
            if (seen[c + r * DNG_GRID]) continue;
            if (k == Kind::Room) {
                if (report.unreachable_rooms < 6) report.where += " cut(" + std::to_string(c) + "," + std::to_string(r) + ")";
                ++report.unreachable_rooms;
                report.cut.push_back(c + r * DNG_GRID);
            }
            if (p == MAP_PARTS_STAIR_UP) report.exit_reachable = false;
        }
    }
    return report;
}

struct FloorSetup {
    int dungeon, floor, shortcuts, warden, chara_mask, room_bonus;
};

// Builds floors the way the Gauntlet asks for them and walks each one. Prints the seeds of broken floors so one
// can be rebuilt in game (DC_MAZE_SEED).
TEST(DungeonReach, GauntletFloorsReachTheExitAndEveryRoom) {
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    CSaveData *previous_save = SaveData;
    CUserStatus *previous_user = UserStatus;
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    UserStatus = status;
    status->res_limit_zone_current = -1;
    status->party_size = 6;
    const int previous_select = selectMapNo;
    auto map = std::make_unique<CDungeonMap>();

    const std::vector<FloorSetup> setups = {
        {0, 3, 0, 0, -1, 0},  {0, 3, 1, 0, -1, 0},  {0, 3, 3, 0, -1, 0},  {0, 10, 1, 0, 0x3F, 0}, {1, 9, 2, 0, 0x3F, 30}, {2, 10, 1, 1, 0x3F, 0},
        {3, 10, 3, 0, 0x3F, 60}, {4, 9, 1, 0, 0x3F, 0}, {4, 10, 0, 1, 0x3F, 30},
        {1, 11, 3, 1, 0x3F, 90}, {2, 12, 2, 0, 0x05, 90}, {3, 13, 3, 1, 0x21, 30}, {4, 14, 3, 0, 0x3F, 90},
    };
    int floors = 0, broken_joins = 0, cut_by_obstacles = 0, voids = 0, with_obstacle = 0;
    // DC_REACH_SEEDS raises the number of floors each setup builds (150 by default).
    const char *seeds_env = std::getenv("DC_REACH_SEEDS");
    const int seeds = seeds_env != nullptr && std::atoi(seeds_env) > 0 ? std::atoi(seeds_env) : 150;
    for (const FloorSetup &setup : setups) {
        for (int seed = 1; seed <= seeds; ++seed) {
            selectMapNo = setup.dungeon;
            status->cur_floor = setup.floor;
            DngShortcutDoors = setup.shortcuts;
            DngWardenRoomEnabled = setup.warden;
            DngCharaBlockMask = setup.chara_mask;
            DngRoomBonusPercent = setup.room_bonus;
#ifdef _WIN32
            _putenv_s("DC_MAZE_SEED", std::to_string(seed * 7919).c_str());
#else
            setenv("DC_MAZE_SEED", std::to_string(seed * 7919).c_str(), 1);
#endif
            map->buildRandomMap(6, 1);
            // DC_REACH_DUMP=<seed> prints that floor's grid: each cell's part number (.. for none), direction after a colon.
            // DC_REACH_DUMP_SHORTCUTS picks one setup when several build the same dungeon and floor.
            const char *only = std::getenv("DC_REACH_DUMP_SHORTCUTS");
            if (const char *dump = std::getenv("DC_REACH_DUMP"); dump != nullptr && std::atoi(dump) == seed * 7919 &&
                                                                (only == nullptr || std::atoi(only) == setup.shortcuts)) {
                for (int r = 0; r < DNG_GRID; ++r) {
                    std::string line;
                    for (int c = 0; c < DNG_GRID; ++c) {
                        const MAP_CELL &cell = map->cells[c + r * DNG_GRID];
                        char text[8];
                        if (cell.parts_no == MAP_PARTS_NONE) std::snprintf(text, sizeof text, "  .. ");
                        else std::snprintf(text, sizeof text, "%2d:%d ", cell.parts_no, cell.direction);
                        line += text;
                    }
                    if (line.find_first_not_of(" .") != std::string::npos) std::printf("%2d %s\n", r, line.c_str());
                }
            }
            ++floors;
            for (int cell = 0; cell < DNG_GRID * DNG_GRID; ++cell) {
                if (CharacterPart(map->cells[cell].parts_no)) {
                    ++with_obstacle;
                    break;
                }
            }
            const Report open = Walk(*map, false);
            const Report blocked = Walk(*map, true);
            if (open.unreachable_rooms > 0 || !open.exit_reachable) {
                ++broken_joins;
                std::printf("BROKEN dungeon %d floor %d shortcuts %d seed %d: %d rooms cut off, exit %s%s\n", setup.dungeon, setup.floor,
                            setup.shortcuts, seed * 7919,
                            open.unreachable_rooms, open.exit_reachable ? "reachable" : "CUT OFF", open.where.c_str());
            } else if (blocked.unreachable_rooms > 0 || !blocked.exit_reachable) {
                ++cut_by_obstacles;
                std::printf("OBSTACLE dungeon %d floor %d seed %d: without the character, %d rooms cut off, exit %s\n", setup.dungeon,
                            setup.floor, seed * 7919, blocked.unreachable_rooms, blocked.exit_reachable ? "reachable" : "CUT OFF");
            }
            if (open.open_into_nothing > 0) {
                ++voids;
                std::printf("VOID dungeon %d floor %d seed %d:%s\n", setup.dungeon, setup.floor, seed * 7919, open.where.c_str());
            }
        }
    }
    std::printf("%d floors: %d with broken joins, %d cut off by a character obstacle, %d with a road opening onto nothing; %d hold a "
                "character part\n", floors, broken_joins, cut_by_obstacles, voids, with_obstacle);
#ifdef _WIN32
    _putenv_s("DC_MAZE_SEED", "");
#else
    unsetenv("DC_MAZE_SEED");
#endif
    DngShortcutDoors = 0;
    DngWardenRoomEnabled = 0;
    DngCharaBlockMask = -1;
    DngRoomBonusPercent = 0;
    selectMapNo = previous_select;
    UserStatus = previous_user;
    SaveData = previous_save;
    EXPECT_EQ(broken_joins, 0);
    EXPECT_EQ(cut_by_obstacles, 0);
    EXPECT_EQ(voids, 0);
}

// dungeons.set_seed: a new floor built from a kept seed is the same floor, cell for cell, whatever was built in
// between; the seed is used once.
TEST(DungeonReach, SetSeedBuildsTheSameFloorAgain) {
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    CSaveData *previous_save = SaveData;
    CUserStatus *previous_user = UserStatus;
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    UserStatus = status;
    status->res_limit_zone_current = -1;
    status->party_size = 6;
    const int previous_select = selectMapNo;
    selectMapNo = 0;
    status->cur_floor = 3;
    auto map = std::make_unique<CDungeonMap>();
    const auto cells = [&] {
        std::vector<int> out;
        for (int cell = 0; cell < DNG_GRID * DNG_GRID; ++cell) out.push_back(map->cells[cell].parts_no * 8 + map->cells[cell].direction);
        return out;
    };

    DngNextSeed = 4242;
    map->buildRandomMap(6, 1);
    EXPECT_EQ(DngNextSeed, -1);
    const int seed = map->map_seed;
    const std::vector<int> first = cells();
    const int rooms = map->room_num;

    map->buildRandomMap(6, 1); // another floor in between, from a seed of its own
    EXPECT_NE(cells(), first);

    DngNextSeed = seed;
    map->buildRandomMap(6, 1);
    EXPECT_EQ(map->map_seed, seed);
    EXPECT_EQ(map->room_num, rooms);
    EXPECT_EQ(cells(), first);

    // Its chests come in the same order too, so a kept floor can name the ones already opened.
    const auto chests = [&] {
        std::vector<int> out;
        for (auto &event : map->events) event.kind = DNG_EVENT_NONE;
        for (auto &box : map->boxes) box.used = 0;
        for (auto &atra : map->atra) atra.used = 0;
        map->box_num = map->atra_num = 0;
        map->buildEventData(status->cur_floor, 1, 1);
        for (int i = 0; i < map->box_num; ++i) {
            out.push_back(static_cast<int>(map->boxes[i].pos[0]) * 1000 + static_cast<int>(map->boxes[i].pos[2]));
            out.push_back(map->boxes[i].item_no);
        }
        return out;
    };
    const std::vector<int> first_chests = chests();
    EXPECT_FALSE(first_chests.empty());
    DngNextSeed = seed;
    map->buildRandomMap(6, 1);
    EXPECT_EQ(chests(), first_chests);

    selectMapNo = previous_select;
    UserStatus = previous_user;
    SaveData = previous_save;
}

// A rest floor (dungeons.set_rest_room) is its two rooms: the hall with the spring against its top wall, the
// antechamber with the back floor's stairs in its top wall, the way in and the exit on the corridor between them,
// nothing else, and the exit reachable.
TEST(DungeonReach, RestFloorsAreAHallAndAnAntechamber) {
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    CSaveData *previous_save = SaveData;
    CUserStatus *previous_user = UserStatus;
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    UserStatus = status;
    status->res_limit_zone_current = -1;
    status->party_size = 6;
    const int previous_select = selectMapNo;
    auto map = std::make_unique<CDungeonMap>();
    DngRestRoomEnabled = 1;
    int floors = 0, bad = 0;
    for (int dungeon = 0; dungeon < 5; ++dungeon) {
        for (int seed = 1; seed <= 40; ++seed) {
            selectMapNo = dungeon;
            status->cur_floor = 2;
#ifdef _WIN32
            _putenv_s("DC_MAZE_SEED", std::to_string(seed * 7919).c_str());
#else
            setenv("DC_MAZE_SEED", std::to_string(seed * 7919).c_str(), 1);
#endif
            map->buildRandomMap(6, 1);
            ++floors;
            int ups = 0, downs = 0, uras = 0, springs = 0, pillars = 0;
            for (int cell = 0; cell < DNG_GRID * DNG_GRID; ++cell) {
                const int part = map->cells[cell].parts_no;
                ups += part == MAP_PARTS_STAIR_UP;
                downs += part == MAP_PARTS_STAIR_DOWN;
                uras += part >= MAP_PARTS_URA_ENTRANCE_NORTH && part <= MAP_PARTS_URA_ENTRANCE_WEST;
                springs += part >= MAP_PARTS_HEAL_SPRING_NORTH && part <= MAP_PARTS_HEAL_SPRING_WEST;
                pillars += part == MAP_PARTS_PILLAR_1 || part == MAP_PARTS_PILLAR_2;
            }
            const int spring_part = map->cells[DngRestSpringX + DngRestSpringY * DNG_GRID].parts_no;
            const int stairs_part = map->cells[DngRestStairsX + DngRestStairsY * DNG_GRID].parts_no;
            const Report open = Walk(*map, false);
            const bool ok = map->room_num == 2 && DngRestRoomSeed == map->map_seed && ups == 1 && downs == 1 && uras == 1 && springs == 1 &&
                            pillars == 0 && spring_part >= MAP_PARTS_HEAL_SPRING_NORTH && spring_part <= MAP_PARTS_HEAL_SPRING_WEST &&
                            stairs_part >= MAP_PARTS_URA_ENTRANCE_NORTH && stairs_part <= MAP_PARTS_URA_ENTRANCE_WEST &&
                            open.unreachable_rooms == 0 && open.exit_reachable;
            if (!ok) {
                ++bad;
                std::printf("REST dungeon %d seed %d: rooms %d stairs %d/%d back %d springs %d pillars %d spring part %d stairs part %d "
                            "exit %s\n", dungeon, seed * 7919, map->room_num, downs, ups, uras, springs, pillars, spring_part, stairs_part,
                            open.exit_reachable ? "reachable" : "CUT OFF");
            }
        }
    }
#ifdef _WIN32
    _putenv_s("DC_MAZE_SEED", "");
#else
    unsetenv("DC_MAZE_SEED");
#endif
    DngRestRoomEnabled = 0;
    DngRestRoomSeed = -1;
    selectMapNo = previous_select;
    UserStatus = previous_user;
    SaveData = previous_save;
    EXPECT_EQ(floors, 200);
    EXPECT_EQ(bad, 0);
}

// Every Warden floor gets its own sealed room, large enough for a boss (at least 4x4, mostly 6x6), and a dead end:
// with its gates shut the rest of the floor and the exit can still all be reached.
TEST(DungeonReach, WardenFloorsHaveALargeSealedDeadEndRoom) {
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    CSaveData *previous_save = SaveData;
    CUserStatus *previous_user = UserStatus;
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    UserStatus = status;
    status->res_limit_zone_current = -1;
    status->party_size = 6;
    const int previous_select = selectMapNo;
    auto map = std::make_unique<CDungeonMap>();
    auto shut = std::make_unique<CDungeonMap>();
    DngWardenRoomEnabled = 1;
    int floors = 0, without = 0, small = 0, full = 0, cut = 0;
    for (int dungeon = 0; dungeon < 5; ++dungeon) {
        for (int seed = 1; seed <= 60; ++seed) {
            selectMapNo = dungeon;
            status->cur_floor = 12 + dungeon % 3;
#ifdef _WIN32
            _putenv_s("DC_MAZE_SEED", std::to_string(seed * 104729).c_str());
#else
            setenv("DC_MAZE_SEED", std::to_string(seed * 104729).c_str(), 1);
#endif
            map->buildRandomMap(6, 1);
            ++floors;
            if (DngWardenRoomState != DNG_WARDEN_SEALED) {
                ++without;
                std::printf("NO ROOM dungeon %d seed %d (state %d)\n", dungeon, seed * 104729, DngWardenRoomState);
                continue;
            }
            if (DngWardenRoomW < 4 || DngWardenRoomH < 4) ++small;
            if (DngWardenRoomW == 6 && DngWardenRoomH == 6) ++full;
            *shut = *map;
            for (int d = 0; d < DngWardenDoorCount; ++d) {
                shut->cells[DngWardenDoorTable[d].x + DngWardenDoorTable[d].y * DNG_GRID].parts_no = MAP_PARTS_DIVIDE_WALL_NORTH;
            }
            const Report open = Walk(*map, false);
            const Report sealed = Walk(*shut, false);
            int outside = 0; // cells cut off by the shut gates that aren't the Warden room's own
            for (int cell : sealed.cut) {
                const int c = cell % DNG_GRID, r = cell / DNG_GRID;
                if (c < DngWardenRoomX || c >= DngWardenRoomX + DngWardenRoomW || r < DngWardenRoomY || r >= DngWardenRoomY + DngWardenRoomH) ++outside;
            }
            if (!sealed.exit_reachable || open.unreachable_rooms > 0 || outside > 0) {
                ++cut;
                std::printf("CUT dungeon %d seed %d: room %dx%d at %d,%d, %d cells outside it cut off with the gates shut, exit %s\n",
                            dungeon, seed * 104729, DngWardenRoomW, DngWardenRoomH, DngWardenRoomX, DngWardenRoomY, outside,
                            sealed.exit_reachable ? "reachable" : "CUT OFF");
            }
        }
    }
    std::printf("%d Warden floors: %d without a room, %d smaller than 4x4, %d at 6x6, %d cut off\n", floors, without, small, full, cut);
#ifdef _WIN32
    _putenv_s("DC_MAZE_SEED", "");
#else
    unsetenv("DC_MAZE_SEED");
#endif
    DngWardenRoomEnabled = 0;
    selectMapNo = previous_select;
    UserStatus = previous_user;
    SaveData = previous_save;
    EXPECT_EQ(without, 0);
    EXPECT_EQ(small, 0);
    EXPECT_EQ(cut, 0);
    EXPECT_GT(full, floors / 2);
}

} // namespace

// dungeons.set_zones: a floor of two zones is crossed only through its gate. The stair down is in the first zone, the
// stair up in the second, and with the gate shut nothing of the second can be reached. A gate the mod says is opened
// is built open.
TEST(DungeonReach, ZonesSplitTheFloorAtOneGate) {
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    CSaveData *previous_save = SaveData;
    CUserStatus *previous_user = UserStatus;
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    UserStatus = status;
    status->res_limit_zone_current = -1;
    status->party_size = 6;
    const int previous_select = selectMapNo;
    selectMapNo = 0;
    status->cur_floor = 3;
    auto map = std::make_unique<CDungeonMap>();
    struct Restore {
        ~Restore() { DngZoneCount = 0; DngZoneOpenMask = 0; DngZoneShrines = 0; DngNextSeed = -1; }
    } restore;
    DngZoneCount = 2;
    DngZoneRooms[0] = 5;
    DngZoneRooms[1] = 5;

    const char *seeds_env = std::getenv("DC_REACH_SEEDS");
    const int seeds = seeds_env != nullptr && std::atoi(seeds_env) > 0 ? std::atoi(seeds_env) : 150;
    int zoned = 0;
    for (int seed = 1; seed <= seeds; ++seed) {
        DngNextSeed = seed * 7;
        map->buildRandomMap(6, 1);
        if (DngZoneFloorCount != 2) continue;
        ++zoned;
        ASSERT_EQ(DngZoneGateCount, 1) << "seed " << map->map_seed;
        const DNG_ZONE_GATE &gate = DngZoneGateTable[0];
        EXPECT_EQ(gate.zone, 1);
        EXPECT_EQ(gate.locked, 1);
        EXPECT_EQ(gate.key_item, ITEM_SILVER_KEY);
        EXPECT_EQ(map->cells[gate.x + gate.y * DNG_GRID].parts_no, gate.gate_part);
        for (int cell = 0; cell < DNG_CELLS; ++cell) {
            if (map->cells[cell].parts_no == MAP_PARTS_STAIR_DOWN) EXPECT_EQ(DngZoneOfCell[cell], 0) << "seed " << map->map_seed;
            if (map->cells[cell].parts_no == MAP_PARTS_STAIR_UP) EXPECT_EQ(DngZoneOfCell[cell], 1) << "seed " << map->map_seed;
        }
        const Report open = Walk(*map, false);
        EXPECT_EQ(open.unreachable_rooms, 0) << "seed " << map->map_seed;
        EXPECT_TRUE(open.exit_reachable) << "seed " << map->map_seed;
        // shut the gate: the second zone is out of reach
        const int part = map->cells[gate.x + gate.y * DNG_GRID].parts_no;
        map->cells[gate.x + gate.y * DNG_GRID].parts_no = MAP_PARTS_NONE;
        const Report shut = Walk(*map, false);
        map->cells[gate.x + gate.y * DNG_GRID].parts_no = part;
        EXPECT_FALSE(shut.exit_reachable) << "seed " << map->map_seed << ": the stair up is reached round the gate";
        EXPECT_GT(shut.unreachable_rooms, 0) << "seed " << map->map_seed << ": the second zone is reached round the gate";
    }
    std::printf("zones: %d of %d floors split into two zones\n", zoned, seeds);
    EXPECT_GE(zoned * 10, seeds * 9) << "most floors come out as zones";

    // three to eight zones: each gate leads into the next, the stair up waits in the last
    for (const int count : {3, 4, 6, 8}) {
        DngZoneCount = count;
        for (int z = 0; z < count; ++z) DngZoneRooms[z] = 4;
        int chained = 0;
        for (int seed = 1; seed <= seeds; ++seed) {
            DngNextSeed = seed * 11;
            map->buildRandomMap(6, 1);
            if (DngZoneFloorCount != count) continue;
            ++chained;
            ASSERT_EQ(DngZoneGateCount, count - 1);
            for (int g = 0; g < DngZoneGateCount; ++g) {
                const DNG_ZONE_GATE &gate = DngZoneGateTable[g];
                EXPECT_EQ(gate.zone, g + 1);
                EXPECT_EQ(DngZoneOfCell[gate.x + gate.y * DNG_GRID], g + 1) << "seed " << map->map_seed;
            }
            for (int cell = 0; cell < DNG_CELLS; ++cell) {
                if (map->cells[cell].parts_no == MAP_PARTS_STAIR_UP) EXPECT_EQ(DngZoneOfCell[cell], count - 1) << "seed " << map->map_seed;
            }
            EXPECT_TRUE(Walk(*map, false).exit_reachable) << "seed " << map->map_seed;
        }
        std::printf("zones: %d of %d floors split into %d zones\n", chained, seeds, count);
        EXPECT_GE(chained * 10, seeds * 8) << count << " zones";
    }
    DngZoneCount = 2;
    DngZoneRooms[0] = DngZoneRooms[1] = 5;

    // the same floor with its gate opened
    DngNextSeed = 7;
    map->buildRandomMap(6, 1);
    const int seed = map->map_seed;
    ASSERT_EQ(DngZoneFloorCount, 2);
    DngZoneOpenMask = 1 << 1;
    DngNextSeed = seed;
    map->buildRandomMap(6, 1);
    ASSERT_EQ(DngZoneGateCount, 1);
    EXPECT_EQ(DngZoneGateTable[0].locked, 0);
    EXPECT_EQ(map->cells[DngZoneGateTable[0].x + DngZoneGateTable[0].y * DNG_GRID].parts_no, DngZoneGateTable[0].open_part);

    // shrines: one spring in each zone, standing in that zone with a cell in front of it, and no other spring
    DngZoneShrines = 1;
    int shrined = 0;
    for (int seed = 1; seed <= seeds; ++seed) {
        DngNextSeed = seed * 13;
        map->buildRandomMap(6, 1);
        if (DngZoneFloorCount != 2) continue;
        ++shrined;
        int springs = 0;
        for (int cell = 0; cell < DNG_CELLS; ++cell) springs += map->cells[cell].parts_no == MAP_PARTS_HEAL_SPRING_NORTH ? 1 : 0;
        EXPECT_EQ(springs, 2) << "seed " << map->map_seed;
        for (int z = 0; z < 2; ++z) {
            const int x = DngZoneShrineX[z], y = DngZoneShrineY[z];
            ASSERT_GE(x, 0) << "seed " << map->map_seed << " zone " << z;
            EXPECT_EQ(map->cells[x + y * DNG_GRID].parts_no, MAP_PARTS_HEAL_SPRING_NORTH) << "seed " << map->map_seed;
            EXPECT_EQ(DngZoneOfCell[x + y * DNG_GRID], z) << "seed " << map->map_seed;
            EXPECT_NE(map->cells[x + (y + 1) * DNG_GRID].parts_no, MAP_PARTS_NONE) << "seed " << map->map_seed << ": nowhere to stand";
        }
    }
    DngZoneShrines = 0;
    std::printf("zones: %d floors with shrines\n", shrined);
    EXPECT_GT(shrined, 0);

    // a whole stretch of a dungeon (Vigil's region of seven floors): each zone's shrine and the cell in front of it are
    // that zone's
    DngZoneShrines = 1;
    DngZoneCount = 7;
    for (int z = 0; z < 7; ++z) DngZoneRooms[z] = 5;
    int long_maps = 0;
    for (int seed = 1; seed <= seeds; ++seed) {
        DngNextSeed = seed * 17;
        map->buildRandomMap(6, 1);
        if (DngZoneFloorCount != 7) continue;
        ++long_maps;
        for (int z = 0; z < 7; ++z) {
            const int x = DngZoneShrineX[z], y = DngZoneShrineY[z];
            ASSERT_GE(x, 0) << "seed " << map->map_seed << " zone " << z;
            EXPECT_EQ(DngZoneOfCell[x + y * DNG_GRID], z) << "seed " << map->map_seed << " zone " << z;
            EXPECT_EQ(DngZoneOfCell[x + (y + 1) * DNG_GRID], z) << "seed " << map->map_seed << " zone " << z << " (in front)";
        }
        EXPECT_TRUE(Walk(*map, false).exit_reachable) << "seed " << map->map_seed;
    }
    std::printf("zones: %d of %d floors split into 7 zones with shrines\n", long_maps, seeds);
    EXPECT_GE(long_maps * 10, seeds * 8);
    DngZoneShrines = 0;
    DngZoneCount = 2;
    DngZoneRooms[0] = DngZoneRooms[1] = 5;

    // zones off again: a floor of its own
    DngZoneCount = 0;
    map->buildRandomMap(6, 1);
    EXPECT_EQ(DngZoneFloorCount, 0);
    EXPECT_EQ(DngZoneGateCount, 0);

    selectMapNo = previous_select;
    UserStatus = previous_user;
    SaveData = previous_save;
}

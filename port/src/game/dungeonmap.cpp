#include "dungeonmap.hpp"

#include "moonmod/game_hooks.hpp"
#include "platform/config.hpp"

#include <libvu0.h>

#include <algorithm>
#include <vector>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <memory>
#include <print>
#include <span>

#include "camera.hpp"
#include "camerafollow.hpp"
#include "character.hpp"
#include "dataset.hpp"
#include "dun/gameloop.hpp"
#include "dungeonparts.hpp"
#include "frame.hpp"
#include "frameattr.hpp"
#include "framevu1.hpp"
#include "gfx/gfx.hpp"
#include "mathutil.hpp"
#include "mglib.hpp"
#include "rect.hpp"
#include "render/screen.hpp"
#include "snd.hpp"
#include "texture.hpp"
#include "textureanime.hpp"
#include "userstatus.hpp"

namespace dc::game {

using namespace dc::render;

/**
 * What a grid cell holds while a floor is being built, as bit flags.
 */
// clang-format off
enum BuildCellFlag {
    BUILD_CELL_ROOM          = 0x001, /**< The cell is part of a room's floor. */
    BUILD_CELL_CORRIDOR      = 0x002, /**< A corridor runs through the cell. */
    BUILD_CELL_DOOR          = 0x004, /**< A corridor leaves the room through the cell. */
    BUILD_CELL_HEAL_SPRING   = 0x008, /**< The cell holds a healing spring against the room's top wall. */
    BUILD_CELL_DIVIDING_WALL = 0x010, /**< A wall dividing the room runs through the cell. */
    BUILD_CELL_DIVIDING_DOOR = 0x020, /**< The gap in a dividing wall. */
    BUILD_CELL_PILLAR        = 0x040, /**< The cell holds a pillar. */
    BUILD_CELL_STAIR_DOWN    = 0x080, /**< The cell holds the stair down. */
    BUILD_CELL_STAIR_UP      = 0x100, /**< The cell holds the stair up. */
    BUILD_CELL_URA_ENTRANCE  = 0x200, /**< The cell holds the entrance to the back floor. */
    BUILD_CELL_ROOM_LINK     = 0x400, /**< The door that leads to the linked room. */
};

// clang-format on

/**
 * A single grid cell while a floor is being built.
 */
struct BUILD_MAP_INFO {
    s32 kind;      /**< MapPartsNo that the cell builds into, or while a corridor is laid, the room it leaves from. */
    s32 direction; /**< Rotation that the cell's map part uses. */
#ifdef PORT
    u8  link[DNG_ROOM_MAX];  /**< 1 for each room whose area or corridor the cell belongs to (a byte each on the port). */
#else
    s32 link[DNG_ROOM_MAX];  /**< 1 for each room whose area or corridor the cell belongs to. */
#endif
    s32 content;   /**< What the cell holds, as BuildCellFlag bits. */
};

#ifndef PORT
STATIC_ASSERT(sizeof(BUILD_MAP_INFO) == 0x4C);
#endif

/**
 * The four cells that touch one cell, as offsets in grid columns and rows.
 */
struct PARTS_ADJACENT {
    int offset[4][2]; /**< Column and row offset of each cell. */
};

namespace {

// Each row of a chain table holds an adjacency mask, the map part it draws and that part's direction.
// clang-format off

/** Gives the door that closes a corridor onto a room. */
constexpr std::array<std::array<s32, 3>, 4> chainTableDoor = {{
    {14, MAP_PARTS_ROOM_DOOR_NORTH, 0},
    {13, MAP_PARTS_ROOM_DOOR_EAST,  0},
    {7,  MAP_PARTS_ROOM_DOOR_SOUTH, 0},
    {11, MAP_PARTS_ROOM_DOOR_WEST,  0},
}};

/** Gives the corridor that joins the cells that touch a corridor cell. */
constexpr std::array<std::array<s32, 3>, 15> chainTableRoad = {{
    {6,  MAP_PARTS_ROAD_STRAIGHT, 0},
    {9,  MAP_PARTS_ROAD_STRAIGHT, 1},
    {5,  MAP_PARTS_ROAD_CORNER,   0},
    {3,  MAP_PARTS_ROAD_CORNER,   1},
    {10, MAP_PARTS_ROAD_CORNER,   2},
    {12, MAP_PARTS_ROAD_CORNER,   3},
    {0,  MAP_PARTS_ROAD_CROSS,    0},
    {1,  MAP_PARTS_ROAD_TEE,      0},
    {2,  MAP_PARTS_ROAD_TEE,      1},
    {8,  MAP_PARTS_ROAD_TEE,      2},
    {4,  MAP_PARTS_ROAD_TEE,      3},
    {7,  MAP_PARTS_ROAD_DEAD_END, 0},
    {11, MAP_PARTS_ROAD_DEAD_END, 1},
    {14, MAP_PARTS_ROAD_DEAD_END, 2},
    {13, MAP_PARTS_ROAD_DEAD_END, 3},
}};

/** Gives the wall that closes the edges of a room cell. */
constexpr std::array<std::array<s32, 3>, 8> chainTableRoom = {{
    {1,  MAP_PARTS_ROOM_WALL_NORTH,        0},
    {2,  MAP_PARTS_ROOM_WALL_EAST,         0},
    {8,  MAP_PARTS_ROOM_WALL_SOUTH,        0},
    {4,  MAP_PARTS_ROOM_WALL_WEST,         0},
    {5,  MAP_PARTS_ROOM_CORNER_NORTH_WEST, 0},
    {3,  MAP_PARTS_ROOM_CORNER_NORTH_EAST, 0},
    {10, MAP_PARTS_ROOM_CORNER_SOUTH_EAST, 0},
    {12, MAP_PARTS_ROOM_CORNER_SOUTH_WEST, 0},
}};

/** Gives the wall that divides a room. */
constexpr std::array<std::array<s32, 3>, 6> chainTableDivid = {{
    {0xE8, MAP_PARTS_DIVIDE_END_NORTH,  0},
    {0xD4, MAP_PARTS_DIVIDE_END_EAST,   0},
    {0x71, MAP_PARTS_DIVIDE_END_SOUTH,  0},
    {0xB2, MAP_PARTS_DIVIDE_END_WEST,   0},
    {0xF9, MAP_PARTS_DIVIDE_WALL_NORTH, 0},
    {0xF6, MAP_PARTS_DIVIDE_WALL_EAST,  0},
}};

/** Gives the door that opens a wall that divides a room. */
constexpr std::array<std::array<s32, 3>, 2> chainTableDividDoor = {{
    {9, MAP_PARTS_DIVIDE_DOOR_NORTH, 0},
    {6, MAP_PARTS_DIVIDE_DOOR_EAST,  0},
}};

// clang-format on

} // namespace

void CDungeonMap::SetNPC(int npc_no, unsigned int *pack, int parts_no, sceVu0FVECTOR pos, sceVu0FVECTOR rot, int visible, int motion_no, CDataAlloc2<1> *alloc,
                         char *cfg_name) {
    if (this->npc[npc_no].used != 0) {
        return;
    }

    if (npc_no >= 4 || npc_no < 0) {
        return;
    }

    CFrameAttr attr;

    attr.fog_enable = true;

    for (int i = 0; i < 32; i++) {
        BtNPCTexAnimeData[npc_no][i].Initialize();
    }

    this->npc[npc_no].chara.InitializeTexAnime(BtNPCTexAnimeData[npc_no], 32);
    this->npc[npc_no].chara.LoadPackData2(pack, cfg_name != nullptr ? cfg_name : info_cfg_literal, alloc, npc_no + 64, alloc, 0);

    if (this->npc[npc_no].chara.frame == nullptr) {
        std::println("******* NPCEntryErr");
#ifdef PORT
        return; // MoonMod: a mod's pack that did not load stays out (retail went on into a null frame)
#endif
    }

    this->npc[npc_no].chara.frame->SetAttr(attr, 1, 64);
    SetFrameAttr(this->npc[npc_no].chara.frame, 1);

    sceVu0CopyVector(this->npc[npc_no].pos, pos);
    sceVu0CopyVector(this->npc[npc_no].rot, rot);
    this->npc[npc_no].parts_no = parts_no;
    this->npc[npc_no].visible = visible;
    this->npc[npc_no].used = 1;
    this->npc[npc_no].motion_no = motion_no;
    this->npc[npc_no].chara.motion_no = motion_no;
    this->npc[npc_no].chara.motion_flags = 0;
    this->npc[npc_no].chara.motion_speed = -1.0f;
}

void CDungeonMap::ClearNPC_Cash() {
    for (int i = 0; i < 4; i++) {
        this->npc[i].draw_num = 0;
    }
}

void CDungeonMap::ReservNPC_Draw(int npc_no, float x, float y, float z, int direction) {
    if (this->npc[npc_no].used != 0 && this->npc[npc_no].visible != 0 && this->npc[npc_no].chara.frame != nullptr) {
        this->npc[npc_no].draw_pos[this->npc[npc_no].draw_num][0] = x;
        this->npc[npc_no].draw_pos[this->npc[npc_no].draw_num][1] = y;
        this->npc[npc_no].draw_pos[this->npc[npc_no].draw_num][2] = z;
        this->npc[npc_no].draw_param[this->npc[npc_no].draw_num] = direction;
        this->npc[npc_no].draw_num++;
    }
}

void CDungeonMap::DrawNPCDraw() {
    float pos[4];

    for (int i = 0; i < 4; i++) {
        for (int j = 0; j < this->npc[i].draw_num; j++) {
            int   direction;
            float angle;
            float deg;

            if (this->npc[i].chara.frame == nullptr) {
                break;
            }

            sceVu0CopyVector(pos, this->npc[i].draw_pos[j]);
            pos[0] += this->npc[i].pos[0];
            pos[1] += this->npc[i].pos[1];
            pos[2] += this->npc[i].pos[2];
            pos[3] = 1.0f;
            this->npc[i].chara.SetPosition(pos);

            direction = this->npc[i].draw_param[j];

            if (direction > 3) {
                direction -= 3;
            }

            if (direction == 3) {
                direction = -1;
            }

            deg = -90.0f * direction;
            angle = (PI * deg) / 180.0f;
            angle += this->npc[i].rot[1];

            if (!(angle <= PI)) {
                angle -= TWO_PI;
            }

            if (angle < -PI) {
                angle += TWO_PI;
            }

            this->npc[i].chara.SetRotation(0.0f, angle, 0.0f);
            TexManager.ReloadTexture(Vif1Packet, i + 64);
            this->npc[i].chara.TextureAnime(i + 64);
            this->npc[i].chara.Draw();
        }
    }
}

void CDungeonMap::StepNPC() {
    for (int i = 0; i < 4; i++) {
        if (this->npc[i].used != 0 && this->npc[i].visible != 0 && this->npc[i].chara.frame != nullptr && this->npc[i].motion_no != -1) {
            this->npc[i].chara.Step();
        }
    }
}

void CDungeonMap::NPCSetMotion(int npc_no, int motion_no) {
    this->npc[npc_no].chara.motion_no = motion_no;
    this->npc[npc_no].chara.motion_flags = 0;
    this->npc[npc_no].chara.motion_speed = -1.0f;
}

void CDungeonMap::NPCSetMotion(int npc_no, int motion_no, float speed, int motion_flags) {
    this->npc[npc_no].chara.motion_no = motion_no;
    this->npc[npc_no].chara.motion_flags = motion_flags;
    this->npc[npc_no].chara.motion_speed = -1.0f;
    this->npc[npc_no].chara.motion_speed = speed;
}

CFrame *CDungeonMap::GetFrameSearch(char *name) {
    for (int i = 0; i < 72; i++) {
        CFrame *frame = this->parts[i].GetSearchFrame(name);

        if (frame != nullptr) {
            return frame;
        }
    }

    return nullptr;
}

void CDungeonMap::DrawMapFreeStyle() {
    int i;
    int npc_no;

    this->ClearNPC_Cash();

    for (i = 0; this->parts[i].frame[0] != nullptr; i++) {
        this->parts[i].pos[0] = 0.0;
        this->parts[i].pos[1] = 0.0;
        this->parts[i].pos[2] = 0.0;
        this->parts[i].pos[3] = 1.0;
        this->parts[i].direction = 0;
        this->parts[i].Draw();

        for (npc_no = 0; npc_no < 4; npc_no++) {
            if (this->npc[npc_no].parts_no == i) {
                this->ReservNPC_Draw(npc_no, 0.0, 0.0, 0.0, 0);
            }
        }
    }
}

void CDungeonMap::DrawMapCalc(int mode) {
    int row;
    int col;
    int i;

    if (mode == 1) {
        // Each part has one shared model that ends where the last cell using it was laid out, and floor
        // scripts read the stairs' models to place the player and open the gate: the walk-through exit
        // room is laid out first so the real stairs come last.
        for (int pass = 0; pass < 2; pass++) {
        for (row = 0; row < DNG_GRID; row++) {
            for (col = 0; col < DNG_GRID; col++) {
                int cell_no = col + row * DNG_GRID;
                if ((DNG_IN_EXIT_ROOM(col, row) ? 0 : 1) != pass) {
                    continue;
                }

                if (this->cells[cell_no].parts_no != MAP_PARTS_NONE) {
                    CDungeonParts *part;
                    int            dir = this->cells[cell_no].direction;

                    part = &this->parts[this->cells[cell_no].parts_no];
                    part->direction = dir;
                    part = &this->parts[this->cells[cell_no].parts_no];
                    part->pos[0] = 160.0f * col;
                    part->pos[1] = 0.0f;
                    part->pos[2] = 160.0f * row;
                    part->pos[3] = 1.0f;
                    this->parts[this->cells[cell_no].parts_no].DrawCalc(col, row, this->cells[cell_no].direction, mode);
                }
            }
        }
        }
    } else {
        for (i = 0; this->parts[i].frame[0] != nullptr; i++) {
            this->parts[i].DrawCalc(0, 0, 0, mode);
        }
    }
}

namespace {

constexpr std::array<int, 7> kItemFreeAreaCounts = {27, 15, 27, 27, 27, 15, 28};

constexpr int         kMapWidth = DNG_GRID;
constexpr int         kMaxPartsCandidates = 0x40;
constexpr std::size_t kPartsListSize = 2 * static_cast<std::size_t>(kMaxPartsCandidates);
constexpr float       kCellSize = 160.0f;

} // namespace

void CDungeonMap::DrawMap(CCameraFollow *camera, CFrameVu1 * /*player*/) {
    float cam_pos[4];
    float cam_ref[4];
    float view_delta[4];
    float view[4];
    float sound_pos[4] = {};
    int   sound_no = 0;

    if (selectMapNo < 0 || selectMapNo >= 7) {
        return;
    }

    static_cast<CCamera *>(camera)->GetPos(cam_pos);
    static_cast<CCamera *>(camera)->GetRef(cam_ref);
    view_delta[0] = cam_ref[0] - cam_pos[0];
    view_delta[1] = cam_ref[1] - cam_pos[1];
    view_delta[2] = cam_ref[2] - cam_pos[2];
    view_delta[3] = 0.0f;
    sceVu0Normalize(view, view_delta);
    ITEM_FREE_AREA *free_area = ItemFreeAreaAll[selectMapNo];

    this->ClearNPC_Cash();
    // MoonMod (dungeons.set_npc_marks): the map's NPCs where a mod stands them (DrawNPCDraw adds the NPC's offset)
    if (this == &MainDungeonMap && DngNpcMarkCount > 0 && DngNpcMarkSeed == this->map_seed) {
        for (int m = 0; m < DngNpcMarkCount; m++) {
            const DngNpcMark &mark = DngNpcMarks[m];
            if (this->npc[mark.npc].draw_num >= 16) continue;
            this->ReservNPC_Draw(mark.npc, 160.0f * mark.x, 0.0f, 160.0f * mark.y, mark.facing);
        }
    }
    float nearest = 10000.0f;

    for (int row = 0; row < kMapWidth; row++) {
        for (int col = 0; col < kMapWidth; col++) {
            float world_x = kCellSize * col;
            float delta_x = world_x - cam_pos[0];
            float world_z = kCellSize * row;
            float delta_z = world_z - cam_pos[2];

            float     cell_delta[4] = {delta_x, 0.0f, delta_z, 0.0f};
            float     dist = DistVector(cell_delta);
            MAP_CELL &cell = this->cells[col + row * kMapWidth];
            cell.camera_dist = dist;

            if (cell.parts_no == MAP_PARTS_NONE) {
                cell.visible = false;
                continue;
            }

            if (cell.parts_no < 0 || cell.parts_no >= 72) {
                cell.visible = false;
                continue;
            }

            if (cell.parts_no == MAP_PARTS_KEY_XIAO && UserStatus->cur_georama == 2 && dist < nearest) {
                nearest = dist;
                sound_pos[0] = world_x;
                sound_pos[1] = 0.0f;
                sound_pos[2] = world_z;
                sound_pos[3] = 1.0f;
                sound_no = 65;
            }

            if (cell.parts_no == 75 && dist < nearest) {
                nearest = dist;
                sound_pos[0] = world_x;
                sound_pos[1] = 0.0f;
                sound_pos[2] = world_z;
                sound_pos[3] = 1.0f;
                sound_no = 75;
            }

            bool draw = true;
            if (selectMapNo != DUNGEON_MOON_SEA) {
                if (!(dist < kCellSize * this->draw_dist_scale)) {
                    draw = false;
                } else {
                    cam_ref[0] = delta_x - view_delta[0];
                    cam_ref[1] = 0.0f;
                    cam_ref[2] = delta_z - view_delta[2];
                    cam_ref[3] = 1.0f;
                    sceVu0Normalize(cam_ref, cam_ref);
                    float facing = sceVu0InnerProduct(view, cam_ref);
                    // Fill keeps the game's vertical FOV and reveals more world at the sides, so a
                    // cell counts as in front up to the angle of the visible viewport edge rather
                    // than the 4:3 centre-plane cutoff.
                    float            horizontal_margin = 0.0f;
                    gfx::FrameLayout layout = gfx::CurrentFrameLayout();
                    if (layout.aspect == gfx::AspectMode::Fill) {
                        gfx::LogicalRect visible = gfx::VisibleLogicalRect(gfx::kMainTarget);
                        horizontal_margin = std::max(0.0f, visible.w - kScreenWidth) / kScreenWidth;
                    }
                    float facing_limit = -horizontal_margin / std::hypot(1.0f, horizontal_margin);
                    int   georama = UserStatus->cur_georama;
                    if (georama == 5 && cell.parts_no == MAP_PARTS_URA_ROAD) {
                        facing = 1.0f;
                    }
                    if (facing <= facing_limit && !(dist < kCellSize)) {
                        draw = false;
                    }
                    if (cell.parts_no >= MAP_PARTS_URA_ENTRANCE_NORTH && cell.parts_no <= MAP_PARTS_URA_ENTRANCE_WEST &&
                        georama == 5) {
                        draw = true;
                    }
                }
            }
            cell.visible = draw;
            if (!draw) {
                continue;
            }

            CDungeonParts &part = this->parts[cell.parts_no];
            part.direction = cell.direction;
            part.pos[0] = world_x;
            part.pos[1] = 0.0f;
            part.pos[2] = world_z;
            part.pos[3] = 1.0f;

            for (int npc_no = 0; npc_no < 4; npc_no++) {
                if (this->npc[npc_no].parts_no == cell.parts_no && this->npc[npc_no].draw_num < 16 &&
                    (dist < kCellSize ||
                     view[0] * (delta_x - view_delta[0]) + view[2] * (delta_z - view_delta[2]) > 0.0f)) {
                    this->ReservNPC_Draw(npc_no, world_x, 0.0f, world_z, cell.direction);
                }
            }

            float ambient[4];
            float old_ambient[4];

            if (selectMapNo != DUNGEON_MOON_SEA && UserStatus->cur_georama == 4) {
                MGGetAmbient(old_ambient);
                MGGetAmbient(ambient);

                if (dist <= 480.0f) {
                    ambient[3] = 128.0f;
                } else {
                    ambient[3] = std::max(128.0f - (dist - 480.0f), 0.0f);
                }

                MGSetAmbient(ambient);
            }

            // MoonMod: a locked shortcut door shows the colour of its key.
            int shortcut_key = -1;
            for (int d = 0; d < DngShortcutDoorCount; ++d) {
                // Only a floor with more than one lock colours them, and its first stays the native door: the
                // colours are there to tell the keys apart.
                if (d > 0 && DngShortcutDoorTable[d].locked && DngShortcutDoorTable[d].x == col && DngShortcutDoorTable[d].y == row) shortcut_key = DngShortcutDoorTable[d].key_item;
            }
            // A locked zone gate always shows its key's colour; a smoke gate (no key) draws pale as smoke.
            bool smoke_gate = false;
            for (int g = 0; this == &MainDungeonMap && g < DngZoneGateCount; ++g) {
                if (DngZoneGateTable[g].locked && DngZoneGateTable[g].x == col && DngZoneGateTable[g].y == row) {
                    shortcut_key = DngZoneGateTable[g].key_item;
                    smoke_gate = shortcut_key < 0;
                }
            }
            // Its frames are lit by the key's colour instead of the scene's lights while this cell draws.
            struct SavedTint {
                CFrame       *frame;
                char          use_color;
                sceVu0FVECTOR color;
            };
            std::vector<SavedTint> shortcut_saved;
            if (shortcut_key >= 0 || smoke_gate) {
                float tint[3] = {600.0f, 600.0f, 640.0f};
                if (!smoke_gate) DngShortcutDoorTint(shortcut_key, tint);
                const auto paint = [&](auto &&self, CFrame *frame) -> void {
                    for (; frame != nullptr; frame = frame->brother) {
                        SavedTint saved{frame, frame->attr.use_color, {}};
                        sceVu0CopyVector(saved.color, frame->attr.color);
                        shortcut_saved.push_back(saved);
                        frame->attr.use_color = true;
                        for (int c = 0; c < 3; ++c) frame->attr.color[c] = 64.0f + tint[c] * 0.3f;
                        self(self, frame->child);
                    }
                };
                for (CFrame *root : part.frame) {
                    if (root == nullptr) continue;
                    SavedTint saved{root, root->attr.use_color, {}};
                    sceVu0CopyVector(saved.color, root->attr.color);
                    shortcut_saved.push_back(saved);
                    root->attr.use_color = true;
                    for (int c = 0; c < 3; ++c) root->attr.color[c] = 64.0f + tint[c] * 0.3f;
                    paint(paint, root->child);
                }
            }

            part.Draw();

            for (const SavedTint &saved : shortcut_saved) {
                saved.frame->attr.use_color = saved.use_color;
                std::copy_n(saved.color, 4, saved.frame->attr.color);
            }

            if (selectMapNo != DUNGEON_MOON_SEA && UserStatus->cur_georama == 4) {
                MGSetAmbient(old_ambient);
            }

            if (DebugStatus[6] != 0) {
                for (int area_no = 0;
                     area_no < kItemFreeAreaCounts[selectMapNo] && free_area[area_no].parts_no != MAP_PARTS_NONE;
                     area_no++) {
                    const ITEM_FREE_AREA &area = free_area[area_no];

                    if (area.parts_no != cell.parts_no) {
                        continue;
                    }

                    if (area.rect_num < 0 || area.rect_num > 4) {
                        continue;
                    }

                    for (int rect_no = 0; rect_no < area.rect_num; rect_no++) {
                        float corner[4][4];
                        int   screen[4][4];

                        int rotation = static_cast<int>(static_cast<float>(area.direction));
                        rotation = rotation + cell.direction;

                        if (rotation > 3) {
                            rotation -= 4;
                        }

                        bool  all_visible = true;
                        float radians = (PI * ((4 - rotation) * 90)) / 180.0f;
                        float x[4];
                        float z[4];
                        x[0] = 10.0f * area.rect[rect_no].x0;
                        float height = 10.0f * area.rect[rect_no].y0;
                        z[0] = 10.0f * area.rect[rect_no].z0;
                        x[3] = 10.0f * area.rect[rect_no].x1;
                        z[3] = 10.0f * area.rect[rect_no].z1;
                        x[1] = x[3];
                        z[1] = z[0];
                        x[2] = x[0];
                        z[2] = z[3];

                        for (int corner_no = 0; corner_no < 4; corner_no++) {
                            corner[corner_no][0] =
                                -z[corner_no] * std::sin(radians) - x[corner_no] * std::cos(radians);
                            corner[corner_no][2] =
                                -x[corner_no] * std::sin(radians) + z[corner_no] * std::cos(radians);
                            corner[corner_no][0] *= -1.0f;
                            corner[corner_no][0] += world_x;
                            corner[corner_no][1] = 2.0f + height;
                            corner[corner_no][2] += world_z;
                            corner[corner_no][3] = 1.0f;

                            if (MGRotTransPers(screen[corner_no], corner[corner_no], 0) == 0) {
                                all_visible = false;
                            }
                        }

                        if (all_visible) {
                            setColSprite(Vif1Packet, screen[0], screen[1], screen[2], screen[3], 0x80, 0, 0, 0x40);
                        }
                    }
                }
            }
        }
    }

    if (nearest < 10000.0f) {
        float volume = 0.0f;
        float pan = 0.0f;
        SndGetVolPan(&volume, &pan, sound_pos, 10.0f, 500.0f);
        SndSetSeVolf(sound_no, volume, 0);
        SndSetSePanf(sound_no, pan, 0);
    }
}

void CDungeonMap::DrawBGModel(CCamera *camera) {
    float pos[4];

    // The background follows the camera, so that it stays at the horizon.
    camera->GetPos(pos);
    pos[1] = 0.0f;

    for (int i = 0; i < 6; i++) {
        if (this->bg_model[i] != nullptr) {
            this->bg_model[i]->SetPosition(pos);
            MGDraw(this->bg_model[i]);
        }
    }
}

void CDungeonMap::DrawDummyModel(CCamera *camera) {
    float pos[4];

    if (this->dummy_num <= 0 || this->dummy_num > 8 || this->dummy_frame_num < 0 || this->dummy_frame_num > 3) {
        return;
    }

    // A dummy model only draws while the camera is near enough to it.
    camera->GetPos(pos);

    for (int i = 0; i < this->dummy_num; i++) {
        if (this->dummy_model[i] >= 0 && this->dummy_model[i] < this->dummy_frame_num &&
            this->dummy_frame[this->dummy_model[i]] != nullptr &&
            DistVector(this->dummy_pos[i], pos) < kCellSize * (1.0f + this->draw_dist_scale)) {
            this->dummy_frame[this->dummy_model[i]]->SetPosition(this->dummy_pos[i]);
            MGDraw(this->dummy_frame[this->dummy_model[i]]);
        }
    }
}

void CDungeonMap::DrawMiniMap(float *pos, float angle) {
    // The full-screen and aerial map (game.dungeon_aerial_map, game.fit_dungeon_map) fit the grid to the screen,
    // and a mod's player tuning can reveal the map and the monsters.
    const auto viewport=gfx::VisibleLogicalRect(gfx::kMainTarget);
    const bool aerial=platform::ConfigGet().dungeon_aerial_map;
    const bool reveal_all=BtEquipMap || DebugStatus[3]!=0;
    int min_x=DNG_GRID, min_y=DNG_GRID, max_x=-1, max_y=-1;
    // In aerial mode, size the view to the portion the player can currently
    // see. Fitting against the undiscovered full layout shrank the explored
    // map to a tiny patch in the corner on large generated floors.
    for (int y=0;y<DNG_GRID;++y) for (int x=0;x<DNG_GRID;++x) {
        const int cell=x+y*DNG_GRID;
        if (cells[cell].parts_no==MAP_PARTS_NONE || (aerial && !reveal_all && !mask[cell])) continue;
        min_x=std::min(min_x,x); min_y=std::min(min_y,y);
        max_x=std::max(max_x,x); max_y=std::max(max_y,y);
    }
    if (max_x<min_x) {
        // At dungeon entry the reveal mask may not be populated yet.
        for (int y=0;y<DNG_GRID;++y) for (int x=0;x<DNG_GRID;++x) {
            if (cells[x+y*DNG_GRID].parts_no==MAP_PARTS_NONE) continue;
            min_x=std::min(min_x,x); min_y=std::min(min_y,y);
            max_x=std::max(max_x,x); max_y=std::max(max_y,y);
        }
    }
    if (platform::ConfigGet().fit_dungeon_map || aerial) {
        if (max_x>=min_x) {
            const float available_w=aerial ? viewport.w-80.0f : std::min(340.0f,viewport.w*0.48f);
            const float available_h=std::max(48.0f,viewport.h-(aerial?112.0f:128.0f));
            const int tile=std::clamp(int(std::floor(std::min(available_w/(max_x-min_x+1), available_h/(max_y-min_y+1)))),1,aerial?32:16);
            DngMiniMapTile=tile;
            if(aerial) {
                DngMiniMapOriginX=int(viewport.x+(viewport.w-(max_x-min_x+1)*tile)*0.5f)-min_x*tile;
                DngMiniMapOriginY=int(viewport.y+56+(available_h-(max_y-min_y+1)*tile)*0.5f)-min_y*tile;
            } else {
                DngMiniMapOriginX=int(viewport.x+viewport.w)-24-(max_x+1)*tile;
                DngMiniMapOriginY=int(viewport.y)+72-min_y*tile;
            }
        }
    } else {
        DngMiniMapTile=DngLayoutArea>20 ? 320/DngLayoutArea : 16;
        // Retail's 16-pixel tiles show 16 cells; a floor reaching further (a Warden room past the other rooms)
        // shrinks them to fit. The back floor's road along the last column doesn't count.
        int extent=0;
        for (int y=0;y<DNG_GRID;++y) for (int x=0;x<DNG_GRID;++x) {
            const int part=cells[x+y*DNG_GRID].parts_no;
            if (part!=MAP_PARTS_NONE && part!=MAP_PARTS_URA_ROAD) extent=std::max(extent,std::max(x,y)+1);
        }
        if (extent>16) DngMiniMapTile=std::min(DngMiniMapTile,256/extent);
        DngMiniMapOriginX=380; DngMiniMapOriginY=64;
    }
    // MoonMod: a map that would shrink below 8-pixel tiles (several floors on one map) shows the part around the
    // player instead, at 12-pixel tiles, scrolling as they walk; player.tuning_set's map_window asks for that square
    // on every floor.
    DngMiniMapClipX1 = DngMiniMapClipY1 = -1;
    if (!aerial && !platform::ConfigGet().fit_dungeon_map && (DngMiniMapTile < 8 || moonmod::ChroniclePlayerMapWindow())) {
        constexpr int kTile = 12, kWide = 21, kHigh = 15;
        DngMiniMapTile = kTile;
        const int box_x = 380 + 256 - kWide * kTile, box_y = 64;
        const float col = pos[0] / 160.0f, row = pos[2] / 160.0f;
        DngMiniMapOriginX = box_x + kWide * kTile / 2 - static_cast<int>(std::lround(col * kTile)) - kTile / 2;
        DngMiniMapOriginY = box_y + kHigh * kTile / 2 - static_cast<int>(std::lround(row * kTile)) - kTile / 2;
        DngMiniMapClipX0 = box_x;
        DngMiniMapClipY0 = box_y;
        DngMiniMapClipX1 = box_x + kWide * kTile;
        DngMiniMapClipY1 = box_y + kHigh * kTile;
    }
    int           status;
    unsigned char red;
    int           saved_map;
    int           saved_crystal;
    unsigned char green;
    unsigned char blue;
    int           row;
    int           col;
    int           direction;
    int           map_x;
    int           map_y;
    int           cellx;
    int           visible;
    int           show;
    float         player_screen_x;
    CTexture     *map_texture;
    CTexture     *symbol_texture;
    unsigned char alpha;
    int           masked;
    int           cell_no;
    CRect_i_      map_dst;
    CRect_i_      map_src;
    CRect_i_      player_dst;
    CRect_i_      player_src;
    CRect_i_      trap_dst;
    CRect_i_      trap_src;
    CRect_i_      box_dst;
    CRect_i_      box_src;
    CRect_i_      atra_dst;
    CRect_i_      atra_src;
    status = UserStatus->minimap_status;

    if (status == 3) return;
    // MoonMod player.tuning_set: map_hidden takes the floor map away.
    if (moonmod::ChroniclePlayerMapHidden()) return;
    status = std::clamp(status, 0, 2);

    if (aerial && max_x>=min_x) {
        // Back only the map, not the gameplay viewport. The previous full-screen
        // 0x68 black wash obscured the dungeon while the explored map was small.
        const int pad=8;
        const int left=std::max(int(viewport.x+4),DNG_MAP_X+min_x*DNG_MAP_TILE-pad);
        const int top=std::max(int(viewport.y+4),DNG_MAP_Y+min_y*DNG_MAP_TILE-pad);
        const int right=std::min(int(viewport.x+viewport.w-4),DNG_MAP_X+(max_x+1)*DNG_MAP_TILE+pad);
        const int bottom=std::min(int(viewport.y+viewport.h-4),DNG_MAP_Y+(max_y+1)*DNG_MAP_TILE+pad);
        if(right>left && bottom>top)
            MGFillBox(CRect_i_(left*16,top*8,(right-left)*16,(bottom-top)*8),0,0,0,0x20);
    }
    unsigned char red_table[3] = {0x20, 0x40, 0x60};
    unsigned char green_table[3] = {0x60, 0x80, 0xA2};
    unsigned char blue_table[3] = {0x40, 0x60, 0xA2};

    red = red_table[status];
    green = green_table[status];
    blue = blue_table[status];
    saved_map = BtEquipMap;
    saved_crystal = BtEquipMasuisyou;

    if (DebugStatus[3] != 0) {
        BtEquipMap = 1;
        BtEquipMasuisyou = 1;
    }

    // MoonMod player.tuning_set: map_reveal and monster_reveal act as the Map and the Magical Crystal.
    if (moonmod::ChroniclePlayerMapReveal()) {
        BtEquipMap = 1;
    }

    if (moonmod::ChroniclePlayerMonsterReveal()) {
        BtEquipMasuisyou = 1;
    }

    visible = BtEquipMap ? 0 : 1;
    map_texture = TexManager.GetTexture("minimap1", -1);
    symbol_texture = TexManager.GetTexture("itempack", -1);

    for (row = 0; row < DNG_GRID; row++) {
        for (col = 0; col < DNG_GRID; col++) {
            cell_no = col + row * DNG_GRID;
            masked = this->mask[cell_no];
            show = masked;

            if (visible == 0 && masked == 0) {
                show = 1;
                alpha = red;
            } else {
                alpha = green;
            }

            if (this->cells[cell_no].parts_no != MAP_PARTS_NONE && show == 1 &&
                DngMiniMapShows(col * DNG_MAP_TILE + DNG_MAP_X, row * DNG_MAP_TILE + DNG_MAP_Y, DNG_MAP_TILE, DNG_MAP_TILE)) {
                direction = this->cells[cell_no].direction + this->parts[this->cells[cell_no].parts_no].direction_offset;
                map_src.x = (direction % 8) * 16;
                map_src.y = (direction / 8) * 16;
                map_src.width = 16;
                map_src.height = 16;
                map_dst.x = col * DNG_MAP_TILE + DNG_MAP_X;
                map_dst.y = row * DNG_MAP_TILE + DNG_MAP_Y;
                map_dst.width = DNG_MAP_TILE;
                map_dst.height = DNG_MAP_TILE;
                set2DSprite(Vif1Packet, map_texture, map_dst, map_src, alpha, alpha, alpha, blue);
            }
        }
    }

    if (DngMiniMapClipX1 >= 0) {
        spRGBA edge = {120, 150, 190, 0x50};
        set2DSpriteC4(Vif1Packet, CRect_i_(DngMiniMapClipX0, DngMiniMapClipY0 - 2, DngMiniMapClipX1 - DngMiniMapClipX0, 2), &edge, &edge, &edge, &edge);
        set2DSpriteC4(Vif1Packet, CRect_i_(DngMiniMapClipX0, DngMiniMapClipY1, DngMiniMapClipX1 - DngMiniMapClipX0, 2), &edge, &edge, &edge, &edge);
        set2DSpriteC4(Vif1Packet, CRect_i_(DngMiniMapClipX0 - 2, DngMiniMapClipY0 - 2, 2, DngMiniMapClipY1 - DngMiniMapClipY0 + 4), &edge, &edge, &edge, &edge);
        set2DSpriteC4(Vif1Packet, CRect_i_(DngMiniMapClipX1, DngMiniMapClipY0 - 2, 2, DngMiniMapClipY1 - DngMiniMapClipY0 + 4), &edge, &edge, &edge, &edge);
    }
    // MoonMod dungeons.set_waypoint: a red flag on the waypoint's cell, held to the box's edge while it is outside.
    if (DngWaypointX >= 0 && DngWaypointSeed == this->map_seed) {
        const int size = std::max(6, DNG_MAP_TILE * 2 / 3);
        int wx = DngWaypointX * DNG_MAP_TILE + DNG_MAP_X + (DNG_MAP_TILE - size) / 2;
        int wy = DngWaypointY * DNG_MAP_TILE + DNG_MAP_Y + (DNG_MAP_TILE - size) / 2;
        if (DngMiniMapClipX1 >= 0) {
            wx = std::clamp(wx, DngMiniMapClipX0, DngMiniMapClipX1 - size);
            wy = std::clamp(wy, DngMiniMapClipY0, DngMiniMapClipY1 - size);
        }
        spRGBA flag = {240, 70, 60, 0x80}, pole = {250, 240, 230, 0x80};
        set2DSpriteC4(Vif1Packet, CRect_i_(wx, wy, size, size), &flag, &flag, &flag, &flag);
        set2DSpriteC4(Vif1Packet, CRect_i_(wx + size / 2 - 1, wy + size / 2 - 1, 2, 2), &pole, &pole, &pole, &pole);
    }
    pos[0] *= DNG_MAP_SCALE;
    pos[2] *= DNG_MAP_SCALE;
    player_src.x = 88;
    player_src.y = 96;
    player_src.width = 12;
    player_src.height = 12;
    map_x = (int) (pos[2] + float(DNG_MAP_Y) + DNG_MAP_TILE*0.5f);
    player_screen_x = pos[0] + float(DNG_MAP_X) + DNG_MAP_TILE*0.5f;
    map_y = (int) player_screen_x;
    player_dst.width = std::max(8,DNG_MAP_TILE*3/4);
    player_dst.height = std::max(8,DNG_MAP_TILE*3/4);
    player_dst.x = (int) std::lround(player_screen_x - player_dst.width * 0.5f);
    player_dst.y = (int) std::lround(map_x - player_dst.height * 0.5f);
    set2DSprite(Vif1Packet, symbol_texture, player_dst, player_src, 7, 7, angle);

    for (int i = 0; i < 3; i++) {
        int cell;

        if (this->trap_circle[i].state == 0) {
            continue;
        }

        map_x = (int) (this->trap_circle[i].pos[0] * DNG_MAP_SCALE);
        map_y = (int) (this->trap_circle[i].pos[2] * DNG_MAP_SCALE);
        cellx = (int) ((80.0f + this->trap_circle[i].pos[0]) / 160.0f);
        cell = cellx + (int) ((80.0f + this->trap_circle[i].pos[2]) / 160.0f) * DNG_GRID;
        if (cell<0 || cell>=DNG_CELLS) continue;
        show = this->mask[cell];

        if (BtEquipMasuisyou == 1) {
            show = 1;
        }

        if (this->cells[cell].parts_no != MAP_PARTS_NONE && show == 1) {
            trap_src.x = 88;
            trap_src.y = 112;
            trap_src.width = 8;
            trap_src.height = 8;
            trap_dst.width = std::max(6,DNG_MAP_TILE/2);
            trap_dst.height = std::max(6,DNG_MAP_TILE/2);
            trap_dst.x = map_x + DNG_MAP_X + (DNG_MAP_TILE - trap_dst.width) / 2;
            trap_dst.y = map_y + DNG_MAP_Y + (DNG_MAP_TILE - trap_dst.height) / 2;
            if (!DngMiniMapShows(trap_dst.x, trap_dst.y, trap_dst.width, trap_dst.height)) continue;
            set2DSprite(Vif1Packet, symbol_texture, trap_dst, trap_src, blue);
        }
    }

    for (int i = 0; i < DNG_BOX_MAX; i++) {
        int cell;

        if (this->boxes[i].used == 0 || this->boxes[i].closed == 0) {
            continue;
        }

        map_x = (int) (this->boxes[i].pos[0] * DNG_MAP_SCALE);
        map_y = (int) (this->boxes[i].pos[2] * DNG_MAP_SCALE);
        cellx = (int) ((80.0f + this->boxes[i].pos[0]) / 160.0f);
        cell = cellx + (int) ((80.0f + this->boxes[i].pos[2]) / 160.0f) * DNG_GRID;
        if (cell<0 || cell>=DNG_CELLS) continue;
        show = this->mask[cell];

        if (BtEquipMasuisyou == 1) {
            show = 1;
        }

        if (this->cells[cell].parts_no != MAP_PARTS_NONE && show == 1) {
            box_src.x = 80;
            box_src.y = 96;
            box_src.width = 8;
            box_src.height = 8;
            box_dst.width = std::max(6,DNG_MAP_TILE/2);
            box_dst.height = std::max(6,DNG_MAP_TILE/2);
            box_dst.x = map_x + DNG_MAP_X + (DNG_MAP_TILE - box_dst.width) / 2;
            box_dst.y = map_y + DNG_MAP_Y + (DNG_MAP_TILE - box_dst.height) / 2;
            if (!DngMiniMapShows(box_dst.x, box_dst.y, box_dst.width, box_dst.height)) continue;
            set2DSprite(Vif1Packet, symbol_texture, box_dst, box_src, blue);
        }
    }

    for (int i = 0; i < static_cast<int>(std::size(this->atra)); i++) { // MoonMod: every ball, not the game's first 8
        int cell;

        if (this->atra[i].used == 0) {
            continue;
        }

        map_x = (int) (this->atra[i].pos[0] * DNG_MAP_SCALE);
        map_y = (int) (this->atra[i].pos[2] * DNG_MAP_SCALE);
        cellx = (int) ((80.0f + this->atra[i].pos[0]) / 160.0f);
        cell = cellx + (int) ((80.0f + this->atra[i].pos[2]) / 160.0f) * DNG_GRID;
        if (cell<0 || cell>=DNG_CELLS) continue;
        show = this->mask[cell];

        if (BtEquipMasuisyou == 1) {
            show = 1;
        }

        if (this->cells[cell].parts_no != MAP_PARTS_NONE && show == 1) {
            atra_src.x = 72;
            atra_src.y = 104;
            atra_src.width = 8;
            atra_src.height = 8;
            atra_dst.width = std::max(6,DNG_MAP_TILE/2);
            atra_dst.height = std::max(6,DNG_MAP_TILE/2);
            atra_dst.x = map_x + DNG_MAP_X + (DNG_MAP_TILE - atra_dst.width) / 2;
            atra_dst.y = map_y + DNG_MAP_Y + (DNG_MAP_TILE - atra_dst.height) / 2;
            if (!DngMiniMapShows(atra_dst.x, atra_dst.y, atra_dst.width, atra_dst.height)) continue;
            set2DSprite(Vif1Packet, symbol_texture, atra_dst, atra_src, blue);
        }
    }

    BtEquipMap = saved_map;
    BtEquipMasuisyou = saved_crystal;
}

void CDungeonMap::checkMask(float x, float z) {
    int cell_x;
    int cell_z;
    int room;

    room = -1;
    cell_x = static_cast<int>((80.0f + x) / 160.0f);
    cell_z = static_cast<int>((80.0f + z) / 160.0f);

    // Find the room that holds the cell that the player stands in.
    for (int i = 0; i < this->room_num; i++) {
        if (cell_x >= this->rooms[i].x && cell_x < this->rooms[i].x + this->rooms[i].width) {
            if (cell_z >= this->rooms[i].y && cell_z < this->rooms[i].y + this->rooms[i].height) {
                room = i;
            }
        }
    }

    // Mark the entire room as seen when the player enters it for the first time.
    if (room != -1) {
        if (this->room_seen[room] == 0) {
            if (this->rooms[room].width != 0) {
                int col;
                int row;

                this->room_seen[room] = 1;

                for (row = this->rooms[room].y; row < this->rooms[room].y + this->rooms[room].height; row++) {
                    for (col = this->rooms[room].x; col < this->rooms[room].x + this->rooms[room].width; col++) {
                        this->mask[col + row * DNG_GRID] = 1;
                    }
                }
            }
        }
    }

    this->mask[cell_x + cell_z * DNG_GRID] = 1;
}

void CDungeonMap::FlushCheckMask() {
    int i;

    // Hide every cell of the mini map and forget every room that the player found.
    for (i = 0; i < DNG_CELLS; i++) {
        this->mask[i] = 0;
    }

    for (i = 0; i < DNG_ROOM_MAX; i++) {
        this->room_seen[i] = 0;
    }

    // The entrance should always show on the mini map; the walk-through exit room's stair is not the entrance.
    for (i = 0; i < DNG_CELLS; i++) {
        if (this->cells[i].parts_no == MAP_PARTS_STAIR_DOWN && !DNG_IN_EXIT_ROOM(i % DNG_GRID, i / DNG_GRID)) {
            this->mask[i] = 1;
        }
    }
}

void CDungeonMap::DrawFireFreeStyle(CFrameVu1 *frame, CCameraFollow *camera) {
    float fire_pos[4];
    float part_pos[4];
    float cam_pos[4];
    float best_pos[4];
    float frame_pos[4];
    float vol;
    float pan;
    float near_dist;
    int   j;
    int   i;

    sceVu0CopyVector(frame_pos, frame->position);
    camera->GetPos(cam_pos);
    sceVu0ScaleVectorXYZ(cam_pos, cam_pos, 0.1f);

    this->fire.SetTexture(TexManager.GetTexture("lightling", -1), TexManager.GetTexture("blender", -1));
    this->fire.FireCreate();

    near_dist = 16000.0f;

    for (i = 0; this->parts[i].frame[0] != nullptr; i++) {
        sceVu0CopyVector(part_pos, this->parts[i].frame_offset[0]);

        for (j = 0; j < this->parts[i].fire_num; j++) {
            float angle;
            float turn;
            float rot_x;
            float rot_z;
            float dist;
            float world_z;
            float world_y;
            int   param;

            fire_pos[0] = this->parts[i].fire_pos[j][0];
            fire_pos[1] = this->parts[i].fire_pos[j][1];
            fire_pos[2] = this->parts[i].fire_pos[j][2];
            fire_pos[3] = 1.0f;
            param = this->parts[i].fire_param[j];

            // The point turns with the part that holds it.
            angle = this->parts[i].frame_turn[0];

            if (angle != 0.0f) {
                angle = (PI * (90.0f * (4.0f - angle))) / 180.0f;
                turn = -fire_pos[2] * sinf(angle);
                rot_x = turn - fire_pos[0] * cosf(angle);
                turn = -fire_pos[0] * sinf(angle);
                rot_z = turn + fire_pos[2] * cosf(angle);
                rot_x *= -1.0f;
            } else {
                rot_x = fire_pos[0];
                rot_z = fire_pos[2];
            }

            fire_pos[0] = part_pos[0] / 10.0f + rot_x;
            fire_pos[1] += part_pos[1] / 10.0f;
            fire_pos[2] = world_z = part_pos[2] / 10.0f + rot_z;
            world_y = fire_pos[1];

            this->fire.pos[0] = 10.0f * fire_pos[0];
            this->fire.pos[1] = 10.0f * world_y;
            this->fire.pos[2] = 10.0f * world_z;
            this->fire.pos[3] = 1.0f;
            this->fire.DrawFire(1, 1, camera, frame_pos, 1.0f, param, 15.0f);

            // The crackle comes from the fire that is nearest to the camera.
            dist = DistVector(cam_pos, fire_pos);

            if (!(near_dist < dist)) {
                near_dist = dist;
                sceVu0CopyVector(best_pos, fire_pos);
                sceVu0ScaleVectorXYZ(best_pos, best_pos, 10.0f);
            }
        }
    }

    if (near_dist <= 30.0f) {
        SndGetVolPan(&vol, &pan, best_pos, 10.0f, 500.0f);
        SndSetSeVolf(SE_AMBIENT_CAVE_DRIPS, vol, 0);
        SndSetSePanf(53, pan, 0);
    } else {
        SndSetSeVol(53, 0, 0);
    }
}

void CDungeonMap::DrawFire(CFrameVu1 *frame, CCameraFollow *camera) {
    float fire_pos[4];
    float frame_pos[4];
    float cam_near[4];
    float cam_pos[4];
    float best_pos[4];
    float frame_near[4];
    float vol;
    float pan;
    float near_dist;
    int   cam_col;
    int   cam_row;
    int   j;
    int   col;
    int   row;
    int   param;

    sceVu0CopyVector(frame_pos, frame->position);
    camera->GetPos(cam_pos);
    camera->GetPos(cam_near);
    sceVu0ScaleVectorXYZ(cam_near, cam_near, 0.1f);

    this->fire.SetTexture(TexManager.GetTexture("lightling", -1), TexManager.GetTexture("blender", -1));
    this->fire.FireCreate();

    sceVu0CopyVector(frame_near, frame->position);
    frame_near[0] /= 10.0f;
    frame_near[1] /= 10.0f;
    frame_near[2] /= 10.0f;

    cam_col = static_cast<int>(cam_pos[0] / 160.0f);
    cam_row = static_cast<int>(cam_pos[2] / 160.0f);
    near_dist = 16000.0f;

    // Only the cells around the camera show their fires.
    for (row = cam_row - 5; row < cam_row + 5; row++) {
        for (col = cam_col - 5; col < cam_col + 5; col++) {
            if (row >= 0 && row < DNG_GRID && col >= 0 && col < DNG_GRID) {
                int cell_no = col + row * DNG_GRID;

                if (this->cells[cell_no].parts_no != MAP_PARTS_NONE && this->cells[cell_no].camera_dist <= 160.0f * (3.0f + this->draw_dist_scale)) {
                    CDungeonParts *part;

#ifdef PORT
                    // DrawMap already decided which cells are on screen and not behind walls;
                    // their torches are the only ones worth drawing (large floors have hundreds).
                    if (!this->cells[cell_no].visible) {
                        continue;
                    }
#endif
                    for (j = 0; j < (part = &this->parts[this->cells[cell_no].parts_no])->fire_num; j++) {
                        float turn;
                        float rot_x;
                        float rot_z;
                        float height;
                        float angle;
                        float dist;

                        fire_pos[0] = part->fire_pos[j][0];
                        fire_pos[1] = part->fire_pos[j][1];
                        fire_pos[2] = part->fire_pos[j][2];
                        fire_pos[3] = 1.0f;
                        param = part->fire_param[j];

                        // The point turns with the part that holds it.
                        angle = (PI * static_cast<float>((4 - this->cells[cell_no].direction) * 90)) / 180.0f;

                        if (this->cells[cell_no].direction != 0) {
                            turn = -fire_pos[2] * sinf(angle);
                            rot_x = turn - fire_pos[0] * cosf(angle);
                            turn = -fire_pos[0] * sinf(angle);
                            rot_z = turn + fire_pos[2] * cosf(angle);
                            rot_x *= -1.0f;
                        } else {
                            rot_x = fire_pos[0];
                            rot_z = fire_pos[2];
                        }

                        fire_pos[0] = rot_x + 16.0f * static_cast<float>(col);
                        rot_z += 16.0f * static_cast<float>(row);
                        fire_pos[2] = rot_z;
                        height = fire_pos[1];

                        this->fire.pos[0] = 10.0f * fire_pos[0];
                        this->fire.pos[1] = 10.0f * height;
                        this->fire.pos[2] = 10.0f * rot_z;
                        this->fire.pos[3] = 1.0f;
                        this->fire.DrawFire(1, 1, camera, frame_pos, 1.0f, param, 15.0f);

                        // The crackle comes from the fire that is nearest to the camera.
                        dist = DistVector(cam_near, fire_pos);

                        if (!(near_dist < dist)) {
                            near_dist = dist;
                            sceVu0CopyVector(best_pos, fire_pos);
                            sceVu0ScaleVectorXYZ(best_pos, best_pos, 10.0f);
                        }
                    }
                }
            }
        }
    }

    if (near_dist <= 30.0f) {
        SndGetVolPan(&vol, &pan, best_pos, 10.0f, 500.0f);
        SndSetSeVolf(SE_AMBIENT_CAVE_DRIPS, vol, 0);
        SndSetSePanf(53, pan, 0);
    } else {
        SndSetSeVol(53, 0, 0);
    }
}

void CDungeonMap::DrawRaster(CFrameVu1 *frame) {
    float pos[4];
    int   i;
    int   col;
    int   row;
    int   cell_z;
    int   cell_x;

    sceVu0CopyVector(pos, frame->position);
    cell_x = static_cast<int>(pos[0] / 160.0f);
    cell_z = static_cast<int>(pos[2] / 160.0f);

    // Only the cells around the player cast a raster.
    for (row = cell_z - 4; row < cell_z + 4; row++) {
        for (col = cell_x - 4; col < cell_x + 4; col++) {
            if (row >= 0 && row < DNG_GRID && col >= 0 && col < DNG_GRID) {
                int cell_no = col + row * DNG_GRID;

                if (this->cells[cell_no].parts_no != MAP_PARTS_NONE && this->cells[cell_no].camera_dist <= 240.0f && this->cells[cell_no].visible == 1) {
                    CDungeonParts *part;

                    for (i = 0; i < (part = &this->parts[this->cells[cell_no].parts_no])->fire_num; i++) {
                        float x;
                        float y;
                        float z;
                        float rot_x;
                        float rot_z;
                        float angle;
                        int   dir;

                        x = part->fire_pos[i][0];
                        y = part->fire_pos[i][1];
                        z = part->fire_pos[i][2];
                        // The point turns with the part that holds it.
                        dir = this->cells[cell_no].direction;
                        angle = (PI * static_cast<float>((4 - dir) * 90)) / 180.0f;

                        if (dir != 0) {
                            rot_x = -z * sinf(angle) - x * cosf(angle);
                            rot_z = -x * sinf(angle) + z * cosf(angle);
                            rot_x *= -1.0f;
                        } else {
                            rot_x = x;
                            rot_z = z;
                        }

                        x = rot_x + 16.0f * static_cast<float>(col);
                        z = rot_z + 16.0f * static_cast<float>(row);

                        this->fire.pos[0] = 10.0f * x;
                        this->fire.pos[1] = 10.0f * y;
                        this->fire.pos[2] = 10.0f * z;
                        this->fire.pos[3] = 1.0f;
                        this->fire.DrawRaster();
                    }
                }
            }
        }
    }
}

void CDungeonMap::DrawWater(float *pos, int mute) {
    static int  wait;
    static char init;

    float          vec[4];
    float          best_pos[4];
    float          vol;
    float          pan;
    float          near_dist;
    float          sound_dist;
    int            shake;
    int            col;
    int            row;
    PARTS_WATER   *best;
    PARTS_WATER   *water;
    CDungeonParts *part;
    int            best_col;
    int            best_row;

    best = nullptr;
    near_dist = 320.0f;
    sound_dist = 300.0f;
    shake = false;

    // The surface only shakes once every thirty frames.
    if (init == 0) {
        wait = 0;
        init = 1;
    }

    if (wait >= 30) {
        wait = 0;
        shake = true;
    } else {
        wait++;
    }

    // One surface draws for the whole floor: the one nearest to the player.
    for (row = 0; row < DNG_ROOM_AREA; row++) {
        for (col = 0; col < DNG_ROOM_AREA; col++) {
            int   cell_no = col + row * DNG_GRID;
            float dist;

            if (this->cells[cell_no].parts_no == MAP_PARTS_NONE) {
                continue;
            }

            part = &this->parts[this->cells[cell_no].parts_no];
            water = &part->water;

            if (this->parts[this->cells[cell_no].parts_no].water.used == 0) {
                continue;
            }

            vec[0] = 160.0f * static_cast<float>(col);
            vec[1] = water->vertex[0][1];
            vec[2] = 160.0f * static_cast<float>(row);
            dist = DistVector(pos, vec);

            if (dist < near_dist) {
                best = water;
                near_dist = dist;
                best_pos[0] = vec[0];
                best_pos[1] = vec[1];
                best_pos[2] = vec[2];
                best_col = col;
                best_row = row;
            }

            if (sound_dist >= dist) {
                sound_dist = dist;
            }
        }
    }

    if (best != nullptr) {
        this->water.SetVertex(best->vertex[0], best->vertex[1], best->vertex[2], best->vertex[3]);
        this->water.frame.SetPosition(best_pos);
        u8 red = best->red;
        u8 green = best->green;
        u8 blue = best->blue;

        this->water.SetColor(red, green, blue, 0x80);

        if (shake != 0) {
            this->water.SetParam(0.12f, 0.018f, 0.0f, 11.0f);
            this->water.Shake(16, 4, 2.15f);
        }

        this->water.Hamon();
        DrawVu1__6CWaterFP10RenderInfoP13sceVif1PacketP1(&this->water, &mgRenderInfo, Vif1Packet, nullptr);
    }

    if (best != nullptr) {

        // A waterfall stands at the cell that the surface came from.
        if (best->has_fall != 0 && this->fall_model != nullptr) {
            float x = 160.0f * static_cast<float>(best_col);
            float z = 160.0f * static_cast<float>(best_row);

            this->fall_model->SetPosition(x, 0.0f, z);
            MGDraw(this->fall_model);
        }

        if (sound_dist < 300.0f) {
            SndGetVolPan(&vol, &pan, best_pos, 50.0f, 300.0f);

            if (mute != 0) {
                vol = 0.0f;
            }

            SndSetSeVolf(SE_AMBIENT_DUNGEON_HISS, vol, 0);
            SndSetSePanf(61, pan, 0);
        } else {
            SndSetSeVol(61, 0, 0);
        }
    } else {
        float vol;
        float pan;

        if (sound_dist < 300.0f) {
            SndGetVolPan(&vol, &pan, best_pos, 50.0f, 300.0f);

            if (mute != 0) {
                vol = 0.0f;
            }

            SndSetSeVolf(SE_AMBIENT_TOWN_NOISE, vol, 0);
            SndSetSePanf(52, pan, 0);
        } else {
            SndSetSeVol(52, 0, 0);
        }
    }
}

void CDungeonMap::DrawItemBox(float *pos) {
    float lid[4];
    int   i;
    float angle;

    if (this->box_lid_model == nullptr || this->box_body_model == nullptr) {
        return;
    }

    if (this->chest_body_model == nullptr || this->chest_lid_model == nullptr) {
        return;
    }

    for (i = 0; i < DNG_BOX_MAX; i++) {
        if (this->boxes[i].used == 0) {
            continue;
        }

        if (!(DistVector(this->boxes[i].pos, pos) <= 160.0f * this->draw_dist_scale)) {
            continue;
        }

        // The lid sits on the box, tilted by however far it has opened.
        switch (this->boxes[i].kind) {
            case TREASURE_BOX_LARGE:
                this->box_body_model->SetPosition(this->boxes[i].pos);
                sceVu0CopyVector(lid, this->box_body_model->position);
                lid[1] += 8.0f;
                lid[2] -= 5.0f;
                this->box_lid_model->SetPosition(lid);
                angle = this->boxes[i].lid_angle;
                this->box_lid_model->SetRotation((PI * angle) / 180.0f, 0.0f, 0.0f);
                MGDraw(this->box_body_model);
                MGDraw(this->box_lid_model);
                break;
            case TREASURE_BOX_SMALL:
                this->chest_body_model->SetPosition(this->boxes[i].pos);
                sceVu0CopyVector(lid, this->chest_body_model->position);
                lid[1] += 3.0f;
                lid[2] -= 3.0f;
                this->chest_lid_model->SetPosition(lid);
                angle = this->boxes[i].lid_angle;
                this->chest_lid_model->SetRotation((PI * angle) / 180.0f, 0.0f, 0.0f);
                MGDraw(this->chest_body_model);
                MGDraw(this->chest_lid_model);
                break;
        }
    }
}

// Moon Sea places Atla across an open field, so distance from the player does not hide them.
void CDungeonMap::DrawAtraBoll(float *pos) {
    float draw_pos[4];

    if (this->atra_model == nullptr) {
        return;
    }

    if (this->atra_num < 0 || this->atra_num > static_cast<int>(std::size(this->atra))) {
        return;
    }

    for (int i = 0; i < this->atra_num; i++) {
        if (this->atra[i].used != 0 &&
            (selectMapNo == DUNGEON_MOON_SEA || DistVector(this->atra[i].pos, pos) <= kCellSize * this->draw_dist_scale)) {
            sceVu0CopyVector(draw_pos, this->atra[i].pos);
            draw_pos[1] += std::sin(this->atra[i].phase);
            this->atra_model->SetPosition(draw_pos);
            MGDraw(this->atra_model);

            if (this->atra[i].phase <= 360.0f) {
                this->atra[i].phase += 0.1f;
            } else {
                this->atra[i].phase = 0.0f;
            }
        }
    }
}

int CDungeonMap::CreateCollision(CCPoly *poly, CBoxVu0 box, int num) {
    float   pos[4];
    CFrame *frame;

    sceVu0CopyVector(pos, CharaMain.frame->position);
    frame = this->collision_model;

    // Deliberately returns no value, so the count that the caller gets is undefined.
    if (frame == nullptr) {
        return;
    }

    for (int i = 0; i < this->atra_num; i++) {
        if (this->atra[i].used != 0) {
            if (DistVector(this->atra[i].pos, pos) <= 400.0f) {
                frame->SetPosition(this->atra[i].pos);
                num += frame->PickUpNearPoly(&poly[num], box);
            }
        }
    }

    return num;
}

// The builder reads and writes up to a row past either end of these grids, so
// they stay plain globals with external linkage, defined in this order.

/** Labels each room while a special linked room is selected. */
#ifdef PORT
s32 scanRoomInfoWork[DNG_ROOM_AREA * DNG_ROOM_AREA];
#else
s32 scanRoomInfoWork[DNG_ROOM_AREA * DNG_ROOM_AREA];
#endif

/** Gives the DNG_GRID x DNG_GRID grid that the map builder works on. */
BUILD_MAP_INFO buildMapDat[DNG_CELLS];

/** Takes a corridor that joinRoom lays out before it is committed. */
BUILD_MAP_INFO buildMapWork[DNG_CELLS];

/** Gives the position and the extent of each room that the builder placed. */
ROOM_INFO roomStack[DNG_ROOM_MAX];

/** Gives the number of rooms that the builder placed. */
s32 roomStackCnt;

namespace {

int buildRoom(int x, int y, int w, int h, int room_no) {
    int ok = 1;
    int i;
    int j;

    // The room needs its own cells and a border of one cell around them, so
    // that two rooms never come out sharing a wall.
    for (j = -1; j < h + 2; j++) {
        for (i = -1; i < w + 2; i++) {
            if (i + x >= 0 && i + x < DNG_GRID && j + y >= 0 && j + y < DNG_GRID) {
                if (buildMapDat[x + (j + y) * DNG_GRID + i].kind != MAP_PARTS_NONE) {
                    ok = 0;
                }
            }
        }
    }

    if (!ok) {
        return ok;
    }

    for (j = 0; j < h; j++) {
        for (i = 0; i < w; i++) {
            buildMapDat[x + (y + j) * DNG_GRID + i].kind = MAP_PARTS_ROOM_FLOOR;
            buildMapDat[x + (y + j) * DNG_GRID + i].link[room_no] = 1;
            buildMapDat[x + (y + j) * DNG_GRID + i].content |= BUILD_CELL_ROOM;
        }
    }

    return 1;
}

} // namespace

#ifdef PORT
// Whether a cell marked as a doorway can be one: a corridor that reaches its room at a corner marks the corner, which
// has room cells on two sides and no door part fits it. It stays a corner wall, so the corridor must not open onto it
// (it opened onto the wall: a drop into nothing).
bool DoorFits(int cell) {
    const int content = buildMapDat[cell].content;
    if ((content & BUILD_CELL_DOOR) == 0 || (content & BUILD_CELL_ROOM_LINK) != 0) return true;
    const int col = cell % DNG_GRID, row = cell / DNG_GRID;
    int mask = 0;
    if (row > 0 && (buildMapDat[cell - DNG_GRID].content & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) mask |= 1;
    if (row < DNG_GRID_LAST && (buildMapDat[cell + DNG_GRID].content & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) mask |= 8;
    if (col < DNG_GRID_LAST && (buildMapDat[cell + 1].content & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) mask |= 2;
    if (col > 0 && (buildMapDat[cell - 1].content & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) mask |= 4;
    for (const auto &door : chainTableDoor) {
        if (door[0] == mask) return true;
    }
    return false;
}

// A corridor that reaches its room at a corner can't enter there (no door fits a corner). Its doorway moves one cell
// along the room's wall and the corridor turns one cell to meet it, when that cell is free. Without this the
// corridor ended against the corner and everything past it was cut off (MoonMod: a floor the Gauntlet built could
// not be finished).
void RepairCornerDoors() {
    static constexpr int kStep[4][2] = {{0, -1}, {1, 0}, {0, 1}, {-1, 0}};
    const auto inside = [](int col, int row) { return col > 0 && row > 0 && col < DNG_GRID_LAST && row < DNG_GRID_LAST; };
    for (int row = 1; row < DNG_GRID_LAST; ++row) {
        for (int col = 1; col < DNG_GRID_LAST; ++col) {
            const int corner = col + row * DNG_GRID;
            const int content = buildMapDat[corner].content;
            if ((content & (BUILD_CELL_ROOM | BUILD_CELL_DOOR)) != (BUILD_CELL_ROOM | BUILD_CELL_DOOR) || DoorFits(corner)) continue;
            bool moved = false;
            for (int v = 0; v < 4 && !moved; ++v) {
                const int rc = col + kStep[v][0], rr = row + kStep[v][1];
                if (!inside(rc, rr) || (buildMapDat[rc + rr * DNG_GRID].content & BUILD_CELL_CORRIDOR) == 0) continue;
                for (int u = 0; u < 4 && !moved; ++u) {
                    if ((u & 1) == (v & 1)) continue; // along the wall: perpendicular to the way the corridor comes in
                    const int wc = col + kStep[u][0], wr = row + kStep[u][1];
                    const int oc = rc + kStep[u][0], orow = rr + kStep[u][1];
                    if (!inside(wc, wr) || !inside(oc, orow)) continue;
                    BUILD_MAP_INFO &wall = buildMapDat[wc + wr * DNG_GRID];
                    BUILD_MAP_INFO &turn = buildMapDat[oc + orow * DNG_GRID];
                    const BUILD_MAP_INFO &road = buildMapDat[rc + rr * DNG_GRID];
                    if ((wall.content & BUILD_CELL_ROOM) == 0 || (wall.content & ~(BUILD_CELL_ROOM | BUILD_CELL_DOOR)) != 0) continue;
                    const bool free = turn.kind == MAP_PARTS_NONE && turn.content == 0;
                    if (!free && (turn.content & ~BUILD_CELL_CORRIDOR) != 0) continue;
                    const int wall_before = wall.content;
                    wall.content |= BUILD_CELL_DOOR;
                    if (!DoorFits(wc + wr * DNG_GRID)) {
                        wall.content = wall_before;
                        continue;
                    }
                    if (free) turn.kind = road.kind;
                    turn.content |= BUILD_CELL_CORRIDOR;
                    for (int r = 0; r < DNG_ROOM_MAX; ++r) {
                        if (road.link[r] == 1) {
                            turn.link[r] = 1;
                            wall.link[r] = 1;
                        }
                    }
                    buildMapDat[corner].content &= ~BUILD_CELL_DOOR;
                    moved = true;
                }
            }
        }
    }
}
#endif

void mapPartsFilter() {
    int col;
    int row;
    int room_no;
    int table_no;
    int north;
    int south;
    int east;
    int west;

    for (row = 0; row < DNG_GRID; row++) {
        for (col = 0; col < DNG_GRID; col++) {
            // A corridor cell joins every neighbour that shares one of its rooms.
            if ((buildMapDat[col + row * DNG_GRID].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) {
                int mask = 15;
                int north = buildMapDat[col + (row - 1) * DNG_GRID].content;
                int south = buildMapDat[col + (row + 1) * DNG_GRID].content;
                int east = buildMapDat[col + row * DNG_GRID + 1].content;
                int west = buildMapDat[col + row * DNG_GRID - 1].content;

                int allow_north = 1;

                if ((north & (BUILD_CELL_ROOM | BUILD_CELL_DOOR)) == BUILD_CELL_ROOM) {
                    allow_north = 0;
                }
#ifdef PORT
                if ((north & BUILD_CELL_ROOM) == BUILD_CELL_ROOM && !DoorFits(col + (row - 1) * DNG_GRID)) {
                    allow_north = 0;
                }
#endif

                if ((north & (BUILD_CELL_STAIR_DOWN | BUILD_CELL_STAIR_UP)) != 0 && buildMapDat[col + (row - 1) * DNG_GRID].direction != 0) {
                    allow_north = 0;
                }

                int allow_south = 1;

                if ((south & (BUILD_CELL_ROOM | BUILD_CELL_DOOR)) == BUILD_CELL_ROOM) {
                    allow_south = 0;
                }
#ifdef PORT
                if ((south & BUILD_CELL_ROOM) == BUILD_CELL_ROOM && !DoorFits(col + (row + 1) * DNG_GRID)) {
                    allow_south = 0;
                }
#endif

                if ((south & (BUILD_CELL_STAIR_DOWN | BUILD_CELL_STAIR_UP)) != 0 && buildMapDat[col + (row + 1) * DNG_GRID].direction != 2) {
                    allow_south = 0;
                }

                int allow_east = 1;

                if ((east & (BUILD_CELL_ROOM | BUILD_CELL_DOOR)) == BUILD_CELL_ROOM) {
                    allow_east = 0;
                }
#ifdef PORT
                if ((east & BUILD_CELL_ROOM) == BUILD_CELL_ROOM && !DoorFits(col + row * DNG_GRID + 1)) {
                    allow_east = 0;
                }
#endif

                if ((east & (BUILD_CELL_STAIR_DOWN | BUILD_CELL_STAIR_UP)) != 0 && buildMapDat[col + row * DNG_GRID + 1].direction != 1) {
                    allow_east = 0;
                }

                int allow_west = 1;

                if ((west & (BUILD_CELL_ROOM | BUILD_CELL_DOOR)) == BUILD_CELL_ROOM) {
                    allow_west = 0;
                }
#ifdef PORT
                if ((west & BUILD_CELL_ROOM) == BUILD_CELL_ROOM && !DoorFits(col + row * DNG_GRID - 1)) {
                    allow_west = 0;
                }
#endif

                if ((west & (BUILD_CELL_STAIR_DOWN | BUILD_CELL_STAIR_UP)) != 0 && buildMapDat[col + row * DNG_GRID - 1].direction != 3) {
                    allow_west = 0;
                }

                for (room_no = 0; room_no < DNG_ROOM_MAX; room_no++) {
                    if (buildMapDat[col + row * DNG_GRID].link[room_no] == 1) {
                        if (buildMapDat[col + (row - 1) * DNG_GRID].link[room_no] == 1 && allow_north != 0) {
                            mask &= ~1;
                        }

                        if (buildMapDat[col + (row + 1) * DNG_GRID].link[room_no] == 1 && allow_south != 0) {
                            mask &= ~8;
                        }

                        if (buildMapDat[col + row * DNG_GRID + 1].link[room_no] == 1 && allow_east != 0) {
                            mask &= ~2;
                        }

                        if (buildMapDat[col + row * DNG_GRID - 1].link[room_no] == 1 && allow_west != 0) {
                            mask &= ~4;
                        }
                    }
                }

                for (table_no = 0; table_no < 15; table_no++) {
                    if (mask == chainTableRoad[table_no][0]) {
                        buildMapDat[col + row * DNG_GRID].kind = chainTableRoad[table_no][1];
                        buildMapDat[col + row * DNG_GRID].direction = chainTableRoad[table_no][2];
                    }
                }
#ifdef PORT
                // A dense layout can leave a corridor cell that shares no room with any
                // neighbour (mask 15, which the table lacks). Its kind would stay the
                // room number, which is no part at all: a pit with no floor or collision.
                // Join it to neighbouring corridors and doors instead, or drop it.
                if (mask == 15) {
                    const int open = BUILD_CELL_CORRIDOR | BUILD_CELL_DOOR;
                    if (row > 0 && (north & open) != 0 && allow_north != 0) mask &= ~1;
                    if (row < DNG_GRID_LAST && (south & open) != 0 && allow_south != 0) mask &= ~8;
                    if (col < DNG_GRID_LAST && (east & open) != 0 && allow_east != 0) mask &= ~2;
                    if (col > 0 && (west & open) != 0 && allow_west != 0) mask &= ~4;
                    buildMapDat[col + row * DNG_GRID].kind = MAP_PARTS_NONE;
                    for (table_no = 0; table_no < 15; table_no++) {
                        if (mask == chainTableRoad[table_no][0]) {
                            buildMapDat[col + row * DNG_GRID].kind = chainTableRoad[table_no][1];
                            buildMapDat[col + row * DNG_GRID].direction = chainTableRoad[table_no][2];
                        }
                    }
                    if (buildMapDat[col + row * DNG_GRID].kind == MAP_PARTS_NONE) {
                        buildMapDat[col + row * DNG_GRID].content &= ~BUILD_CELL_CORRIDOR;
                    }
                }
#endif
            }

            // A room cell joins the neighbouring room cells.
            if ((buildMapDat[col + row * DNG_GRID].content & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) {
                int mask = 15;
                north = row != 0 ? buildMapDat[col + (row - 1) * DNG_GRID].content & BUILD_CELL_ROOM : 0;
                south = row != DNG_GRID_LAST ? buildMapDat[col + (row + 1) * DNG_GRID].content & BUILD_CELL_ROOM : 0;
                east = col != DNG_GRID_LAST ? buildMapDat[col + row * DNG_GRID + 1].content & BUILD_CELL_ROOM : 0;
                west = col != 0 ? buildMapDat[col + row * DNG_GRID - 1].content & BUILD_CELL_ROOM : 0;

                if (north == BUILD_CELL_ROOM) {
                    mask &= ~1;
                }

                if (south == BUILD_CELL_ROOM) {
                    mask &= ~8;
                }

                if (east == BUILD_CELL_ROOM) {
                    mask &= ~2;
                }

                if (west == BUILD_CELL_ROOM) {
                    mask &= ~4;
                }

                for (table_no = 0; table_no < 8; table_no++) {
                    if (mask == chainTableRoom[table_no][0]) {
                        buildMapDat[col + row * DNG_GRID].kind = chainTableRoom[table_no][1];
                        buildMapDat[col + row * DNG_GRID].direction = chainTableRoom[table_no][2];
                    }
                }
            }

            // A door faces the room cell beside it.
            if ((buildMapDat[col + row * DNG_GRID].content & (BUILD_CELL_DOOR | BUILD_CELL_ROOM_LINK)) == BUILD_CELL_DOOR) {
                int mask = 0;

                if ((buildMapDat[col + (row - 1) * DNG_GRID].content & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) {
                    mask |= 1;
                }

                if ((buildMapDat[col + (row + 1) * DNG_GRID].content & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) {
                    mask |= 8;
                }

                if ((buildMapDat[col + row * DNG_GRID + 1].content & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) {
                    mask |= 2;
                }

                if ((buildMapDat[col + row * DNG_GRID - 1].content & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) {
                    mask |= 4;
                }

                for (table_no = 0; table_no < 4; table_no++) {
                    if (mask == chainTableDoor[table_no][0]) {
                        buildMapDat[col + row * DNG_GRID].kind = chainTableDoor[table_no][1];
                    }
                }
            }

            if ((buildMapDat[col + row * DNG_GRID].content & BUILD_CELL_HEAL_SPRING) == BUILD_CELL_HEAL_SPRING) {
                buildMapDat[col + row * DNG_GRID].kind = MAP_PARTS_HEAL_SPRING_NORTH;
                buildMapDat[col + row * DNG_GRID].direction = 0;
            }

            if ((buildMapDat[col + row * DNG_GRID].content & BUILD_CELL_PILLAR) == BUILD_CELL_PILLAR) {
                if ((100.0f * static_cast<float>(rand())) / 2147483648.0f <= 50.0f) {
                    buildMapDat[col + row * DNG_GRID].kind = MAP_PARTS_PILLAR_1;
                } else {
                    buildMapDat[col + row * DNG_GRID].kind = MAP_PARTS_PILLAR_2;
                }

                buildMapDat[col + row * DNG_GRID].direction = 0;
            }

            // A dividing wall joins the walls and rooms around it.
            if ((buildMapDat[col + row * DNG_GRID].content & BUILD_CELL_DIVIDING_WALL) == BUILD_CELL_DIVIDING_WALL) {
                int mask = 0;

                north = south = east = west = 0;

                if (row != 0) {
                    north = buildMapDat[col + (row - 1) * DNG_GRID].content;
                }

                if (row != DNG_GRID_LAST) {
                    south = buildMapDat[col + (row + 1) * DNG_GRID].content;
                }

                if (col != DNG_GRID_LAST) {
                    east = buildMapDat[col + row * DNG_GRID + 1].content;
                }

                if (col != 0) {
                    west = buildMapDat[col + row * DNG_GRID - 1].content;
                }

                if ((north & BUILD_CELL_DIVIDING_WALL) == BUILD_CELL_DIVIDING_WALL) {
                    mask |= 1;
                }

                if ((south & BUILD_CELL_DIVIDING_WALL) == BUILD_CELL_DIVIDING_WALL) {
                    mask |= 8;
                }

                if ((east & BUILD_CELL_DIVIDING_WALL) == BUILD_CELL_DIVIDING_WALL) {
                    mask |= 2;
                }

                if ((west & BUILD_CELL_DIVIDING_WALL) == BUILD_CELL_DIVIDING_WALL) {
                    mask |= 4;
                }

                if ((north & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) {
                    mask |= 0x10;
                }

                if ((south & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) {
                    mask |= 0x80;
                }

                if ((east & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) {
                    mask |= 0x20;
                }

                if ((west & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) {
                    mask |= 0x40;
                }

                for (table_no = 0; table_no < 6; table_no++) {
                    if (mask == chainTableDivid[table_no][0]) {
                        buildMapDat[col + row * DNG_GRID].kind = chainTableDivid[table_no][1];
                    }
                }
            }

            if ((buildMapDat[col + row * DNG_GRID].content & BUILD_CELL_DIVIDING_DOOR) == BUILD_CELL_DIVIDING_DOOR) {
                int mask = 0;

                north = south = east = west = 0;

                if (row != 0) {
                    north = buildMapDat[col + (row - 1) * DNG_GRID].content;
                }

                if (row != DNG_GRID_LAST) {
                    south = buildMapDat[col + (row + 1) * DNG_GRID].content;
                }

                if (col != DNG_GRID_LAST) {
                    east = buildMapDat[col + row * DNG_GRID + 1].content;
                }

                if (col != 0) {
                    west = buildMapDat[col + row * DNG_GRID - 1].content;
                }

                if ((north & BUILD_CELL_DIVIDING_WALL) == BUILD_CELL_DIVIDING_WALL) {
                    mask |= 1;
                }

                if ((south & BUILD_CELL_DIVIDING_WALL) == BUILD_CELL_DIVIDING_WALL) {
                    mask |= 8;
                }

                if ((east & BUILD_CELL_DIVIDING_WALL) == BUILD_CELL_DIVIDING_WALL) {
                    mask |= 2;
                }

                if ((west & BUILD_CELL_DIVIDING_WALL) == BUILD_CELL_DIVIDING_WALL) {
                    mask |= 4;
                }

                for (table_no = 0; table_no < 2; table_no++) {
                    if (mask == chainTableDividDoor[table_no][0]) {
                        buildMapDat[col + row * DNG_GRID].kind = chainTableDividDoor[table_no][1];
                    }
                }
            }

            if ((buildMapDat[col + row * DNG_GRID].content & BUILD_CELL_STAIR_DOWN) == BUILD_CELL_STAIR_DOWN) {
                buildMapDat[col + row * DNG_GRID].kind = MAP_PARTS_STAIR_DOWN;
            }

            if ((buildMapDat[col + row * DNG_GRID].content & BUILD_CELL_STAIR_UP) == BUILD_CELL_STAIR_UP) {
                buildMapDat[col + row * DNG_GRID].kind = MAP_PARTS_STAIR_UP;
            }

            if ((buildMapDat[col + row * DNG_GRID].content & BUILD_CELL_URA_ENTRANCE) == BUILD_CELL_URA_ENTRANCE) {
                buildMapDat[col + row * DNG_GRID].kind = MAP_PARTS_URA_ENTRANCE_NORTH;
            }
        }
    }
}

namespace {

void copyMapInfo(BUILD_MAP_INFO *dst, const BUILD_MAP_INFO *src) {
    std::copy_n(src, DNG_CELLS, dst);
}

int joinRoom(int from_room, int to_room);

// Shortcut doors (moonmod/dungeonmap_layout.hpp): the rooms the builder joined, and the shortcuts it planned.
std::vector<std::pair<int, int>> shortcutJoins;
struct ShortcutPlan {
    int room, door_x, door_y;
};
std::vector<ShortcutPlan> shortcutPlans;
BUILD_MAP_INFO shortcutBackup[DNG_CELLS];
// The Warden room the floor was given (BuildWardenRoom), or -1.
int wardenRoom = -1;
// Zones (dungeons.set_zones): on, the zone each room is in, the gates the builder planned, the corridor cells that lead
// up to a gate from the zone before it, and whether the floor failed to come out as zones (it is built again).
bool zonesOn = false;
int  roomZone[DNG_ROOM_MAX];
struct ZoneGatePlan {
    int zone, room, door_x, door_y;
};
std::vector<ZoneGatePlan> zoneGatePlans;
std::vector<char> zoneGateCorridor(DNG_CELLS, 0);
bool zonesFailed = false;

// The corner of a zone's square: a snake two squares wide (east, south, west, south, ...), so each zone shares a side
// with the one before; a map of more than eight zones (a whole dungeon) snakes five squares wide.
int ZoneSnake() { return DngZoneCount > 8 ? 5 : 2; }
int ZoneOriginX(int zone) {
    const int w = ZoneSnake(), row = zone / w, col = zone % w;
    return (row % 2 == 0 ? col : w - 1 - col) * DNG_BUILD_AREA;
}
int ZoneOriginY(int zone) { return (zone / ZoneSnake()) * DNG_BUILD_AREA; }
// True when the zones' squares fit on the grid.
bool ZonesFit(int zones) {
    const int w = ZoneSnake();
    return w * DNG_BUILD_AREA <= DNG_GRID - 1 && ((zones + w - 1) / w) * DNG_BUILD_AREA <= DNG_GRID - 1;
}
// The zone the room numbered `room` is built in.
int ZoneOfRoom(int room) {
    for (int z = 0, end = 0; z < DngZoneCount; ++z) {
        end += DngZoneRooms[z];
        if (room < end) return z;
    }
    return DngZoneCount - 1;
}

int joinRoomRecorded(int from_room, int to_room) {
    shortcutJoins.push_back({from_room, to_room});
    return joinRoom(from_room, to_room);
}

// A room's doorways: the door cells inside it that its own corridors use.
std::vector<std::pair<int, int>> RoomDoors(int room) {
    std::vector<std::pair<int, int>> doors;
    const ROOM_INFO &r = roomStack[room];
    for (int y = r.y - 1; y <= r.y + r.height; ++y) {
        for (int x = r.x - 1; x <= r.x + r.width; ++x) {
            if (x < 0 || y < 0 || x >= DNG_GRID || y >= DNG_GRID) continue;
            const BUILD_MAP_INFO &cell = buildMapDat[x + y * DNG_GRID];
            if ((cell.content & BUILD_CELL_DOOR) != 0 && (cell.content & BUILD_CELL_ROOM) != 0 && cell.link[room] == 1) {
                doors.push_back({x, y});
            }
        }
    }
    return doors;
}

// Which side of a room a doorway cell is on: 0 north, 1 south, 2 west, 3 east.
int DoorSide(int room, int x, int y) {
    const ROOM_INFO &r = roomStack[room];
    if (y <= r.y) return 0;
    if (y >= r.y + r.height - 1) return 1;
    if (x <= r.x) return 2;
    return 3;
}

// Each dead end (a room with one doorway) joined to a room at least three joins away, leaving by another side.
void PlanShortcuts(int wanted) {
    shortcutPlans.clear();
    if (wanted <= 0 || roomStackCnt < 4) return;
    std::vector<std::vector<int>> next(roomStackCnt);
    for (const auto &[a, b] : shortcutJoins) {
        if (a >= 0 && b >= 0 && a < roomStackCnt && b < roomStackCnt) { next[a].push_back(b); next[b].push_back(a); }
    }
    for (int dead = 0; dead < roomStackCnt && static_cast<int>(shortcutPlans.size()) < wanted; ++dead) {
        if (dead == wardenRoom) continue;
        const std::vector<std::pair<int, int>> before = RoomDoors(dead);
        if (before.size() != 1) continue;
        const int old_side = DoorSide(dead, before[0].first, before[0].second);
        // join distances from the dead end
        std::vector<int> hops(roomStackCnt, -1);
        std::vector<int> queue = {dead};
        hops[dead] = 0;
        for (std::size_t q = 0; q < queue.size(); ++q) {
            for (int n : next[queue[q]]) if (hops[n] < 0) { hops[n] = hops[queue[q]] + 1; queue.push_back(n); }
        }
        std::vector<std::pair<int, int>> far; // (distance between middles, room)
        const auto middle = [](int room, int axis) { return axis == 0 ? roomStack[room].x + roomStack[room].width / 2 : roomStack[room].y + roomStack[room].height / 2; };
        for (int r = 0; r < roomStackCnt; ++r) {
            if (r == dead || r == wardenRoom || hops[r] < 3 || (zonesOn && roomZone[r] != roomZone[dead])) continue;
            const int dx = middle(r, 0) - middle(dead, 0), dy = middle(r, 1) - middle(dead, 1);
            far.push_back({dx * dx + dy * dy, r});
        }
        std::ranges::sort(far);
        for (std::size_t t = 0; t < far.size() && t < 4; ++t) {
            copyMapInfo(shortcutBackup, buildMapDat);
            joinRoom(dead, far[t].second);
            ShortcutPlan plan{-1, -1, -1};
            for (const auto &[x, y] : RoomDoors(dead)) {
                if (std::ranges::find(before, std::make_pair(x, y)) == before.end() && DoorSide(dead, x, y) != old_side) plan = {dead, x, y};
            }
            if (plan.room >= 0) {
                shortcutJoins.push_back({dead, far[t].second});
                shortcutPlans.push_back(plan);
                break;
            }
            copyMapInfo(buildMapDat, shortcutBackup); // the corridor came back to its own side: try the next room
        }
    }
}

// MoonMod zones (dungeons.set_zones): each zone after the first is joined to the one before by one corridor, dug last
// from a room of the zone toward a room of the zone before, nearest pairs first. A corridor stops at the first cell
// already dug, so it is kept only when that cell is not the zone's own: the zone is then reached only through it, and its
// doorway in the zone's room becomes the gate. False when a zone could not be joined.
bool JoinZones() {
    std::ranges::fill(zoneGateCorridor, 0);
    zoneGatePlans.clear();
    const auto middle = [](int room, int axis) { return axis == 0 ? roomStack[room].x + roomStack[room].width / 2 : roomStack[room].y + roomStack[room].height / 2; };
    for (int zone = 1; zone < DngZoneCount; ++zone) {
        std::vector<std::pair<int, std::pair<int, int>>> pairs; // (distance, (room in the zone, room before it))
        for (int b = 0; b < roomStackCnt; ++b) {
            if (roomZone[b] != zone) continue;
            for (int a = 0; a < roomStackCnt; ++a) {
                if (roomZone[a] != zone - 1) continue;
                const int dx = middle(a, 0) - middle(b, 0), dy = middle(a, 1) - middle(b, 1);
                pairs.push_back({dx * dx + dy * dy, {b, a}});
            }
        }
        std::ranges::sort(pairs);
        bool joined = false;
        for (std::size_t t = 0; t < pairs.size() && t < 8 && !joined; ++t) {
            const auto [b, a] = pairs[t].second;
            copyMapInfo(shortcutBackup, buildMapDat);
            const std::vector<std::pair<int, int>> before = RoomDoors(b);
            joinRoom(b, a);
            // the cell it stopped at: one that was dug before and is now linked to the zone's room
            bool own = false;
            std::vector<int> dug;
            for (int cell = 0; cell < DNG_CELLS; ++cell) {
                if (shortcutBackup[cell].content == 0 && buildMapDat[cell].content != 0) dug.push_back(cell);
                if (shortcutBackup[cell].content == 0 || shortcutBackup[cell].link[b] != 0 || buildMapDat[cell].link[b] == 0) continue;
                for (int r = 0; r < roomStackCnt; ++r) own = own || (roomZone[r] == zone && shortcutBackup[cell].link[r] != 0);
            }
            ZoneGatePlan plan{zone, -1, -1, -1};
            for (const auto &[x, y] : RoomDoors(b)) {
                if (std::ranges::find(before, std::make_pair(x, y)) == before.end()) plan = {zone, b, x, y};
            }
            if (own || plan.room < 0) {
                copyMapInfo(buildMapDat, shortcutBackup);
                continue;
            }
            for (const int cell : dug) zoneGateCorridor[cell] = 1;
            shortcutJoins.push_back({b, a});
            zoneGatePlans.push_back(plan);
            joined = true;
        }
        if (!joined) return false;
    }
    return true;
}

// MoonMod rest floor (dungeons.set_rest_room): a 6x6 hall and a 4x4 antechamber level with it, three cells apart,
// so the corridor that joins them runs straight between the middles of their facing walls.
void BuildRestRooms() {
    static constexpr ROOM_INFO kRooms[2] = {{3, 3, 6, 6}, {12, 4, 4, 4}};
    for (const ROOM_INFO &r : kRooms) {
        if (buildRoom(r.x, r.y, r.width, r.height, roomStackCnt) != 1) continue;
        roomStack[roomStackCnt++] = r;
    }
}

// The rest floor's spring (the hall's top wall) and back floor stairs (the antechamber's), in place of the random
// room objects and the random back floor entrance.
void SetRestObjects() {
    const ROOM_INFO &hall = roomStack[0];
    const ROOM_INFO &ante = roomStack[1];
    DngRestSpringX = hall.x + hall.width / 2;
    DngRestSpringY = hall.y;
    buildMapDat[DngRestSpringX + DngRestSpringY * DNG_GRID].content |= BUILD_CELL_HEAL_SPRING;
    DngRestStairsX = ante.x + ante.width / 2;
    DngRestStairsY = ante.y;
    BUILD_MAP_INFO &stairs = buildMapDat[DngRestStairsX + DngRestStairsY * DNG_GRID];
    stairs.content = BUILD_CELL_ROOM | BUILD_CELL_URA_ENTRANCE;
    stairs.kind = MAP_PARTS_ROOM_GATE_NORTH;
    stairs.direction = 0;
    DngRestHallX = hall.x;
    DngRestHallY = hall.y;
    DngRestHallW = hall.width;
    DngRestHallH = hall.height;
}

// MoonMod Warden room (dungeons.set_warden_room): one more room, as large as fits (six cells a side down to four),
// joined to its nearest room only. It is a dead end, so sealing it cuts nothing else off. It may stand a little past
// the area the other rooms keep to (within the 20 cells retail's map shows, then further on a crowded floor).
// Returns its index, or -1.
int BuildWardenRoom() {
    if (roomStackCnt < 2 || roomStackCnt >= DNG_ROOM_MAX) return -1;
    static constexpr int kSizes[][2] = {{6, 6}, {6, 5}, {5, 6}, {5, 5}, {5, 4}, {4, 5}, {4, 4}};
    for (const auto &[w, h] : kSizes)
    for (int reach = 0; reach < 3; ++reach) {
        // a room cell's col/row stays below the edge
        const int edge = std::min(DNG_GRID - 2, std::max(DNG_BUILD_AREA, 20) - 1 + 4 * reach);
        for (int tries = 0; tries < 0x200; ++tries) {
            const int x = 1 + static_cast<int>((static_cast<float>(edge - w - 1) * static_cast<float>(rand())) / 2147483648.0f);
            const int y = 1 + static_cast<int>((static_cast<float>(edge - h - 1) * static_cast<float>(rand())) / 2147483648.0f);
            if (buildRoom(x, y, w, h, roomStackCnt) != 1) continue;
            const int room = roomStackCnt++;
            roomStack[room] = {};
            roomStack[room].x = x;
            roomStack[room].y = y;
            roomStack[room].width = w;
            roomStack[room].height = h;
            int near = -1, near_dist = 1 << 30;
            for (int j = 0; j < room; ++j) {
                const int dx = (x + w / 2) - (roomStack[j].x + roomStack[j].width / 2);
                const int dy = (y + h / 2) - (roomStack[j].y + roomStack[j].height / 2);
                if (dx * dx + dy * dy < near_dist) {
                    near = j;
                    near_dist = dx * dx + dy * dy;
                }
            }
            joinRoomRecorded(room, near);
            return room;
        }
    }
    return -1;
}

int joinRoom(int from_room, int to_room) {
    int done;    int target_x;
    int target_y;
    int dir;
    int distance;
    int step;
    int i;
    int cx;
    int cy;
    int steps;
    int dx;
    int dy;

    copyMapInfo(buildMapWork, buildMapDat);

    // The corridor leaves the room by whichever side faces the other room.
    dx = roomStack[from_room].x - roomStack[to_room].x;
    dy = roomStack[from_room].y - roomStack[to_room].y;

    if (dx * dx > dy * dy) {
        if (dx < 0) {
            dir = MAP_DIR_EAST;
        } else {
            dir = MAP_DIR_WEST;
        }
    } else {
        if (dy < 0) {
            dir = MAP_DIR_SOUTH;
        } else {
            dir = MAP_DIR_NORTH;
        }
    }

    switch (dir) {
        case MAP_DIR_NORTH:
            cx = roomStack[from_room].x + (roomStack[from_room].width >> 1);
            cy = roomStack[from_room].y - 1;
            buildMapWork[cx + (cy + 1) * DNG_GRID].content |= BUILD_CELL_DOOR;
            buildMapWork[cx + (cy + 1) * DNG_GRID].link[from_room] = 1;
            steps = roomStack[from_room].y - (roomStack[to_room].y + roomStack[to_room].height);
            break;
        case MAP_DIR_SOUTH:
            cx = roomStack[from_room].x + (roomStack[from_room].width >> 1);
            buildMapWork[cx + (roomStack[from_room].y + roomStack[from_room].height - 1) * DNG_GRID].content |= BUILD_CELL_DOOR;
            buildMapWork[cx + (roomStack[from_room].y + roomStack[from_room].height - 1) * DNG_GRID].link[from_room] = 1;
            cy = roomStack[from_room].y + roomStack[from_room].height;
            steps = roomStack[to_room].y - cy;
            break;
        case MAP_DIR_EAST:
            cx = roomStack[from_room].x + roomStack[from_room].width;
            cy = roomStack[from_room].y + (roomStack[from_room].height >> 1);
            buildMapWork[cx + cy * DNG_GRID - 1].content |= BUILD_CELL_DOOR;
            buildMapWork[cx + cy * DNG_GRID - 1].link[from_room] = 1;
            steps = roomStack[to_room].x - (roomStack[from_room].x + roomStack[from_room].width);
            break;
        case MAP_DIR_WEST:
            cx = roomStack[from_room].x - 1;
            cy = roomStack[from_room].y + (roomStack[from_room].height >> 1);
            buildMapWork[cx + cy * DNG_GRID + 1].content |= BUILD_CELL_DOOR;
            buildMapWork[cx + cy * DNG_GRID + 1].link[from_room] = 1;
            steps = roomStack[from_room].x - (roomStack[to_room].x + roomStack[to_room].width);
            break;
    }

    target_x = roomStack[to_room].x + (roomStack[to_room].width >> 1);
    target_y = roomStack[to_room].y + (roomStack[to_room].height >> 1);
    steps = (steps >> 1) + 1;

    // Two rooms that share a wall still need a cell between them.
    if (roomStack[from_room].y == roomStack[to_room].y + roomStack[to_room].height) {
        if (dir == MAP_DIR_NORTH || dir == MAP_DIR_SOUTH) {
            if (steps < 2) {
                steps = 2;
            }
        }
    }

    if (roomStack[to_room].y == roomStack[from_room].y + roomStack[from_room].height) {
        if (dir == MAP_DIR_NORTH || dir == MAP_DIR_SOUTH) {
            if (steps < 2) {
                steps = 2;
            }
        }
    }

    done = 0;

    while (done == 0) {
        if (steps != 0) {
            if (buildMapWork[cx + cy * DNG_GRID].kind == MAP_PARTS_NONE) {
                buildMapWork[cx + cy * DNG_GRID].kind = from_room;
                buildMapWork[cx + cy * DNG_GRID].link[from_room] = 1;
                buildMapWork[cx + cy * DNG_GRID].content |= BUILD_CELL_CORRIDOR;
                steps--;

                if (steps != 0) {
                    switch (dir) {
                        case MAP_DIR_NORTH:
                            cy--;
                            break;
                        case MAP_DIR_SOUTH:
                            cy++;
                            break;
                        case MAP_DIR_EAST:
                            cx++;
                            break;
                        case MAP_DIR_WEST:
                            cx--;
                            break;
                    }
                }
            } else {
                if ((buildMapWork[cx + cy * DNG_GRID].content & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) {
                    buildMapWork[cx + cy * DNG_GRID].content |= BUILD_CELL_DOOR;
                    buildMapWork[cx + cy * DNG_GRID].link[from_room] = 1;
                } else {
                    buildMapWork[cx + cy * DNG_GRID].link[from_room] = 1;
                }

                done = 1;
            }
        } else {

            // The corridor has come far enough out; turn and run at the room.
            switch (dir) {
                case MAP_DIR_NORTH:
                case MAP_DIR_SOUTH:
                    distance = target_x - cx;

                    if (distance < 0) {
                        distance = -distance;
                        step = -1;
                    } else {
                        step = 1;
                    }

                    for (i = 0; i < distance; i++) {
                        cx += step;

                        if (buildMapWork[cx + cy * DNG_GRID].kind == MAP_PARTS_NONE) {
                            buildMapWork[cx + cy * DNG_GRID].kind = from_room;
                            buildMapWork[cx + cy * DNG_GRID].link[from_room] = 1;
                            buildMapWork[cx + cy * DNG_GRID].content |= BUILD_CELL_CORRIDOR;
                        } else {
                            if ((buildMapWork[cx + cy * DNG_GRID].content & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) {
                                buildMapWork[cx + cy * DNG_GRID].content |= BUILD_CELL_DOOR;
                                buildMapWork[cx + cy * DNG_GRID].link[from_room] = 1;
                            } else {
                                buildMapWork[cx + cy * DNG_GRID].link[from_room] = 1;
                            }

                            done = 1;
                        }
                    }

                    break;
                case MAP_DIR_EAST:
                case MAP_DIR_WEST:
                    distance = target_y - cy;

                    if (distance < 0) {
                        distance = -distance;
                        step = -1;
                    } else {
                        step = 1;
                    }

                    for (i = 0; i < distance; i++) {
                        cy += step;

                        if (buildMapWork[cx + cy * DNG_GRID].kind == MAP_PARTS_NONE) {
                            buildMapWork[cx + cy * DNG_GRID].kind = from_room;
                            buildMapWork[cx + cy * DNG_GRID].link[from_room] = 1;
                            buildMapWork[cx + cy * DNG_GRID].content |= BUILD_CELL_CORRIDOR;
                        } else {
                            if ((buildMapWork[cx + cy * DNG_GRID].content & BUILD_CELL_ROOM) == BUILD_CELL_ROOM) {
                                buildMapWork[cx + cy * DNG_GRID].content |= BUILD_CELL_DOOR;
                                buildMapWork[cx + cy * DNG_GRID].link[from_room] = 1;
                            } else {
                                buildMapWork[cx + cy * DNG_GRID].link[from_room] = 1;
                            }

                            done = 1;
                        }
                    }

                    break;
            }

            switch (dir) {
                case MAP_DIR_NORTH:
                    cy--;
                    break;
                case MAP_DIR_SOUTH:
                    cy++;
                    break;
                case MAP_DIR_EAST:
                    cx++;
                    break;
                case MAP_DIR_WEST:
                    cx--;
                    break;
            }

            steps = 999;
        }
    }

    copyMapInfo(buildMapDat, buildMapWork);
    return 1;
}

void setRoomObject() {
    int i;
    int x;
    int y;
    int found;
    int found_x;
    int found_y;
    int num;

    num = 0;

    // MoonMod (dungeons.set_zones shrines): each zone's shrine room takes a spring against its top wall (zone 0's first
    // room, a later zone's gate room, or the zone's next room when that wall is all doors), and no room a random one.
    const bool shrines = zonesOn && DngZoneShrines != 0;
    if (zonesOn) {
        for (int z = 0; z < DNG_ZONE_MAX; z++) DngZoneShrineX[z] = DngZoneShrineY[z] = -1;
    }
    if (shrines) {
        for (int z = 0; z < DngZoneCount; z++) {
            std::vector<int> order;
            for (const ZoneGatePlan &plan : zoneGatePlans) {
                if (plan.zone == z) order.push_back(plan.room);
            }
            for (int r = 0; r < roomStackCnt; r++) {
                if (roomZone[r] == z && r != wardenRoom) order.push_back(r);
            }
            for (const int r : order) {
                const ROOM_INFO &room = roomStack[r];
                int best = -1;
                for (x = 1; x < room.width - 1; x++) {
                    const int cell = room.x + x + room.y * DNG_GRID;
                    if ((buildMapDat[cell].content & (BUILD_CELL_ROOM | BUILD_CELL_DOOR)) != BUILD_CELL_ROOM) continue;
                    if (best < 0 || std::abs(x - room.width / 2) < std::abs(best - room.width / 2)) best = x;
                }
                if (best < 0) continue;
                buildMapDat[room.x + best + room.y * DNG_GRID].content |= BUILD_CELL_HEAL_SPRING;
                DngZoneShrineX[z] = room.x + best;
                DngZoneShrineY[z] = room.y;
                break;
            }
        }
    }

    for (i = 0; i < roomStackCnt; i++) {

        // Only two rooms on the floor take an object against their top wall.
        if (num < 2) {
            found = 0;

            for (x = 1; x < roomStack[i].width - 1; x++) {
                if ((buildMapDat[roomStack[i].x + (x + roomStack[i].y * DNG_GRID)].content & (BUILD_CELL_ROOM | BUILD_CELL_DOOR)) == BUILD_CELL_ROOM) {
                    found_x = x + roomStack[i].x;
                    found_y = roomStack[i].y;
                    found = 1;
                }
            }

            if ((100.0f * static_cast<float>(rand())) / 2147483648.0f <= 30.0f) {
                if (found == 1 && !shrines) {
                    buildMapDat[found_x + found_y * DNG_GRID].content |= BUILD_CELL_HEAL_SPRING;
                    num++;
                }
            }
        }

        // Every room can take an object anywhere inside it.
        found = 0;

        for (y = 1; y < roomStack[i].height - 1; y++) {
            for (x = 1; x < roomStack[i].width - 1; x++) {
                if ((buildMapDat[roomStack[i].x + (x + (y + roomStack[i].y) * DNG_GRID)].content & (BUILD_CELL_ROOM | BUILD_CELL_DOOR | BUILD_CELL_HEAL_SPRING | BUILD_CELL_PILLAR)) == BUILD_CELL_ROOM) {
                    found_x = x + roomStack[i].x;
                    found_y = y + roomStack[i].y;
                    found = 1;
                }
            }
        }

        if ((100.0f * static_cast<float>(rand())) / 2147483648.0f <= 30.0f) {
            if (found == 1) {
                buildMapDat[found_x + found_y * DNG_GRID].content |= BUILD_CELL_PILLAR;
            }
        }
    }
}

void setRoomDivid() {
    int line_x[64];
    int line_y[64];
    int line_len[64];
    int line_dir[64];
    int i;
    int line_num;
    int ok;
    int x;
    int y;
    int line_no;

    for (i = 0; i < roomStackCnt; i++) {
        line_num = 0;

        // A room can be divided along any row that is free from end to end.
        for (y = 1; y < roomStack[i].height - 1; y++) {
            ok = 1;

            for (x = 0; x < roomStack[i].width; x++) {
                if ((buildMapDat[roomStack[i].x + (x + (y + roomStack[i].y) * DNG_GRID)].content & (BUILD_CELL_ROOM | BUILD_CELL_DOOR | BUILD_CELL_HEAL_SPRING | BUILD_CELL_PILLAR)) != BUILD_CELL_ROOM) {
                    ok = 0;
                }
                // MoonMod shrines (dungeons.set_zones shrines): no wall across the cell in front of a zone's spring.
                if (zonesOn && DngZoneShrines != 0 &&
                    (buildMapDat[roomStack[i].x + (x + (y - 1 + roomStack[i].y) * DNG_GRID)].content & BUILD_CELL_HEAL_SPRING) != 0) {
                    ok = 0;
                }
            }

            if (ok == 1) {
                line_x[line_num] = roomStack[i].x;
                line_y[line_num] = y + roomStack[i].y;
                line_len[line_num] = roomStack[i].width;
                line_dir[line_num] = 0;
                line_num++;
            }
        }

        // ...or along any such column.
        for (x = 1; x < roomStack[i].width - 1; x++) {
            ok = 1;

            for (y = 0; y < roomStack[i].height; y++) {
                if ((buildMapDat[roomStack[i].x + (x + (y + roomStack[i].y) * DNG_GRID)].content & (BUILD_CELL_ROOM | BUILD_CELL_DOOR | BUILD_CELL_HEAL_SPRING | BUILD_CELL_PILLAR)) != BUILD_CELL_ROOM) {
                    ok = 0;
                }
            }

            if (ok == 1) {
                line_x[line_num] = x + roomStack[i].x;
                line_y[line_num] = roomStack[i].y;
                line_len[line_num] = roomStack[i].height;
                line_dir[line_num] = 1;
                line_num++;
            }
        }

        if (line_num == 0) {
            continue;
        }

        if (!((100.0f * static_cast<float>(rand())) / 2147483648.0f <= 40.0f)) {
            continue;
        }

        // The wall runs the whole length of the line, with one gap in it.
        line_no = static_cast<int>((static_cast<float>(line_num) * static_cast<float>(rand())) / 2147483648.0f);

        if (line_dir[line_no] == 0) {
            for (x = line_x[line_no]; x < line_x[line_no] + line_len[line_no]; x++) {
                buildMapDat[x + line_y[line_no] * DNG_GRID].content |= BUILD_CELL_DIVIDING_WALL;
            }

            buildMapDat[line_x[line_no] + line_y[line_no] * DNG_GRID + static_cast<int>((static_cast<float>(line_len[line_no] - 2) * static_cast<float>(rand())) / 2147483648.0f) + 1]
                .content |= BUILD_CELL_DIVIDING_DOOR;
        } else {
            for (y = line_y[line_no]; y < line_y[line_no] + line_len[line_no]; y++) {
                buildMapDat[y * DNG_GRID + line_x[line_no]].content |= BUILD_CELL_DIVIDING_WALL;
            }

            buildMapDat[line_x[line_no] + (line_y[line_no] + static_cast<int>((static_cast<float>(line_len[line_no] - 2) * static_cast<float>(rand())) / 2147483648.0f) + 1) * DNG_GRID]
                .content |= BUILD_CELL_DIVIDING_DOOR;
        }
    }
}

void setUnderDungeonStart() {
    int safe[120];
    int safe_num;
    int i;
    int x;
    int y;
    int searching;
    int tries;
    int pick;

    safe_num = 0;

    for (i = 0; i < 120; i++) {
        safe[i] = -1;
    }

    // A room cell with no door and no dividing wall can hold the way down.
    for (y = 0; y < DNG_GRID; y++) {
        for (x = 0; x < DNG_GRID; x++) {
            // (MoonMod: never in the sealed Warden room, nor on a zone's shrine spring.)
            if ((buildMapDat[x + y * DNG_GRID].content & (BUILD_CELL_ROOM | BUILD_CELL_DOOR | BUILD_CELL_DIVIDING_WALL)) == BUILD_CELL_ROOM && safe_num < 60 &&
                (wardenRoom < 0 || buildMapDat[x + y * DNG_GRID].link[wardenRoom] == 0) &&
                !(zonesOn && DngZoneShrines != 0 && (buildMapDat[x + y * DNG_GRID].content & BUILD_CELL_HEAL_SPRING) != 0)) {
                safe[safe_num * 2] = x;
                safe[safe_num * 2 + 1] = y;
                safe_num++;
            }
        }
    }

    // Keep picking cells until one comes out with an empty cell above it and a
    // door-free room cell on both sides.
    searching = 1;
    tries = 0;

    while (searching) {
        pick = static_cast<int>((static_cast<float>(safe_num) * static_cast<float>(rand())) / 2147483648.0f);
        std::println("[s]{}", pick);
        x = safe[pick * 2];
        y = safe[pick * 2 + 1];

        if (buildMapDat[x + (y - 1) * DNG_GRID].content == 0) {
            if ((buildMapDat[x + y * DNG_GRID + 1].content & (BUILD_CELL_ROOM | BUILD_CELL_DOOR)) == BUILD_CELL_ROOM) {
                if ((buildMapDat[x + y * DNG_GRID - 1].content & (BUILD_CELL_ROOM | BUILD_CELL_DOOR)) == BUILD_CELL_ROOM) {
                    buildMapDat[x + y * DNG_GRID].content = BUILD_CELL_ROOM | BUILD_CELL_URA_ENTRANCE;
                    buildMapDat[x + y * DNG_GRID].kind = MAP_PARTS_ROOM_GATE_NORTH;
                    buildMapDat[x + y * DNG_GRID].direction = 0;
                    searching = 0;
                }
            }
        }

        tries++;

        if (tries >= 5000) {
            searching = 0;
        }
    }

    if (searching == 0) {
        std::println("safeNum = {}, escapeCnt = {}", safe_num, tries);
    }
}

void setStair() {
#ifdef PORT
    int safe[DNG_CELLS * 2]; /* every corridor cell can qualify on a large layout */
#else
    int safe[120];
#endif
    int safe_num;
    int i;
    int x;
    int y;
    int pick;

    // MoonMod zones: the stair down in the first zone and the stair up in the last, each off a corridor the zone dug
    // (the corridor up to a gate is the zone before's).
    const auto in_zone = [](int x, int y, int zone) {
        if (!zonesOn) return true;
        const int cell = x + y * DNG_GRID, room = buildMapDat[cell].kind;
        return zoneGateCorridor[cell] == 0 && room >= 0 && room < roomStackCnt && roomZone[room] == zone;
    };

    // The stair down stands above a corridor that has a free cell over it.
    safe_num = 0;

    for (i = 0; i < 120; i++) {
        safe[i] = -1;
    }

    for (y = 0; y < DNG_GRID; y++) {
        for (x = 0; x < DNG_GRID; x++) {
            if ((buildMapDat[x + y * DNG_GRID].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR && in_zone(x, y, 0)) {
                if (buildMapDat[x + (y - 1) * DNG_GRID].content == 0) {
                    safe[safe_num * 2] = x;
                    safe[safe_num * 2 + 1] = y;
                    safe_num++;
                }
            }
        }
    }

    pick = static_cast<int>((static_cast<float>(safe_num) * static_cast<float>(rand())) / 2147483648.0f);

    if (safe_num <= 0) {
        std::println("ERR: DownStair!!");
        y = 1;
        x = 1;
    } else {
        x = safe[pick * 2];
        y = safe[pick * 2 + 1];
    }

    buildMapDat[x + (y - 1) * DNG_GRID].content = BUILD_CELL_STAIR_DOWN;
    buildMapDat[x + (y - 1) * DNG_GRID].kind = buildMapDat[x + y * DNG_GRID].kind;
    buildMapDat[x + (y - 1) * DNG_GRID].link[buildMapDat[x + y * DNG_GRID].kind] = 1;
    buildMapDat[x + (y - 1) * DNG_GRID].direction = 0;

    // The stair up stands below one that has a free cell under it.
    safe_num = 0;

    for (i = 0; i < 120; i++) {
        safe[i] = -1;
    }

    for (y = 0; y < DNG_GRID; y++) {
        for (x = 0; x < DNG_GRID; x++) {
            if ((buildMapDat[x + y * DNG_GRID].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR && in_zone(x, y, DngZoneCount - 1)) {
                if (buildMapDat[x + (y + 1) * DNG_GRID].content == 0) {
                    safe[safe_num * 2] = x;
                    safe[safe_num * 2 + 1] = y;
                    safe_num++;
                }
            }
        }
    }

    pick = static_cast<int>((static_cast<float>(safe_num) * static_cast<float>(rand())) / 2147483648.0f);

    if (safe_num <= 0) {
        std::println("ERR: UpStair!!");
        y = 1;
        x = 1;
    } else {
        x = safe[pick * 2];
        y = safe[pick * 2 + 1];
    }

    buildMapDat[x + (y + 1) * DNG_GRID].content = BUILD_CELL_STAIR_UP;
    buildMapDat[x + (y + 1) * DNG_GRID].kind = buildMapDat[x + y * DNG_GRID].kind;
    buildMapDat[x + (y + 1) * DNG_GRID].link[buildMapDat[x + y * DNG_GRID].kind] = 1;
    buildMapDat[x + (y + 1) * DNG_GRID].direction = 2;
}

// Returns the first free event slot; running out of slots stops the game.
int FindFreeEvent(std::span<const DUNGEON_EVENT> events) {
    for (std::size_t i = 0; i < events.size(); i++) {
        if (events[i].kind == DNG_EVENT_NONE) {
            return static_cast<int>(i);
        }
    }

    std::println("ivent buffer over !!");
    exit__2(-1);
}

} // namespace

void CDungeonMap::initTrapCircle() {
    for (int i = 0; i < 3; i++) {
        this->trap_circle[i].state = 0;
    }
}

float *CDungeonMap::CheckTrapCircle(float *pos, float dist) {
    for (int i = 0; i < 3; i++) {
        if (this->trap_circle[i].state != 0) {
            if (DistVector(this->trap_circle[i].pos, pos) < dist) {
                return this->trap_circle[i].pos;
            }
        }
    }

    return nullptr;
}

void CDungeonMap::SetupTrapCircle(float *pos) {
    for (int i = 0; i < 3; i++) {
        bool is_free = !this->trap_circle[i].state;

        // Populate the first free slot with a trap circle of a random kind.
        if (is_free) {
            sceVu0CopyVector(this->trap_circle[i].pos, pos);
            this->trap_circle[i].state = 1;
            this->trap_circle[i].kind = static_cast<int>((10.0f * static_cast<float>(rand())) / 2147483648.0f);

            if (this->trap_circle[i].kind >= TRAP_CIRCLE_COUNT) {
                this->trap_circle[i].kind = 9;
            }

            this->trap_circle[i].timer = 0.0f;
            break;
        }
    }
}

void CDungeonMap::DrawTrapCircle() {
    float pos[4];
    int   loaded;
    int   i;

    loaded = false;
    sceVu0CopyVector(pos, CharaMain.pos);

    for (i = 0; i < 3; i++) {
        if (this->trap_circle[i].state != 0) {
            if (DistVector(this->trap_circle[i].pos, pos) <= 160.0f * this->draw_dist_scale) {
                float wide;
                float tall;

                // The texture is only wanted once, however many circles draw.
                if (!loaded) {
                    loaded = true;
                    TexManager.ReloadTexture(Vif1Packet, 71);
                    Trap_Circle.TextureAnime(71);
                }

                tall = 1.0f + sinf(this->trap_circle[i].timer);
                wide = 1.0f + sinf(this->trap_circle[i].timer) / 2.0f;
                Trap_Circle.SetScale(wide, tall, wide);
                Trap_Circle.SetPosition(this->trap_circle[i].pos);
                Trap_Circle.Draw();
            }
        }
    }
}

MAP_TRAP_CIRCLE *CDungeonMap::DistTrapCircle() {
    float pos[4];

    sceVu0CopyVector(pos, CharaMain.pos);

    for (int i = 0; i < 3; i++) {
        if (this->trap_circle[i].state == 1) {
            if (DistVector(this->trap_circle[i].pos, pos) <= 5.0f) {
                this->trap_circle[i].state = 2;
                return &this->trap_circle[i];
            }
        }
    }

    return nullptr;
}

void CDungeonMap::StepTrapCircle() {
    for (int i = 0; i < 3; i++) {
        if (this->trap_circle[i].state == 2) {
            this->trap_circle[i].timer += 0.10471976f;

            if (this->trap_circle[i].timer >= THREE_HALVES_PI) {
                this->trap_circle[i].state = 0;
            }
        }
    }
}

int CDungeonMap::CheckTreasureBox(float *pos, float dist) {
    // Every slot: a mod's chests can leave used boxes past box_num.
    for (int i = 0; i < DNG_BOX_MAX; i++) {
        if (this->boxes[i].used != 0 && DistVector(this->boxes[i].pos, pos) < dist) {
            return 0;
        }
    }

    return 1;
}

int CDungeonMap::CheckAtra(float *pos, float dist) {
    // Demon Shaft does not have atla, only the first five maps do.
    if (selectMapNo >= DUNGEON_DEMON_SHAFT) {
        return 1;
    }

    for (int i = 0; i < this->atra_num; i++) {
        if (this->atra[i].used != 0 && DistVector(this->atra[i].pos, pos) < dist) {
            return 0;
        }
    }

    return 1;
}

void CDungeonMap::SetAtraBoll(float *pos, int atra_no) {
    sceVu0CopyVector(this->atra[this->atra_num].pos, pos);
    this->atra[this->atra_num].phase = 0.0f;
    this->atra[this->atra_num].atra_no = atra_no;
    this->atra[this->atra_num].used = 1;
    this->atra[this->atra_num].zone_floor = 0;

    int event_no = FindFreeEvent(this->events);

    this->events[event_no].kind = DNG_EVENT_ATRA;
    this->events[event_no].placed_flag = 0;
    this->events[event_no].index = this->atra_num;
    sceVu0CopyVector(this->events[event_no].pos, pos);
    this->events[event_no].radius = 13.0f;
    this->atra_num++;
}

int CDungeonMap::PutAtraBoll(float *pos, int atra_no) {
    if (this->atra_num < 0 || this->atra_num > static_cast<int>(std::size(this->atra))) {
        return -1;
    }
    if (this->atra_num < static_cast<int>(std::size(this->atra))) {
        const int slot = this->atra_num;
        this->SetAtraBoll(pos, atra_no);
        return slot;
    }
    // every slot taken: one whose ball has been picked up (its event is gone with it) holds the new one
    for (int slot = 0; slot < this->atra_num; slot++) {
        if (this->atra[slot].used != 0) {
            continue;
        }
        sceVu0CopyVector(this->atra[slot].pos, pos);
        this->atra[slot].phase = 0.0f;
        this->atra[slot].atra_no = atra_no;
        this->atra[slot].used = 1;
        this->atra[slot].zone_floor = 0;
        const int event_no = FindFreeEvent(this->events);
        this->events[event_no].kind = DNG_EVENT_ATRA;
        this->events[event_no].placed_flag = 0;
        this->events[event_no].index = slot;
        sceVu0CopyVector(this->events[event_no].pos, pos);
        this->events[event_no].radius = 13.0f;
        return slot;
    }
    return -1;
}

int CDungeonMap::PutAtraBollAnywhere(int atra_no) {
    sceVu0FVECTOR object_pos;
    for (int tries = 0; tries < 64; tries++) {
        SearchiDoPutArea(this->cells, 0, 0, DNG_GRID, DNG_GRID, object_pos);
        if (this->CheckTreasureBox(object_pos, 20.0f) != 0 && this->CheckAtra(object_pos, 20.0f) != 0 &&
            this->CheckTrapCircle(object_pos, 20.0f) == nullptr) {
            return this->PutAtraBoll(object_pos, atra_no);
        }
    }
    return -1;
}

int CDungeonMap::SetTreasureBox(float *pos, int item_no, int kind, int trap_no) {
    int box_no = -1;

    for (int i = 0; i < DNG_BOX_MAX; i++) {
        if (this->boxes[i].used == 0) {
            box_no = i;
            break;
        }
    }

    if (box_no == -1) {
        return -1;
    }

    sceVu0CopyVector(this->boxes[box_no].pos, pos);
    this->boxes[box_no].used = 1;
    this->boxes[box_no].lid_angle = 0;
    this->boxes[box_no].kind = kind;
    this->boxes[box_no].item_no = item_no;
    moonmod::RecordChronicleChestValue(this, box_no, item_no);
    this->boxes[box_no].closed = 1;
    this->boxes[box_no].trap_no = trap_no;

    int event_no = FindFreeEvent(this->events);

    this->events[event_no].kind = DNG_EVENT_TREASURE_BOX;
    this->events[event_no].placed_flag = 0;
    this->events[event_no].origin_x = static_cast<int>(pos[0]);
    this->events[event_no].origin_z = static_cast<int>(pos[2]);
    this->events[event_no].index = this->box_num;

    // The event sits in front of the box, and a small box stands nearer to it.
    switch (kind) {
        case TREASURE_BOX_LARGE:
            this->events[event_no].pos[0] = pos[0];
            this->events[event_no].pos[1] = pos[1];
            this->events[event_no].pos[2] = pos[2] + 10.0f;
            this->events[event_no].radius = 8.0f;
            break;
        case TREASURE_BOX_SMALL:
            this->events[event_no].pos[0] = pos[0];
            this->events[event_no].pos[1] = pos[1];
            this->events[event_no].pos[2] = pos[2] + 8.0f;
            this->events[event_no].radius = 8.0f;
            break;
    }

    this->box_num++;
    return event_no;
}

void CDungeonMap::buildEventData(int floor_no, int enabled, int place_atla) {
    float box_pos[4];
    float object_pos[4];
    int   atra_no[8]; // BtAtraFloorCyoice fills and counts all eight of a floor's atla slots.
    int   special;
    int   object_count;
    int   valid;
    int   event_x;
    int   event_y;
    int   target_count;

    if (enabled != 1) {
        return;
    }

    special = 0;

    if (place_atla == 0) {
        special = 1;
    }

    object_count = 0;
    target_count = static_cast<int>((4.0f * static_cast<float>(rand())) / 2147483648.0f) + 1;

    // A mod's chest bonus and per-room density; bounded so a full floor cannot loop forever.
    const int chest_target = DngChestTarget(this, target_count + 3);
    int       chest_attempts = 0;
    // MoonMod: the weapons put in chests on this floor so far, and the share of large chests a mod asked for.
    int       chest_weapons[DNG_BOX_MAX];
    int       chest_weapon_count = 0;
    const float large_from = DngLargeChestPercent >= 0 ? 100.0f - static_cast<float>(DngLargeChestPercent) : 88.0f;
    const auto chest_item = [&](int item_no) {
        item_no = DngChestWeapon(item_no, chest_weapons, chest_weapon_count);
        if (item_no >= ITEM_WEAPON_START) item_no = MoonModChestWeapon(item_no, selectMapNo, floor_no, chest_weapons, chest_weapon_count);
        if (item_no >= ITEM_WEAPON_START && chest_weapon_count < DNG_BOX_MAX) chest_weapons[chest_weapon_count++] = item_no;
        return item_no;
    };
    while (object_count < chest_target && ++chest_attempts <= 5000) {
        SearchiDoPutArea(this->cells, 0, 0, DNG_GRID, DNG_GRID, box_pos);
        valid = 1;

        if (this->CheckTreasureBox(box_pos, 20.0f) == 0) {
            valid = 0;
        }

        if (this->CheckAtra(box_pos, 20.0f) == 0) {
            valid = 0;
        }

        if (this->CheckTrapCircle(box_pos, 20.0f) != nullptr) {
            valid = 0;
        }

        if (valid != 0) {
            if ((100.0f * static_cast<float>(rand())) / 2147483648.0f >= large_from && object_count >= 2) {
                int item_no = chest_item(PresetSmallItemNo_Get(selectMapNo, DngZoneSourceFloorAt(this->map_seed, box_pos, floor_no), special, 0));

                if (item_no != -1) {
                    int trap_no = static_cast<int>((6.0f * static_cast<float>(rand())) / 2147483648.0f);

                    if (trap_no < 0 || trap_no >= 6) {
                        trap_no = 1;
                    }

                    if (this->SetTreasureBox(box_pos, item_no, TREASURE_BOX_LARGE, trap_no) >= 0) {
                        object_count++;
                    }
                }
            } else {
                int item_no = chest_item(PresetSmallItemNo_Get(selectMapNo, DngZoneSourceFloorAt(this->map_seed, box_pos, floor_no), special, 1));

                if (place_atla != 0) {
                    if (object_count == 0) {
                        item_no = ITEM_MAP;
                    }

                    if (object_count == 1) {
                        item_no = ITEM_MAGICAL_CRYSTAL;
                    }
                }

                if (item_no != -1) {
                    if (this->SetTreasureBox(box_pos, item_no, TREASURE_BOX_SMALL, 0) >= 0) {
                        object_count++;
                    }
                }
            }
        }
    }

    this->initTrapCircle();

    for (int i = 0; i < 3; i++) {
        if (static_cast<int>((100.0f * static_cast<float>(rand())) / 2147483648.0f) < 21) {
            SearchiDoPutArea(this->cells, 0, 0, DNG_GRID, DNG_GRID, object_pos);
            valid = 1;

            if (this->CheckTreasureBox(object_pos, 20.0f) == 0) {
                valid = 0;
            }

            if (this->CheckAtra(object_pos, 20.0f) == 0) {
                valid = 0;
            }

            if (this->CheckTrapCircle(object_pos, 20.0f) != nullptr) {
                valid = 0;
            }

            if (valid != 0) {
                this->SetupTrapCircle(object_pos);
            }
        }
    }

    // MoonMod (dungeons.set_zones source_floor): a floor whose zones come from several floors lays each floor's atla in
    // its own zone, each ball remembering its floor for the pickup.
    if (place_atla != 0 && DngZoneSourcesShown(this->map_seed)) {
        for (int z = 0; z < DngZoneFloorCount; ++z) {
            const int zone_floor = DngZoneFloorSource[z] >= 0 ? DngZoneFloorSource[z] : floor_no;
            const int zone_count = BtAtraFloorCyoice(selectMapNo, zone_floor, atra_no);
            int       placed = 0;
            const int square_x = ZoneOriginX(z), square_y = ZoneOriginY(z);
            const int square_w = std::min(DNG_BUILD_AREA, DNG_GRID - square_x), square_h = std::min(DNG_BUILD_AREA, DNG_GRID - square_y);
            for (int tries = 0; placed < zone_count && tries < 2000 && this->atra_num < static_cast<int>(std::size(this->atra)); ++tries) {
                SearchiDoPutArea(this->cells, square_x, square_y, square_w, square_h, object_pos);
                const int col = static_cast<int>((object_pos[0] + 80.0f) / 160.0f), row = static_cast<int>((object_pos[2] + 80.0f) / 160.0f);
                if (col < 0 || row < 0 || col >= DNG_GRID || row >= DNG_GRID || DngZoneOfCell[col + row * DNG_GRID] != z) continue;
                if (this->CheckTreasureBox(object_pos, 20.0f) == 0 || this->CheckAtra(object_pos, 20.0f) == 0 ||
                    this->CheckTrapCircle(object_pos, 20.0f) != nullptr) {
                    continue;
                }
                ATRA_BOLL &ball = this->atra[this->atra_num];
                sceVu0CopyVector(ball.pos, object_pos);
                ball.phase = 0.0f;
                ball.atra_no = atra_no[placed];
                ball.used = 1;
                ball.zone_floor = zone_floor + 1;
                int event_no = FindFreeEvent(this->events);
                this->events[event_no].kind = DNG_EVENT_ATRA;
                this->events[event_no].placed_flag = 0;
                this->events[event_no].origin_x = col;
                this->events[event_no].origin_z = row;
                this->events[event_no].index = this->atra_num;
                sceVu0CopyVector(this->events[event_no].pos, object_pos);
                this->events[event_no].radius = 13.0f;
                this->atra_num++;
                placed++;
            }
        }
        place_atla = 0;
    }

    if (place_atla != 0) {
        object_count = 0;
        int atra_count = BtAtraFloorCyoice(selectMapNo, floor_no, atra_no);

        while (object_count < atra_count) {
            SearchiDoPutArea(this->cells, 0, 0, DNG_GRID, DNG_GRID, object_pos);
            valid = 1;

            if (this->CheckTreasureBox(object_pos, 20.0f) == 0) {
                valid = 0;
            }

            if (this->CheckAtra(object_pos, 20.0f) == 0) {
                valid = 0;
            }

            if (this->CheckTrapCircle(object_pos, 20.0f) != nullptr) {
                valid = 0;
            }

            if (valid != 0) {
                sceVu0CopyVector(this->atra[this->atra_num].pos, object_pos);
                this->atra[this->atra_num].phase = 0.0f;
                this->atra[this->atra_num].atra_no = atra_no[object_count];
                this->atra[this->atra_num].used = 1;
                this->atra[this->atra_num].zone_floor = 0;

                int event_no = FindFreeEvent(this->events);

                this->events[event_no].kind = DNG_EVENT_ATRA;
                this->events[event_no].placed_flag = 0;
                this->events[event_no].origin_x = event_x;
                this->events[event_no].origin_z = event_y;
                this->events[event_no].index = this->atra_num;
                sceVu0CopyVector(this->events[event_no].pos, object_pos);
                this->events[event_no].radius = 13.0f;
                this->atra_num++;
                object_count++;
            }
        }
    }

    moonmod::ChronicleFloorEventsBuilt(this); // MoonMod: chests a mod keeps opened open again
}

void CDungeonMap::SetMimicEvent(float x, float y, float z, int item_no, int kind) {
    float pos[4];

    pos[0] = x;
    pos[1] = y;
    pos[2] = z;

    int box_no = -1;

    for (int i = 0; i < DNG_BOX_MAX; i++) {
        if (this->boxes[i].used == 0) {
            box_no = i;
            break;
        }
    }

    if (box_no == -1) {
        return;
    }

    int event_no = FindFreeEvent(this->events);

    // The mimic looks like a treasure box, so it takes a box slot as well.
    this->events[event_no].kind = DNG_EVENT_MIMIC;
    this->events[event_no].placed_flag = 0;
    this->events[event_no].index = box_no;

    sceVu0CopyVector(this->boxes[box_no].pos, pos);
    this->boxes[box_no].used = 1;
    this->boxes[box_no].lid_angle = 0;
    this->boxes[box_no].kind = kind;
    this->boxes[box_no].item_no = item_no;
    moonmod::RecordChronicleChestValue(this, box_no, item_no);
    this->boxes[box_no].closed = 1;

    switch (kind) {
        case TREASURE_BOX_LARGE:
            this->events[event_no].pos[0] = x;
            this->events[event_no].pos[1] = y;
            this->events[event_no].pos[2] = z + 10.0f;
            this->events[event_no].radius = 8.0f;
            break;
        case TREASURE_BOX_SMALL:
            this->events[event_no].pos[0] = x;
            this->events[event_no].pos[1] = y;
            this->events[event_no].pos[2] = z + 8.0f;
            this->events[event_no].radius = 8.0f;
            break;
    }
}

void CDungeonMap::RsetMimicEvent() {
    for (int i = 0; i < DNG_EVENT_MAX; i++) {
        if (this->events[i].kind != DNG_EVENT_NONE) {
            if (this->events[i].kind == DNG_EVENT_MIMIC) {
                this->events[i].kind = DNG_EVENT_NONE;
                this->boxes[this->events[i].index].used = 0;
            }
        }
    }
}

int CDungeonMap::GetActiveIvent(CFrameVu1 *frame) {
    float pos[4];

    sceVu0CopyVector(pos, frame->position);

    for (int i = 0; i < DNG_EVENT_MAX; i++) {
        if (this->events[i].kind != DNG_EVENT_NONE && DistVector(this->events[i].pos, pos) <= this->events[i].radius) {
            return i;
        }
    }

    return -1;
}

void CDungeonMap::buildDummyModel() {
    float pos[4];
    float stair_pos[4];
    int   i;
    int   j;
    int   kind;
    int   num;
    int   tries;
    int   x;
    int   z;
    int   shape;

    num = 0;
    tries = 0;
    this->dummy_num = 0;

    while (num < 8) {
        x = static_cast<int>((21.0f * static_cast<float>(rand())) / 2147483648.0f) - 1;
        z = static_cast<int>((21.0f * static_cast<float>(rand())) / 2147483648.0f) - 1;
        shape = static_cast<int>((3.0f * static_cast<float>(rand())) / 2147483648.0f);
        kind = -1;

        // Each kind covers a different footprint, and only goes down where
        // every cell it would cover is empty.
        switch (shape) {
            case 0:
                kind = 0;

                for (j = 0; j < 2; j++) {
                    for (i = 0; i < 2; i++) {
                        if (this->cells[x + (i + (j + z) * DNG_GRID)].parts_no != MAP_PARTS_NONE) {
                            kind = -1;
                        }
                    }
                }

                if (kind != -1) {
                    pos[0] = 80.0f + 160.0f * x;
                    pos[1] = 0.0f;
                    pos[2] = 80.0f + 160.0f * z;
                    pos[3] = 1.0f;
                }

                break;
            case 1:
                kind = 1;

                for (j = 0; j < 2; j++) {
                    for (i = 0; i < 2; i++) {
                        if (this->cells[x + (i + (j + z) * DNG_GRID)].parts_no != MAP_PARTS_NONE) {
                            kind = -1;
                        }
                    }
                }

                if (kind != -1) {
                    pos[0] = 160.0f + 160.0f * x;
                    pos[1] = 0.0f;
                    pos[2] = 160.0f + 160.0f * z;
                    pos[3] = 1.0f;
                }

                break;
            case 2:
                kind = 2;

                for (j = 0; j < 3; j++) {
                    for (i = 0; i < 3; i++) {
                        if (this->cells[x + (i + (j + z) * DNG_GRID)].parts_no != MAP_PARTS_NONE) {
                            kind = -1;
                        }
                    }
                }

                if (kind != -1) {
                    pos[0] = 240.0f + 160.0f * x;
                    pos[1] = 0.0f;
                    pos[2] = 240.0f + 160.0f * z;
                    pos[3] = 1.0f;
                }

                break;
        }

        if (kind != -1) {

            // A bigger model needs more room around it.
            for (x = 0; x < num; x++) {
                if (DistVector(pos, this->dummy_pos[x]) < 40.0f * kind) {
                    kind = -1;
                    break;
                }
            }

            for (x = 0; x < DNG_GRID; x++) {
                for (z = 0; z < DNG_GRID; z++) {
                    if (this->cells[z + x * DNG_GRID].parts_no == MAP_PARTS_URA_ENTRANCE_NORTH) {
                        stair_pos[0] = 80.0f + 160.0f * z;
                        stair_pos[1] = 0.0f;
                        stair_pos[2] = 80.0f + 160.0f * x;
                        stair_pos[3] = 1.0f;

                        if (DistVector(stair_pos, pos) < 160.0f * (kind + 2)) {
                            x = 21;
                            z = x;
                            kind = -1;
                        }
                    }
                }
            }

            if (kind != -1) {
                if (this->dummy_num < 8) {
                    sceVu0CopyVector(this->dummy_pos[this->dummy_num], pos);
                    this->dummy_model[this->dummy_num] = kind;
                    this->dummy_num++;
                }

                num++;
            }
        }

        if (tries > 10000) {
            break;
        }

        tries++;
    }
}

void CDungeonMap::GetRoomLinkInfo() {
    int   door_pos[32][2];
    int   room_list[DNG_ROOM_MAX]; // every room can match on a large layout
    float pos[4];
    int   door_count;
    int   stair_x;
    int   stair_y;
    int   door_part;
    int   result_no;
    int   direction_mask;
    int   room_count;
    int   x;
    int   propagation_mask;
    int   active;
    int   y;

    result_no = 0;

    for (int i = 0; i < 4; i++) {
        this->room_link[i].used = 0;
    }

    for (int i = 0; i < DNG_ROOM_AREA * DNG_ROOM_AREA; i++) {
        scanRoomInfoWork[i] = -1;
    }

    // Record the stair cell and every candidate room-to-corridor door.
    door_count = 0;

    for (y = 0; y < DNG_ROOM_AREA; y++) {
        for (x = 0; x < DNG_ROOM_AREA; x++) {
            if (this->cells[x + y * DNG_GRID].parts_no == MAP_PARTS_STAIR_DOWN && !DNG_IN_EXIT_ROOM(x, y)) {
                stair_x = x;
                stair_y = y;
                break;
            }
        }
    }

    for (y = 0; y < DNG_ROOM_AREA; y++) {
        for (x = 0; x < DNG_ROOM_AREA; x++) {
            if ((buildMapDat[x + y * DNG_GRID].content & BUILD_CELL_DOOR) != 0) {
                door_pos[door_count][0] = x;
                door_pos[door_count][1] = y;
                door_count++;

                if (door_count >= 32) {
                    y = DNG_ROOM_AREA;
                    break;
                }
            }
        }
    }

    int door_no = static_cast<int>((static_cast<float>(door_count) * static_cast<float>(rand())) / 2147483648.0f);
    x = door_pos[door_no][0];
    y = door_pos[door_no][1];
    scanRoomInfoWork[x + y * DNG_ROOM_AREA] = 9;
    buildMapDat[x + y * DNG_GRID].content |= BUILD_CELL_ROOM_LINK;
    this->room_link[0].door_x = x;
    this->room_link[0].door_y = y;
    this->room_link[0].unk_14 = 0;

    direction_mask = 0;

    if ((buildMapDat[x + (y - 1) * DNG_GRID].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) {
        direction_mask = MAP_DIR_NORTH;
    }

    if ((buildMapDat[x + (y + 1) * DNG_GRID].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) {
        direction_mask = MAP_DIR_SOUTH;
    }

    if ((buildMapDat[x + y * DNG_GRID + 1].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) {
        direction_mask = MAP_DIR_EAST;
    }

    if ((buildMapDat[x + y * DNG_GRID - 1].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) {
        direction_mask = MAP_DIR_WEST;
    }

    switch (direction_mask) {
        case MAP_DIR_NORTH:
            door_part = MAP_PARTS_ROOM_GATE_NORTH;
            scanRoomInfoWork[x + (y - 1) * DNG_ROOM_AREA] = 0;
            scanRoomInfoWork[x + (y + 1) * DNG_ROOM_AREA] = 1;
            break;
        case MAP_DIR_EAST:
            door_part = MAP_PARTS_ROOM_GATE_EAST;
            scanRoomInfoWork[x + y * DNG_ROOM_AREA + 1] = 0;
            scanRoomInfoWork[x + y * DNG_ROOM_AREA - 1] = 1;
            break;
        case MAP_DIR_SOUTH:
            door_part = MAP_PARTS_ROOM_GATE_SOUTH;
            scanRoomInfoWork[x + (y - 1) * DNG_ROOM_AREA] = 0;
            scanRoomInfoWork[x + (y + 1) * DNG_ROOM_AREA] = 1;
            break;
        case MAP_DIR_WEST:
            door_part = MAP_PARTS_ROOM_GATE_WEST;
            scanRoomInfoWork[x + y * DNG_ROOM_AREA + 1] = 0;
            scanRoomInfoWork[x + y * DNG_ROOM_AREA - 1] = 1;
            break;
        default:
            std::println("ERROR !!");
            return;
    }

    // Spread each side label only through cells that share a room membership.
    active = true;

    while (active != 0) {
        active = false;

        for (y = 1; y < DNG_ROOM_AREA_LAST; y++) {
            for (x = 1; x < DNG_ROOM_AREA_LAST; x++) {
                if (scanRoomInfoWork[x + y * DNG_ROOM_AREA] == -1 || scanRoomInfoWork[x + y * DNG_ROOM_AREA] == 9) {
                    continue;
                }

                switch (buildMapDat[x + y * DNG_GRID].content & (BUILD_CELL_ROOM | BUILD_CELL_CORRIDOR | BUILD_CELL_DOOR)) {
                    case BUILD_CELL_CORRIDOR:
                        propagation_mask = BUILD_CELL_CORRIDOR | BUILD_CELL_DOOR | BUILD_CELL_STAIR_DOWN;
                        break;
                    case BUILD_CELL_ROOM:
                        propagation_mask = BUILD_CELL_ROOM | BUILD_CELL_DOOR;
                        break;
                    case BUILD_CELL_ROOM | BUILD_CELL_DOOR:
                        propagation_mask = BUILD_CELL_ROOM | BUILD_CELL_CORRIDOR;
                        break;
                    default:
                        direction_mask = 0;
                        break;
                }

                if (buildMapDat[x + (y - 1) * DNG_GRID].kind != MAP_PARTS_NONE && scanRoomInfoWork[x + (y - 1) * DNG_ROOM_AREA] == -1 && (propagation_mask & buildMapDat[x + (y - 1) * DNG_GRID].content) != 0) {
                    int shared = 0;

                    for (int i = 0; i < DNG_ROOM_MAX; i++) {
                        if (buildMapDat[x + y * DNG_GRID].link[i] == 1 && buildMapDat[x + (y - 1) * DNG_GRID].link[i] == 1) {
                            shared = 1;
                        }
                    }

                    if (shared != 0) {
                        scanRoomInfoWork[x + (y - 1) * DNG_ROOM_AREA] = scanRoomInfoWork[x + y * DNG_ROOM_AREA];
                        active = true;
                    }
                }

                if (buildMapDat[x + (y + 1) * DNG_GRID].kind != MAP_PARTS_NONE && scanRoomInfoWork[x + (y + 1) * DNG_ROOM_AREA] == -1 && (propagation_mask & buildMapDat[x + (y + 1) * DNG_GRID].content) != 0) {
                    int shared = 0;

                    for (int i = 0; i < DNG_ROOM_MAX; i++) {
                        if (buildMapDat[x + y * DNG_GRID].link[i] == 1 && buildMapDat[x + (y + 1) * DNG_GRID].link[i] == 1) {
                            shared = 1;
                        }
                    }

                    if (shared != 0) {
                        scanRoomInfoWork[x + (y + 1) * DNG_ROOM_AREA] = scanRoomInfoWork[x + y * DNG_ROOM_AREA];
                        active = true;
                    }
                }

                if (buildMapDat[x + y * DNG_GRID - 1].kind != MAP_PARTS_NONE && scanRoomInfoWork[x + y * DNG_ROOM_AREA - 1] == -1 && (propagation_mask & buildMapDat[x + y * DNG_GRID - 1].content) != 0) {
                    int shared = 0;

                    for (int i = 0; i < DNG_ROOM_MAX; i++) {
                        if (buildMapDat[x + y * DNG_GRID].link[i] == 1 && buildMapDat[x + y * DNG_GRID - 1].link[i] == 1) {
                            shared = 1;
                        }
                    }

                    if (shared != 0) {
                        scanRoomInfoWork[x + y * DNG_ROOM_AREA - 1] = scanRoomInfoWork[x + y * DNG_ROOM_AREA];
                        active = true;
                    }
                }

                if (buildMapDat[x + y * DNG_GRID + 1].kind != MAP_PARTS_NONE && scanRoomInfoWork[x + y * DNG_ROOM_AREA + 1] == -1 && (propagation_mask & buildMapDat[x + y * DNG_GRID + 1].content) != 0) {
                    int shared = 0;

                    for (int i = 0; i < DNG_ROOM_MAX; i++) {
                        if (buildMapDat[x + y * DNG_GRID].link[i] == 1 && buildMapDat[x + y * DNG_GRID + 1].link[i] == 1) {
                            shared = 1;
                        }
                    }

                    if (shared != 0) {
                        scanRoomInfoWork[x + y * DNG_ROOM_AREA + 1] = scanRoomInfoWork[x + y * DNG_ROOM_AREA];
                        active = true;
                    }
                }
            }
        }
    }

    // The item goes in a room on the stair's side of the door.
    int target_label = scanRoomInfoWork[stair_x + (stair_y + 1) * DNG_ROOM_AREA];
    room_count = 0;
    active = true;

    for (int i = 0; i < this->room_num; i++) {
        if (target_label == scanRoomInfoWork[this->rooms[i].x + (this->rooms[i].y + 1) * DNG_ROOM_AREA + 1]) {
            room_list[room_count++] = i;
        }
    }

    if (room_count == 0) {
        std::println("//////--------> SET NOOMAL DOOR PUTERR!![{}] ///////// ", target_label);
        active = false;
    }

    while (active != 0) {
        int valid;

        int list_no = static_cast<int>((static_cast<float>(room_count) * static_cast<float>(rand())) / 2147483648.0f);

        if (list_no < 0 || list_no > room_count) {
            list_no = 0;
        }

        SearchiDoPutArea(this->cells, this->rooms[room_list[list_no]].x, this->rooms[room_list[list_no]].y, this->rooms[room_list[list_no]].width, this->rooms[room_list[list_no]].height, pos);
        valid = 1;

        if (this->CheckTreasureBox(pos, 20.0f) == 0) {
            valid = 0;
        }

        if (this->CheckAtra(pos, 20.0f) == 0) {
            valid = 0;
        }

        if (this->CheckTrapCircle(pos, 20.0f) != nullptr) {
            valid = 0;
        }

        if (valid != 0) {
            if (this->SetTreasureBox(pos, selectMapNo + ITEM_BONE_KEY, TREASURE_BOX_SMALL, 0) == -1) {
                active = false;
                return;
            }

            this->room_link[0].item_x = (pos[0] + 80.0f) / 160.0f;
            this->room_link[0].item_y = (pos[2] + 80.0f) / 160.0f;
            this->cells[this->room_link[result_no].door_x + this->room_link[result_no].door_y * DNG_GRID].parts_no = door_part;
            this->cells[this->room_link[result_no].door_x + this->room_link[result_no].door_y * DNG_GRID].direction = 0;
            this->room_link[0].used = 1;
            result_no++;
            active = false;
        }
    }
}

// MoonMod shortcut doors: lock each planned doorway with a gate tinted for its key, and hide the key in a small
// chest in the dead end. A rebuild of the same floor locks again only the doors still locked.
void CDungeonMap::PlaceShortcutDoors(int first_build) {
    if (first_build != 0) DngShortcutDoorCount = 0;
    if (first_build == 0 && DngShortcutDoorSeed != this->map_seed) return;
    int placed = 0;
    for (const ShortcutPlan &plan : shortcutPlans) {
        MAP_CELL &cell = this->cells[plan.door_x + plan.door_y * DNG_GRID];
        if (first_build == 0) {
            const int k = placed++;
            if (k < DngShortcutDoorCount && DngShortcutDoorTable[k].locked) {
                cell.parts_no = DngShortcutDoorTable[k].gate_part;
                cell.direction = 0;
            }
            continue;
        }
        if (cell.parts_no < MAP_PARTS_ROOM_DOOR_NORTH || cell.parts_no > MAP_PARTS_ROOM_DOOR_WEST) continue; // another part took it
        // The gate faces the corridor it closes.
        const int x = plan.door_x, y = plan.door_y;
        int gate = -1;
        if ((buildMapDat[x + (y - 1) * DNG_GRID].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) gate = MAP_PARTS_ROOM_GATE_NORTH;
        if ((buildMapDat[x + (y + 1) * DNG_GRID].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) gate = MAP_PARTS_ROOM_GATE_SOUTH;
        if ((buildMapDat[x + y * DNG_GRID + 1].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) gate = MAP_PARTS_ROOM_GATE_EAST;
        if ((buildMapDat[x + y * DNG_GRID - 1].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) gate = MAP_PARTS_ROOM_GATE_WEST;
        if (gate < 0) continue;
        // The key goes in the dead end: the next of the seven keys, passing over the floor's own gate key so no one key
        // fits two locks.
        int key = ITEM_BONE_KEY + DngShortcutDoorCount;
        if (key >= ITEM_BONE_KEY + selectMapNo) ++key;
        if (key > ITEM_BONE_KEY + DNG_SHORTCUT_MAX - 1) break;
        if (zonesOn && key >= DngZoneGateKey(DngZoneCount - 1)) break; // the rest belong to the zone gates
        const ROOM_INFO &room = roomStack[plan.room];
        float pos[4] = {};
        bool chest = false;
        for (int tries = 0; tries < 8 && !chest; ++tries) {
            SearchiDoPutArea(this->cells, room.x, room.y, room.width, room.height, pos);
            if (this->CheckTreasureBox(pos, 20.0f) == 0 || this->CheckAtra(pos, 20.0f) == 0 || this->CheckTrapCircle(pos, 20.0f) != nullptr) continue;
            chest = this->SetTreasureBox(pos, key, TREASURE_BOX_SMALL, 0) != -1;
        }
        if (!chest) continue;
        DNG_SHORTCUT_DOOR &door = DngShortcutDoorTable[DngShortcutDoorCount++];
        door = {x, y, key, gate, cell.parts_no, cell.direction, 1};
        cell.parts_no = gate;
        cell.direction = 0;
        if (DngShortcutDoorCount >= DNG_SHORTCUT_MAX) break;
    }
    if (first_build != 0) DngShortcutDoorSeed = this->map_seed;
}

// MoonMod Warden room: the floor's Warden room (BuildWardenRoom; failing that, the room behind the floor's room gate)
// has its doorways closed, each by a gate facing the corridor it shuts. The back floor (built with first_build 0, to
// its own layout) has none.
void CDungeonMap::PlaceWardenRoom(int first_build) {
    if (first_build == 0) return;
    DngWardenRoomState = DNG_WARDEN_NONE;
    DngWardenDoorCount = 0;
    DngWardenRoomSeed = this->map_seed;
    if (!DngWardenRoomEnabled) return;
    int room = wardenRoom;
    // The room gate: a gate that is not a shortcut door. The room lies on the side away from its corridor.
    for (int row = 0; row < DNG_GRID && room < 0; ++row) {
        for (int col = 0; col < DNG_GRID && room < 0; ++col) {
            const int part = this->cells[col + row * DNG_GRID].parts_no;
            if (part < MAP_PARTS_ROOM_GATE_NORTH || part > MAP_PARTS_ROOM_GATE_WEST) continue;
            bool shortcut = false;
            for (int d = 0; d < DngShortcutDoorCount; ++d) {
                shortcut = shortcut || (DngShortcutDoorTable[d].x == col && DngShortcutDoorTable[d].y == row);
            }
            if (shortcut) continue;
            for (int r = 0; r < roomStackCnt; ++r) {
                const ROOM_INFO &info = roomStack[r];
                if (col >= info.x - 1 && col <= info.x + info.width && row >= info.y - 1 && row <= info.y + info.height &&
                    (buildMapDat[col + row * DNG_GRID].content & BUILD_CELL_ROOM) != 0 && buildMapDat[col + row * DNG_GRID].link[r] == 1) {
                    room = r;
                    break;
                }
            }
        }
    }
    if (room < 0) return;
    const ROOM_INFO &info = roomStack[room];
    DngWardenRoomX = info.x;
    DngWardenRoomY = info.y;
    DngWardenRoomW = info.width;
    DngWardenRoomH = info.height;
    for (const auto &[x, y] : RoomDoors(room)) {
        MAP_CELL &cell = this->cells[x + y * DNG_GRID];
        const bool doorway = (cell.parts_no >= MAP_PARTS_ROOM_DOOR_NORTH && cell.parts_no <= MAP_PARTS_ROOM_DOOR_WEST) ||
                             (cell.parts_no >= MAP_PARTS_ROOM_DOOR_B_NORTH && cell.parts_no <= MAP_PARTS_ROOM_DOOR_B_WEST);
        if (!doorway || DngWardenDoorCount >= DNG_WARDEN_DOORS) continue;
        int gate = -1;
        if ((buildMapDat[x + (y - 1) * DNG_GRID].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) gate = MAP_PARTS_ROOM_GATE_NORTH;
        if ((buildMapDat[x + (y + 1) * DNG_GRID].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) gate = MAP_PARTS_ROOM_GATE_SOUTH;
        if ((buildMapDat[x + y * DNG_GRID + 1].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) gate = MAP_PARTS_ROOM_GATE_EAST;
        if ((buildMapDat[x + y * DNG_GRID - 1].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) gate = MAP_PARTS_ROOM_GATE_WEST;
        if (gate < 0) continue;
        DngWardenDoorTable[DngWardenDoorCount++] = {x, y, gate, cell.parts_no, cell.direction};
        cell.parts_no = gate;
        cell.direction = 0;
    }
    DngWardenRoomState = DngWardenDoorCount > 0 ? DNG_WARDEN_SEALED : DNG_WARDEN_OPEN;
}

void CDungeonMap::SetUnderLoad() {
    for (int i = 0; i < DNG_GRID; i++) {
        this->cells[i * DNG_GRID + DNG_GRID_LAST].parts_no = MAP_PARTS_URA_ROAD;
        this->cells[i * DNG_GRID + DNG_GRID_LAST].direction = 0;
    }
}

int CDungeonMap::CreatPartsList(int *list, int max, int first_part, int last_part) {
    int            num = 0;
    PARTS_ADJACENT adjacent = {
        {{0, -1}, {0, 1}, {-1, 0}, {1, 0}}
    };
    int row;
    int col;
    int free;
    int i;

    for (row = 1; row < DNG_ROOM_AREA_LAST; row++) {
        for (col = 1; col < DNG_ROOM_AREA_LAST; col++) {
            // Key pieces and other specials never replace the exit room's cells.
            if (DNG_IN_EXIT_ROOM(col, row)) {
                continue;
            }
            if (last_part != -1) {

                // Any cell that holds a part of a kind in the range will do.
                if (this->cells[col + row * DNG_GRID].parts_no >= first_part && this->cells[col + row * DNG_GRID].parts_no <= last_part) {
                    list[num * 2] = col;
                    list[num * 2 + 1] = row;
                    num++;

                    if (num >= max - 1) {
                        row = DNG_ROOM_AREA;
                        break;
                    }
                }
            } else {
                free = 1;

                // A cell that touches one that is already spoken for will not.
                for (i = 0; i < 4; i++) {
                    int dx = adjacent.offset[i][0];
                    int dy = adjacent.offset[i][1];
                    int cell_no = dx + (col + (row + dy) * DNG_GRID);

                    if (this->cells[cell_no].parts_no != MAP_PARTS_NONE && (buildMapDat[cell_no].content & BUILD_CELL_ROOM_LINK) == BUILD_CELL_ROOM_LINK) {
                        free = 0;
                    }
                }

                if (this->cells[col + row * DNG_GRID].parts_no == first_part && free != 0) {
                    list[num * 2] = col;
                    list[num * 2 + 1] = row;
                    num++;

                    if (num >= max - 1) {
                        row = DNG_ROOM_AREA;
                        break;
                    }
                }
            }
        }
    }

    return num;
}

namespace {

// Whether a character's own part may go on the floor: the retail rule counts the party (the character is in it
// when it is at least `party` strong); a mod's mask names the characters who can be used (dungeons.set_character_blocks).
bool CharaBlockAllowed(int chara, int party) {
    if (DngCharaBlockMask >= 0) return ((DngCharaBlockMask >> chara) & 1) != 0;
    return UserStatus->party_size >= party;
}

int RollPercent() {
    return static_cast<int>((100.0f * static_cast<float>(rand())) / 2147483648.0f);
}

int RollIndex(int count) {
    return static_cast<int>((static_cast<float>(count) * static_cast<float>(rand())) / 2147483648.0f);
}

constexpr bool HasCandidates(int num) {
    return num > 0 && num <= kMaxPartsCandidates;
}

// The candidate list holds (column, row) pairs.
MAP_CELL &PickCell(CDungeonMap &map, std::span<const int> list, int num) {
    std::size_t entry = static_cast<std::size_t>(RollIndex(num)) * 2;
    return map.cells[list[entry] + list[entry + 1] * kMapWidth];
}

// MoonMod: walks the finished grid from the stair down, the way the floor's parts open, with the cell `closed` (or -1)
// shut as well as every character part already placed. Gates count as open: the exit gate takes the floor's key, a
// shortcut gate its own and a Warden gate opens when the floor is clear. True when every room cell and the stair up
// are reached.
// Which neighbouring cells of a floor a player can walk between: FloorReach's rules, and the map screen's
// (DngMapCellShapes). `closed` cells are shut; with obstacles_block, a character's door and seal stop the way.
struct ReachRules {
    enum { kNone, kRoadCell, kRoom, kDoor, kStair, kShut };
    const CDungeonMap   &map;
    std::span<const int> closed;
    bool                 obstacles_block;

    static int Outward(int part) {
        static constexpr int kOut[4] = {1, 2, 8, 4};
        for (int first : {MAP_PARTS_ROOM_DOOR_NORTH, MAP_PARTS_ROOM_GATE_NORTH, MAP_PARTS_ROOM_DOOR_B_NORTH, MAP_PARTS_ROOM_DOOR_C_NORTH,
                          MAP_PARTS_KEY_DOOR_XIAO_NORTH, MAP_PARTS_KEY_DOOR_GORO_NORTH, MAP_PARTS_KEY_DOOR_RUBY_NORTH}) {
            if (part >= first && part < first + 4) return kOut[part - first];
        }
        return 0;
    }
    int Kind(int cell) const {
        const int part = map.cells[cell].parts_no;
        const bool character = part == MAP_PARTS_KEY_XIAO || part == MAP_PARTS_KEY_UNGAGA || part == MAP_PARTS_KEY_OSMOND ||
                               (part >= MAP_PARTS_KEY_DOOR_XIAO_NORTH && part <= MAP_PARTS_KEY_DOOR_RUBY_WEST);
        if (character && !obstacles_block) {
            if (part == MAP_PARTS_KEY_XIAO || part == MAP_PARTS_KEY_UNGAGA || part == MAP_PARTS_KEY_OSMOND) return static_cast<int>(kStair);
            return static_cast<int>(kDoor);
        }
        if (std::ranges::contains(closed, cell) || character ||
            (part >= MAP_PARTS_DIVIDE_END_NORTH && part <= MAP_PARTS_DIVIDE_END_WEST) ||
            (part >= MAP_PARTS_DIVIDE_WALL_NORTH && part <= MAP_PARTS_DIVIDE_WALL_WEST)) {
            return part == MAP_PARTS_NONE ? static_cast<int>(kNone) : static_cast<int>(kShut);
        }
        if (part == MAP_PARTS_NONE || part == MAP_PARTS_URA_ROAD) return static_cast<int>(kNone);
        if (part <= MAP_PARTS_ROAD_DEAD_END) return static_cast<int>(kRoadCell);
        if (Outward(part) != 0) return static_cast<int>(kDoor);
        if (part == MAP_PARTS_STAIR_DOWN || part == MAP_PARTS_STAIR_UP ||
            (part >= MAP_PARTS_URA_ENTRANCE_NORTH && part <= MAP_PARTS_URA_ENTRANCE_WEST)) {
            return static_cast<int>(kStair);
        }
        return static_cast<int>(kRoom);
    }
    // Side bits: 1 north, 2 east, 4 west, 8 south.
    bool Opens(int cell, int side, int next) const {
        static constexpr int kRoad[][3] = {{6, 0, 0}, {9, 0, 1}, {5, 1, 0}, {3, 1, 1}, {10, 1, 2}, {12, 1, 3}, {0, 2, 0}, {1, 3, 0},
                                           {2, 3, 1}, {8, 3, 2}, {4, 3, 3}, {7, 4, 0}, {11, 4, 1}, {14, 4, 2}, {13, 4, 3}};
        const int part = map.cells[cell].parts_no;
        const int k = Kind(cell), nk = Kind(next);
        switch (k) {
            case kRoadCell:
                for (const auto &road : kRoad) {
                    if (road[1] == part && road[2] == map.cells[cell].direction) return ((15 & ~road[0]) & side) != 0;
                }
                return false;
            case kRoom: return nk == kRoom || nk == kDoor || nk == kStair;
            case kDoor: return side == Outward(part) || nk == kRoom || nk == kDoor;
            case kStair: return true;
            default: return false;
        }
    }
};

std::vector<char> FloorReach(const CDungeonMap &map, std::span<const int> closed, bool obstacles_block) {
    static constexpr int kStep[4][2] = {{0, -1}, {1, 0}, {-1, 0}, {0, 1}};
    static constexpr int kSide[4] = {1, 2, 4, 8};
    const ReachRules rules{map, closed, obstacles_block};
    enum { kRoom = ReachRules::kRoom };
    const auto kind = [&](int cell) { return rules.Kind(cell); };
    const auto opens = [&](int cell, int side, int next) { return rules.Opens(cell, side, next); };
    int start = -1;
    for (int cell = 0; cell < DNG_GRID * DNG_GRID && start < 0; ++cell) {
        if (map.cells[cell].parts_no == MAP_PARTS_STAIR_DOWN) start = cell;
    }
    if (start < 0) return {}; // nothing to walk from
    std::vector<char> seen(DNG_GRID * DNG_GRID, 0);
    std::vector<int>  queue = {start};
    seen[start] = 1;
    while (!queue.empty()) {
        const int at = queue.back();
        queue.pop_back();
        const int col = at % DNG_GRID, row = at / DNG_GRID;
        for (int s = 0; s < 4; ++s) {
            const int nc = col + kStep[s][0], nr = row + kStep[s][1];
            if (nc < 0 || nr < 0 || nc >= DNG_GRID || nr >= DNG_GRID) continue;
            const int next = nc + nr * DNG_GRID;
            const int back = kSide[s] == 1 ? 8 : kSide[s] == 8 ? 1 : kSide[s] == 2 ? 4 : 2;
            if (seen[next] || !opens(at, kSide[s], next) || !opens(next, back, at)) continue;
            seen[next] = 1;
            queue.push_back(next);
        }
    }
    // bit 1: reached; bit 2: a room cell
    for (int cell = 0; cell < DNG_GRID * DNG_GRID; ++cell) {
        if (kind(cell) == kRoom) seen[cell] |= 2;
    }
    return seen;
}

bool FloorReachable(const CDungeonMap &map, int closed, bool obstacles_block = true) {
    const int shut[1] = {closed};
    const std::vector<char> seen = FloorReach(map, std::span<const int>(shut, closed >= 0 ? 1 : 0), obstacles_block);
    if (seen.empty()) return true; // nothing to walk from: leave the floor as the game builds it
    for (int cell = 0; cell < DNG_GRID * DNG_GRID; ++cell) {
        if ((seen[cell] & 1) != 0) continue;
        if ((seen[cell] & 2) != 0 || map.cells[cell].parts_no == MAP_PARTS_STAIR_UP) return false;
    }
    return true;
}

// MoonMod zones (dungeons.set_zones): the planned gates go in, and every cell is given its zone: what the stair down
// reaches with every gate shut is the first zone, what opening the gate into the next adds is the next, and so on.
// Gates the mod named as opened are put back to doorways afterwards. A floor where a room landed outside its own zone,
// or the stair up isn't in the last, is marked to be built again.
void PlaceZoneGates(CDungeonMap &map) {
    DngZoneGateCount = 0;
    DngZoneFloorCount = 0;
    std::fill(DngZoneOfCell, DngZoneOfCell + DNG_CELLS, static_cast<unsigned char>(DNG_ZONE_NONE));
    if (!zonesOn || zonesFailed) return;
    DngZoneFloorSeed = map.map_seed;
    std::vector<int> gates;
    for (const ZoneGatePlan &plan : zoneGatePlans) {
        const int x = plan.door_x, y = plan.door_y;
        MAP_CELL &cell = map.cells[x + y * DNG_GRID];
        const bool doorway = (cell.parts_no >= MAP_PARTS_ROOM_DOOR_NORTH && cell.parts_no <= MAP_PARTS_ROOM_DOOR_WEST) ||
                             (cell.parts_no >= MAP_PARTS_ROOM_DOOR_B_NORTH && cell.parts_no <= MAP_PARTS_ROOM_DOOR_B_WEST);
        int gate = -1;
        if ((buildMapDat[x + (y - 1) * DNG_GRID].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) gate = MAP_PARTS_ROOM_GATE_NORTH;
        if ((buildMapDat[x + (y + 1) * DNG_GRID].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) gate = MAP_PARTS_ROOM_GATE_SOUTH;
        if ((buildMapDat[x + y * DNG_GRID + 1].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) gate = MAP_PARTS_ROOM_GATE_EAST;
        if ((buildMapDat[x + y * DNG_GRID - 1].content & BUILD_CELL_CORRIDOR) == BUILD_CELL_CORRIDOR) gate = MAP_PARTS_ROOM_GATE_WEST;
        if (!doorway || gate < 0) {
            zonesFailed = true; // another part took the doorway
            return;
        }
        const bool smoke = (DngZoneSmokeMask & (1 << plan.zone)) != 0;
        DngZoneGateTable[DngZoneGateCount++] = {x, y, plan.zone, smoke ? -1 : DngZoneGateKey(plan.zone), gate, cell.parts_no, cell.direction,
                                                (DngZoneOpenMask & (1 << plan.zone)) == 0 ? 1 : 0};
        cell.parts_no = gate;
        cell.direction = 0;
        gates.push_back(x + y * DNG_GRID);
    }
    for (int zone = 0; zone < DngZoneCount; ++zone) {
        // gates[k] leads into zone k + 1: the gates past this zone stay shut
        const std::vector<char> seen = FloorReach(map, std::span<const int>(gates).subspan(static_cast<std::size_t>(zone)), false);
        if (seen.empty()) {
            zonesFailed = true;
            return;
        }
        for (int cell = 0; cell < DNG_CELLS; ++cell) {
            if ((seen[cell] & 1) != 0 && DngZoneOfCell[cell] == DNG_ZONE_NONE) DngZoneOfCell[cell] = static_cast<unsigned char>(zone);
        }
    }
    for (int r = 0; r < roomStackCnt; ++r) {
        if (r == wardenRoom) continue;
        bool home = false;
        for (int y = roomStack[r].y; y < roomStack[r].y + roomStack[r].height; ++y) {
            for (int x = roomStack[r].x; x < roomStack[r].x + roomStack[r].width; ++x) {
                const int zone = DngZoneOfCell[x + y * DNG_GRID];
                if (zone == DNG_ZONE_NONE) continue;
                if (zone != roomZone[r]) zonesFailed = true;
                home = true;
            }
        }
        if (!home) zonesFailed = true;
    }
    for (int cell = 0; cell < DNG_CELLS; ++cell) {
        if (map.cells[cell].parts_no == MAP_PARTS_STAIR_UP && DngZoneOfCell[cell] != DngZoneCount - 1) zonesFailed = true;
    }
    for (int g = 0; g < DngZoneGateCount; ++g) {
        if (DngZoneGateTable[g].locked) continue;
        MAP_CELL &cell = map.cells[DngZoneGateTable[g].x + DngZoneGateTable[g].y * DNG_GRID];
        cell.parts_no = DngZoneGateTable[g].open_part;
        cell.direction = DngZoneGateTable[g].open_direction;
    }
    if (!zonesFailed) {
        DngZoneFloorCount = DngZoneCount;
        for (int z = 0; z < DNG_ZONE_MAX; ++z) DngZoneFloorSource[z] = z < DngZoneCount ? DngZoneSourceFloor[z] : -1;
        DngZoneFloorExit = DngZoneExitFloor;
    }
}

// A floor that would not come out as zones in its rebuilds is left whole: its gates open, no zones.
void DropZones(CDungeonMap &map) {
    for (int g = 0; g < DngZoneGateCount; ++g) {
        MAP_CELL &cell = map.cells[DngZoneGateTable[g].x + DngZoneGateTable[g].y * DNG_GRID];
        if (cell.parts_no == DngZoneGateTable[g].gate_part) {
            cell.parts_no = DngZoneGateTable[g].open_part;
            cell.direction = DngZoneGateTable[g].open_direction;
        }
    }
    DngZoneGateCount = 0;
    DngZoneFloorCount = 0;
    std::fill(DngZoneOfCell, DngZoneOfCell + DNG_CELLS, static_cast<unsigned char>(DNG_ZONE_NONE));
}

// A character part goes on one of the candidate cells. When a mod chooses who can be used (the Gauntlet's drafted
// party), it only goes where the whole floor stays reachable without passing it, so it is a way round, never the only
// way on; a floor with no such cell gets none. The retail rule picks any candidate.
// (MoonMod: never on the Warden room's doorway, which its own gate closes.)
MAP_CELL *PickCharacterCell(CDungeonMap &map, std::span<const int> list, int num) {
    if (DngCharaBlockMask < 0 && wardenRoom < 0) return &PickCell(map, list, num);
    const int first = RollIndex(num);
    for (int k = 0; k < num; ++k) {
        const std::size_t entry = static_cast<std::size_t>((first + k) % num) * 2;
        const int cell = list[entry] + list[entry + 1] * kMapWidth;
        if (wardenRoom >= 0 && buildMapDat[cell].link[wardenRoom] != 0) continue;
        if (DngCharaBlockMask < 0 || FloorReachable(map, cell)) return &map.cells[cell];
    }
    return nullptr;
}

int PlaceKeyPart(CDungeonMap &map, std::span<int> list, int key_part) {
    int num = map.CreatPartsList(list.data(), kMaxPartsCandidates, 0, -1);

    if (HasCandidates(num)) {
        MAP_CELL *cell = PickCharacterCell(map, list, num);
        if (cell == nullptr) return 0;
        cell->parts_no = key_part;
    }

    return num;
}

int PlaceKeyDoor(CDungeonMap &map, std::span<int> list, int key_door_north) {
    int num = map.CreatPartsList(list.data(), kMaxPartsCandidates, MAP_PARTS_ROOM_DOOR_NORTH, MAP_PARTS_ROOM_DOOR_WEST);

    if (HasCandidates(num)) {
        MAP_CELL *cell = PickCharacterCell(map, list, num);
        if (cell == nullptr) return 0;
        cell->parts_no += key_door_north - MAP_PARTS_ROOM_DOOR_NORTH;
    }

    return num;
}

} // namespace

void CDungeonMap::BuildCharaSpecialParts() {
    if (selectMapNo >= DUNGEON_DEMON_SHAFT) {
        return;
    }

    auto           list_storage = std::make_unique<int[]>(kPartsListSize);
    std::span<int> list(list_storage.get(), kPartsListSize);
    int            zone = UserStatus->res_limit_zone_current;

    // A resurrection zone puts that character's door on the floor and nothing else.
    if (zone >= 0 && zone < 6) {
        if (zone == RES_LIMIT_ZONE_XIAO) {
            // Both outcomes of this roll place Toan's door, but the roll still advances rand().
            static_cast<void>(RollPercent());
            this->SetCharaDoor(CHARA_TOAN);
            return;
        }

        this->SetCharaDoor(zone);
        return;
    }

    // The first floors of the first dungeon never take a special part.
    int roll = RollPercent();

    if (selectMapNo == DUNGEON_DIVINE_BEAST_CAVE && UserStatus->cur_floor < 8) {
        roll = 0;
    }

    if (CharaBlockAllowed(CHARA_XIAO, 2) && roll > 50) {
        PlaceKeyPart(*this, list, MAP_PARTS_KEY_XIAO);
    }

    // Which special part a floor takes depends on the dungeon, how far the party has come and a
    // roll of the dice.
    roll = RollPercent();
    int floor = UserStatus->cur_floor;

    switch (selectMapNo) {
        case DUNGEON_WISE_OWL_FOREST:
            if (CharaBlockAllowed(CHARA_GORO, 3) && roll > 10 && floor >= 9 &&
                HasCandidates(PlaceKeyDoor(*this, list, MAP_PARTS_KEY_DOOR_GORO_NORTH))) {
                return;
            }

            break;
        case DUNGEON_SHIPWRECK:
            if (CharaBlockAllowed(CHARA_GORO, 3) && roll < 40 &&
                HasCandidates(PlaceKeyDoor(*this, list, MAP_PARTS_KEY_DOOR_GORO_NORTH))) {
                return;
            }

            if (CharaBlockAllowed(CHARA_RUBY, 4) && roll >= 40 && floor >= 9 &&
                HasCandidates(PlaceKeyDoor(*this, list, MAP_PARTS_KEY_DOOR_RUBY_NORTH))) {
                return;
            }

            break;
        case DUNGEON_SUN_MOON_TEMPLE:
            if (CharaBlockAllowed(CHARA_GORO, 3) && roll < 20 &&
                HasCandidates(PlaceKeyDoor(*this, list, MAP_PARTS_KEY_DOOR_GORO_NORTH))) {
                return;
            }

            if (CharaBlockAllowed(CHARA_RUBY, 4) && roll < 40 &&
                HasCandidates(PlaceKeyDoor(*this, list, MAP_PARTS_KEY_DOOR_RUBY_NORTH))) {
                return;
            }

            if (CharaBlockAllowed(CHARA_UNGAGA, 5) && roll >= 40 && floor >= 9 &&
                HasCandidates(PlaceKeyPart(*this, list, MAP_PARTS_KEY_UNGAGA))) {
                return;
            }

            break;
        case DUNGEON_MOON_SEA:
            if (CharaBlockAllowed(CHARA_GORO, 3) && roll < 10 &&
                HasCandidates(PlaceKeyDoor(*this, list, MAP_PARTS_KEY_DOOR_GORO_NORTH))) {
                return;
            }

            if (CharaBlockAllowed(CHARA_RUBY, 4) && roll < 20 &&
                HasCandidates(PlaceKeyDoor(*this, list, MAP_PARTS_KEY_DOOR_RUBY_NORTH))) {
                return;
            }

            if (CharaBlockAllowed(CHARA_UNGAGA, 5) && roll < 30 &&
                HasCandidates(PlaceKeyPart(*this, list, MAP_PARTS_KEY_UNGAGA))) {
                return;
            }

            if (CharaBlockAllowed(CHARA_OSMOND, 6) && roll > 40 && floor >= 8 &&
                HasCandidates(PlaceKeyPart(*this, list, MAP_PARTS_KEY_OSMOND))) {
                return;
            }

            break;
        case DUNGEON_GALLERY_OF_TIME:
            if (CharaBlockAllowed(CHARA_GORO, 3) && roll < 10 &&
                HasCandidates(PlaceKeyDoor(*this, list, MAP_PARTS_KEY_DOOR_GORO_NORTH))) {
                return;
            }

            if (CharaBlockAllowed(CHARA_RUBY, 4) && roll < 20 &&
                HasCandidates(PlaceKeyDoor(*this, list, MAP_PARTS_KEY_DOOR_RUBY_NORTH))) {
                return;
            }

            if (CharaBlockAllowed(CHARA_UNGAGA, 5) && roll < 30 &&
                HasCandidates(PlaceKeyPart(*this, list, MAP_PARTS_KEY_UNGAGA))) {
                return;
            }

            if (CharaBlockAllowed(CHARA_OSMOND, 6) && roll > 40) {
                PlaceKeyPart(*this, list, MAP_PARTS_KEY_OSMOND);
            }

            break;
        default:
            break;
    }
}

int CDungeonMap::SetCharaDoor(int chara_no) {
    auto           list_storage = std::make_unique<int[]>(kPartsListSize);
    std::span<int> list(list_storage.get(), kPartsListSize);

    // Each character's door stands on a map part that suits it.
    switch (chara_no) {
        case CHARA_TOAN:
            if (UserStatus->party_size >= 2) {
                return PlaceKeyPart(*this, list, MAP_PARTS_KEY_XIAO);
            }

            return 0;
        case CHARA_XIAO:
            return PlaceKeyDoor(*this, list, MAP_PARTS_KEY_DOOR_XIAO_NORTH);
        case CHARA_GORO:
            return PlaceKeyDoor(*this, list, MAP_PARTS_KEY_DOOR_GORO_NORTH);
        case CHARA_RUBY:
            return PlaceKeyDoor(*this, list, MAP_PARTS_KEY_DOOR_RUBY_NORTH);
        case CHARA_UNGAGA:
            return PlaceKeyPart(*this, list, MAP_PARTS_KEY_UNGAGA);
        case CHARA_OSMOND:
            return PlaceKeyPart(*this, list, MAP_PARTS_KEY_OSMOND);
        default:
            return 0;
    }
}

// How many times the floor being built has been built again because it couldn't be crossed (MoonMod).
int floorRebuilds = 0;

// Lays the walk-through exit room into buildMapDat (defined after buildRandomMap).
static int buildExitRoom();

void CDungeonMap::buildRandomMap(int room_max, int first_build) {
#ifdef PORT
    const int room_max_asked = room_max;
#endif
    int w;
    int h;
    int j;
    int i;
    int col;
    int row;
    int x;
    int y;
    int done;
    int retry;
    int dist;
    int near_no;
    int near_dist;

    // A mod's layout (dungeons.set_layout) and room bonus decide how many rooms the floor gets.
    moonmod::ChroniclePrepareFloorGeneration(first_build == 0);
    if (DngLayoutRooms > 0) {
        room_max = DngLayoutRooms;
    }
    room_max = std::min(DNG_ROOM_MAX, (room_max * (100 + DngRoomBonusPercent) + 99) / 100);
    // MoonMod dungeons.set_rest_room: a rest floor is its two rooms and nothing else.
    // (A rest floor built again from its seed, as on a reload, comes out the same.)
    const bool rest = DngRestRoomEnabled != 0 && (first_build != 0 || this->map_seed == DngRestRoomSeed);
    if (rest) room_max = 2;
    // MoonMod dungeons.set_zones: the zones' rooms, each zone in its own square.
    zonesOn = first_build != 0 && DngZoneCount >= 2 && !rest && ZonesFit(DngZoneCount);
    zonesFailed = false;
    if (zonesOn) {
        room_max = 0;
        for (int z = 0; z < DngZoneCount; ++z) room_max += DngZoneRooms[z];
        room_max = std::min(room_max, DNG_ROOM_MAX - 1);
    }

    // A floor that is built for the first time picks the seed it is built from.
    if (first_build != 0) {
        this->map_seed = static_cast<int>(static_cast<float>(rand()) / 100000.0f);
        // Test hook: rebuild a reported floor exactly.
        if (const char *forced = std::getenv("DC_MAZE_SEED"); forced != nullptr && *forced != '\0' && floorRebuilds == 0) {
            this->map_seed = std::atoi(forced);
        }
        // MoonMod dungeons.set_seed: a mod's seed for this floor (a floor that can't be crossed still gets another).
        if (DngNextSeed >= 0 && floorRebuilds == 0) this->map_seed = DngNextSeed;
        srand(this->map_seed);
    }
    DngNextSeed = -1;

    for (i = 0; i < DNG_EVENT_MAX; i++) {
        this->events[i].kind = DNG_EVENT_NONE;
        this->events[i].reset_flag = 0;
    }

    for (i = 0; i < DNG_BOX_MAX; i++) {
        this->boxes[i].used = 0;
        this->boxes[i].lid_angle = 0;
        this->boxes[i].trap_no = 0;
    }

    this->box_num = 0;

    for (i = 0; i < 8; i++) {
        this->atra[i].used = 0;
    }

    this->atra_num = 0;

    for (i = 0; i < 4; i++) {
        this->room_link[i].used = 0;
    }

    for (row = 0; row < DNG_GRID; row++) {
        for (col = 0; col < DNG_GRID; col++) {
            buildMapDat[col + row * DNG_GRID].kind = MAP_PARTS_NONE;
            buildMapDat[col + row * DNG_GRID].direction = 0;

            for (j = 0; j < DNG_ROOM_MAX; j++) {
                buildMapDat[col + row * DNG_GRID].link[j] = 0;
            }

            buildMapDat[col + row * DNG_GRID].content = 0;
            this->cells[col + row * DNG_GRID].direction = 0;
        }
    }

    // Two rooms have to stand before any other can join to one.
    roomStackCnt = 0;
    retry = 0;
    if (rest) BuildRestRooms();

    while (roomStackCnt < 2 && retry < 0x40) {
        done = 0;
        retry = 0;

        while (done == 0) {
            w = static_cast<int>((DNG_BUILD_ROOM_SPAN_F * static_cast<float>(rand())) / 2147483648.0f) + DNG_BUILD_ROOM_MIN;
            h = static_cast<int>((DNG_BUILD_ROOM_SPAN_F * static_cast<float>(rand())) / 2147483648.0f) + DNG_BUILD_ROOM_MIN;
            x = static_cast<int>((static_cast<float>(DNG_BUILD_AREA - w) * static_cast<float>(rand())) / 2147483648.0f);
            y = static_cast<int>((static_cast<float>(DNG_BUILD_AREA - h) * static_cast<float>(rand())) / 2147483648.0f);

            if (buildRoom(x, y, w, h, roomStackCnt) == 1) {
                roomStack[roomStackCnt].x = x;
                roomStack[roomStackCnt].y = y;
                roomStack[roomStackCnt].width = w;
                roomStack[roomStackCnt].height = h;
                roomZone[roomStackCnt] = 0;
                roomStackCnt++;
                done = 1;
            } else {
                retry++;

                if (retry >= 0x40) {
                    done = 1;
                }
            }
        }
    }

    shortcutJoins.clear();
    joinRoomRecorded(0, 1);

    // Every further room joins to whichever room its middle is nearest to.
    // (MoonMod zones: a room goes in its zone's square and joins the nearest room of its own zone; a zone's first room
    // waits for JoinZones.)
    while (roomStackCnt <= room_max - 1 && retry < 0x200) {
        done = 0;
        retry = 0;

        while (done == 0) {
            const int zone = zonesOn ? ZoneOfRoom(roomStackCnt) : 0;
            w = static_cast<int>((DNG_BUILD_ROOM_SPAN_F * static_cast<float>(rand())) / 2147483648.0f) + DNG_BUILD_ROOM_MIN;
            h = static_cast<int>((DNG_BUILD_ROOM_SPAN_F * static_cast<float>(rand())) / 2147483648.0f) + DNG_BUILD_ROOM_MIN;
            x = static_cast<int>((static_cast<float>(DNG_BUILD_AREA - w) * static_cast<float>(rand())) / 2147483648.0f);
            y = static_cast<int>((static_cast<float>(DNG_BUILD_AREA - h) * static_cast<float>(rand())) / 2147483648.0f);
            if (zonesOn) {
                x += ZoneOriginX(zone);
                y += ZoneOriginY(zone);
            }

            if (buildRoom(x, y, w, h, roomStackCnt) == 1) {
                roomStack[roomStackCnt].x = x;
                roomStack[roomStackCnt].y = y;
                roomStack[roomStackCnt].width = w;
                roomStack[roomStackCnt].height = h;
                roomZone[roomStackCnt] = zone;
                roomStackCnt++;
                near_no = -1;
                near_dist = 10000;

                for (j = 0; j < roomStackCnt - 1; j++) {
                    if (zonesOn && roomZone[j] != zone) continue;
                    w = (roomStack[roomStackCnt - 1].x + roomStack[roomStackCnt - 1].width / 2) - (roomStack[j].x + roomStack[j].width / 2);
                    h = (roomStack[roomStackCnt - 1].y + roomStack[roomStackCnt - 1].height / 2) - (roomStack[j].y + roomStack[j].height / 2);

                    dist = static_cast<int>(std::sqrt(static_cast<double>(w * w + h * h)));

                    if (dist < near_dist) {
                        near_no = j;
                        near_dist = dist;
                    }
                }

                if (near_no >= 0) joinRoomRecorded(roomStackCnt - 1, near_no);
                done = 1;
            } else {
                retry++;

                if (retry >= 0x200) {
                    done = 1;
                }
            }
        }
    }

    if (zonesOn && !JoinZones()) zonesFailed = true;

    // MoonMod dungeons.set_warden_room: the Warden's own room, built last so nothing else joins to it.
    wardenRoom = first_build != 0 && DngWardenRoomEnabled && !rest ? BuildWardenRoom() : -1;

    // MoonMod dungeons.set_shortcuts: dead ends loop back before the rooms are furnished.
    PlanShortcuts(std::min(DngShortcutDoors, DNG_SHORTCUT_MAX));

#ifdef PORT
    RepairCornerDoors();
#endif

    if (rest && roomStackCnt == 2) {
        SetRestObjects();
        DngRestRoomSeed = this->map_seed;
    } else {
        if (first_build != 0) DngRestRoomSeed = -1;
        setRoomObject();

        if (first_build != 0) {
            setRoomDivid();
        }

        setUnderDungeonStart();
    }

    if (first_build != 0) {
        setStair();
    }

    mapPartsFilter();
    // The walk-through exit room (moonmod/dungeonmap_layout.hpp), when it is turned on.
    if (first_build != 0) {
        DngExitRoomValid = 0;
        if (DngExitRoomEnabled) {
            buildExitRoom();
        }
    }

    this->room_num = roomStackCnt;
    std::ranges::copy(roomStack, this->rooms);

    for (row = 0; row < DNG_GRID; row++) {
        for (col = 0; col < DNG_GRID; col++) {
            this->cells[col + row * DNG_GRID].parts_no = buildMapDat[col + row * DNG_GRID].kind;
            this->cells[col + row * DNG_GRID].direction = buildMapDat[col + row * DNG_GRID].direction;
        }
    }

    if (selectMapNo == DUNGEON_MOON_SEA) {
        this->buildDummyModel();
    }

    // (MoonMod: a floor with a Warden room has that sealed room instead of a gated one.)
    if (static_cast<int>((100.0f * static_cast<float>(rand())) / 2147483648.0f) < 0x33 && first_build != 0 && shortcutPlans.empty() &&
        wardenRoom < 0 && !rest && !zonesOn) {
        this->GetRoomLinkInfo();
    }

    if (first_build != 0 && !rest) {
        this->BuildCharaSpecialParts();
    }

    this->PlaceShortcutDoors(first_build);
    this->PlaceWardenRoom(first_build);
    if (first_build != 0) PlaceZoneGates(*this);

    // Which key opens the floor's gate.
    switch (selectMapNo) {
        case DUNGEON_DIVINE_BEAST_CAVE:
            this->gate_key_no = ITEM_DRAN_S_CREST;
            break;
        case DUNGEON_SHIPWRECK:
            this->gate_key_no = ITEM_HOOK;
            break;
        case DUNGEON_SUN_MOON_TEMPLE:
            this->gate_key_no = ITEM_KING_S_SLATE;
            break;
        case DUNGEON_MOON_SEA:
            this->gate_key_no = ITEM_GUN_POWDER;
            break;
        case DUNGEON_GALLERY_OF_TIME:
            this->gate_key_no = ITEM_CLOCK_HANDS;
            break;
        case DUNGEON_DEMON_SHAFT:
            this->gate_key_no = ITEM_BLACK_KNIGHT_CREST;
            break;
    }

    if (selectMapNo == DUNGEON_WISE_OWL_FOREST) {
        int                          pick = static_cast<int>((3.0f * static_cast<float>(rand())) / 2147483648.0f);
        constexpr std::array<int, 3> gate_keys = {0xC4, 0xC6, 0xCD};

        if (pick < 0 || pick > 2) {
            pick = 0;
        }

        this->gate_key_no = gate_keys[pick];
    }

    this->SetUnderLoad();

#ifdef PORT
    // A floor whose rooms or exit can't all be reached (the old builder's rare joins that miss) is built again from
    // another seed, a few times at most. Character parts don't count against it: retail floors keep theirs.
    if (first_build != 0 && floorRebuilds < 8 && (!FloorReachable(*this, -1, false) || zonesFailed)) {
        std::println("floor {} could not be {} (seed {}); building another", UserStatus != nullptr ? UserStatus->cur_floor : -1,
                     zonesFailed ? "split into zones" : "crossed", this->map_seed);
        ++floorRebuilds;
        this->buildRandomMap(room_max_asked, first_build);
        --floorRebuilds;
        return;
    }
    if (zonesFailed) DropZones(*this);
#endif
    std::println("map build success!!");
    moonmod::ChronicleFloorGenerated(first_build == 0, this->map_seed, this->room_num);
}

void CDungeonMap::initSubmap(CDataAlloc2<1> *alloc) {
    int i;
    int j;

    for (i = 0; i < 6; i++) {
        this->bg_model[i] = nullptr;
    }

    for (i = 0; i < 3; i++) {
        this->dummy_frame[i] = nullptr;
    }

    this->box_lid_model = nullptr;
    this->box_body_model = nullptr;
    this->box_collision_model = nullptr;
    this->chest_lid_model = nullptr;
    this->chest_body_model = nullptr;
    this->chest_collision_model = nullptr;
    this->fall_model = nullptr;

    for (i = 0; i < DNG_BOX_MAX; i++) {
        this->boxes[i].used = 0;
    }

    for (i = 0; i < this->atra_num; i++) {
        this->atra[i].used = 0;
    }

    this->bg_model_num = 0;
    this->dummy_frame_num = 0;
    this->dummy_num = 0;
    this->water.SetSize(32, 32, alloc);

    for (i = 0; i < 72; i++) {
        this->parts[i].initalize();
    }

    for (i = 0; i < DNG_CELLS; i++) {
        this->cells[i].parts_no = MAP_PARTS_NONE;
        this->cells[i].camera_dist = -1.0f;
        this->mask[i] = 0;
        this->cells[i].visible = false;
    }

    for (i = 0; i < DNG_ROOM_MAX; i++) {
        this->room_seen[i] = 0;
    }

    for (i = 0; i < 4; i++) {
        this->npc[i].chara.Initialize();
        this->npc[i].used = 0;
        this->npc[i].draw_num = 0;
    }

    for (j = 0; j < DNG_EVENT_MAX; j++) {
        this->events[j].kind = DNG_EVENT_NONE;
        this->events[j].reset_flag = 0;
    }

    for (j = 0; j < DNG_BOX_MAX; j++) {
        this->boxes[j].used = 0;
        this->boxes[j].lid_angle = 0;
        this->boxes[j].trap_no = 0;
    }

    this->box_num = 0;

    for (j = 0; j < 8; j++) {
        this->atra[j].used = 0;
    }

    this->atra_num = 0;

    for (j = 0; j < 4; j++) {
        this->room_link[j].used = 0;
    }
}

void CDungeonMap::initalize() {
    int i;
    int j;

    for (i = 0; i < 6; i++) {
        this->bg_model[i] = nullptr;
    }

    for (i = 0; i < 3; i++) {
        this->dummy_frame[i] = nullptr;
    }

    this->box_lid_model = nullptr;
    this->box_body_model = nullptr;
    this->box_collision_model = nullptr;
    this->chest_lid_model = nullptr;
    this->chest_body_model = nullptr;
    this->chest_collision_model = nullptr;
    this->fall_model = nullptr;

    for (i = 0; i < DNG_BOX_MAX; i++) {
        this->boxes[i].used = 0;
    }

    for (i = 0; i < this->atra_num; i++) {
        this->atra[i].used = 0;
    }

    this->bg_model_num = 0;
    this->dummy_frame_num = 0;
    this->dummy_num = 0;

    for (i = 0; i < 72; i++) {
        this->parts[i].initalize();
    }

    for (i = 0; i < DNG_CELLS; i++) {
        this->cells[i].parts_no = MAP_PARTS_NONE;
        this->cells[i].camera_dist = -1.0f;
        this->mask[i] = 0;
        this->cells[i].visible = false;
    }

    for (i = 0; i < DNG_ROOM_MAX; i++) {
        this->room_seen[i] = 0;
    }

    for (i = 0; i < 4; i++) {
        this->npc[i].chara.Initialize();
        this->npc[i].used = 0;
        this->npc[i].draw_num = 0;
    }

    for (j = 0; j < DNG_EVENT_MAX; j++) {
        this->events[j].kind = DNG_EVENT_NONE;
        this->events[j].reset_flag = 0;
    }

    for (j = 0; j < DNG_BOX_MAX; j++) {
        this->boxes[j].used = 0;
        this->boxes[j].lid_angle = 0;
        this->boxes[j].trap_no = 0;
    }

    this->box_num = 0;

    for (j = 0; j < 8; j++) {
        this->atra[j].used = 0;
    }

    this->atra_num = 0;

    for (j = 0; j < 4; j++) {
        this->room_link[j].used = 0;
    }

    this->draw_dist_scale = 4.0f;
}


#ifdef PORT
/* Finds a free block and lays the exit room into buildMapDat after the parts
 * filter has run. Returns 0 when the grid has no room for it. */
static int buildExitRoom() {
    DngExitRoomValid = 0;
    // Room columns ox..ox+2, rows oy-1 (town stair) .. oy+3 (next-floor stair),
    // plus a one-cell empty border. Columns DNG_GRID-2/-1 hold the back-floor road.
    for (int oy = DNG_GRID - 6; oy >= 3; oy--) {
        for (int ox = DNG_GRID - 6; ox >= 1; ox--) {
            int free = 1;
            for (int y = oy - 2; y <= oy + 4 && free; y++) {
                for (int x = ox - 1; x <= ox + 3; x++) {
                    const BUILD_MAP_INFO &cell = buildMapDat[x + y * DNG_GRID];
                    if (cell.kind != MAP_PARTS_NONE || cell.content != 0) {
                        free = 0;
                        break;
                    }
                }
            }
            if (!free) continue;
            static const int layout[5][3][2] = {
                {{MAP_PARTS_NONE, 0}, {MAP_PARTS_STAIR_DOWN, 0}, {MAP_PARTS_NONE, 0}},
                {{MAP_PARTS_ROOM_CORNER_NORTH_WEST, 0}, {MAP_PARTS_ROOM_DOOR_NORTH, 0}, {MAP_PARTS_ROOM_CORNER_NORTH_EAST, 0}},
                {{MAP_PARTS_ROOM_WALL_WEST, 0}, {MAP_PARTS_ROOM_FLOOR, 0}, {MAP_PARTS_ROOM_WALL_EAST, 0}},
                {{MAP_PARTS_ROOM_CORNER_SOUTH_WEST, 0}, {MAP_PARTS_ROOM_DOOR_SOUTH, 0}, {MAP_PARTS_ROOM_CORNER_SOUTH_EAST, 0}},
                {{MAP_PARTS_NONE, 0}, {MAP_PARTS_STAIR_UP, 2}, {MAP_PARTS_NONE, 0}},
            };
            for (int r = 0; r < 5; r++) {
                for (int c = 0; c < 3; c++) {
                    if (layout[r][c][0] == MAP_PARTS_NONE) continue;
                    BUILD_MAP_INFO &cell = buildMapDat[(ox + c) + (oy - 1 + r) * DNG_GRID];
                    cell.kind = layout[r][c][0];
                    cell.direction = layout[r][c][1];
                    cell.content = r == 0 ? BUILD_CELL_STAIR_DOWN : r == 4 ? BUILD_CELL_STAIR_UP : BUILD_CELL_ROOM;
                }
            }
            DngExitRoomX = ox;
            DngExitRoomY = oy;
            DngExitRoomValid = 1;
            printf("exit room: cells %d..%d, %d..%d\n", ox, ox + 2, oy - 1, oy + 3);
            return 1;
        }
    }
    printf("exit room: no free space on this floor\n");
    return 0;
}
#endif


// MoonMod map screen (moonmod/map_screen.hpp): each cell's shape, as DngMapCellShapes documents.
void DngMapCellShapes(const CDungeonMap &map, unsigned char *out) {
    static constexpr int kStep[4][2] = {{0, -1}, {1, 0}, {-1, 0}, {0, 1}};
    static constexpr int kSide[4] = {1, 2, 4, 8};
    const ReachRules rules{map, {}, false};
    for (int cell = 0; cell < DNG_CELLS; ++cell) {
        const int kind = rules.Kind(cell);
        if (kind == ReachRules::kNone) {
            out[cell] = 0;
            continue;
        }
        unsigned char shape = 32;
        if (kind == ReachRules::kRoom || kind == ReachRules::kStair || kind == ReachRules::kShut) shape |= 16;
        const int col = cell % DNG_GRID, row = cell / DNG_GRID;
        for (int s = 0; s < 4; ++s) {
            const int nc = col + kStep[s][0], nr = row + kStep[s][1];
            if (nc < 0 || nr < 0 || nc >= DNG_GRID || nr >= DNG_GRID) continue;
            const int next = nc + nr * DNG_GRID;
            const int back = kSide[s] == 1 ? 8 : kSide[s] == 8 ? 1 : kSide[s] == 2 ? 4 : 2;
            if (rules.Kind(next) != ReachRules::kNone && rules.Opens(cell, kSide[s], next) && rules.Opens(next, back, cell)) shape |= kSide[s];
        }
        out[cell] = shape;
    }
}

} // namespace dc::game

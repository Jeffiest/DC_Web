// A mod's figure on a dungeon floor (world.dungeon_npc_set): a town villager's model brought into the dungeon, standing
// just inside the floor's exit gate (or beside the player), turning to face the player. Walking up to it tells its
// mod (game.dungeon_npc.near), which answers as it likes, e.g. by opening a shop on its run menu. It has its own
// model and texture arenas, as the companion does.

#include "moonmod/dungeon_npc.hpp"

#include <algorithm>
#include <cmath>
#include <string>

#include "character.hpp"
#include "dataalloc.hpp"
#include "dataread.hpp"
#include "dun/gameloop.hpp"
#include "dungeonmap.hpp"
#include "dungeonparts.hpp"
#include "gamemode.hpp"
#include "main.hpp"
#include "mglib.hpp"
#include "texture.hpp"
#include "userstatus.hpp"

namespace dc::game {

namespace {

constexpr int kNpcTextureBlock = 0x62;
constexpr float kNear = 34.0f, kRearm = 80.0f;

alignas(64) unsigned char g_file_storage[3 * 1024 * 1024];
alignas(64) unsigned char g_model_storage[4 * 1024 * 1024];
CCharacter     g_body;
CTexAnimeData  g_anime[32];
CDataAlloc2<1> g_models(-1);

std::string g_owner, g_id, g_wanted_model, g_loaded_model;
int         g_place = 0; // 0 inside the exit gate, 1 beside the player
bool        g_placed = false, g_near = false;
int         g_floor_seed = -1;

bool InDungeon() { return mode == GAME_MODE_DUNGEON && UserStatus != nullptr && NowDngMap != nullptr && CharaMain.frame != nullptr; }

void Unload() {
    if (g_loaded_model.empty()) return;
    TexManager.DeleteTextureBlock(kNpcTextureBlock);
    TexManager.CleanUpBuffer();
    TexManager.CleanUpTextureList();
    g_body.Initialize();
    g_loaded_model.clear();
    g_placed = false;
}

bool Load(const std::string &path) {
    Unload();
    int size = 0;
    auto *pack = reinterpret_cast<unsigned int *>(g_file_storage);
    if (LoadFile(const_cast<char *>(path.c_str()), pack, &size) == 0 || size <= 0 || size > static_cast<int>(sizeof(g_file_storage))) return false;
    // A villager's pack names its config info.cfg or after the model.
    std::string stem = path.substr(path.find_last_of('/') + 1);
    stem = stem.substr(0, stem.find('.'));
    std::string config = GetPackFile(pack, const_cast<char *>("info.cfg"), nullptr) != nullptr ? "info.cfg" : stem + ".cfg";
    g_models.base = g_model_storage;
    g_models.used = 0;
    g_models.limit = static_cast<int>(sizeof(g_model_storage) >> 4);
    g_body.Initialize();
    for (CTexAnimeData &anime : g_anime) anime.Initialize();
    g_body.InitializeTexAnime(g_anime, 32);
    g_body.LoadPackData2(pack, config.data(), &g_models, kNpcTextureBlock, &g_models, 0);
    if (g_body.frame == nullptr) return false;
    CFrameAttr attr;
    attr.fog_enable = true;
    attr.clip_enable = false;
    attr.program_option = 0;
    g_body.frame->SetAttr(attr, 1, 4);
    g_body.motion_no = 0; // standing
    g_body.motion_flags = 0;
    g_body.motion_speed = -1.0f;
    g_loaded_model = path;
    g_placed = false;
    return true;
}

void Place(const float *at, float facing) {
    float p[4] = {at[0], at[1], at[2], 1.0f};
    g_body.frame->SetPosition(p);
    g_body.SetPosition(p);
    g_body.frame->SetRotation(0.0f, facing, 0.0f);
    g_body.SetRotation(0.0f, facing, 0.0f);
}

bool IsShortcut(int col, int row) {
    for (int d = 0; d < DngShortcutDoorCount; ++d) {
        if (DngShortcutDoorTable[d].x == col && DngShortcutDoorTable[d].y == row) return true;
    }
    return false;
}

// Just inside the floor's exit gate, a little to one side of it; false when the floor has none.
bool ExitSpot(const float *player, float *out) {
    for (int row = 0; row < DNG_GRID; ++row) {
        for (int col = 0; col < DNG_GRID; ++col) {
            const int part = NowDngMap->cells[col + row * DNG_GRID].parts_no;
            if (part < MAP_PARTS_ROOM_GATE_NORTH || part > MAP_PARTS_ROOM_GATE_WEST || IsShortcut(col, row)) continue;
            // The corridor lies past the gate's side; the room is the other way.
            const int dx = part == MAP_PARTS_ROOM_GATE_EAST ? 1 : (part == MAP_PARTS_ROOM_GATE_WEST ? -1 : 0);
            const int dy = part == MAP_PARTS_ROOM_GATE_SOUTH ? 1 : (part == MAP_PARTS_ROOM_GATE_NORTH ? -1 : 0);
            out[0] = 160.0f * (static_cast<float>(col) - 0.45f * static_cast<float>(dx)) + 34.0f * static_cast<float>(dy);
            out[1] = player[1];
            out[2] = 160.0f * (static_cast<float>(row) - 0.45f * static_cast<float>(dy)) + 34.0f * static_cast<float>(dx);
            out[3] = 1.0f;
            return true;
        }
    }
    // A floor whose exit stands on a corridor (a rest floor): in front of the gate's stair, to one side.
    for (int cell = 0; cell < DNG_CELLS; ++cell) {
        if (NowDngMap->cells[cell].parts_no != MAP_PARTS_STAIR_UP) continue;
        out[0] = 160.0f * static_cast<float>(cell % DNG_GRID) + 48.0f;
        out[1] = player[1];
        out[2] = 160.0f * static_cast<float>(cell / DNG_GRID - 1);
        out[3] = 1.0f;
        return true;
    }
    return false;
}

} // namespace

bool MoonModNpcSet(const std::string &owner, const std::string &id, const std::string &model, int place) {
    if (!g_owner.empty() && g_owner != owner) return false;
    g_owner = owner;
    g_id = id;
    g_wanted_model = model;
    g_place = place;
    g_placed = false;
    g_near = false;
    return true;
}

void MoonModNpcClear(const std::string &owner) {
    if (g_owner != owner) return;
    g_owner.clear();
    g_id.clear();
    g_wanted_model.clear();
}

void MoonModNpcStep() {
    if (!InDungeon()) {
        if (mode != GAME_MODE_DUNGEON) {
            Unload();
            g_wanted_model.clear();
            g_owner.clear();
        }
        return;
    }
    // A new floor sends the figure away; its mod asks again when it wants it there.
    if (g_floor_seed != NowDngMap->map_seed) {
        g_floor_seed = NowDngMap->map_seed;
        g_wanted_model.clear();
        g_owner.clear();
    }
    if (g_wanted_model != g_loaded_model) {
        if (g_wanted_model.empty()) Unload();
        else if (!Load(g_wanted_model)) {
            Unload();
            g_wanted_model.clear();
        }
    }
    if (g_loaded_model.empty()) return;

    float player[4];
    CharaMain.GetPosition(player);
    if (!g_placed) {
        float at[4];
        if (g_place != 0 || !ExitSpot(player, at)) {
            float facing[4];
            CharaMain.frame->GetRotation(facing);
            at[0] = player[0] + 40.0f * std::sin(facing[1]);
            at[1] = player[1];
            at[2] = player[2] + 40.0f * std::cos(facing[1]);
        }
        Place(at, 0.0f);
        g_placed = true;
    }
    float me[4];
    g_body.GetPosition(me);
    const float dx = player[0] - me[0], dz = player[2] - me[2];
    const float apart = std::hypot(dx, dz);
    // Turns to the player once they are close enough to notice.
    if (apart < 220.0f) {
        const float at[4] = {me[0], me[1], me[2], 1.0f};
        Place(at, std::atan2(dx, dz));
    }
    if (!g_near && apart < kNear) {
        g_near = true;
        moonmod::ChronicleNpcNear(g_owner, g_id);
    } else if (g_near && apart > kRearm) {
        g_near = false;
    }
    g_body.Step();
}

void MoonModNpcDraw(bool shadow) {
    if (g_loaded_model.empty() || !InDungeon()) return;
    if (shadow) {
        g_body.DrawShadow();
        return;
    }
    TexManager.ReloadTexture(Vif1Packet, kNpcTextureBlock);
    g_body.TextureAnime(kNpcTextureBlock);
    g_body.Draw();
}

} // namespace dc::game

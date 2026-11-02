// MoonMod code that used to live in ps2/src/monstorunit.cpp: the registered monster variants, the
// awake-monster limit, the floor population a mod asks for, and retail's DrawMapSymbol with the
// enlarged map grid and the mod reveal.

#include "monstorunit.hpp"

#include <algorithm>
#include <cmath>
#include <cstring>

#include "dun/gameloop.hpp"
#include "dungeonmap.hpp"
#include "dungeonparts.hpp"
#include "dngstatusdata.hpp"
#include "menu_inventory.hpp"
#include "randomitem.hpp"
#include "gameutil.hpp"
#include "snd.hpp"
#include "mglib.hpp"
#include "rect.hpp"
#include "texture.hpp"

namespace dc::game {

namespace moonmod {
bool ChroniclePlayerMonsterReveal();
} // namespace moonmod

/* Monsters kept awake near the player; retail wakes 4, mods may raise it. */
int MonstorActiveLimit = 4;

namespace {

struct MoonModMonstorVariant {
    int used;
    int model_no;
    MONSTOR_MODEL definition;
};

// Mod enemy variants (characters.register), numbered from 0x400. A Gauntlet run alone registers a boss's whole cast.
constexpr int kMonstorVariantMax = 128;
MoonModMonstorVariant moonmod_monstor_variants[kMonstorVariantMax];

} // namespace

MONSTOR_MODEL *GetMonstorTable(int model_no) {
    for (int slot = 0; slot < kMonstorVariantMax; ++slot) {
        if (moonmod_monstor_variants[slot].used && moonmod_monstor_variants[slot].model_no == model_no) {
            return &moonmod_monstor_variants[slot].definition;
        }
    }
    if (model_no < 0 || model_no >= 167) return NULL;
    return &MonstorTable[model_no];
}

bool RegisterMoonModMonstorVariant(int template_model_no, int hp, int defense, int reward_exp, int reward_money,
                                   const char *model_name, const char *script_name, int *out_model_no) {
    MONSTOR_MODEL *template_definition = GetMonstorTable(template_model_no);
    if (template_definition == NULL || hp <= 0 || hp > 1000000 || defense < 0 || defense > 32767 ||
        reward_exp < 0 || reward_exp > 32767 || reward_money < 0 || model_name == NULL || model_name[0] == '\0' ||
        std::strlen(model_name) >= sizeof(MONSTOR_MODEL::model_name[0]) || script_name == NULL ||
        script_name[0] == '\0' || std::strlen(script_name) >= sizeof(MONSTOR_MODEL::script_name) ||
        out_model_no == NULL) {
        return false;
    }
    int slot = -1;
    for (int index = 0; index < kMonstorVariantMax; ++index) {
        if (!moonmod_monstor_variants[index].used) {
            slot = index;
            break;
        }
    }
    if (slot < 0) return false;

    MoonModMonstorVariant &variant = moonmod_monstor_variants[slot];
    std::memcpy(&variant.definition, template_definition, sizeof(MONSTOR_MODEL));
    std::memset(variant.definition.model_name, 0, sizeof(variant.definition.model_name));
    std::memcpy(variant.definition.model_name[0], model_name, std::strlen(model_name));
    std::memset(variant.definition.script_name, 0, sizeof(variant.definition.script_name));
    std::memcpy(variant.definition.script_name, script_name, std::strlen(script_name));
    variant.definition.max_hp = hp;
    variant.definition.defense = static_cast<s16>(defense);
    variant.definition.exp = static_cast<s16>(reward_exp);
    variant.definition.money = reward_money;
    variant.model_no = 0x400 + slot;
    variant.used = 1;
    *out_model_no = variant.model_no;
    return true;
}

bool UnregisterMoonModMonstorVariant(int model_no) {
    if (model_no < 0x400 || model_no >= 0x400 + kMonstorVariantMax) return false;
    const int slot = model_no - 0x400;
    MoonModMonstorVariant &variant = moonmod_monstor_variants[slot];
    if (!variant.used || variant.model_no != model_no) return false;
    std::memset(&variant, 0, sizeof(variant));
    return true;
}

void ResetMoonModMonstorVariants() {
    std::memset(moonmod_monstor_variants, 0, sizeof(moonmod_monstor_variants));
}


int MoonModArrangementCount(CDungeonMap *map, int count, int model_no) {
    if (count > 0 && model_no == -1) {
        // a floor whose zones come from several floors holds each of their floor's worth (dungeons.set_zones source_floor)
        if (map != nullptr && DngZoneSourcesShown(map->map_seed)) count *= DngZoneFloorCount;
        count = std::min(MONSTOR_SLOT_MAX - 1, (count * (100 + DngMonsterBonusPercent) + 99) / 100);
        // a floor built in zones (dungeons.set_zones) is its mod's: another mod's room density doesn't apply to it
        const bool zoned = map != nullptr && DngZoneFloorCount > 0 && DngZoneFloorSeed == map->map_seed;
        if (map != nullptr && map->room_num > 0 && DngMonstersPerRoomMax > 0.0 && !zoned) {
            // Roll population density once per generated floor from its map seed.
            // This makes the configured band predictable for a seed and avoids
            // letting unrelated RNG consumption change an otherwise identical floor.
            const u32 seed = static_cast<u32>(map->map_seed);
            const u32 mixed = seed * 747796405u + 2891336453u;
            const double roll = static_cast<double>((mixed >> 8) & 0x00ffffffu) / 16777215.0;
            const double per_room = DngMonstersPerRoomMin + (DngMonstersPerRoomMax - DngMonstersPerRoomMin) * roll;
            // Apply the additive Ascension bonus to the density target too. The
            // native baseline above already has this bonus, so each path is
            // scaled once before choosing the larger target.
            const double bonus_scale = (100.0 + DngMonsterBonusPercent) / 100.0;
            const int room_target = static_cast<int>(std::ceil(map->room_num * per_room * bonus_scale));
            count = std::min(MONSTOR_SLOT_MAX - 1, std::max(count, room_target));
        }
    }
    return count;
}

bool MoonModLoadZoneMonsters(CMonstorUnit *unit, BT_ENEMY_FLOOR *floors, int cur_floor, CDataAlloc2<1> *alloc) {
    std::fill(std::begin(DngZoneModelMask), std::end(DngZoneModelMask), 0);
    if (!DngZoneSourcesShown(MainDungeonMap.map_seed) || selectMapNo < 0 || selectMapNo >= 7) return false;
    int monster_of[MONSTOR_MODEL_MAX];
    int count = 0;
    for (int z = 0; z < DngZoneFloorCount; ++z) {
        int floor = DngZoneFloorSource[z] >= 0 ? DngZoneFloorSource[z] : cur_floor;
        if (floor >= maxFloorTbl__3[selectMapNo]) floor = cur_floor;
        for (const auto &entry : floors[floor].monster) {
            if (entry.monster_no == -1) break;
            int slot = -1;
            for (int m = 0; m < count; ++m) {
                if (monster_of[m] == entry.monster_no) slot = m;
            }
            if (slot < 0 && count < MONSTOR_MODEL_MAX && count < static_cast<int>(std::size(DngZoneModelMask))) {
                if (unit->SetupBaseModel(count, entry.monster_no, 0x26, alloc) <= 0) continue;
                slot = count;
                monster_of[count++] = entry.monster_no;
            }
            if (slot >= 0) DngZoneModelMask[slot] |= 1 << z;
        }
    }
    return true;
}

bool MoonModZoneModelAllowed(CDungeonMap *map, const float *position, int model) {
    if (map != &MainDungeonMap || !DngZoneSourcesShown(map->map_seed) || model < 0 || model >= static_cast<int>(std::size(DngZoneModelMask))) return true;
    const int col = static_cast<int>((position[0] + 80.0f) / 160.0f), row = static_cast<int>((position[2] + 80.0f) / 160.0f);
    if (col < 0 || row < 0 || col >= DNG_GRID || row >= DNG_GRID) return false;
    const int zone = DngZoneOfCell[col + row * DNG_GRID];
    if (zone == DNG_ZONE_NONE) return DngZoneModelMask[model] != 0;
    return (DngZoneModelMask[model] & (1 << zone)) != 0;
}

// dungeons.set_zones: each locked zone gate's key goes to an ordinary monster standing in the zone before the gate,
// one that carries nothing else; it drops the key as a monster drops the floor's gate key. With no such monster there,
// a small chest in that zone holds it.
void MoonModZoneKeyCarriers(CMonstorUnit *unit, CDungeonMap *map) {
    const auto zone_at = [](const float *at) {
        const int col = static_cast<int>((at[0] + 80.0f) / 160.0f), row = static_cast<int>((at[2] + 80.0f) / 160.0f);
        if (col < 0 || row < 0 || col >= DNG_GRID || row >= DNG_GRID) return static_cast<int>(DNG_ZONE_NONE);
        return static_cast<int>(DngZoneOfCell[col + row * DNG_GRID]);
    };
    for (int g = 0; g < DngZoneGateCount; ++g) {
        const DNG_ZONE_GATE &gate = DngZoneGateTable[g];
        if (!gate.locked || gate.key_item < 0) continue; // a smoke gate takes no key
        const int zone = gate.zone - 1;
        bool held = false; // placing more monsters later hands the key out once
        for (const MONSTOR &m : unit->monster) held = held || (m.state != -1 && m.drop_item == gate.key_item);
        for (const TREASURE_BOX &box : map->boxes) held = held || (box.used != 0 && box.item_no == gate.key_item);
        // the key the player already picked up (a floor built again from its seed) or left lying on the floor
        if (UserStatus != nullptr) {
            held = held || GetNowItemNum(static_cast<short>(gate.key_item), &static_cast<CDngStatusData *>(UserStatus)->item_pack) > 0;
        }
        held = held || (RandomItem != nullptr && RandomItem->CheckItemNo(gate.key_item) != 0);
        if (held) continue;
        int carriers[MONSTOR_SLOT_MAX];
        int count = 0;
        for (int i = 0; i < MONSTOR_SLOT_MAX; ++i) {
            const MONSTOR &m = unit->monster[i];
            if (m.state == -1 || m.drop_item != -1 || m.kind != MONSTER_KIND_NORMAL) continue;
            sceVu0FVECTOR at;
            unit->chara[i][0].GetPosition(at);
            if (zone_at(at) == zone) carriers[count++] = i;
        }
        if (count > 0) {
            const int pick = carriers[std::min(count - 1, static_cast<int>(static_cast<float>(count) * static_cast<float>(rand()) / 2147483648.0f))];
            unit->monster[pick].drop_item = static_cast<s16>(gate.key_item);
            unit->monster[pick].drops_items = 1;
            continue;
        }
        float pos[4] = {};
        for (int tries = 0; tries < 400; ++tries) {
            SearchiDoPutArea(map->cells, 0, 0, DNG_GRID, DNG_GRID, pos);
            if (zone_at(pos) != zone) continue;
            if (map->CheckTreasureBox(pos, 20.0f) == 0 || map->CheckAtra(pos, 20.0f) == 0 || map->CheckTrapCircle(pos, 20.0f) != nullptr) continue;
            if (map->SetTreasureBox(pos, gate.key_item, TREASURE_BOX_SMALL, 0) != -1) break;
        }
    }
}

} // namespace dc::game

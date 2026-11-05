// Part banks (dungeons.set_zones source_dungeon): other dungeons' parts beside the map's own, so one floor can run
// from one dungeon's rooms into another's (moonmod/dungeonmap_layout.hpp).

#include <cmath>
#include <cstdio>
#include <cstring>

#include "dun/gameloop.hpp"
#include "dungeonmap.hpp"
#include "dungeonparts.hpp"
#include "itemdata.hpp"
#include "texture.hpp"

namespace dc::game {

int DngZoneSourceDungeon[DNG_ZONE_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
int DngZoneFloorDungeon[DNG_ZONE_MAX] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
int DngPartBankCount = 0;
int DngPartBankDungeon[DNG_PART_BANK_MAX] = {-1, -1, -1, -1, -1, -1};

namespace {

CDungeonParts g_banks[DNG_PART_BANK_MAX][72];
int           g_loaded = 0;

// A dungeon's lighting, as its map file sets the main_* globals (colours in floats so an ease does not stall).
struct ZoneLight {
    float light[4][4];
    float lightcolor[4][4];
    float ambient[4];
    float fog_rate[4];
    float fog_color[3];
    float bg[3];
};

ZoneLight g_bank_light[DNG_PART_BANK_MAX];
ZoneLight g_own_light;   // the floor's own
ZoneLight g_shown_light; // as last written
bool      g_light_eased = false;

ZoneLight ReadLight() {
    ZoneLight l{};
    std::memcpy(l.light, main_light, sizeof(l.light));
    std::memcpy(l.lightcolor, main_lightcolor, sizeof(l.lightcolor));
    std::memcpy(l.ambient, main_ambientlight, sizeof(l.ambient));
    std::memcpy(l.fog_rate, main_fogRate, sizeof(l.fog_rate));
    for (int i = 0; i < 3; ++i) {
        l.fog_color[i] = main_fogColor[i];
        l.bg[i] = main_bgColor[i];
    }
    return l;
}

void WriteLight(const ZoneLight &l) {
    std::memcpy(main_light, l.light, sizeof(l.light));
    std::memcpy(main_lightcolor, l.lightcolor, sizeof(l.lightcolor));
    std::memcpy(main_ambientlight, l.ambient, sizeof(l.ambient));
    std::memcpy(main_fogRate, l.fog_rate, sizeof(l.fog_rate));
    for (int i = 0; i < 3; ++i) {
        main_fogColor[i] = static_cast<u8>(std::lround(l.fog_color[i]));
        main_bgColor[i] = static_cast<u8>(std::lround(l.bg[i]));
    }
}

// Whether the globals still hold what was last written (else something else has lit the floor since).
bool LightIsShown() {
    const ZoneLight now = ReadLight();
    ZoneLight       shown = g_shown_light;
    for (int i = 0; i < 3; ++i) {
        shown.fog_color[i] = static_cast<float>(std::lround(shown.fog_color[i]));
        shown.bg[i] = static_cast<float>(std::lround(shown.bg[i]));
    }
    return std::memcmp(&now, &shown, sizeof(now)) == 0;
}

// The parts a zone of another dungeon takes from its bank: the ones the builder lays rooms and corridors with.
bool BankedPart(int parts_no) {
    return (parts_no >= MAP_PARTS_ROAD_STRAIGHT && parts_no <= MAP_PARTS_DIVIDE_WALL_WEST) ||
           (parts_no >= MAP_PARTS_HEAL_SPRING_NORTH && parts_no <= MAP_PARTS_HEAL_SPRING_WEST) || parts_no == MAP_PARTS_PILLAR_1 ||
           parts_no == MAP_PARTS_PILLAR_2;
}

} // namespace

bool DngZoneDungeonsShown(int map_seed) {
    if (DngPartBankCount <= 0 || DngZoneFloorCount <= 0 || DngZoneFloorSeed != map_seed) return false;
    for (int z = 0; z < DngZoneFloorCount; ++z) {
        if (DngZoneFloorDungeon[z] >= 0) return true;
    }
    return false;
}

CDungeonParts *DngCellPart(CDungeonMap *map, int cell) {
    const int parts_no = map->cells[cell].parts_no;
    if (DngPartBankCount > 0 && map == &MainDungeonMap && BankedPart(parts_no) && DngZoneFloorIs(map->map_seed)) {
        const int zone = DngZoneOfCell[cell];
        if (zone != DNG_ZONE_NONE && zone < DNG_ZONE_MAX && DngZoneFloorDungeon[zone] >= 0) {
            for (int b = 0; b < DngPartBankCount; ++b) {
                if (DngPartBankDungeon[b] == DngZoneFloorDungeon[zone] && g_banks[b][parts_no].frame[0] != nullptr) return &g_banks[b][parts_no];
            }
        }
    }
    return &map->parts[parts_no];
}

int DngPartBanksWanted(int own_dungeon) {
    int mask = 0;
    for (int z = 0; z < DngZoneCount && z < DNG_ZONE_MAX; ++z) {
        const int d = DngZoneSourceDungeon[z];
        if (d >= 0 && d < 7 && d != own_dungeon) mask |= 1 << d;
    }
    return mask;
}

int DngPartBanksLoaded() { return g_loaded; }

void DngLoadPartBanks(int own_dungeon) {
    for (int b = 0; b < DNG_PART_BANK_MAX; ++b) TexManager.DeleteTextureBlock(DNG_PART_BANK_TEXTURE_BLOCK + b);
    TexManager.CleanUpTextureList();
    TexManager.CleanUpBuffer();
    DngPartBankCount = 0;
    g_loaded = DngPartBanksWanted(own_dungeon);
    for (int d = 0; d < 7 && DngPartBankCount < DNG_PART_BANK_MAX; ++d) {
        if ((g_loaded & (1 << d)) == 0) continue;
        char name[16];
        std::snprintf(name, sizeof(name), "d%02dmain_a", d + 1);
        const int       b = DngPartBankCount;
        const ZoneLight floor_light = ReadLight();
        const bool      loaded = TEIGILoadPartBank(name, g_banks[b], DNG_PART_BANK_TEXTURE_BLOCK + b);
        g_bank_light[b] = ReadLight();
        WriteLight(floor_light);
        if (!loaded) {
            std::printf("moonmod: part bank %s did not load\n", name);
            continue;
        }
        DngPartBankDungeon[b] = d;
        DngPartBankCount++;
        std::printf("moonmod: part bank %d holds dungeon %d (%s)\n", b, d, name);
    }
}

void DngZoneLightingStep() {
    if (NowDngMap != &MainDungeonMap || !DngZoneDungeonsShown(MainDungeonMap.map_seed)) {
        if (g_light_eased && LightIsShown()) WriteLight(g_own_light);
        g_light_eased = false;
        return;
    }
    if (!g_light_eased || !LightIsShown()) {
        g_own_light = ReadLight();
        g_shown_light = g_own_light;
        g_light_eased = true;
    }
    static const ZoneLight *target = nullptr;
    float at[4];
    CharaMain.GetPosition(at);
    const int col = static_cast<int>((at[0] + 80.0f) / 160.0f), row = static_cast<int>((at[2] + 80.0f) / 160.0f);
    const int zone = col >= 0 && row >= 0 && col < DNG_GRID && row < DNG_GRID ? DngZoneOfCell[col + row * DNG_GRID] : DNG_ZONE_NONE;
    if (zone != DNG_ZONE_NONE && zone < DNG_ZONE_MAX) { // between zones the last one's lighting stays
        target = &g_own_light;
        for (int b = 0; b < DngPartBankCount; ++b) {
            if (DngZoneFloorDungeon[zone] >= 0 && DngPartBankDungeon[b] == DngZoneFloorDungeon[zone]) target = &g_bank_light[b];
        }
    }
    if (target == nullptr) target = &g_own_light;
    constexpr float kEase = 0.06f;
    float          *to = &g_shown_light.light[0][0];
    const float    *from = &target->light[0][0];
    for (std::size_t i = 0; i < sizeof(ZoneLight) / sizeof(float); ++i) {
        to[i] += (from[i] - to[i]) * kEase;
        if (std::fabs(from[i] - to[i]) < 0.001f) to[i] = from[i];
    }
    for (int light = 0; light < 3; ++light) { // the eased directions stay unit length
        const float x = g_shown_light.light[0][light], y = g_shown_light.light[1][light], z = g_shown_light.light[2][light];
        const float length = std::sqrt(x * x + y * y + z * z);
        if (length > 0.0001f) {
            for (int axis = 0; axis < 3; ++axis) g_shown_light.light[axis][light] /= length;
        }
    }
    WriteLight(g_shown_light);
}

} // namespace dc::game

#include "shot_freefuncs.hpp"

#include <libvu0.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <numbers>
#include <print>
#include <string>

#include "btactstatus.hpp"
#include "btmisc.hpp"
#include "character.hpp"
#include "charaheight.hpp"
#include "charm_slots.hpp"
#include "cloth.hpp"
#include "clothread.hpp"
#include "clsmes.hpp"
#include "dataalloc.hpp"
#include "dataread.hpp"
#include "dranmapfield.hpp"
#include "dun/gameloop.hpp"
#include "dungeoneventman.hpp"
#include "dungeonmap.hpp"
#include "frame.hpp"
#include "gfx/gfx.hpp"
#include "healeffect.hpp"
#include "hitvalue.hpp"
#include "itemdata.hpp"
#include "mainselect.hpp"
#include "mathutil.hpp"
#include "menu_inventory.hpp"
#include "mglib.hpp"
#include "monstorunit.hpp"
#include "motionmodel.hpp"
#include "nowload.hpp"
#include "rect.hpp"
#include "snd.hpp"
#include "texture.hpp"
#include "userstatus.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/dungeonmap_layout.hpp"
#include "moonmod/game_hooks.hpp"
#include "moonmod/item_icon_override.hpp"
#include "moonmod/manager_ui.hpp"
#include "text/gametext.hpp"
#include "ui/framework.hpp"
#include "monstorunit.hpp"

namespace dc::game {

s32   DebugStatus[21];
float StatusColor[3];
int   statusAlarmRate;

namespace {

/**
 * Number of floors in each dungeon.
 */

/**
 * Number of ripple slots on the water.
 */
constexpr int WATER_WAVE_LING_COUNT = 6;

/**
 * The water the party is standing in and the nearest water surface.
 */
CHECK_WATER_INFO CheckWaterInfo;

/**
 * Rings spreading across the water.
 */
std::array<WATER_WAVE_LING, WATER_WAVE_LING_COUNT> WaterWaveLing;

/**
 * Name of the map held in the jump cache.
 */
std::string BtCfgCash;

/**
 * Frames remaining before the next ring appears.
 */
int WaterWaveLingWait;

/**
 * Frames remaining before healing water restores the party again.
 */
int healingSpeed;

/**
 * Whether the healing effect still owes the scene its ambient colour back.
 */
int healingSpeed_flg;

/**
 * Phase of the low-life warning pulse.
 */
float statusAlarmCounter;

using StatusColorRgba = std::array<u8, 4>;

StatusColorRgba statusRGBColor_life;     /**< Life bar above a fifth full. */
StatusColorRgba statusRGBColor_weapon;   /**< Weapon bar above a fifth full. */
StatusColorRgba statusRGBColor_30;       /**< Bar above 15% and at most a fifth full. */
StatusColorRgba statusRGBColor_15;       /**< Bar at most 15% full; its alpha pulses. */
StatusColorRgba statusRGBColor_life_2;   /**< Second colour of a life bar above a fifth full. */
StatusColorRgba statusRGBColor_weapon_2; /**< Second colour of a weapon bar above a fifth full. */
StatusColorRgba statusRGBColor_30_2;     /**< Second colour of a bar above 15% and at most a fifth full. */
StatusColorRgba statusRGBColor_15_2;     /**< Second colour of a bar at most 15% full. */

/**
 * Steps since poison last took its toll.
 */
int poison_counter;

/**
 * Whether the jump cache holds a map.
 */
int BtCfgFlag;

/**
 * Message file number loaded for the cached map jump.
 */
int BtSteebMsgNo;

/**
 * Reports whether the party stands in the water of the nearest map part, records the surface
 * point in CheckWaterInfo and starts the splash as they enter it.
 */
int CheckHealingWater() {
    float        position[4];
    float        water_position[4];
    PARTS_WATER *nearest_water = nullptr;
    float        nearest_distance = 160.0f;
    float        water_x = 0.0f;
    float        water_y = 0.0f;
    float        water_z = 0.0f;

    sceVu0CopyVector(position, CharaMain.pos);

    for (int row = 0; row < DNG_ROOM_AREA; row++) {
        for (int column = 0; column < DNG_ROOM_AREA; column++) {
            const int parts_no = NowDngMap->cells[(row * DNG_GRID) + column].parts_no;

            if (parts_no != -1) {
                CDungeonParts *part = &NowDngMap->parts[parts_no];
                PARTS_WATER   *water = &part->water;

                if (part->water.used != 0) {
                    water_position[0] = 160.0f * static_cast<float>(column);
                    water_position[1] = water->vertex[0][1] + DngFloorHeight(NowDngMap, 160.0f * static_cast<float>(column), 160.0f * static_cast<float>(row));
                    water_position[2] = 160.0f * static_cast<float>(row);

                    const float distance = DistVector(position, water_position);

                    if (distance < nearest_distance) {
                        nearest_water = water;
                        nearest_distance = distance;
                        water_x = water_position[0];
                        water_y = water_position[1];
                        water_z = water_position[2];
                    }
                }
            }
        }
    }

    if (nearest_distance < 160.0f && position[0] >= water_x + nearest_water->vertex[0][0] && position[0] < water_x + nearest_water->vertex[3][0] && position[2] >= water_z + nearest_water->vertex[0][2] && position[2] < water_z + nearest_water->vertex[3][2] && position[1] < water_y) {
        position[1] = water_y;
        sceVu0CopyVector(CheckWaterInfo.surface_position, position);
        BtActStatus.in_water = true;

        if (Water_Splash_actFlag == 0 && CheckWaterInfo.in_water == 0 && CheckWaterInfo.dry_position[1] - CheckWaterInfo.surface_position[1] > 0.5f) {
            Water_Splash.SetPosition(CheckWaterInfo.surface_position);
            Water_Splash.SetMotion(0, 6);
            SndSePlay(SE_WATER_SPLASH, -1, 0);
            Water_Splash_actFlag = 1;
        }

        CheckWaterInfo.in_water = true;
        return 1;
    }

    sceVu0CopyVector(CheckWaterInfo.dry_position, position);
    CheckWaterInfo.in_water = false;
    return 0;
}

/**
 * Reports whether the party stands in the healing zone of the map part under them.
 */
int CheckHealZone() {
    float position[4];
    float center[4];
    float low[4];
    float high[4];

    sceVu0CopyVector(position, CharaMain.pos);
    BtActStatus.in_water = false;

    const int column = static_cast<int>((80.0f + position[0]) / 160.0f);
    const int row = static_cast<int>((80.0f + position[2]) / 160.0f);
    const int parts_no = NowDngMap->cells[(row * DNG_GRID) + column].parts_no;

    if (parts_no == MAP_PARTS_NONE) {
        return 0;
    }

    CDungeonParts *part = DngCellPart(NowDngMap, (row * DNG_GRID) + column); // MoonMod: a zone's own spring (part banks)

    if (part->heal_on == 0) {
        return 0;
    }

    sceVu0CopyVector(center, part->heal_pos);
    center[0] += 160.0f * static_cast<float>(column);
    center[2] += 160.0f * static_cast<float>(row);
    center[1] += DngFloorHeight(NowDngMap, center[0], center[2]);
    sceVu0CopyVector(low, center);
    sceVu0CopyVector(high, center);
    low[0] -= part->heal_width / 2.0f;
    low[2] -= part->heal_depth / 2.0f;
    high[0] += part->heal_width / 2.0f;
    high[2] += part->heal_depth / 2.0f;

    if (position[0] >= low[0] && position[0] <= high[0] && position[2] >= low[2] && position[2] < high[2] && position[1] < center[1]) {
        BtActStatus.in_water = true;
        return 1;
    }

    return 0;
}

} // namespace

void WaterSplash_Init() {
    Water_Splash_actFlag = 0;
    CheckWaterInfo.in_water = false;

    for (WATER_WAVE_LING &ling : WaterWaveLing) {
        ling.life = 0;
    }

    WaterWaveLingWait = 0;
}

bool ShrineWaterDry() {
    if (!(DngZoneShrineHeal == 0 && NowDngMap == &MainDungeonMap && DngZoneFloorIs(NowDngMap->map_seed))) return false;
    if (DngZoneFloorHealMask == 0) return true;
    float at[4];
    CharaMain.GetPosition(at); // set_zones heal: the spring of a zone that heals
    const int col = static_cast<int>((at[0] + 80.0f) / 160.0f), row = static_cast<int>((at[2] + 80.0f) / 160.0f);
    if (col < 0 || row < 0 || col >= DNG_GRID || row >= DNG_GRID) return true;
    const int zone = DngZoneOfCell[col + row * DNG_GRID];
    return zone == DNG_ZONE_NONE || (DngZoneFloorHealMask & (1 << zone)) == 0;
}

void HealingWater() {
    if (CheckHealZone() || CheckHealingWater()) {
        // MoonMod (set_zones shrine_heal = false): a shrine's water quenches thirst; the mod's rest heals.
        const bool dry = ShrineWaterDry();
        if (healingSpeed <= 0) {
            const int chara = UserStatus->cur_chara;
            UserStatus->AddDrink(chara, 0xFF, 255.0f);
            if (!dry) UserStatus->AddNowLife(chara, 0xFF, 255.0f);
            healingSpeed = 10;
        } else {
            healingSpeed--;
        }

        if (dry) {
        } else if (HealEffect.active == 0) {
            const int max_hp = UserStatus->max_hp[UserStatus->cur_chara];
            const int now_hp = UserStatus->hp[UserStatus->cur_chara];

            if (now_hp != max_hp) {
                float position[4];
                sceVu0CopyVector(position, CharaMain.pos);
                HealEffect.Set(position);
                SndSePlay(SE_CHARA_RECOVER, -1, 0);
                healingSpeed_flg = 1;
            }
        } else if (healingSpeed_flg) {
            setUnitAmbientAnime(60.0f, 1.0f, 0.0f, 122.0f, 208.0f);
            healingSpeed_flg = 0;
        }
    }
}

void DrawWaterLing() {
    float      corner[4][4];
    int        screen[4][4];
    sceGsAlpha alpha;
    sceGsZbuf  zbuf;

    CTexture *ripple = TexManager.GetTexture("d00e01", -1);

    alpha = mgAlpha;
    alpha.bits.a = 0;
    alpha.bits.b = 2;
    alpha.bits.c = 0;
    alpha.bits.d = 1;
    MGSetGsALPHA(&alpha);

    zbuf = mgZBuffer;
    zbuf.bits.zmsk = 1;
    MGSetGsZBUF(&zbuf);

    for (const WATER_WAVE_LING &ling : WaterWaveLing) {
        if (ling.life > 0) {
            const float radius = ling.radius;
            const float x = ling.center[0];
            const float left = x - radius;
            corner[0][0] = left;
            const float y = ling.center[1];
            corner[0][1] = y;
            const float z = ling.center[2];
            const float near = z - radius;
            corner[0][2] = near;
            const float right = radius + x;
            corner[1][0] = right;
            corner[1][1] = y;
            corner[1][2] = near;
            corner[2][0] = left;
            corner[2][1] = y;
            const float far = radius + z;
            corner[2][2] = far;
            corner[3][0] = right;
            corner[3][1] = y;
            corner[3][2] = far;

            bool visible = true;

            for (int corner_no = 0; corner_no < 4; corner_no++) {
                corner[corner_no][3] = 1.0f;

                if (MGRotTransPers(screen[corner_no], corner[corner_no], 0) == 0) {
                    visible = false;
                }
            }

            if (visible) {
                const float fade = 2.8444445f * static_cast<float>(ling.life);
                set3DSprite(Vif1Packet, ripple, CRect_i_(0, 64, 64, 64), screen[0], screen[1], screen[2], screen[3], static_cast<unsigned char>(fade));
            }
        }
    }

    MGSetGsALPHA(nullptr);
    MGSetGsZBUF(nullptr);
}

void StepWaterLing() {
    if (CheckWaterInfo.in_water != 0) {
        WaterWaveLingWait++;

        if (WaterWaveLingWait >= 20) {
            WaterWaveLingWait = 0;

            for (WATER_WAVE_LING &ling : WaterWaveLing) {
                if (ling.life == 0) {
                    sceVu0CopyVector(ling.center, CheckWaterInfo.surface_position);
                    ling.radius = 2.0f;
                    ling.life = 45;
                    break;
                }
            }
        }
    }

    for (WATER_WAVE_LING &ling : WaterWaveLing) {
        if (ling.life > 0) {
            ling.radius += 0.2f;
            ling.life--;
        }
    }
}

float SetBattleStyle(int map_no, int preserve_bgm) {
    float         nearest = 10000.0f;
    sceVu0FVECTOR position;
    sceVu0FVECTOR player;
    float         bgm_volume = 0.0f;
    float         ambient_volume = 0.0f;

    sceVu0CopyVector(player, CharaMain.pos);

    for (int monster_no = 0; monster_no < MONSTOR_SLOT_MAX; monster_no++) {
        if (NowMonstorUnit->monster[monster_no].state == -1) {
            continue;
        }

        if (NowMonstorUnit->monster[monster_no].revealed == 0) {
            continue;
        }

        NowMonstorUnit->chara[monster_no][0].GetPosition(position);
        nearest = std::min(nearest, DistVector(player, position));
    }

    if (map_no != 5) {
        if (!IsLastFloor(map_no, UserStatus->cur_floor)) {
            BtBattleMusic_Excg(nearest, &bgm_volume, &ambient_volume);

            if (ambient_volume > 0.0f) {
                SndAmbientPlay(0);
            }

            if (preserve_bgm == 0) {
                SndSetBgmVolf(bgm_volume);
            }

            SndAmbientSetVolf(ambient_volume);
        }
    }

    return nearest;
}

int ValuePrint(int x, int y, int value, int palette, unsigned char alpha) {
    CTexture *texture = TexManager.GetTexture("stayframe", -1);
    const int source_y = (palette * 12) + 0xB0;
    int       hundreds_shown = 0;
    int       digit = value / 100;

    if (digit > 0) {
        set2DSprite(Vif1Packet, texture, CRect_i_(x, y, 12, 12), CRect_i_(digit * 12, source_y, 12, 12), alpha);
        value -= digit * 100;
        x += 12;
        hundreds_shown++;
    }

    digit = value / 10;
    set2DSprite(Vif1Packet, texture, CRect_i_(x, y, 12, 12), CRect_i_(digit * 12, source_y, 12, 12), alpha);
    digit = value % 10;
    set2DSprite(Vif1Packet, texture, CRect_i_(x + 12, y, 12, 12), CRect_i_(digit * 12, source_y, 12, 12), alpha);
    return hundreds_shown + 2;
}

void BtStatusAlarmInit() {
    statusAlarmRate = 128;
    statusAlarmCounter = 0.0f;

    statusRGBColor_life = {0x40, 0xC1, 0x82, 0x80};
    statusRGBColor_weapon = {0xFF, 0x80, 0x00, 0x80};
    statusRGBColor_30 = {0xFF, 0x00, 0xC6, 0x80};
    statusRGBColor_15 = {0xFF, 0x00, 0x00, 0x80};
    statusRGBColor_life_2 = {0x00, 0x81, 0xFF, 0x80};
    statusRGBColor_weapon_2 = {0xFF, 0x20, 0x00, 0x80};
    statusRGBColor_30_2 = {0x3F, 0x00, 0x31, 0x80};
    statusRGBColor_15_2 = {0x3F, 0x00, 0x00, 0x80};
}

void BtStatusAlarmAnime() {
    statusAlarmCounter += 0.10471976f;

    if (!(statusAlarmCounter < std::numbers::pi_v<float>) ) {
        statusAlarmCounter -= std::numbers::pi_v<float>;
    }

    statusAlarmRate = 192 - static_cast<int>(2.0f * (64.0f * std::sin(statusAlarmCounter)));
    statusRGBColor_15[3] = static_cast<u8>(192 - static_cast<int>(2.0f * (64.0f * std::sin(statusAlarmCounter))));
}

u8 *BtGetStatusPal(int bar, float max, float value) {
    if (!(value <= 0.2f * max)) {
        return bar == 0 ? statusRGBColor_life.data() : statusRGBColor_weapon.data();
    }

    if (value <= 0.3f * max && !(value <= 0.15f * max)) {
        return statusRGBColor_30.data();
    }

    return statusRGBColor_15.data();
}

u8 *BtGetStatusPal2(int bar, float max, float value) {
    if (!(value <= 0.2f * max)) {
        return bar == 0 ? statusRGBColor_life_2.data() : statusRGBColor_weapon_2.data();
    }

    if (value <= 0.3f * max && !(value <= 0.15f * max)) {
        return statusRGBColor_30_2.data();
    }

    return statusRGBColor_15_2.data();
}

namespace {

// Fills a status bar's trough: the frame is `cells` four-pixel columns from x 0x30, and the trough
// runs from the left cap's fourth column to the right cap's second on the frame's rows 4 to 10.
void StatusBarFill(int frame_y, int cells, float value, float max, spRGBA *left, spRGBA *right) {
    const int trough = cells * 4 - 5;
    const int length = std::clamp(static_cast<int>(static_cast<float>(trough) * value / max), 0, trough);

    set2DSpriteC4(Vif1Packet, CRect_i_(0x33, frame_y + 4, length, 7), left, right, left, right);
}

} // namespace

void topStatusInfo(int y, int selected_item, int floor) {
    const int chara = UserStatus->cur_chara;
    if (chara < 0 || chara >= CharaCount()) { // MoonMod: a registered character's gauges too
        return;
    }

    const int weapon_slot = UserStatus->equipped_weapon_slot[chara];
    if (weapon_slot < 0 || weapon_slot >= 11) {
        return;
    }

    const int georama = UserStatus->cur_georama;
    if (georama < 0 || georama >= 7) {
        return;
    }

    y -= 10;
    y += BtActStatus.hud_shake_y;
    CTexture *icons = TexManager.GetTexture(const_cast<char *>("itempack"), -1);
    CTexture *frame = TexManager.GetTexture(const_cast<char *>("stayframe"), -1);

    float life = UserStatus->hp[chara];
    float life_max = UserStatus->max_hp[chara];
    int   length = static_cast<int>(0.74f * life_max);

    length = std::max(length, 2);

    int cells = (length >> 2) + 1;

    cells = std::max(cells, 2);

    int alpha = 0x80;

    if (life <= 0.2f * life_max) {
        alpha = statusAlarmRate;
    }

    auto *upper = reinterpret_cast<spRGBA *>(BtGetStatusPal(0, life_max, life));
    auto *lower = reinterpret_cast<spRGBA *>(BtGetStatusPal2(0, life_max, life));
    set2DSprite(Vif1Packet, icons, CRect_i_(0x30, y, 4, 0x10), CRect_i_(0x68, 0x70, 4, 0x10), alpha);

    for (int i = 0; i < cells - 2; i++) {
        set2DSprite(Vif1Packet, icons, CRect_i_(i * 4 + 0x34, y, 4, 0x10), CRect_i_(0x6C, 0x70, 4, 0x10), alpha);
    }

    set2DSprite(Vif1Packet, icons, CRect_i_((cells - 1) * 4 + 0x30, y, 4, 0x10), CRect_i_(0x70, 0x70, 4, 0x10), alpha);

    if (life > 0.0f) {
        StatusBarFill(y, cells, life, life_max, upper, lower);
    }

    cells = (cells - 1) * 4 + 0x38;
    cells += ValuePrint(cells, y + 7, static_cast<int>(life), 0, alpha) * 10;
    set2DSprite(Vif1Packet, frame, CRect_i_(cells, y + 7, 0xC, 0xC), CRect_i_(0x78, 0xB0, 0xC, 0xC), alpha);
    ValuePrint(cells + 10, y + 7, static_cast<int>(life_max), 0, alpha);
    set2DSprite(Vif1Packet, icons, CRect_i_(0x20, y, 0x10, 0x10), CRect_i_(0x40, 0xA0, 0x10, 0x10), alpha);

    WEAPON_HAVE *weapon = &UserStatus->chara_weapons[chara][weapon_slot];
    float        durability_max = weapon->durability;
    float        durability = weapon->durability_f;
    float        bar = 1.4949495f * durability_max;
    length = static_cast<int>(bar);

    length = std::max(length, 2);

    cells = (length >> 2) + 2;

    cells = std::max(cells, 2);

    alpha = 0x80;

    if (durability <= 0.2f * durability_max) {
        alpha = statusAlarmRate;
    }

    upper = reinterpret_cast<spRGBA *>(BtGetStatusPal(1, durability_max, durability));
    lower = reinterpret_cast<spRGBA *>(BtGetStatusPal2(1, durability_max, durability));
    set2DSprite(Vif1Packet, icons, CRect_i_(0x30, y + 0x11, 4, 0x10), CRect_i_(0x68, 0x70, 4, 0x10), alpha);

    for (int i = 0; i < cells - 2; i++) {
        set2DSprite(Vif1Packet, icons, CRect_i_(i * 4 + 0x34, y + 0x11, 4, 0x10), CRect_i_(0x6C, 0x70, 4, 0x10), alpha);
    }

    set2DSprite(Vif1Packet, icons, CRect_i_((cells - 1) * 4 + 0x30, y + 0x11, 4, 0x10), CRect_i_(0x70, 0x70, 4, 0x10), alpha);

    if (durability > 0.0f) {
        StatusBarFill(y + 0x11, cells, durability, durability_max, upper, lower);
    }

    int exp_max = GetWeaponMaxExp(weapon);
    int width = static_cast<int>(bar);
    MGFillBox(CRect_i_(0x340, ((y + 0x1F) >> 1) * 16, (width - 1) * 16, 0x10), 0x40, 0x40, 0x40, alpha);

    if (weapon->experience > 0) {
        int filled = static_cast<int>(static_cast<float>(weapon->experience) * (static_cast<float>(width) / static_cast<float>(exp_max)));
        MGFillBox(CRect_i_(0x340, ((y + 0x1F) >> 1) * 16, (filled - 1) * 16, 0x10), 0, 0xA2, 0xFF, alpha);
    }

    cells = (cells - 1) * 4 + 0x38;
    int shown = static_cast<int>(durability);

    if (durability - static_cast<float>(shown) > 0.0f) {
        shown++;
    }

    cells += ValuePrint(cells, y + 0x18, shown, 0, alpha) * 10;
    set2DSprite(Vif1Packet, frame, CRect_i_(cells, y + 0x18, 0xC, 0xC), CRect_i_(0x78, 0xB0, 0xC, 0xC), alpha);
    ValuePrint(cells + 10, y + 0x18, static_cast<int>(durability_max), 0, alpha);
    set2DSprite(Vif1Packet, icons, CRect_i_(0x20, y + 0x10, 0x10, 0x10), CRect_i_(0x40, 0xB0, 0x10, 0x10), alpha);

    float water_max = UserStatus->water_max[chara];
    float water = UserStatus->water_now[chara];
    alpha = 0x80;

    if (water < 0.15f * water_max) {
        alpha = statusAlarmRate;
    }

    set2DSprite(Vif1Packet, icons, CRect_i_(0x20, y + 0x22, 0x12, 0x14), CRect_i_(0x64, 0x84, 0x12, 0x14), alpha);
    int drops = static_cast<int>(water_max) / 10;

    for (cells = 0; cells < drops; cells++) {
        set2DSprite(Vif1Packet, icons, CRect_i_(cells * 0x12 + 0x32, y + 0x22, 0x12, 0x14), CRect_i_(0x64, 0x98, 0x12, 0x14), alpha);
    }

    if (static_cast<int>(water_max) % 10 != 0) {
        set2DSprite(Vif1Packet, icons, CRect_i_(drops * 0x12 + 0x32, y + 0x22, 0x12, 0x14), CRect_i_(0x64, 0xAC, 0x18, 0x14), alpha);
    }

    int full_drops = static_cast<int>(water) / 10;

    for (cells = 0; cells < full_drops; cells++) {
        set2DSprite(Vif1Packet, icons, CRect_i_(cells * 0x12 + 0x33, y + 0x22, 0x10, 0x14), CRect_i_(0, 0x48, 0x10, 0x14), 0x50);
    }

    int leftover = static_cast<int>(water) % 10;

    if (leftover != 0) {
        set2DSprite(Vif1Packet, icons, CRect_i_(full_drops * 0x12 + 0x33, y + 0x22, 0x10, 0x14), CRect_i_((3 - static_cast<int>(static_cast<float>(leftover) / 2.5f)) * 16, 0x48, 0x10, 0x14), 0x50);
    }

    // Mod HUD bars (MoonMod render.bar_register, e.g. Ascension XP) under the water drops, as wide as the
    // drop row.
    const int bar_width = std::max(0x36, (static_cast<int>(water_max) / 10) * 0x12);
    int       bar_y = y + 0x38;
    spRGBA    trough = {0x10, 0x10, 0x18, 0x60};
    spRGBA    fill_left = {0x60, 0x30, 0xC0, 0x80};
    spRGBA    fill_right = {0xE0, 0xB0, 0x30, 0x80};
    for (const auto &bar : moonmod::ChronicleHudBars()) {
        const double ratio = bar.maximum > 0.0 ? std::clamp(bar.current / bar.maximum, 0.0, 1.0) : 0.0;
        if (bar.boss) {
            // A boss's bar (render.bar_register boss = true): wide along the bottom, its name over it.
            spRGBA boss_trough = {0x08, 0x04, 0x04, 0x70};
            spRGBA boss_left = {0x90, 0x10, 0x10, 0x80};
            spRGBA boss_right = {0xD0, 0x40, 0x20, 0x80};
            constexpr int kBossX = 0x78, kBossY = 0x19A, kBossWidth = 0x190;
            set2DSpriteC4(Vif1Packet, CRect_i_(kBossX - 1, kBossY - 1, kBossWidth + 2, 9), &boss_trough, &boss_trough, &boss_trough, &boss_trough);
            const int boss_filled = static_cast<int>(kBossWidth * ratio);
            if (boss_filled > 0) {
                set2DSpriteC4(Vif1Packet, CRect_i_(kBossX, kBossY, boss_filled, 7), &boss_left, &boss_right, &boss_left, &boss_right);
            }
            ui::Label(bar.label, ui::ColorWhite, ui::Alignment::Left, true, ui::metrics::kScaleSmall).Draw(kBossX, kBossY - 18);
            continue;
        }
        set2DSpriteC4(Vif1Packet, CRect_i_(0x32, bar_y, bar_width + 2, 7), &trough, &trough, &trough, &trough);
        const int filled = static_cast<int>(bar_width * ratio);
        if (filled > 0) {
            set2DSpriteC4(Vif1Packet, CRect_i_(0x33, bar_y + 1, filled, 5), &fill_left, &fill_right, &fill_left,
                          &fill_right);
        }
        if (bar.number >= 0) {
            ValuePrint(0x22, bar_y - 3, bar.number, 0, alpha);
        }
        bar_y += 12;
    }

    // Mod HUD text (render.layer_register) under the bars, in the game's own lettering; the presenter leaves it out
    // while a dungeon draws it here. Mod notices have the message window (DrawModNotice).
    constexpr float kModTextScale = ui::metrics::kScaleSmall;
    constexpr int   kModTextRow = 18;
    int             text_y = bar_y + 2;
    for (const std::string &line : moonmod::ChronicleOverlayLines()) {
        ui::Label(line, ui::ColorWhite, ui::Alignment::Left, true, kModTextScale).Draw(0x30, text_y);
        text_y += kModTextRow;
    }

    static float popupYRate = -PI;
    popupYRate += 0.20943952f;

    if (!(popupYRate < PI)) {
        popupYRate -= TWO_PI;
    }

    static float popupRGBRate = -PI;
    popupRGBRate += SIXTEENTH_PI;

    if (!(popupRGBRate < 0.0f)) {
        popupRGBRate -= PI;
    }

    ITEM_PACK *pack = &UserStatus->item_pack;

    for (int i = 0; i < 3; i++) {
        int bob = 0;
        int glow = 0;

        if (BtBySpeedFlag != 0 && pack->quick_item_slot[i] == ITEM_UNUSED_156) {
            glow = static_cast<int>(64.0f * std::sin(popupRGBRate)) + 0x3F;
            bob = 0;
        }

        set2DSprite(Vif1Packet, icons, CRect_i_(i * 40 + 0x126, y + 2, 0x24, 0x24), CRect_i_(0x24, 0x5C, 0x24, 0x24), glow + 0x80);

        if (pack->quick_item_slot[i] != -1) {
            set2DSprite(Vif1Packet, icons, CRect_i_(i * 40 + 0x128, y + 4 + bob, 0x1F, 0x1F), CRect_i_((i << 5) + 0x20, 0, 0x1F, 0x1F));

            if (pack->quick_item_qty[i] > 1) {
                int tens = pack->quick_item_qty[i] / 10;

                if (tens > 0) {
                    set2DSprite(Vif1Packet, frame, CRect_i_(i * 40 + 0x12F, y + 0x17, 0xC, 0xC), CRect_i_(tens * 12, 0xD4, 0xC, 0xC));
                }

                set2DSprite(Vif1Packet, frame, CRect_i_(i * 40 + 0x13B, y + 0x17, 0xC, 0xC), CRect_i_(pack->quick_item_qty[i] % 10 * 12, 0xD4, 0xC, 0xC));
            }
        }
    }

    set2DSprite(Vif1Packet, icons, CRect_i_(0x1FC, y, 0x66, 0x29), CRect_i_(0x9A, 1, 0x66, 0x29));

    if (BtUraDongeon != 0) {
        set2DSprite(Vif1Packet, icons, CRect_i_(0x23C, y, 0x26, 0x29), CRect_i_(0xDA, 0x2B, 0x26, 0x29));
    }

    if (BtEquipMasuisyou != 0) {
        set2DSprite(Vif1Packet, icons, CRect_i_(0x1FE, y + 5, 0x20, 0x20), CRect_i_(0, 0xA0, 0x20, 0x20));
    }

    if (BtEquipMap != 0) {
        set2DSprite(Vif1Packet, icons, CRect_i_(0x21E, y + 5, 0x20, 0x20), CRect_i_(0x20, 0xA0, 0x20, 0x20));
    }

    // A mod's label (render.floor_label_set) takes the number's place, lettered like it and shrunk to fit.
    if (const std::string label = moonmod::ChronicleFloorLabel(); !label.empty()) {
        ui::TextStyle style;
        style.align = ui::Alignment::Center;
        style.drop_shadow = false;
        style.use_custom_rgb = true;
        style.fill_r = 250;
        style.fill_g = 244;
        style.fill_b = 226;
        style.outline_width = 1.5f;
        style.outline_r = 72;
        style.outline_g = 40;
        style.outline_b = 18;
        style.scale = 0.86f;
        ui::Label plate_label(label, ui::ColorWhite, ui::Alignment::Center, false, style.scale);
        plate_label.SetStyle(style);
        constexpr int kPlateNumberWidth = 0x24;
        if (const int width = plate_label.GetWidth(); width > kPlateNumberWidth) {
            plate_label.SetScale(style.scale * static_cast<float>(kPlateNumberWidth) / static_cast<float>(width));
        }
        plate_label.DrawCenteredInRect(0x23C, y + 0x11, 0x26, 0x13);
    } else if (IsLastFloor(georama, floor)) {
        set2DSprite(Vif1Packet, frame, CRect_i_(0x23C, y + 0x12, 0x26, 0x11), CRect_i_(0x78, 0x9E, 0x26, 0x12));
    } else {
        const int x = floor + 1 >= 10 ? 0x245 : 0x23F;

        int number_y = y + 0x12;

        if (floor + 1 >= 10) {
            set2DSprite(Vif1Packet, frame, CRect_i_(x - 4, number_y, 0xE, 0x11), CRect_i_((floor + 1) / 10 * 12, 0x9E, 0xC, 0x12));
        }

        set2DSprite(Vif1Packet, frame, CRect_i_(x + 9, number_y, 0xE, 0x11), CRect_i_((floor + 1) % 10 * 12, 0x9E, 0xC, 0x12));
    }

    // Mod HUD icons (render.hud_icon_set) in a row left of the key: native status badges at half size, or a
    // mod's own 32x32 png at 22x22. A blinking one pulses. The row keeps to the floor plate's side on a wide window
    // (by its own centre it would stay mid-screen, over the game's ailment badge).
    static int hud_icon_pulse = 0;
    hud_icon_pulse = (hud_icon_pulse + 1) % 64;
    const int           pulse_alpha = 0x30 + std::abs(32 - hud_icon_pulse) * 3;
    int                 icon_right = 0x1FA;
    const gfx::UiAnchor status_anchor = gfx::CurrentUiAnchor();
    gfx::SetUiAnchor(gfx::UiAnchor::Side(1, -1));
    // MoonMod: the charms the active character wears (charm_slots.hpp) next to the key, each with its uses left in
    // its corner, then the mod icons beyond them.
    for (int i = 0; i < CharmSlotsShown(chara); i++) {
        const int        index = CharmSlotIndex(chara, i);
        const CharmSlot &charm = CharmSlotAt(chara, index);
        if (charm.item_no < 0) {
            continue;
        }
        // A mod's own icon, else the copy of the game's in the HUD sheet (DngActiveItemTextureCopy).
        CTexture *texture = ItemIconOverrideTexture(charm.item_no);
        int       u = 0;
        int       v = 0;
        if (texture == nullptr) {
            texture = icons;
            CharmHudCell(index, u, v);
        }
        constexpr int kCharmIcon = 26;
        icon_right -= kCharmIcon;
        spRGBA backing = {0x10, 0x14, 0x20, 0x50};
        set2DSpriteC4(Vif1Packet, CRect_i_(icon_right, y + 0x08, kCharmIcon, kCharmIcon), &backing, &backing, &backing, &backing);
        const int texel = texture == icons ? CHARM_HUD_ICON_SIZE : 32;
        set2DSprite(Vif1Packet, texture, CRect_i_(icon_right + 1, y + 0x09, kCharmIcon - 2, kCharmIcon - 2), CRect_i_(u, v, texel, texel));
        // The uses left, right-aligned in the bottom corner in the quick slots' digits.
        int digit_x = icon_right + kCharmIcon - 8;
        for (int uses = CharmUsesShown(charm.item_no, charm.uses); ; uses /= 10) {
            set2DSprite(Vif1Packet, frame, CRect_i_(digit_x, y + 0x08 + kCharmIcon - 10, 9, 9), CRect_i_(uses % 10 * 12, 0xD4, 0xC, 0xC));
            digit_x -= 7;
            if (uses < 10) {
                break;
            }
        }
        icon_right -= 3;
    }
    for (const moonmod::ChronicleHudIcon &icon : moonmod::ChronicleHudIcons()) {
        const int icon_alpha = icon.blink ? pulse_alpha : 0x80;
        if (icon.native >= 0 && icon.native < 5) {
            constexpr std::array<std::array<int, 2>, 5> badge_cells = {{{1, 0}, {0, 1}, {1, 1}, {1, 2}, {0, 2}}};
            const auto &cell = badge_cells[icon.native];
            icon_right -= 31;
            set2DSprite(Vif1Packet, icons, CRect_i_(icon_right, y + 0x0C, 31, 18),
                        CRect_i_(cell[0] * 62 + 132, cell[1] * 36 + 84, 62, 36), icon_alpha);
        } else if (CTexture *image = HudImageTexture(icon.image.c_str())) {
            icon_right -= 22;
            set2DSprite(Vif1Packet, image, CRect_i_(icon_right, y + 0x0A, 22, 22), CRect_i_(0, 0, 32, 32), icon_alpha);
        } else {
            continue;
        }
        icon_right -= 3;
    }
    gfx::SetUiAnchor(status_anchor);

    selected_item *= 40;
    set2DSprite(Vif1Packet, frame, CRect_i_(selected_item + 0xFC, y, 0x10, 0x10), CRect_i_(0x4A, 0x48, 0x10, 0x10));
    set2DSprite(Vif1Packet, frame, CRect_i_(selected_item + 0x113, y, 0x10, 0x10), CRect_i_(0x5A, 0x48, 0x10, 0x10));
    set2DSprite(Vif1Packet, frame, CRect_i_(selected_item + 0xFC, y + 0x16, 0x10, 0x10), CRect_i_(0x4A, 0x58, 0x10, 0x10));
    set2DSprite(Vif1Packet, frame, CRect_i_(selected_item + 0x113, y + 0x16, 0x10, 0x10), CRect_i_(0x5A, 0x58, 0x10, 0x10));

    y -= BtActStatus.hud_shake_y;
    alpha = y + 0x6A;
    set2DSprite(Vif1Packet, icons, CRect_i_(0x18, 0x1AC, 0x2A, 0x1D), CRect_i_(0x78, 0x37, 0x2A, 0x1D), alpha);
    set2DSprite(Vif1Packet, icons, CRect_i_(0x45, 0x1A8, 0x2B, 0x10), CRect_i_(0xA2, 0x37, 0x2B, 0x10), alpha);
    int u = 0;
    int v = 0;
    int item = UserStatus->chara_weapons[chara][weapon_slot].item_no;

    if (item == defWeapon__2[CombatChara(chara)] + 1) {
        v = 0x20;
    }

    if (item == defWeapon__2[CombatChara(chara)]) {
        u = 0x20;
        v = 0x20;
    }

    set2DSprite(Vif1Packet, icons, CRect_i_(0x1D, 0x1A4, 0x20, 0x20), CRect_i_(u, v, 0x20, 0x20), alpha);
}

int StatusErrCheck(int status) {
    return (UserStatus->ailments[UserStatus->cur_chara] & status) ? 1 : 0;
}

int BtStatusErrColorSet() {
    int       ailing = 0;
    const int status = UserStatus->ailments[UserStatus->cur_chara];

    if (status & AILMENT_FREEZE) {
        StatusColor[0] = 127.5f;
        StatusColor[1] = 127.5f;
        StatusColor[2] = 127.5f;
        ailing = 1;
    }

    if (status & AILMENT_GOO) {
        StatusColor[0] = 50.0f;
        StatusColor[1] = 75.0f;
        StatusColor[2] = 127.5f;
        ailing = 1;
    }

    if (status & AILMENT_POISON) {
        StatusColor[0] = 47.0f;
        StatusColor[1] = 0.5f;
        StatusColor[2] = 63.75f;
        ailing = 1;
    }

    if (status & AILMENT_STAMINA) {
        StatusColor[0] = 127.5f;
        StatusColor[1] = 80.0f;
        StatusColor[2] = 15.0f;
        ailing = 1;
    }

    return ailing;
}

void BtStatusErrStep() {
    CUserStatus *status = UserStatus;
    const int    flags = status->ailments[status->cur_chara];

    poison_counter++;

    // Poison takes 4% of the character's max HP every 180 steps and shows the loss over them.
    if ((flags & AILMENT_POISON) && poison_counter >= 180) {
        const int chara = status->cur_chara;
        const int max_hp = status->max_hp[chara];
        const int damage = static_cast<int>(0.04 * max_hp);
        status->AddNowLife(chara, static_cast<s16>(-damage), 10.0f);

        float position[4] = {0.0f, 0.0f, 0.0f, 1.0f};
        position[1] = CharaHeight(UserStatus);
        HitValueEntry(NowHitValue, position, damage, HIT_VALUE_PLAYER, CharaMain.frame);
    }

    if (poison_counter >= 180) {
        poison_counter = 0;
    }

    BtStatusErrColorSet();
}

void BtSetStatusErr(int status) {
    const int chara = UserStatus->cur_chara;
    // MoonMod player.tuning_set: status_immune refuses the ailment; status_duration scales Freeze and Stamina.
    if (moonmod::ChroniclePlayerStatusImmune() & status) return;
    const float duration = moonmod::ChroniclePlayerStatusDuration();
    const auto frames = [duration](int retail) { return std::max(1, static_cast<int>(std::lround(retail * duration))); };

    switch (status) {
        case AILMENT_FREEZE:
            UserStatus->ailments[chara] |= status;
            UserStatus->ailments[chara] &= ~(AILMENT_STAMINA | AILMENT_POISON | AILMENT_GOO);
            UserStatus->ailment_frames[chara] = frames(300);
            SndSePlay(SE_STATUS_AILMENT, -1, 0);
            break;

        case AILMENT_STAMINA:
            if (!(UserStatus->ailments[chara] & AILMENT_FREEZE)) {
                UserStatus->ailments[chara] |= status;
                UserStatus->ailment_frames[chara] = frames(1800);
            }

            break;

        case AILMENT_POISON:
            if (!(UserStatus->ailments[chara] & (AILMENT_FREEZE | AILMENT_STAMINA))) {
                UserStatus->ailments[chara] |= status;
                UserStatus->ailments[chara] &= ~AILMENT_GOO;
                SndSePlay(SE_STATUS_AILMENT, -1, 0);
            }

            break;

        case AILMENT_CURSE:
            UserStatus->ailments[chara] |= status;
            SndSePlay(SE_STATUS_AILMENT, -1, 0);
            break;

        case AILMENT_GOO:
            if (!(UserStatus->ailments[chara] & (AILMENT_FREEZE | AILMENT_STAMINA | AILMENT_POISON))) {
                UserStatus->ailments[chara] |= status;
                SndSePlay(SE_STATUS_AILMENT, -1, 0);
            }

            break;

        default:
            break;
    }
}

namespace {

// Up to three lines of at most `columns` characters, broken between words; a longer notice ends in "...".
std::string WrapNotice(std::string_view text, std::size_t columns, std::string_view indent) {
    std::vector<std::string> lines(1);
    std::size_t              at = 0;
    while (at < text.size()) {
        std::size_t end = text.find(' ', at);
        if (end == std::string_view::npos) end = text.size();
        const std::string_view word = text.substr(at, end - at);
        at = end + 1;
        if (word.empty()) continue;
        if (!lines.back().empty() && lines.back().size() + 1 + word.size() > columns) lines.emplace_back();
        if (!lines.back().empty()) lines.back() += ' ';
        lines.back() += word;
    }
    constexpr std::size_t kRows = 3;
    if (lines.size() > kRows) {
        lines.resize(kRows);
        lines.back() = lines.back().substr(0, columns - 3) + "...";
    }
    std::string out;
    for (const std::string &line : lines) {
        if (!out.empty()) out += '\n';
        out += indent;
        out += line;
    }
    return out;
}

} // namespace

void DrawModNotice(bool native_busy) {
    std::string text;
    std::string icon;
    if (!moonmod::CurrentGameNotice(text, icon)) return;
    if (native_busy) return; // the game's own message has the window's place; this one waits its turn
    moonmod::MarkGameNoticeShown();
    // The game's dungeon message window (DngMes1's settings), holding a mod's notice for its frames.
    static dc::text::GameText notice;
    static std::string        shown;
    static bool               configured = false;
    if (!configured) {
        configured = true;
        notice.KeepGameRandom(true);
        game::ClsMes &mes = notice.Mes();
        mes.page_arrow = false;
        mes.stay_frame = true;
        mes.char_height = 22;
        mes.style = MES_EDGE_NONE;
    }
    constexpr std::array<std::string_view, 5> kBadges = {"freeze", "stamina", "poison", "curse", "goo"};
    const auto badge = std::ranges::find(kBadges, icon);
    CTexture  *image = badge == kBadges.end() && !icon.empty() ? HudImageTexture(icon.c_str()) : nullptr;
    const bool has_icon = badge != kBadges.end() || image != nullptr;
    const std::string wrapped = WrapNotice(text, has_icon ? 21 : 24, has_icon ? "    " : "");
    if (wrapped != shown) {
        shown = wrapped;
        notice.Set(wrapped);
    }
    game::ClsMes &mes = notice.Mes();
    mes.text_x = 0x136;
    mes.text_y = 0x154;
    mes.auto_pos = MES_POS_BOTTOM_RIGHT;
    mes.DrawMesWin();
    // The icon sits in the text's indent, centred on its lines (win_x and win_y follow the window's grow animation).
    const int icon_x = mes.text_x + 2;
    const int icon_mid = mes.text_y + mes.text_height / 2;
    if (badge != kBadges.end()) {
        constexpr std::array<std::array<int, 2>, 5> badge_cells = {{{1, 0}, {0, 1}, {1, 1}, {1, 2}, {0, 2}}};
        const auto &cell = badge_cells[badge - kBadges.begin()];
        CTexture   *icons = TexManager.GetTexture(const_cast<char *>("itempack"), -1);
        set2DSprite(Vif1Packet, icons, CRect_i_(icon_x, icon_mid - 11, 38, 22),
                    CRect_i_(cell[0] * 62 + 132, cell[1] * 36 + 84, 62, 36));
    } else if (image != nullptr) {
        set2DSprite(Vif1Packet, image, CRect_i_(icon_x + 4, icon_mid - 15, 30, 30), CRect_i_(0, 0, 32, 32));
    }
}

void BtStatusErrDraw(int y) {
    CTexture                                   *texture = TexManager.GetTexture("itempack", -1);
    constexpr std::array<int, 5>                status_flags = {AILMENT_FREEZE, AILMENT_STAMINA, AILMENT_POISON, AILMENT_CURSE, AILMENT_GOO};
    constexpr std::array<std::array<int, 2>, 5> icon_cells = {
        {
         {1, 0},
         {0, 1},
         {1, 1},
         {1, 2},
         {0, 2},
         }
    };
    const int status = UserStatus->ailments[UserStatus->cur_chara];

    for (int icon = 4; icon >= 0; icon--) {
        if (status & status_flags[icon]) {
            set2DSprite(Vif1Packet, texture, CRect_i_(430, y - 10, 62, 35), CRect_i_((icon_cells[icon][0] * 62) + 132, (icon_cells[icon][1] * 36) + 84, 62, 36));
        }
    }
}

void setItemToReserved(char *page_name, int x, int y, char *item_name, int dsax, int dsay, int size) {
    CTexture *page = TexManager.GetTexture(page_name, -1);
    CTexture *item = TexManager.GetTexture(item_name, -1);

    int sbp = page->tex0 & 0x3FFF;
    int dbp = item->tex0 & 0x3FFF;
    int sbw = (page->tex0 >> 14) & 0x3F;
    int dbw = (item->tex0 >> 14) & 0x3F;

    MoveImageTest(Vif1Packet, sbp, sbw, 0x13, CRect_i_(x, y, size, size), dbp, dbw, 0x13, dsax, dsay, 0);
}

void BtMapJumpCashClear() {
    BtCfgCash.clear();
    BtCfgFlag = 0;
    BtSteebMsgNo = -1;
}

#ifdef PORT
namespace {
int g_bank_mark = -1; // where the map's part banks start in its buffer
} // namespace

void BtReloadPartBanks() {
    if (g_bank_mark < 0 || NowDngMap != &MainDungeonMap || DngPartBanksWanted(selectMapNo) == DngPartBanksLoaded()) return;
    MapModelBuffer.used = g_bank_mark;
    u_int *pack = read_buffer;
    read_buffer = old_read_buffer;
    DngLoadPartBanks(selectMapNo);
    read_buffer = pack;
    const int map_used = MapModelBuffer.used;
    MonstorModelBuffer.base = &MapModelBuffer.base[static_cast<std::ptrdiff_t>(map_used) * 16];
    MonstorModelBuffer.limit = MapModelBuffer.limit - map_used - 0x88B8;
    MonstorModelBuffer.used = 0;
}
#endif

int BtMapJumpLoad(char *map_name) {
    // A floor added by game.dungeons.register with its own map reads that floor pack instead.
    if (const char *mod_map = ChronicleModFloorMap(UserStatus->cur_georama, UserStatus->cur_floor)) {
        std::println(stderr, "moonmod: floor {} of dungeon {} reads floor pack {} (script asked for {})",
                     UserStatus->cur_floor + 1, UserStatus->cur_georama, mod_map, map_name);
        map_name = const_cast<char *>(mod_map);
    }
    int mes_size = 0;

    CUserStatus *status = UserStatus;
    int          msg_no = status->cur_georama;
    const int    floor = status->cur_floor;

    // From dungeon 6 on, the message file also changes every twenty floors.
    if (msg_no >= 6) {
        if (floor >= 20) {
            msg_no++;
        }

        if (floor >= 40) {
            msg_no++;
        }

        if (floor >= 60) {
            msg_no++;
        }

        if (floor >= 80) {
            msg_no++;
        }
    }

    if (msg_no != BtSteebMsgNo) {
        BtSteebMesBuffer.used = 0;
        u_char *mes = &BtSteebMesBuffer.base[static_cast<std::ptrdiff_t>(BtSteebMesBuffer.used) * 16];

        std::string mes_path = status->cur_georama == 6 ? std::format("dun/message/ww_mes/steve07_{}_{}.mes", msg_no - 5, LanguageCode)
                                                        : std::format("dun/message/ww_mes/steve0{}_{}.mes", msg_no + 1, LanguageCode);

        LoadFile(mes_path.data(), mes, &mes_size);
        wait_now_loading_vsync();
        DngMesStb.SetBuff(reinterpret_cast<short *>(mes));
        BtSteebMesBuffer.Alloc((((mes_size >> 6) + 1) << 6) >> 4);
        BtSteebMesBuffer.Align64();
        BtSteebMsgNo = msg_no;
    }

    TexManager.DeleteTextureBlock(42);
    MainMonstorUnit.CleanViewMonstor(0);
    NowDngMap = &MainDungeonMap;
    NowEventMan = &DngEventMan;

    // MoonMod: a change in the dungeons a zoned floor takes parts from loads the map again (part banks).
    if (BtCfgCash == map_name && BtCfgFlag != 0 && DngPartBanksWanted(selectMapNo) == DngPartBanksLoaded()) {
        return 0;
    }

    BtCfgCash = map_name;

    MapModelBuffer.used = 0;
    u_int *pack = read_buffer;
    read_buffer = old_read_buffer;

    // NowDranMapField points at DranMapField.field, the start of the whole set.
    auto *field_set = reinterpret_cast<DRAN_MAP_FIELD_SET *>(NowDranMapField);

    for (int field_no = 0; field_no < 12; field_no++) {
        field_set->field[field_no].Initialize();
        field_set->collision[field_no] = nullptr;
        field_set->state[field_no] = 3;
    }

    field_set->field_count = 0;
    field_set->collision_count = 0;

    MainDungeonMap.initSubmap(&MapModelBuffer);

    for (CDungeonEvent &event : DngEventMan.slot) {
        event.name[0] = '\0';
        event.script_no = -1;
        event.parts_id = -1;
        event.switch_on = false;
        event.enabled = false;
    }

    for (CDungeonEventData &data : DngEventMan.event) {
        data.event = nullptr;
        data.switch_on = false;
        data.enabled = false;
        data.hold = 0;
        data.chara_done = -1;
    }

    std::string mpd_path = std::format("dun/mpd_pack/{}.mpd", map_name);
    std::string cfg_path = std::format("dun/{}.cfg", map_name);
    TEIGIAnalyz(cfg_path.data());

    int size = 0;
    LoadFile(mpd_path.data(), read_buffer, &size);
    wait_now_loading_vsync();

    if (size >= 6400000) {
        std::println("MAPDATA BUFFER OVER!! {}", size);
        exit__2(1);
    }

    TEIGIImgLoad(read_buffer, &MapModelBuffer);
    TEIGIMdsLoad(read_buffer, 0);

    CFrameAttr attr;
    attr.fog_enable = true;
    attr.clip_depth = 100.0f;
    attr.clip_enable = false;
    attr.program_option = 0;
    NowDngMap->box_body_model = LoadMDSFilePack(read_buffer, "ibox_0.mds", &MapModelBuffer);
    NowDngMap->box_lid_model = LoadMDSFilePack(read_buffer, "ibox_t.mds", &MapModelBuffer);
    NowDngMap->box_collision_model = LoadCollisionFilePack(read_buffer, "ibox_a.mds", &MapModelBuffer);
    NowDngMap->box_body_model->SetAttr(attr, 1, 0);
    NowDngMap->box_lid_model->SetAttr(attr, 1, 0);
    NowDngMap->chest_body_model = LoadMDSFilePack(read_buffer, "iboxs_0.mds", &MapModelBuffer);
    NowDngMap->chest_lid_model = LoadMDSFilePack(read_buffer, "iboxs_t.mds", &MapModelBuffer);
    NowDngMap->chest_collision_model = LoadCollisionFilePack(read_buffer, "iboxs_a.mds", &MapModelBuffer);
    NowDngMap->chest_body_model->SetAttr(attr, 1, 0);
    NowDngMap->chest_lid_model->SetAttr(attr, 1, 0);

    itemOpenSmall.LoadPack(read_buffer, "dun/etc/itemget_s/d01i02m", &MapModelBuffer, &MapModelBuffer, &itemOpenSmall_info, 0);
    itemOpenBig.LoadPack(read_buffer, "dun/etc/itemget_b/d01i01m", &MapModelBuffer, &MapModelBuffer, &itemOpenBig_info, 0);

#ifdef PORT
    // MoonMod (dungeons.set_npc_models): characters a mod stands on this dungeon's floors, in NPC slots its parts left free
    for (int m = 0; m < DngNpcModelCount; m++) {
        DngNpcModel &model = DngNpcModels[m];
        if (model.dungeon != selectMapNo || MainDungeonMap.npc[model.npc].used != 0) continue;
        int pack_size = 0;
        LoadFile(model.pack, read_buffer, &pack_size);
        if (pack_size <= 0) continue;
        sceVu0FVECTOR origin = {0.0f, 0.0f, 0.0f, 1.0f};
        sceVu0FVECTOR turn = {0.0f, 0.0f, 0.0f, 0.0f};
        MainDungeonMap.SetNPC(model.npc, read_buffer, 9999, origin, turn, 1, model.motion, &MapModelBuffer, model.cfg);
    }
#endif

#ifdef PORT
    // MoonMod (dungeons.set_zones source_dungeon): other dungeons' parts, beside the map's own.
    g_bank_mark = MapModelBuffer.used;
    DngLoadPartBanks(selectMapNo);
#endif

    // The monsters get whatever the map left of its buffer.
    const int map_used = MapModelBuffer.used;
    MonstorModelBuffer.base = &MapModelBuffer.base[static_cast<std::ptrdiff_t>(map_used) * 16];
#ifdef PORT
    // MoonMod: what the arena really holds (the port carves it with headroom), not retail's 0xA7F80: a map of several
    // floors' zones (dungeons.set_zones source_floor) loads all their monsters. A system script borrows the pool and
    // 0x88B8 quadwords past it (movechara.cpp), which must stay inside the arena too.
    MonstorModelBuffer.limit = MapModelBuffer.limit - map_used - 0x88B8;
#else
    MonstorModelBuffer.limit = 0xA7F80 - map_used;
#endif
    MonstorModelBuffer.used = 0;
    read_buffer = pack;

    // Only the low 32 bits of each arena address are shown.
    const auto low_address = [](CDataAlloc2<1> &arena) { return static_cast<u32>(reinterpret_cast<std::uintptr_t>(&arena.base[static_cast<std::ptrdiff_t>(arena.used) * 16])); };
    std::println("TotalMem = {}", 0xA7F80);
    std::println("MapVisualData   [{:x}]  {}/{}", low_address(MapModelBuffer), MapModelBuffer.used, MapModelBuffer.limit);
    std::println("MonstorData     [{:x}]  {}/{}", low_address(MonstorModelBuffer), MonstorModelBuffer.used, MonstorModelBuffer.limit);
    BtCfgFlag = 1;
    return 1;
}

void BtSet3DCellModel(float *world, CTexture *texture, float size, int x, int y, int width, int height, int alpha) {
    int top_left[4];
    int top_right[4];
    int bottom_left[4];
    int bottom_right[4];

    world[3] = 1.0f;

    if (MGRotTransPers3DSprite(top_left, bottom_right, world, size, size / 2.0f, 0) == 1) {
        top_right[0] = bottom_right[0];
        top_right[1] = top_left[1];
        top_right[2] = top_left[2];
        bottom_left[0] = top_left[0];
        bottom_left[1] = bottom_right[1];
        bottom_left[2] = bottom_right[2];

        set3DSprite(Vif1Packet, texture, CRect_i_(x, y, width, height), top_left, top_right, bottom_left, bottom_right, static_cast<unsigned char>(alpha));
    }
}

} // namespace dc::game


// The Gauntlet's companion (party.companion_set): a second party member on screen beside the player, driven by
// the engine with simple rules. It follows the player, attacks the lock-on target (or, aggressive, any monster
// near the player), deals its weapon's base attack and cannot be hurt. Monsters never target it. It has its own
// model, weapon and texture arenas, so loading it takes nothing from the player's.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <string>
#include <vector>

#include "btactstatus.hpp"
#include "btmisc.hpp"
#include "character.hpp"
#include "dataalloc.hpp"
#include "dataread.hpp"
#include "dun/gameloop.hpp"
#include "gamemode.hpp"
#include "itemdata.hpp"
#include "main.hpp"
#include "mglib.hpp"
#include "monstorunit.hpp"
#include "playable_chara.hpp"
#include "shot_utils.hpp"
#include "snd.hpp"
#include "sound_effect.h"
#include "texture.hpp"
#include "userstatus.hpp"
#include "moonmod/game_hooks.hpp"

namespace dc::game {

namespace {

constexpr std::array<const char *, 6> kCompanionFiles = {
    "dun/mainchara/c01d.chr", "dun/mainchara/c04b.chr", "dun/mainchara/c06b.chr",
    "dun/mainchara/c05a.chr", "dun/mainchara/c10b.chr", "dun/mainchara/c18a.chr",
};
// Each character's first attack motion, and whether it attacks from range.
constexpr std::array<int, 6>  kAttackMotion = {0x24, 11, 0x24, 11, 0x25, 11};
constexpr std::array<bool, 6> kRanged = {false, true, false, true, false, true};
constexpr int kCompanionTextureBlock = 0x60;
constexpr int kCompanionWeaponTextureBlock = 0x61;
constexpr int kMotionIdle = 0, kMotionRun = 2;

// Arenas of its own (the player's model buffers stay as retail sized them). Quadword units, 64-byte aligned.
alignas(64) unsigned char g_file_storage[6 * 1024 * 1024];
alignas(64) unsigned char g_model_storage[8 * 1024 * 1024];

CCharacter     g_body;
CCharacter     g_weapon;
CTexAnimeData  g_body_anime[64];
CDataAlloc2<1> g_models(-1);

int   g_wanted = -1, g_wanted_weapon = -1, g_loaded = -1, g_loaded_weapon = -1;
int   g_stance = 1; // 0 passive, 1 follows the lock-on target, 2 aggressive
float g_damage_scale = 1.0f;
int   g_motion = -1, g_cooldown = 0, g_target = -1;

// A ranged companion's shot: a pebble, orb or bullet sprite (the dungeon's basefx01 cells, as Xiao's pebbles are
// drawn) that flies to its target; the blow lands when it arrives.
struct Bolt {
    float pos[4];
    float step[4];
    int   frames = 0; // left until it arrives; 0 when unused
    int   target = -1;
    int   damage = 0;
    int   cell = 0;
    float size = 1.0f;
};
std::array<Bolt, 8> g_bolts{};
// basefx01 cell and size per character: Xiao's pebble, a big pink one for Ruby's orb, a small white one for Osmond's bullet.
constexpr std::array<int, 6>   kBoltCell = {0, 0, 0, 1, 0, 10};
constexpr std::array<float, 6> kBoltSize = {1.0f, 1.0f, 1.0f, 1.8f, 1.0f, 0.8f};
bool  g_placed = false;
// MoonMod: the request's own shot (party.companion_set shot), -1 for the character's own way of fighting
int   g_shot_cell = -1;
float g_shot_size = 1.0f;

// A registered character (index past the six) fights as the character he fights as (CombatChara), from his own pack.
int         Style(int chara) { return chara < 6 ? chara : CombatChara(chara); }
const char *CompanionFile(int chara) { return chara < 6 ? kCompanionFiles[chara] : CharaPackPath(chara); }
bool        Ranged(int chara) { return g_shot_cell >= 0 || kRanged[Style(chara)]; }

// Following, as a follower in an open-world game does: it walks the player's own trail (so it rounds the corners
// the player rounded instead of cutting through walls), keeps back about two body lengths, and only sets off
// again once the player has pulled ahead.
constexpr float kFollowStop = 30.0f;  // close enough: it stops here
constexpr float kFollowStart = 50.0f; // and stays put until the player is this far
constexpr float kCrumbStep = 10.0f;   // a trail point each time the player has moved this far
std::array<std::array<float, 2>, 48> g_trail{};
int  g_trail_head = 0, g_trail_count = 0; // ring of the player's trail, oldest at head
bool g_following = false;

void TrailClear() { g_trail_head = g_trail_count = 0; }

void TrailPush(const float *player) {
    if (g_trail_count > 0) {
        const auto &last = g_trail[(g_trail_head + g_trail_count - 1) % g_trail.size()];
        if (std::hypot(player[0] - last[0], player[2] - last[1]) < kCrumbStep) return;
    }
    if (g_trail_count == static_cast<int>(g_trail.size())) {
        g_trail_head = (g_trail_head + 1) % static_cast<int>(g_trail.size());
        --g_trail_count;
    }
    g_trail[(g_trail_head + g_trail_count) % g_trail.size()] = {player[0], player[2]};
    ++g_trail_count;
}

bool InDungeon() { return mode == GAME_MODE_DUNGEON && UserStatus != nullptr && NowMonstorUnit != nullptr && CharaMain.frame != nullptr; }

void Unload() {
    if (g_loaded < 0) return;
    TexManager.DeleteTextureBlock(kCompanionTextureBlock);
    TexManager.DeleteTextureBlock(kCompanionWeaponTextureBlock);
    TexManager.CleanUpBuffer();
    TexManager.CleanUpTextureList();
    g_body.Initialize();
    g_weapon.Initialize();
    g_loaded = g_loaded_weapon = -1;
    g_motion = -1;
    g_placed = false;
    TrailClear();
    for (Bolt &bolt : g_bolts) bolt.frames = 0;
}

unsigned int *ReadPack(const char *path, unsigned char *&at, unsigned char *end) {
    int size = 0;
    auto *buffer = reinterpret_cast<unsigned int *>(at);
    if (LoadFile(const_cast<char *>(path), buffer, &size) == 0 || size <= 0 || at + size > end) return nullptr;
    at += ((size >> 6) + 1) << 6;
    return buffer;
}

bool Load(int chara, int weapon_item) {
    Unload();
    unsigned char *at = g_file_storage, *end = g_file_storage + sizeof(g_file_storage);
    unsigned int  *body = ReadPack(CompanionFile(chara), at, end);
    if (body == nullptr) return false;
    g_models.base = g_model_storage;
    g_models.used = 0;
    g_models.limit = static_cast<int>(sizeof(g_model_storage) >> 4);
    g_body.Initialize();
    for (CTexAnimeData &anime : g_body_anime) anime.Initialize();
    g_body.InitializeTexAnime(g_body_anime, 64);
    g_body.LoadPackData2(body, const_cast<char *>("base.cfg"), &g_models, kCompanionTextureBlock, &g_models, 0);
    if (g_body.frame == nullptr) return false;
    CFrameAttr attr;
    attr.fog_enable = true;
    attr.clip_enable = false;
    attr.program_option = 0;
    g_body.frame->SetAttr(attr, 1, 4);
    g_loaded = chara;

    // The weapon in its hand, when the companion has one.
    std::array<char, 64> file{}, cfg{};
    g_weapon.Initialize();
    if (weapon_item >= ITEM_WEAPON_START) {
        BtGetWeaponNamePath3(file.data(), cfg.data(), weapon_item);
        const std::string path = std::string("commenu/weapon/") + file.data();
        if (unsigned int *weapon = file[0] != 0 ? ReadPack(path.c_str(), at, end) : nullptr) {
            g_weapon.LoadPackData3(weapon, cfg.data(), &g_models, kCompanionWeaponTextureBlock, &g_models, 1, 0);
            if (CFrame *hand = g_body.frame->SearchFrame(const_cast<char *>("weapon")); hand != nullptr && g_weapon.frame != nullptr) {
                g_weapon.SetPosition(0.0f, 0.0f, 0.0f);
                g_weapon.SetRotation(0.0f, 0.0f, 0.0f);
                g_weapon.frame->SetReference(hand);
                g_loaded_weapon = weapon_item;
            }
        }
    }
    g_placed = false;
    return true;
}

void Position(float *out) { g_body.GetPosition(out); }

void Place(const float *at, float facing) {
    float p[4] = {at[0], at[1], at[2], 1.0f};
    g_body.frame->SetPosition(p);
    g_body.SetPosition(p);
    g_body.frame->SetRotation(0.0f, facing, 0.0f);
    g_body.SetRotation(0.0f, facing, 0.0f);
}

void SetMotion(int motion, bool restart) {
    if (motion == g_motion && !restart) return;
    g_body.motion_no = motion;
    g_body.motion_flags = restart ? 4 : 0;
    g_body.motion_speed = -1.0f;
    g_motion = motion;
}

bool MonsterAlive(int slot) {
    if (slot < 0 || slot >= MONSTOR_SLOT_MAX) return false;
    const MONSTOR &m = NowMonstorUnit->monster[slot];
    return m.state == 2 && m.hp > 0 && m.revealed != 0;
}

void MonsterPosition(int slot, float *out) { NowMonstorUnit->chara[slot][0].GetPosition(out); }

float Flat(const float *a, const float *b) { return std::hypot(a[0] - b[0], a[2] - b[2]); }

int PickTarget(const float *player) {
    if (g_stance == 0) return -1;
    if (lockOnTargetFlag != 0 && MonsterAlive(lockOnTargetNo)) return lockOnTargetNo;
    if (g_stance != 2) return -1;
    int   best = -1;
    float best_d = 150.0f; // "in range": about two seconds of walking around the player
    for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) {
        if (!MonsterAlive(slot)) continue;
        float at[4];
        MonsterPosition(slot, at);
        const float d = Flat(at, player);
        if (d < best_d) { best_d = d; best = slot; }
    }
    return best;
}

} // namespace

void MoonModCompanionRequest(int chara, int weapon_item, int stance, float damage_scale, int shot_cell, float shot_size) {
    g_wanted = (chara >= CHARA_TOAN && chara < CharaCount()) ? chara : -1;
    g_shot_cell = shot_cell >= 0 && shot_cell < 16 ? shot_cell : -1;
    g_shot_size = std::clamp(shot_size, 0.3f, 4.0f);
    g_wanted_weapon = weapon_item;
    g_stance = std::clamp(stance, 0, 2);
    g_damage_scale = std::clamp(damage_scale, 0.0f, 10.0f);
}

int MoonModCompanionLoaded() { return g_loaded; }

// Once a frame from the dungeon loop, after the player has moved.
void MoonModCompanionStep() {
    if (!InDungeon()) {
        if (mode != GAME_MODE_DUNGEON) Unload();
        return;
    }
    // A companion can't be the character the player is controlling.
    const int wanted = g_wanted == UserStatus->cur_chara ? -1 : g_wanted;
    if (wanted != g_loaded || (wanted >= 0 && g_wanted_weapon != g_loaded_weapon && g_wanted_weapon >= ITEM_WEAPON_START)) {
        if (wanted < 0) Unload();
        else if (!Load(wanted, g_wanted_weapon)) { Unload(); g_wanted = -1; }
    }
    if (g_loaded < 0) return;

    float player[4], me[4], facing[4];
    CharaMain.GetPosition(player);
    CharaMain.frame->GetRotation(facing);
    if (!g_placed) {
        // Arriving: beside the player and a step ahead, into the room (behind is the doorway they came in by).
        const float side = facing[1] + 0.9f;
        const float beside[4] = {player[0] + 22.0f * std::sin(side), player[1], player[2] + 22.0f * std::cos(side), 1.0f};
        Place(beside, facing[1]);
        TrailClear();
        g_following = false;
        g_placed = true;
    }
    Position(me);
    TrailPush(player);

    g_target = PickTarget(player);
    float goal[4];
    float keep = 10.0f;
    const float apart = Flat(me, player);
    if (g_target >= 0) {
        MonsterPosition(g_target, goal);
        keep = Ranged(g_loaded) ? 45.0f : 11.0f;
        TrailClear();
    } else {
        // Following: set off once the player is kFollowStart away, walk their trail, stop kFollowStop short.
        if (apart > kFollowStart) g_following = true;
        if (apart < kFollowStop) g_following = false;
        // Trail points it has reached, or that lie no nearer the player than it is, are behind it.
        while (g_trail_count > 0) {
            const auto &crumb = g_trail[g_trail_head];
            const float at[4] = {crumb[0], player[1], crumb[1], 1.0f};
            if (Flat(me, at) > 8.0f && Flat(at, player) < apart) break;
            g_trail_head = (g_trail_head + 1) % static_cast<int>(g_trail.size());
            --g_trail_count;
        }
        if (g_trail_count > 0 && apart > kFollowStop + 10.0f) {
            goal[0] = g_trail[g_trail_head][0];
            goal[2] = g_trail[g_trail_head][1];
            keep = 0.0f;
        } else {
            goal[0] = player[0];
            goal[2] = player[2];
            keep = kFollowStop;
        }
        if (!g_following) {
            // Resting: a step to the player's left or right (whichever is nearer), out of the camera's line behind
            // them; it only walks over when it has drifted well off that spot.
            const float sides[2] = {facing[1] + 1.5708f, facing[1] - 1.5708f};
            float       best[2] = {me[0], me[2]}, best_d = 1e30f;
            for (const float side : sides) {
                const float spot[4] = {player[0] + 30.0f * std::sin(side) - 8.0f * std::sin(facing[1]), player[1],
                                       player[2] + 30.0f * std::cos(side) - 8.0f * std::cos(facing[1]), 1.0f};
                const float d = Flat(me, spot);
                if (d < best_d) { best_d = d; best[0] = spot[0]; best[1] = spot[2]; }
            }
            goal[0] = best_d > 16.0f ? best[0] : me[0];
            goal[2] = best_d > 16.0f ? best[1] : me[2];
            keep = 0.0f;
        }
    }
    goal[1] = player[1];

    // Lost (a teleport, a fall, a door the player ran far beyond): it comes back a little way down the player's
    // trail, out of sight behind them.
    if (apart > 220.0f) {
        const float back = facing[1] + 3.14159265f;
        float near[4] = {player[0] + 36.0f * std::sin(back), player[1], player[2] + 36.0f * std::cos(back), 1.0f};
        if (g_trail_count >= 4) { // a point the player walked, so never inside a wall
            const auto &crumb = g_trail[(g_trail_head + g_trail_count - 4) % g_trail.size()];
            near[0] = crumb[0];
            near[2] = crumb[1];
        }
        Place(near, facing[1]);
        TrailClear();
        Position(me);
        goal[0] = me[0];
        goal[2] = me[2];
        g_following = false;
    }

    const float dx = goal[0] - me[0], dz = goal[2] - me[2];
    const float distance = std::hypot(dx, dz);
    const float heading = distance > 0.01f ? std::atan2(dx, dz) : facing[1];
    if (g_cooldown > 0) --g_cooldown;

    if (g_cooldown > 20) {
        // Mid-swing: hold still, facing the target.
    } else if (distance > keep + 1.0f) {
        const float speed = std::min(distance - keep, g_target >= 0 ? (distance > 60.0f ? 2.6f : 1.5f) : (apart > 90.0f ? 2.8f : 1.9f));
        const float next[4] = {me[0] + dx / distance * speed, player[1], me[2] + dz / distance * speed, 1.0f};
        Place(next, heading);
        SetMotion(kMotionRun, false);
    } else if (g_target >= 0 && g_cooldown == 0) {
        Place(me, heading);
        SetMotion(kAttackMotion[Style(g_loaded)], true);
        g_cooldown = 48;
    } else if (g_cooldown == 0) {
        const float still[4] = {me[0], player[1], me[2], 1.0f};
        Place(still, g_target >= 0 ? heading : facing[1]);
        SetMotion(kMotionIdle, false);
    }
    // The blow lands partway into the swing.
    if (g_cooldown == 36 && g_target >= 0 && MonsterAlive(g_target)) {
        const int base = std::max(1, static_cast<int>(std::lround(moonmod::ChronicleCompanionBaseAttack(g_loaded) * g_damage_scale)));
        if (!Ranged(g_loaded)) {
            moonmod::ChronicleCompanionStrike(g_target, base, g_loaded);
        } else if (auto free = std::ranges::find(g_bolts, 0, &Bolt::frames); free != g_bolts.end()) {
            // From about hand height to the target's middle, at 5 units a frame.
            float aim[4];
            MonsterPosition(g_target, aim);
            const float from[3] = {me[0], player[1] + 9.0f, me[2]};
            const float to[3] = {aim[0], aim[1] + 6.0f, aim[2]};
            const float length = std::max(1.0f, std::hypot(to[0] - from[0], to[1] - from[1], to[2] - from[2]));
            free->frames = std::max(1, static_cast<int>(length / 5.0f));
            for (int axis = 0; axis < 3; ++axis) {
                free->pos[axis] = from[axis];
                free->step[axis] = (to[axis] - from[axis]) / static_cast<float>(free->frames);
            }
            free->pos[3] = 1.0f;
            free->step[3] = 0.0f;
            free->target = g_target;
            free->damage = base;
            free->cell = g_shot_cell >= 0 ? g_shot_cell : kBoltCell[Style(g_loaded)];
            free->size = g_shot_cell >= 0 ? g_shot_size : kBoltSize[Style(g_loaded)];
            SndSePlay(SE_CHARA_ACTION, -1, 0);
        }
    }
    for (Bolt &bolt : g_bolts) {
        if (bolt.frames <= 0) continue;
        for (int axis = 0; axis < 3; ++axis) bolt.pos[axis] += bolt.step[axis];
        if (--bolt.frames == 0 && MonsterAlive(bolt.target)) moonmod::ChronicleCompanionStrike(bolt.target, bolt.damage, g_loaded);
    }

    g_body.Step();
    g_body.ClothStep(0); // Toan's poncho and the rest of a character's cloth
    if (g_loaded_weapon >= 0) g_weapon.Step();
}

void MoonModCompanionDraw(bool shadow) {
    if (g_loaded < 0 || !InDungeon()) return;
    if (shadow) {
        g_body.DrawShadow();
        return;
    }
    TexManager.ReloadTexture(Vif1Packet, kCompanionTextureBlock);
    g_body.TextureAnime(kCompanionTextureBlock);
    g_body.Draw();
    if (g_loaded_weapon >= 0 && g_weapon.frame != nullptr) {
        TexManager.ReloadTexture(Vif1Packet, kCompanionWeaponTextureBlock);
        g_weapon.Draw();
    }
}

// In the effects pass beside the player's own shots, which sets up the sprites' blending.
void MoonModCompanionDrawShots() {
    if (g_loaded < 0 || !InDungeon()) return;
    for (const Bolt &bolt : g_bolts) {
        if (bolt.frames <= 0) continue;
        float at[4] = {bolt.pos[0], bolt.pos[1], bolt.pos[2], 1.0f};
        set3DCellModel(at, const_cast<char *>("basefx01"), bolt.size, (bolt.cell % 4) << 5, (bolt.cell / 4) << 5, 32, 32, 128);
    }
}

} // namespace dc::game

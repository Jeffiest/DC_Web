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
#include "collision.hpp"
#include "collisiondata.hpp"
#include "dungeonmap.hpp"
#include "dungeonparts.hpp"
#include "hitvalue.hpp"
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
#include "moonmod/companion.hpp"
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
// MoonMod: hostile (an invader): its life, the blow it lands, its defence, frames it can't be hurt again, frames of its
// fall left, its swings (every third is heavy) and the ambient it draws in.
bool  g_hostile = false, g_heavy = false;
int   g_hp = 0, g_max_hp = 0, g_blow = 10, g_defense = 0, g_hurt = 0, g_dying = 0, g_swings = 0;
// An invader reads the player's swings: each swing started close by is weighed once (g_read), and now and then it
// raises the hero's own guard (motions 8 up, 9 held, 10 down) for g_guard frames, which blocks a blow from the front.
int   g_guard = 0, g_read_seed = 1;
// Or it dashes aside, out of the swing's line (g_dash frames along g_dash_step), and can't be struck meanwhile.
int   g_dash = 0;
float g_dash_step[2] = {0.0f, 0.0f};
bool  g_read = false;
constexpr int kMotionGuardUp = 8, kMotionGuardHold = 9, kMotionGuardDown = 10;
// Between swings it circles the player at its distance, locked on as the player's own lock-on walks (g_circle the
// way round, 1 or -1, turning back at a wall and every other swing). Low on life it backs off (g_retreat frames) and drinks
// one of its g_flasks (g_drink frames), healing at the end; a hit while it drinks spills it.
int   g_circle = 1, g_retreat = 0, g_drink = 0, g_flasks = 0;
constexpr int kMotionLockIdle = 0x12, kMotionStrafeBack = 0x16;
constexpr int kInvaderFlasks = 2, kDrinkFrames = 50;
float g_tint[3] = {-1.0f, -1.0f, -1.0f};
constexpr int kMotionHurt = 6, kMotionDown = 4;
constexpr int kTargetPlayer = -2; // a bolt flying at the player
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

// MoonMod: a blow on the player at that point, as a monster's would be: the game's own hit test takes it.
void StrikePlayer(const float *at, int damage, bool heavy) {
    float point[4] = {at[0], at[1], at[2], 1.0f};
    NowColData->Set(point, damage, 2, heavy ? 11.0f : 9.0f, 0.0f, 1, heavy ? 3 : 2, 0, 0);
}

void StepModels() {
    g_body.Step();
    g_body.ClothStep(0);
    if (g_loaded_weapon >= 0) g_weapon.Step();
}

// Whether the floor's walls stand between two points at waist height (the dash's check).
bool Walled(const float *from, const float *to) {
    if (NowDngMap == nullptr) return false;
    static std::vector<CCPoly> polygons(2048);
    float a[4] = {from[0], from[1] + 5.0f, from[2], 1.0f};
    float b[4] = {to[0], from[1] + 5.0f, to[2], 1.0f};
    float hit[4] = {};
    const float reach = std::hypot(b[0] - a[0], b[2] - a[2]) + 12.0f;
    const int   count = setCollisionData(NowDngMap, polygons.data(), a, reach, 2.0f);
    return count > 0 && count <= static_cast<int>(polygons.size()) && CheckHit(polygons.data(), count, a, b, hit, 1, 4) >= 0;
}

// An invader's frame: it falls when its life is gone, takes the player's hits, closes in and swings.
void HostileStep(const float *player, const float *me) {
    if (g_dying > 0) {
        if (--g_dying == 0) {
            Unload();
            g_wanted = -1;
            g_hostile = false;
            return;
        }
        StepModels();
        return;
    }
    // The player's weapon: a hit in the game's list that can strike monsters and that no monster dealt.
    if (g_hurt > 0) {
        --g_hurt;
    } else {
        float     body[4] = {me[0], me[1] + 8.0f, me[2], 1.0f};
        const int hit = NowColData->FindMonsterHit(body, 7.0f);
        if (hit != -1 && NowColData->GetMonsterOwner(hit) == -1 && g_dash > 0) {
            // Dashing aside: the blow passes through.
        } else if (hit != -1 && NowColData->GetMonsterOwner(hit) == -1 && g_guard > 0) {
            // Blocked on the guard: a clang, and nothing taken.
            g_hurt = 12;
            SndSePlay(SE_HIT_BLOCKED, -1, 0);
        } else if (hit != -1 && NowColData->GetMonsterOwner(hit) == -1) {
            g_drink = 0; // a drink is spilled
            const int dealt = std::max(1, NowColData->Get(hit)->damage - g_defense);
            g_hp = std::max(0, g_hp - dealt);
            g_hurt = 18;
            SndSePlay(SE_MONSTER_HIT, -1, 0);
            float number[4] = {0.0f, 14.0f, 0.0f, 1.0f};
            HitValueEntry(NowHitValue, number, dealt, HIT_VALUE_MONSTER, g_body.frame);
            const bool killed = g_hp == 0;
            moonmod::ChronicleCompanionStruck(g_loaded, dealt, g_hp, g_max_hp, killed);
            if (killed) {
                SetMotion(kMotionDown, true);
                g_dying = 90;
                for (Bolt &bolt : g_bolts) bolt.frames = 0;
                StepModels();
                return;
            }
            // A flinch, which breaks off a swing that hasn't landed yet.
            SetMotion(kMotionHurt, true);
            if (g_cooldown > 30) g_cooldown = 24;
        }
    }

    const float dx = player[0] - me[0], dz = player[2] - me[2];
    const float distance = std::hypot(dx, dz);
    const float heading = distance > 0.01f ? std::atan2(dx, dz) : 0.0f;
    const bool  ranged = Ranged(g_loaded);
    const float keep = ranged ? 55.0f : 12.0f;
    if (g_cooldown > 0) --g_cooldown;

    // A swing of the player's starting close by: weighed once; about two in five it guards, when it's free to.
    const bool swinging = BtActStatus.action_on == 1 && BtActStatus.action_no != 9 && BtActStatus.action_no != 8;
    if (!swinging) {
        g_read = false;
    } else if (!g_read && distance < 32.0f) {
        g_read = true;
        g_read_seed = g_read_seed * 1103515245 + 12345;
        const int roll = ((g_read_seed >> 16) & 0x7fff) % 10;
        if (g_guard == 0 && g_dash == 0 && g_drink == 0 && g_cooldown <= 24 && g_hurt == 0) {
            if (!ranged && roll < 4) {
                g_guard = 40;
                SetMotion(kMotionGuardUp, true);
            } else if (roll < (ranged ? 5 : 7) && distance > 0.01f) {
                // Square to the player, to whichever side the seed picks and is open: 36 units over 12 frames. Walled
                // in on both sides, it stands its ground.
                float side = (g_read_seed & 0x10000) != 0 ? 1.0f : -1.0f;
                for (int tries = 0; tries < 2; ++tries, side = -side) {
                    const float end[3] = {me[0] - dz / distance * side * 40.0f, me[1], me[2] + dx / distance * side * 40.0f};
                    if (Walled(me, end)) continue;
                    g_dash = 12;
                    g_dash_step[0] = -dz / distance * side * 3.0f;
                    g_dash_step[1] = dx / distance * side * 3.0f;
                    SetMotion(kMotionRun, true);
                    SndSePlay(SE_CHARA_ACTION, -1, 0);
                    break;
                }
            }
        }
    }
    // Below a third of its life, between swings, with a flask left: it backs away to drink.
    if (g_flasks > 0 && g_retreat == 0 && g_drink == 0 && g_dash == 0 && g_guard == 0 && g_hurt == 0 && g_cooldown <= 24 &&
        g_hp * 3 < g_max_hp) {
        g_retreat = 50;
    }
    if (g_dash > 0) {
        --g_dash;
        const float next[4] = {me[0] + g_dash_step[0], player[1], me[2] + g_dash_step[1], 1.0f};
        if (Walled(me, next)) {
            g_dash = 0; // a wall the start didn't see: it stops there
        } else {
            Place(next, std::atan2(g_dash_step[0], g_dash_step[1]));
        }
        if (g_dash == 0) g_cooldown = std::min(g_cooldown, 10); // and straight back in with a swing
    } else if (g_guard > 0) {
        Place(me, heading);
        if (--g_guard == 0) {
            SetMotion(kMotionGuardDown, true);
            g_cooldown = std::max(g_cooldown, 12);
        } else if (g_guard == 30) {
            SetMotion(kMotionGuardHold, false);
        }
    } else if (g_drink > 0) {
        // Drinking: it stands still, open to a blow, and heals at the end.
        const float still[4] = {me[0], player[1], me[2], 1.0f};
        Place(still, heading);
        if (--g_drink == 0) {
            const int healed = std::min(g_max_hp - g_hp, std::max(1, g_max_hp * 2 / 5));
            g_hp += healed;
            SndSePlay(SE_EFFECT_CHIME, -1, 0);
            moonmod::ChronicleCompanionStruck(g_loaded, -healed, g_hp, g_max_hp, false);
            g_cooldown = std::max(g_cooldown, 20);
        }
    } else if (g_retreat > 0 && g_hurt <= 8) {
        // Backing off, still facing the player, until it's well clear, walled or out of time; then it drinks.
        const float step = distance > 0.01f ? 2.0f : 0.0f;
        const float next[4] = {me[0] - dx / std::max(distance, 0.01f) * step, player[1], me[2] - dz / std::max(distance, 0.01f) * step, 1.0f};
        const bool  blocked = Walled(me, next);
        if (!blocked) Place(next, heading);
        SetMotion(kMotionStrafeBack, false);
        if (--g_retreat == 0 || blocked || distance > 100.0f) {
            g_retreat = 0;
            --g_flasks;
            g_drink = kDrinkFrames;
            SetMotion(kMotionIdle, true);
            SndSePlay(SE_DRINK, -1, 0);
        }
    } else if (g_cooldown > 24 || g_hurt > 8) {
        // Mid-swing it holds still and keeps its aim, so a dodge to the side beats it; flinching, it waits.
    } else if (distance > keep + 1.0f) {
        const float speed = std::min(distance - keep, distance > 60.0f ? 2.4f : 1.6f);
        const float next[4] = {me[0] + dx / distance * speed, player[1], me[2] + dz / distance * speed, 1.0f};
        Place(next, heading);
        SetMotion(kMotionRun, false);
    } else if (g_cooldown == 0) {
        // A swing, slower to come than a helper's so it can be read; every third is a heavy one, slower still.
        Place(me, heading);
        SetMotion(kAttackMotion[Style(g_loaded)], true);
        g_heavy = g_swings % 3 == 2;
        g_cooldown = g_heavy ? 84 : 64;
        ++g_swings;
        if (g_swings % 2 == 0) g_circle = -g_circle; // and the other way round every other swing
    } else if (distance > 0.01f) {
        // Waiting on its next swing: it circles the player, locked on, holding its distance, with the player's own
        // lock-on strafe motions (picked as movechara picks them, from facing against the way it moves).
        const float pull = std::clamp(distance - keep, -0.6f, 0.6f);
        const float step[2] = {-dz / distance * g_circle * 0.9f + dx / distance * pull, dx / distance * g_circle * 0.9f + dz / distance * pull};
        float next[4] = {me[0] + step[0], player[1], me[2] + step[1], 1.0f};
        if (Walled(me, next)) {
            g_circle = -g_circle;
            next[0] = me[0];
            next[2] = me[2];
        }
        Place(next, heading);
        float turn = heading - std::atan2(step[0], step[1]);
        if (turn < -PI_SHORT) turn += TWO_PI_SHORT;
        if (turn > PI_SHORT) turn -= TWO_PI_SHORT;
        int motion = kMotionLockIdle;
        if (turn > -1.2f && turn < 1.2f) motion = 0x15;
        if (turn < -2.6f || turn > 2.6f) motion = kMotionStrafeBack;
        if (turn > 1.2f && turn < 2.6f) motion = 0x13;
        if (turn < -1.2f && turn > -2.6f) motion = 0x14;
        SetMotion(motion, false);
    } else {
        const float still[4] = {me[0], player[1], me[2], 1.0f};
        Place(still, heading);
        SetMotion(kMotionIdle, false);
    }
    // The blow, partway into the swing: in front of it for a blade, a shot for the ranged.
    if (g_cooldown == (g_heavy ? 58 : 46)) {
        float aim[4];
        g_body.frame->GetRotation(aim);
        const int damage = g_heavy ? g_blow * 3 / 2 : g_blow;
        if (!ranged) {
            const float at[3] = {me[0] + 10.0f * std::sin(aim[1]), player[1] + 6.0f, me[2] + 10.0f * std::cos(aim[1])};
            StrikePlayer(at, damage, g_heavy);
            SndSePlay(SE_CHARA_ACTION, -1, 0);
        } else if (auto free = std::ranges::find(g_bolts, 0, &Bolt::frames); free != g_bolts.end()) {
            const float from[3] = {me[0], player[1] + 9.0f, me[2]};
            const float to[3] = {player[0], player[1] + 6.0f, player[2]};
            const float length = std::max(1.0f, std::hypot(to[0] - from[0], to[1] - from[1], to[2] - from[2]));
            free->frames = std::max(1, static_cast<int>(length / 4.0f));
            for (int axis = 0; axis < 3; ++axis) {
                free->pos[axis] = from[axis];
                free->step[axis] = (to[axis] - from[axis]) / static_cast<float>(free->frames);
            }
            free->pos[3] = 1.0f;
            free->step[3] = 0.0f;
            free->target = kTargetPlayer;
            free->damage = damage;
            free->cell = g_shot_cell >= 0 ? g_shot_cell : kBoltCell[Style(g_loaded)];
            free->size = g_shot_cell >= 0 ? g_shot_size : kBoltSize[Style(g_loaded)];
            SndSePlay(SE_CHARA_ACTION, -1, 0);
        }
    }
    // Its shots land where they arrive: dodged, they're spent on the floor.
    for (Bolt &bolt : g_bolts) {
        if (bolt.frames <= 0) continue;
        for (int axis = 0; axis < 3; ++axis) bolt.pos[axis] += bolt.step[axis];
        if (--bolt.frames == 0 && bolt.target == kTargetPlayer) StrikePlayer(bolt.pos, bolt.damage, false);
    }
    StepModels();
}

} // namespace

void MoonModCompanionHostile(int hp, int damage, int defense, const float *tint) {
    g_hostile = hp > 0;
    g_hp = g_max_hp = std::max(0, hp);
    g_blow = std::max(1, damage);
    g_defense = std::max(0, defense);
    g_hurt = g_dying = g_swings = g_guard = g_dash = g_retreat = g_drink = 0;
    g_flasks = kInvaderFlasks;
    g_heavy = g_read = false;
    for (int axis = 0; axis < 3; ++axis) g_tint[axis] = tint != nullptr ? tint[axis] : -1.0f;
}

MoonModCompanionLife MoonModCompanionLifeGet() { return {g_hostile && g_loaded >= 0, g_hp, g_max_hp}; }

bool MoonModCompanionInvaderAt(float *out) {
    if (!g_hostile || g_loaded < 0 || !g_placed || g_dying > 0 || g_hp <= 0) return false;
    Position(out);
    return true;
}

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
    // MoonMod: an invader can be a dark copy of the hero the player is.
    const int wanted = g_wanted == UserStatus->cur_chara && !g_hostile ? -1 : g_wanted;
    if (wanted != g_loaded || (wanted >= 0 && g_wanted_weapon != g_loaded_weapon && g_wanted_weapon >= ITEM_WEAPON_START)) {
        if (wanted < 0) Unload();
        else if (!Load(wanted, g_wanted_weapon)) { Unload(); g_wanted = -1; }
    }
    if (g_loaded < 0) return;

    float player[4], me[4], facing[4];
    CharaMain.GetPosition(player);
    CharaMain.frame->GetRotation(facing);
    if (!g_placed && g_hostile) {
        // An invader comes in ahead of the player, in the room they walked into, facing them.
        const float ahead[4] = {player[0] + 70.0f * std::sin(facing[1]), player[1], player[2] + 70.0f * std::cos(facing[1]), 1.0f};
        Place(ahead, facing[1] + 3.14159265f);
        TrailClear();
        g_placed = true;
    }
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
    if (g_hostile) {
        HostileStep(player, me);
        return;
    }
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
    // MoonMod: an invader draws in its own colour, and white for a moment when it's hit.
    float      saved[4];
    const bool tinted = g_hostile && (g_tint[0] >= 0.0f || g_hurt > 12);
    if (tinted) {
        MGGetAmbient(saved);
        float ambient[4] = {g_tint[0], g_tint[1], g_tint[2], saved[3]};
        if (g_hurt > 12) ambient[0] = ambient[1] = ambient[2] = 128.0f;
        MGSetAmbient(ambient);
    }
    TexManager.ReloadTexture(Vif1Packet, kCompanionTextureBlock);
    g_body.TextureAnime(kCompanionTextureBlock);
    g_body.Draw();
    if (g_loaded_weapon >= 0 && g_weapon.frame != nullptr) {
        TexManager.ReloadTexture(Vif1Packet, kCompanionWeaponTextureBlock);
        g_weapon.Draw();
    }
    if (tinted) MGSetAmbient(saved);
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

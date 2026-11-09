// Co-op's monsters (moonmod/coop_monsters.hpp): on the host they see every player; on a guest they mirror the host's.

#include "moonmod/coop_monsters.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <numbers>
#include <vector>

#include "dun/gameloop.hpp"
#include "gamemode.hpp"
#include "main.hpp"
#include "monstorunit.hpp"
#include "collisiondata.hpp"
#include "moonmod/coop_avatars.hpp"
#include "moonmod/game_hooks.hpp"

namespace dc::game {

namespace {

struct Puppet {
    bool          used = false;
    MoonModPuppet host;
    float         at[3] = {};
    float         heading = 0.0f;
    bool          placed = false;
    int           motion = -1;
    // Leading the host: its last report's place and step, and how fast it was going between reports (units a frame,
    // on the ground), so the copy moves on from the report rather than trailing it.
    float         vel[2] = {};
    int           report_step = 0;
    // This player's blows not yet in the host's reports: taken off the copy's HP at once, until a report shows them
    // (the host's HP dropping) or a moment has passed (the host took less, or none).
    int           pending = 0;
    int           pending_until = 0;
};
std::array<Puppet, MONSTOR_SLOT_MAX> g_puppets;
int g_step = 0; // the guest's frames, counted by MoonModCoopPuppetsStep

constexpr int   kLeadFrames = 12;       // how far past the last report a copy is led: a fifth of a second at most
constexpr int   kPendingFrames = 45;    // how long a blow waits for the host's report to show it
constexpr float kTeleport = 160.0f;     // a jump this far is placed, not eased (a cell)

constexpr float kAvatarHeight = 16.0f; // a hero's height for a hit, as the player's (CharaHeight)

bool InDungeon() { return mode == GAME_MODE_DUNGEON && NowMonstorUnit != nullptr; }

float Dist3(const float *a, const float *b) {
    const float dx = a[0] - b[0], dy = a[1] - b[1], dz = a[2] - b[2];
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

// The real player's place, kept while a monster's turn has it look at another player.
float g_saved[4] = {0.0f, 0.0f, 0.0f, 1.0f};
bool  g_swapped = false;
std::vector<MoonModAvatarHit> g_hits;
std::array<int, kMoonModAvatars> g_hit_cooldown{}; // frames before an avatar can be hit again (as the game's own grace)

// The avatars on this floor: where each stands.
// Down, not out: the rule, whether this player is down, and what the mod asked for.
bool  g_down_rule = false;
bool  g_player_down = false;
bool  g_revive = false, g_revive_has_at = false, g_give_up = false;
int   g_revive_hp = 1;
float g_revive_at[3] = {};

int Avatars(std::array<std::array<float, 3>, kMoonModAvatars> &at, std::array<bool, kMoonModAvatars> &here) {
    int count = 0;
    for (int i = 0; i < kMoonModAvatars; ++i) {
        here[i] = MoonModAvatarAt(i, at[i].data());
        count += here[i] ? 1 : 0;
    }
    return count;
}

} // namespace

// ---- Guest: puppets ------------------------------------------------------------------------------------------------

bool MoonModCoopPuppetSet(int slot, const MoonModPuppet &puppet) {
    if (slot < 0 || slot >= MONSTOR_SLOT_MAX || !InDungeon()) return false;
    MONSTOR &local = NowMonstorUnit->monster[slot];
    Puppet  &p = g_puppets[slot];
    if (puppet.state < 0 || puppet.hp <= 0) {
        // Gone on the host: a copy still up here dies, its own death and drops playing out.
        if (local.state != -1 && local.hp > 0) {
            local.hp = 0;
            local.state = 2; // stepped this frame, so the death is taken
            p.used = false;
            moonmod::QueueChronicleMonsterDamageResult(NowMonstorUnit, slot, static_cast<int>(MonsterDamageResult::Killed));
        }
        p.used = false;
        return true;
    }
    if (local.state == -1 || local.kind != puppet.kind) {
        p.used = false;
        return false;
    }
    if (!p.used) {
        p.placed = false;
        p.motion = -1;
        p.vel[0] = p.vel[1] = 0.0f;
        p.pending = 0;
    } else {
        // how fast the host's monster went since its last report, eased with the speed before
        const int   dt = std::clamp(g_step - p.report_step, 1, 30);
        const float dx = puppet.x - p.host.x, dz = puppet.z - p.host.z;
        if (puppet.state != 2 || std::hypot(dx, dz) > kTeleport) {
            p.vel[0] = p.vel[1] = 0.0f;
        } else {
            p.vel[0] = p.vel[0] * 0.5f + dx / static_cast<float>(dt) * 0.5f;
            p.vel[1] = p.vel[1] * 0.5f + dz / static_cast<float>(dt) * 0.5f;
        }
        // the host took this player's blows: they're in its HP now
        if (puppet.hp < p.host.hp) p.pending = std::max(0, p.pending - (p.host.hp - puppet.hp));
    }
    p.report_step = g_step;
    p.used = true;
    p.host = puppet;
    return true;
}

namespace {
std::vector<MoonModPuppetHit> g_puppet_hits;
} // namespace

void MoonModCoopPuppetHit(int slot, int damage) {
    if (damage > 0 && g_puppet_hits.size() < 32) g_puppet_hits.push_back({slot, damage});
    // shown on the copy at once, before the host's report has it
    if (damage > 0 && slot >= 0 && slot < MONSTOR_SLOT_MAX && g_puppets[slot].used) {
        g_puppets[slot].pending += damage;
        g_puppets[slot].pending_until = g_step + kPendingFrames;
    }
}

std::vector<MoonModPuppetHit> MoonModCoopTakePuppetHits() {
    std::vector<MoonModPuppetHit> out;
    out.swap(g_puppet_hits);
    return out;
}

void MoonModCoopPuppetsClear() {
    g_puppet_hits.clear();
    for (Puppet &p : g_puppets) p = Puppet{};
}

bool MoonModCoopIsPuppet(int slot) {
    if (slot < 0 || slot >= MONSTOR_SLOT_MAX || !g_puppets[slot].used || !InDungeon()) return false;
    // A dying copy is its own again: its death program runs as the game's.
    const MONSTOR &local = NowMonstorUnit->monster[slot];
    return local.hp > 0 && local.state != -1;
}

void MoonModCoopPuppetsStep() {
    ++g_step;
    if (!InDungeon()) {
        if (mode != GAME_MODE_DUNGEON) MoonModCoopPuppetsClear();
        return;
    }
    for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) {
        Puppet &p = g_puppets[slot];
        if (!MoonModCoopIsPuppet(slot)) continue;
        MONSTOR    &m = NowMonstorUnit->monster[slot];
        CCharacter &body = NowMonstorUnit->chara[slot][0];
        // Asleep or up as on the host, its life the host's.
        m.state = p.host.state == 1 ? 1 : 2;
        if (m.revealed == 0) m.revealed = -1; // shown on the host, so shown here (its entrance is the host's)
        if (p.host.max_hp > 0) m.max_hp = p.host.max_hp;
        if (p.pending > 0 && g_step > p.pending_until) p.pending = 0;
        m.hp = std::clamp(p.host.hp - p.pending, 1, std::max(1, m.max_hp)); // its death is the host's to say
        // where the host's monster is by now: its last report, led on at the speed it was going
        const float lead = static_cast<float>(std::min(g_step - p.report_step, kLeadFrames));
        const float to[3] = {p.host.x + p.vel[0] * lead, p.host.y, p.host.z + p.vel[1] * lead};
        if (!p.placed || std::hypot(to[0] - p.at[0], to[2] - p.at[2]) > kTeleport) {
            std::copy(to, to + 3, p.at);
            p.heading = p.host.heading;
            p.placed = true;
        } else {
            for (int axis = 0; axis < 3; ++axis) p.at[axis] += (to[axis] - p.at[axis]) * 0.25f;
            constexpr float kTau = 2.0f * std::numbers::pi_v<float>;
            float           d = std::fmod(p.host.heading - p.heading, kTau);
            if (d > std::numbers::pi_v<float>) d -= kTau;
            if (d < -std::numbers::pi_v<float>) d += kTau;
            p.heading += d * 0.3f;
        }
        float at[4] = {p.at[0], p.at[1], p.at[2], 1.0f};
        body.SetPosition(at);
        body.SetRotation(0.0f, p.heading, 0.0f);
        // Its blows are the host's to land: none from the copy.
        for (int &active : NowMonstorUnit->effect2[slot].active) active = 0;
        if (p.host.motion >= 0 && p.host.motion != p.motion) {
            body.SetMotion(p.host.motion, 2);
            p.motion = p.host.motion;
        }
    }
}

// ---- Host: every player is seen -----------------------------------------------------------------------------------

float MoonModCoopNearestDistance(const float *player, const float *monster) {
    float best = g_player_down ? 1.0e9f : Dist3(player, monster); // a player who is down is passed by
    std::array<std::array<float, 3>, kMoonModAvatars> at{};
    std::array<bool, kMoonModAvatars>                 here{};
    if (Avatars(at, here) == 0) return best;
    for (int i = 0; i < kMoonModAvatars; ++i) {
        if (here[i]) best = std::min(best, Dist3(at[i].data(), monster));
    }
    return best;
}

void MoonModCoopMonstersStepBegin() {
    std::copy(CharaMain.pos, CharaMain.pos + 4, g_saved);
    g_swapped = false;
}

void MoonModCoopMonsterTarget(int slot) {
    if (NowMonstorUnit == nullptr || slot < 0 || slot >= MONSTOR_SLOT_MAX) return;
    std::array<std::array<float, 3>, kMoonModAvatars> at{};
    std::array<bool, kMoonModAvatars>                 here{};
    if (Avatars(at, here) == 0) return;
    float monster[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    NowMonstorUnit->chara[slot][0].GetPosition(monster);
    const float *best = g_saved;
    float        best_d = g_player_down ? 1.0e9f : Dist3(g_saved, monster); // a player who is down is passed by
    for (int i = 0; i < kMoonModAvatars; ++i) {
        if (!here[i]) continue;
        const float d = Dist3(at[i].data(), monster);
        if (d < best_d) {
            best_d = d;
            best = at[i].data();
        }
    }
    // For this monster's turn the player stands at the nearest player.
    CharaMain.pos[0] = best[0];
    CharaMain.pos[1] = best[1];
    CharaMain.pos[2] = best[2];
    g_swapped = true;
}

void MoonModCoopMonstersStepEnd() {
    if (g_swapped) std::copy(g_saved, g_saved + 4, CharaMain.pos);
    g_swapped = false;
}

void MoonModCoopAvatarHitsScan() {
    if (!InDungeon() || NowColData == nullptr) return;
    std::array<std::array<float, 3>, kMoonModAvatars> at{};
    std::array<bool, kMoonModAvatars>                 here{};
    if (Avatars(at, here) == 0) return;
    // Whatever would strike the player (a monster's blow, its shot or spell, a trap), measured on each hero as the
    // game measures it on the player (CCollisionData::CheckHitUser).
    for (int a = 0; a < kMoonModAvatars; ++a) {
        if (g_hit_cooldown[a] > 0) {
            --g_hit_cooldown[a];
            continue;
        }
        if (!here[a] || g_hits.size() >= 8) continue;
        float feet[4] = {at[a][0], at[a][1], at[a][2], 1.0f};
        const int i = NowColData->CheckHitUser(feet, 1, kAvatarHeight);
        if (i < 0) continue;
        const COLLISION_HIT &hit = NowColData->hit[i];
        int monster = hit.monster_no;
        if (monster < 0 && hit.owner >= 200) monster = (hit.owner - 200) / 5;
        const int damage = moonmod::ChronicleMonsterHitDamage(hit.damage, hit.owner, hit.monster_no);
        g_hits.push_back({a + 1, monster, damage, hit.kind, hit.flags});
        g_hit_cooldown[a] = 60; // about the game's own grace after a hit
    }
}

void MoonModCoopSetDownRule(bool on) { g_down_rule = on; }
bool MoonModCoopDownRule() { return g_down_rule; }
void MoonModCoopSetPlayerDown(bool down) {
    g_player_down = down;
    g_revive = g_give_up = false;
}
bool MoonModCoopPlayerDown() {
    if (g_player_down && mode != GAME_MODE_DUNGEON) MoonModCoopSetPlayerDown(false); // left the dungeon meanwhile
    return g_player_down;
}
bool MoonModCoopRequestRevive(int hp, const float *at) {
    if (!MoonModCoopPlayerDown()) return false;
    g_revive = true;
    g_revive_hp = hp;
    g_revive_has_at = at != nullptr;
    if (at != nullptr) std::copy(at, at + 3, g_revive_at);
    return true;
}
bool MoonModCoopRequestGiveUp() {
    if (!MoonModCoopPlayerDown()) return false;
    g_give_up = true;
    return true;
}
bool MoonModCoopTakeRevive(int &hp, float at[3], bool &has_at) {
    if (!g_revive) return false;
    g_revive = false;
    hp = g_revive_hp;
    has_at = g_revive_has_at;
    std::copy(g_revive_at, g_revive_at + 3, at);
    return true;
}
bool MoonModCoopTakeGiveUp() {
    if (!g_give_up) return false;
    g_give_up = false;
    return true;
}

std::vector<MoonModAvatarHit> MoonModCoopTakeAvatarHits() {
    std::vector<MoonModAvatarHit> out;
    out.swap(g_hits);
    return out;
}

} // namespace dc::game

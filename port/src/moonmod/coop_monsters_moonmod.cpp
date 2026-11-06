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
};
std::array<Puppet, MONSTOR_SLOT_MAX> g_puppets;

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
    }
    p.used = true;
    p.host = puppet;
    return true;
}

void MoonModCoopPuppetsClear() {
    for (Puppet &p : g_puppets) p = Puppet{};
}

bool MoonModCoopIsPuppet(int slot) {
    if (slot < 0 || slot >= MONSTOR_SLOT_MAX || !g_puppets[slot].used || !InDungeon()) return false;
    // A dying copy is its own again: its death program runs as the game's.
    const MONSTOR &local = NowMonstorUnit->monster[slot];
    return local.hp > 0 && local.state != -1;
}

void MoonModCoopPuppetsStep() {
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
        m.hp = std::clamp(p.host.hp, 1, std::max(1, m.max_hp));
        const float to[3] = {p.host.x, p.host.y, p.host.z};
        if (!p.placed || std::hypot(to[0] - p.at[0], to[2] - p.at[2]) > 480.0f) {
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
    float best = Dist3(player, monster);
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
    float        best_d = Dist3(g_saved, monster);
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
    if (!InDungeon()) return;
    std::array<std::array<float, 3>, kMoonModAvatars> at{};
    std::array<bool, kMoonModAvatars>                 here{};
    if (Avatars(at, here) == 0) return;
    // A blow whose sphere overlaps an avatar, as the game measures one on the player.
    for (int a = 0; a < kMoonModAvatars; ++a) {
        if (g_hit_cooldown[a] > 0) {
            --g_hit_cooldown[a];
            continue;
        }
        if (!here[a] || g_hits.size() >= 8) continue;
        bool hit = false;
        for (int i = 0; i < MONSTOR_SLOT_MAX && !hit; ++i) {
            const MONSTOR &m = NowMonstorUnit->monster[i];
            if (m.state != 2 || m.hp <= 0 || m.stop_timer > 0) continue;
            const MONSTOR_EFFECT_STATE2 &e = NowMonstorUnit->effect2[i];
            const float time = NowMonstorUnit->chara[i][0].motion_type.state.time;
            for (int k = 0; k < 16; ++k) {
                if (e.active[k] == 0 || !(e.motion_start[k] < time) || e.motion_end[k] <= time) continue;
                const float dx = e.position[k][0] - at[a][0], dz = e.position[k][2] - at[a][2];
                const float dy = e.position[k][1] - at[a][1];
                const float reach = e.radius[k] + 14.0f;
                if (dx * dx + dz * dz <= reach * reach && dy > -e.radius[k] - 10.0f && dy < 55.0f + e.radius[k]) {
                    g_hits.push_back({a + 1, i, e.damage[k] * (m.anger_timer > 0 ? 2 : 1), e.kind[k], e.flags[k]});
                    g_hit_cooldown[a] = 60; // about the game's own grace after a hit
                    hit = true;
                    break;
                }
            }
        }
    }
}

std::vector<MoonModAvatarHit> MoonModCoopTakeAvatarHits() {
    std::vector<MoonModAvatarHit> out;
    out.swap(g_hits);
    return out;
}

} // namespace dc::game

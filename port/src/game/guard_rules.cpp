#include "guard_rules.hpp"

#include <algorithm>
#include <cmath>

namespace dc::game {

float GuardAngle(float yaw, float px, float pz, float ax, float az) {
    const float dx = ax - px;
    const float dz = az - pz;
    const float length = std::sqrt(dx * dx + dz * dz);
    if (length < 0.001f) {
        return 0.0f;
    }

    const float along = (std::sin(yaw) * dx + std::cos(yaw) * dz) / length;
    return std::acos(std::clamp(along, -1.0f, 1.0f)) * 180.0f / 3.14159265f;
}

GuardOutcome ResolveGuard(const GuardRules &rules, const GuardHit &hit) {
    GuardOutcome out;
    out.damage = hit.damage;

    // The game's guard: a heavy hit only while held, anything else in any phase, from every side, for nothing.
    if (!rules.on) {
        out.blocked = hit.heavy ? hit.held : hit.guarding;
        out.damage = out.blocked ? 0 : hit.damage;
        return out;
    }

    if (!hit.guarding && !hit.held) {
        return out;
    }

    if (hit.angle > rules.arc / 2.0f) {
        return out;
    }

    if (rules.parry_frames > 0 && hit.guard_frames < rules.parry_frames) {
        out.blocked = true;
        out.parried = true;
        out.damage = 0;
        return out;
    }

    if (hit.heavy && !hit.held) {
        return out;
    }

    const float cost = rules.cost_base + rules.cost_per_damage * static_cast<float>(hit.damage);
    if (cost > 0.0f && cost > hit.gauge) {
        out.broke = true;
        out.cost = std::max(hit.gauge, 0.0f);
        return out;
    }

    out.blocked = true;
    out.cost = cost;
    out.damage = static_cast<int>(std::lround(static_cast<float>(hit.damage) * std::clamp(rules.chip, 0.0f, 1.0f)));
    return out;
}

bool PoiseTakes(PoiseState &state, int poise, bool heavy, int now) {
    if (now - state.last >= POISE_RESET_FRAMES) state.spent = 0;
    state.last = now;
    const int cost = heavy ? 2 : 1;
    if (poise <= 0 || state.spent + cost > poise) {
        state.spent = 0;
        return false;
    }
    state.spent += cost;
    return true;
}

} // namespace dc::game

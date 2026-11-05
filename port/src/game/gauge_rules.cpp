#include "gauge_rules.hpp"

#include <algorithm>

#include "itemdata.hpp"

namespace dc::game {

float GaugeRefillGain(const GaugeRefill &in) {
    const bool own = in.stamina > 0.0f;
    float      gain = own ? in.stamina / 60.0f
                          : static_cast<float>(in.lightness ? LIGHTNESS_SPEED : in.weapon_speed) / 30.0f;

    if (in.goo) {
        gain /= 2.0f;
    }

    if (in.stamina_ailment) {
        gain *= 2.0f;
    }

    if (!own) {
        gain = std::max(gain, 1.0f);
    }

    // Toan and Ungaga recharge slowly, and not at all on a frame they swing.
    if (in.chara == CHARA_TOAN || in.chara == CHARA_UNGAGA) {
        if (in.swinging) {
            gain = 0.0f;
        } else if (!own) {
            gain *= 0.25f;
        }
    }

    if (in.chara == CHARA_OSMOND) {
        if (in.gun_type != 1) {
            gain = in.exhausted ? gain * 0.5f : 0.0f;
        } else {
            gain *= 1.5f;
        }
    }

    if ((in.chara == CHARA_XIAO || in.chara == CHARA_RUBY) && !own) {
        gain *= 1.5f;
    }

    // Gear's stamina comes on top, but not on a frame the refill is stopped (a swing, Osmond's empty gun).
    if (gain > 0.0f && in.gear_stamina > 0.0f) {
        gain += in.gear_stamina / 60.0f * (in.goo ? 0.5f : 1.0f);
    }

    return gain;
}

float LightnessSwingScale(int speed) {
    if (speed <= 0) {
        return 2.0f;
    }
    return std::clamp(static_cast<float>(LIGHTNESS_SPEED) / static_cast<float>(speed), 0.5f, 2.0f);
}

float GaugeAfterHit(const GaugeRules &rules, float before, const GuardOutcome &guard, bool guard_rules) {
    if (guard.broke) {
        return 0.0f;
    }
    if (guard.blocked) {
        if (guard_rules) {
            return std::clamp(before - guard.cost, 0.0f, 100.0f);
        }
        return rules.on ? before : 100.0f;
    }
    return rules.on ? std::clamp(before + rules.hit_refill, 0.0f, 100.0f) : 100.0f;
}

bool GaugeDelayStep(GaugeDelay &delay, float gauge, int refill_delay) {
    if (gauge < delay.last - 0.001f) {
        delay.frames = 0;
    } else if (delay.frames < (1 << 20)) {
        delay.frames++;
    }
    delay.last = gauge;
    return delay.frames >= refill_delay;
}

void GaugeDelaySeen(GaugeDelay &delay, float gauge) {
    delay.last = gauge;
}

} // namespace dc::game

#pragma once

/**
 * @file
 * The action gauge's refill and what a swing costs from it, as the game has them and as a mod can change them
 * (player.tuning_set `stamina` and `lightness`). With neither set, GaugeRefillGain is the game's own refill.
 *
 * - Stamina: each character's own refill, in gauge a second. It replaces the weapon's Speed / 30 a tick and the
 *   character's factor (Toan and Ungaga a quarter, Xiao and Ruby half again); a frame Toan or Ungaga swings still
 *   refills nothing, and Osmond's gun rules stay.
 * - Lightness: the weapon's Speed no longer sets the refill (every weapon refills as one of Speed 60 would) and sets
 *   what a swing costs instead: LIGHTNESS_SPEED / Speed times the game's cost, from half to double.
 */

#include "guard_rules.hpp"

namespace dc::game {

/** The Speed a weapon costs its swings' own gauge at, and the one every weapon refills as with lightness on. */
constexpr int LIGHTNESS_SPEED = 60;

/** One tick's refill inputs. */
struct GaugeRefill {
    int   chara = 0;               /**< The combat character (CombatChara): Toan, Xiao, Goro, Ruby, Ungaga, Osmond. */
    int   weapon_speed = 0;        /**< The equipped weapon's Speed. */
    float stamina = 0.0f;          /**< The character's refill in gauge a second; 0 keeps the game's. */
    bool  lightness = false;       /**< Speed sets the swing cost, not the refill. */
    bool  goo = false;             /**< The Goo ailment: half. */
    bool  stamina_ailment = false; /**< The Stamina ailment (a power-up): double. */
    bool  swinging = false;        /**< Toan and Ungaga refill nothing on a frame they swing. */
    int   gun_type = 0;            /**< Osmond's gun. */
    bool  exhausted = false;       /**< Osmond's gauge ran out (BtActStatus.gauge_exhausted). */
    float gear_stamina = 0.0f;     /**< Gauge a second the worn gear adds (gear.hpp), unless the refill is stopped. */
};

/** The gauge one tick adds, before the mod's gauge_refill scale. */
float GaugeRefillGain(const GaugeRefill &in);

/** What lightness makes a swing cost of a weapon of Speed speed, as a scale of the game's cost (0.5 to 2). */
float LightnessSwingScale(int speed);

/**
 * A mod's rules for spending the gauge (player.gauge_rules_set). With none set the game's hold: any hit fills the
 * gauge to 100, the first swing of a combo is free, and the refill never waits.
 */
struct GaugeRules {
    bool  on = false;              /**< Rules are set. */
    float hit_refill = 0.0f;       /**< What a hit that lands adds to the gauge (-100 to 100); no longer fills it. */
    float first_swing_cost = 0.0f; /**< What the first swing of a combo costs. */
    float min_to_swing = 0.0f;     /**< Gauge the first swing needs; below it the attack button does nothing. */
    float swing_cost_scale = 1.0f; /**< Scales what the combo's later swings cost. */
    int   refill_delay = 0;        /**< Frames the refill waits after the gauge last went down. */
};

/**
 * The gauge after a hit, from the gauge before it. A broken guard empties it; a block under guard rules takes the
 * block's cost, and any other block leaves it be under gauge rules. A hit that lands adds hit_refill under gauge
 * rules. Anything else fills it to 100, as the game does.
 */
float GaugeAfterHit(const GaugeRules &rules, float before, const GuardOutcome &guard, bool guard_rules);

/** Watches the gauge for refill_delay: frames since it last went down. */
struct GaugeDelay {
    float last = 100.0f;
    int   frames = 1 << 20;
};

/** One tick, before the refill: true when the refill may run. Call GaugeDelaySeen with the gauge after it. */
bool GaugeDelayStep(GaugeDelay &delay, float gauge, int refill_delay);
void GaugeDelaySeen(GaugeDelay &delay, float gauge);

} // namespace dc::game

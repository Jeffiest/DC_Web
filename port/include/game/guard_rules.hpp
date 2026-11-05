#pragma once

/**
 * @file
 * What the player's guard does to a hit, as the game has it and as a mod can change it (player.guard_rules_set).
 * With no rules set, ResolveGuard is the game's own guard: a light hit or a trap is blocked in any phase of the guard
 * (raising, held, lowering), a heavy hit only while it is held, from every side, for nothing.
 *
 * With rules set:
 * - Arc: only a hit from within arc degrees of where the player faces is blocked; one from the side or behind lands.
 * - Parry: a hit within parry_frames of R1 is parried, heavy ones too, even while the guard is still rising. It costs
 *   nothing and takes nothing.
 * - Cost: a blocked hit costs cost_base + cost_per_damage x its damage from the action gauge, and chip x its damage
 *   still gets through.
 * - Break: a hit that would cost more than the gauge holds breaks the guard. It lands in full, the gauge empties, and
 *   the player is knocked down for break_recoil frames (the game's heavy recoil when 0).
 */

namespace dc::game {

/** A mod's guard rules (player.guard_rules_set). */
struct GuardRules {
    bool  on = false;            /**< Rules are set; false is the game's guard. */
    float arc = 360.0f;          /**< Degrees the guard covers, centred on the player's facing. */
    float cost_base = 0.0f;      /**< Gauge a blocked hit costs. */
    float cost_per_damage = 0.0f; /**< Gauge a blocked hit costs for each point of its damage. */
    float chip = 0.0f;           /**< Share of a blocked hit's damage that gets through (0 to 1). */
    int   break_recoil = 0;      /**< Recoil frames a guard break leaves (0: the game's heavy recoil, 160). */
    int   parry_frames = 0;      /**< Frames from R1 in which a hit is parried (0: no parry). */
    bool  without_lock = false;  /**< R1 guards without a lock-on target. */
};

/** One hit on the player, as the guard sees it. */
struct GuardHit {
    bool  guarding = false;  /**< The guard is up in any phase (action_on 6). */
    bool  held = false;      /**< The guard is held (action 9), not rising or lowering. The game reads action 9 alone
                                  for a heavy hit, even on the frame a block has just set action_on back to 0. */
    bool  heavy = false;     /**< A heavy hit (kind 3), which the game blocks only while the guard is held. */
    int   damage = 0;        /**< The hit's damage after the player's defence. */
    float angle = 0.0f;      /**< Degrees between the player's facing and the attacker (0 to 180). */
    float gauge = 100.0f;    /**< The action gauge before the hit. */
    int   guard_frames = 0;  /**< Frames since the guard went up. */
};

/** What the guard made of a hit. */
struct GuardOutcome {
    bool  blocked = false; /**< The guard stopped it (parried hits too). */
    bool  parried = false; /**< It came inside the parry window. */
    bool  broke = false;   /**< It broke the guard and landed. */
    int   damage = 0;      /**< What gets through to the player. */
    float cost = 0.0f;     /**< Gauge the block takes (all of it on a break). */
};

/**
 * Degrees between where the player faces (yaw, radians, the model's rotation: forward is (sin, cos) in x and z) and
 * the attacker at (ax, az), seen from the player at (px, pz). 0 is dead ahead, 180 behind; 0 when they stand together.
 */
float GuardAngle(float yaw, float px, float pz, float ax, float az);

/** What the guard makes of one hit. */
GuardOutcome ResolveGuard(const GuardRules &rules, const GuardHit &hit);

/** How much of the player's poise the hits since the last pause have spent, and when the last one landed. */
struct PoiseState {
    int spent = 0;
    int last = 0;
};

/** Frames without a hit after which the player's poise is whole again. */
constexpr int POISE_RESET_FRAMES = 120;

/**
 * Whether the player's poise (worn gear's points, gear.hpp) takes a landed hit at frame now without a stagger: a light
 * hit costs 1, a heavy one (kind 3) 2. Poise is whole again POISE_RESET_FRAMES after the last hit; a hit it can't take
 * staggers and leaves it whole for the next. A lethal hit or a broken guard never counts (the caller asks only for the
 * rest).
 */
bool PoiseTakes(PoiseState &state, int poise, bool heavy, int now);

} // namespace dc::game

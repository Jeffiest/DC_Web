#pragma once

/**
 * @file
 * MoonMod: running on the dodge button (player.tuning_set dodge_run), as in Souls: a tap of L2 dodges when it is let
 * go, and L2 held past DODGE_RUN_HOLD_TICKS runs instead, at the dash speed Dran's Feather gave, draining the action
 * gauge while the stick is pushed. An empty gauge drops the run back to a walk until L2 is let go. The gauge does not
 * refill while running. With the setting at 0 (retail, and every mod that leaves it alone) L2 dodges on the press, as
 * before.
 */

namespace dc::game {

/** Game ticks (60 a second) L2 is held before it runs rather than dodging on release. */
constexpr int DODGE_RUN_HOLD_TICKS = 12;

/** The run's state from tick to tick. */
struct DodgeRun {
    int  held = 0;        /**< Ticks L2 has been held, 0 while it is up. */
    bool running = false; /**< Running this tick. */
    bool spent = false;   /**< The gauge ran out on this hold: no running until L2 is let go. */
};

/**
 * One tick of the run. held is L2 now; can_run whether the player could run now (stick pushed, free to move); gauge is
 * the action gauge (0 to 100), lowered by drain while running. Returns true on the tick a tap's dodge should start.
 */
bool DodgeRunStep(DodgeRun &state, bool held, bool can_run, float &gauge, float drain);

/** The run's state in the dungeon, which the walk reads for its speed and the gauge for its refill. */
extern DodgeRun PlayerDodgeRun;

} // namespace dc::game

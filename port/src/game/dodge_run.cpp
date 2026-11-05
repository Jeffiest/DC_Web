#include "dodge_run.hpp"

namespace dc::game {

DodgeRun PlayerDodgeRun;

bool DodgeRunStep(DodgeRun &state, bool held, bool can_run, float &gauge, float drain) {
    if (!held) {
        const bool tap = state.held > 0 && state.held < DODGE_RUN_HOLD_TICKS;
        state = DodgeRun{};
        return tap;
    }
    if (state.held < DODGE_RUN_HOLD_TICKS) {
        state.held++;
    }
    state.running = state.held >= DODGE_RUN_HOLD_TICKS && !state.spent && can_run && gauge > 0.0f;
    if (state.running) {
        gauge -= drain;
        if (gauge <= 0.0f) {
            gauge = 0.0f;
            state.running = false;
            state.spent = true;
        }
    }
    return false;
}

} // namespace dc::game

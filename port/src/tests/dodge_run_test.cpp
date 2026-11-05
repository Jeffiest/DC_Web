#include <gtest/gtest.h>

#include "dodge_run.hpp"

namespace {

using namespace dc::game;

// Holds L2 for ticks ticks, stick pushed, and returns how many of them dodged.
int Hold(DodgeRun &run, int ticks, float &gauge, float drain, bool can_run = true) {
    int dodges = 0;
    for (int i = 0; i < ticks; i++) {
        dodges += DodgeRunStep(run, true, can_run, gauge, drain) ? 1 : 0;
    }
    return dodges;
}

TEST(DodgeRun, ATapDodgesAsItIsLetGo) {
    DodgeRun run;
    float    gauge = 100.0f;
    EXPECT_EQ(Hold(run, 3, gauge, 1.0f), 0);
    EXPECT_FALSE(run.running);
    EXPECT_TRUE(DodgeRunStep(run, false, true, gauge, 1.0f));
    EXPECT_EQ(gauge, 100.0f);
    EXPECT_FALSE(DodgeRunStep(run, false, true, gauge, 1.0f)); // once
}

TEST(DodgeRun, HoldingRunsDrainingTheGaugeAndDoesNotDodge) {
    DodgeRun run;
    float    gauge = 100.0f;
    EXPECT_EQ(Hold(run, DODGE_RUN_HOLD_TICKS - 1, gauge, 1.0f), 0);
    EXPECT_FALSE(run.running);
    Hold(run, 10, gauge, 1.0f);
    EXPECT_TRUE(run.running);
    EXPECT_FLOAT_EQ(gauge, 90.0f);
    EXPECT_FALSE(DodgeRunStep(run, false, true, gauge, 1.0f));
    EXPECT_FALSE(run.running);
}

TEST(DodgeRun, StandingStillWithItHeldNeitherRunsNorDrains) {
    DodgeRun run;
    float    gauge = 50.0f;
    Hold(run, 30, gauge, 1.0f, false);
    EXPECT_FALSE(run.running);
    EXPECT_EQ(gauge, 50.0f);
    Hold(run, 1, gauge, 1.0f);
    EXPECT_TRUE(run.running);
}

TEST(DodgeRun, AnEmptyGaugeWalksUntilL2IsLetGo) {
    DodgeRun run;
    float    gauge = 5.0f;
    Hold(run, DODGE_RUN_HOLD_TICKS + 4, gauge, 2.0f);
    EXPECT_EQ(gauge, 0.0f);
    EXPECT_FALSE(run.running);
    EXPECT_TRUE(run.spent);
    gauge = 40.0f; // refilled meanwhile: still no run on this hold
    Hold(run, 5, gauge, 2.0f);
    EXPECT_FALSE(run.running);
    EXPECT_EQ(gauge, 40.0f);
    EXPECT_FALSE(DodgeRunStep(run, false, true, gauge, 2.0f));
    Hold(run, DODGE_RUN_HOLD_TICKS, gauge, 2.0f);
    EXPECT_TRUE(run.running);
}

} // namespace

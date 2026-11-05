#include <gtest/gtest.h>

#include <cmath>

#include "gauge_rules.hpp"
#include "guard_rules.hpp"

namespace {

using namespace dc::game;

TEST(GuardRules, AngleIsFromWhereThePlayerFaces) {
    // Facing +z (yaw 0): ahead, to the side, behind.
    EXPECT_NEAR(GuardAngle(0.0f, 0.0f, 0.0f, 0.0f, 10.0f), 0.0f, 0.01f);
    EXPECT_NEAR(GuardAngle(0.0f, 0.0f, 0.0f, 10.0f, 0.0f), 90.0f, 0.01f);
    EXPECT_NEAR(GuardAngle(0.0f, 0.0f, 0.0f, 0.0f, -10.0f), 180.0f, 0.01f);
    // Facing +x (yaw 90 degrees) from elsewhere on the map.
    EXPECT_NEAR(GuardAngle(1.5707963f, 5.0f, 5.0f, 15.0f, 5.0f), 0.0f, 0.01f);
    EXPECT_NEAR(GuardAngle(1.5707963f, 5.0f, 5.0f, 5.0f, 15.0f), 90.0f, 0.01f);
    EXPECT_FLOAT_EQ(GuardAngle(0.0f, 1.0f, 1.0f, 1.0f, 1.0f), 0.0f);
}

TEST(GuardRules, WithNoRulesItIsTheGamesGuard) {
    const GuardRules none;
    for (const bool guarding : {false, true}) {
        for (const bool held : {false, true}) {
            for (const bool heavy : {false, true}) {
                for (const float angle : {0.0f, 90.0f, 180.0f}) {
                    const GuardOutcome out = ResolveGuard(none, {.guarding = guarding, .held = held, .heavy = heavy,
                                                                 .damage = 30, .angle = angle, .gauge = 5.0f});
                    const bool blocked = heavy ? held : guarding;
                    EXPECT_EQ(out.blocked, blocked) << guarding << held << heavy << angle;
                    EXPECT_EQ(out.damage, blocked ? 0 : 30);
                    EXPECT_FALSE(out.parried);
                    EXPECT_FALSE(out.broke);
                    EXPECT_FLOAT_EQ(out.cost, 0.0f);
                }
            }
        }
    }
}

TEST(GuardRules, OnlyTheFrontIsCovered) {
    const GuardRules rules{.on = true, .arc = 120.0f};
    EXPECT_TRUE(ResolveGuard(rules, {.guarding = true, .held = true, .damage = 20, .angle = 59.0f}).blocked);
    const GuardOutcome side = ResolveGuard(rules, {.guarding = true, .held = true, .damage = 20, .angle = 61.0f});
    EXPECT_FALSE(side.blocked);
    EXPECT_EQ(side.damage, 20);
    EXPECT_FALSE(ResolveGuard(rules, {.guarding = true, .held = true, .damage = 20, .angle = 180.0f}).blocked);
}

TEST(GuardRules, ABlockCostsGaugeAndLetsChipThrough) {
    const GuardRules     rules{.on = true, .cost_base = 10.0f, .cost_per_damage = 1.5f, .chip = 0.2f};
    const GuardOutcome   out = ResolveGuard(rules, {.guarding = true, .held = true, .damage = 20, .gauge = 80.0f});
    EXPECT_TRUE(out.blocked);
    EXPECT_FLOAT_EQ(out.cost, 40.0f);
    EXPECT_EQ(out.damage, 4);
    EXPECT_FLOAT_EQ(GaugeAfterHit({}, 80.0f, out, true), 40.0f);
}

TEST(GuardRules, AHitThatCostsMoreThanTheGaugeBreaksTheGuard) {
    const GuardRules   rules{.on = true, .cost_base = 10.0f, .cost_per_damage = 1.5f, .chip = 0.2f};
    const GuardOutcome out = ResolveGuard(rules, {.guarding = true, .held = true, .damage = 20, .gauge = 39.0f});
    EXPECT_FALSE(out.blocked);
    EXPECT_TRUE(out.broke);
    EXPECT_EQ(out.damage, 20);
    EXPECT_FLOAT_EQ(GaugeAfterHit({}, 39.0f, out, true), 0.0f);
    EXPECT_FLOAT_EQ(GaugeAfterHit({.on = true, .hit_refill = 50.0f}, 39.0f, out, true), 0.0f);
}

TEST(GuardRules, ParryComesEarlyAndCatchesHeavyHitsWhileRising) {
    const GuardRules rules{.on = true, .cost_base = 50.0f, .parry_frames = 6};
    const GuardOutcome early = ResolveGuard(rules, {.guarding = true, .heavy = true, .damage = 40, .gauge = 10.0f, .guard_frames = 5});
    EXPECT_TRUE(early.blocked);
    EXPECT_TRUE(early.parried);
    EXPECT_EQ(early.damage, 0);
    EXPECT_FLOAT_EQ(early.cost, 0.0f);
    EXPECT_FLOAT_EQ(GaugeAfterHit({}, 10.0f, early, true), 10.0f);
    // Past the window a heavy hit on a rising guard lands, as in the game.
    const GuardOutcome late = ResolveGuard(rules, {.guarding = true, .heavy = true, .damage = 40, .gauge = 100.0f, .guard_frames = 6});
    EXPECT_FALSE(late.blocked);
    EXPECT_FALSE(late.parried);
    // A parry from behind is no parry.
    const GuardRules front{.on = true, .arc = 120.0f, .parry_frames = 6};
    EXPECT_FALSE(ResolveGuard(front, {.guarding = true, .damage = 10, .angle = 150.0f, .guard_frames = 1}).parried);
}

TEST(GaugeRules, AHitNoLongerFillsTheGaugeUnderRules) {
    const GuardOutcome landed{.damage = 10};
    EXPECT_FLOAT_EQ(GaugeAfterHit({}, 30.0f, landed, false), 100.0f); // the game's
    EXPECT_FLOAT_EQ(GaugeAfterHit({.on = true}, 30.0f, landed, false), 30.0f);
    EXPECT_FLOAT_EQ(GaugeAfterHit({.on = true, .hit_refill = -20.0f}, 30.0f, landed, false), 10.0f);
    EXPECT_FLOAT_EQ(GaugeAfterHit({.on = true, .hit_refill = -50.0f}, 30.0f, landed, false), 0.0f);
    // A block with no guard rules: the game fills the gauge; gauge rules leave it be.
    const GuardOutcome blocked{.blocked = true};
    EXPECT_FLOAT_EQ(GaugeAfterHit({}, 30.0f, blocked, false), 100.0f);
    EXPECT_FLOAT_EQ(GaugeAfterHit({.on = true, .hit_refill = -20.0f}, 30.0f, blocked, false), 30.0f);
}

TEST(GaugeRules, TheRefillWaitsAfterTheGaugeGoesDown) {
    GaugeDelay delay;
    EXPECT_TRUE(GaugeDelayStep(delay, 100.0f, 30)); // never spent: no wait
    GaugeDelaySeen(delay, 100.0f);
    EXPECT_FALSE(GaugeDelayStep(delay, 75.0f, 30)); // a swing took 25
    GaugeDelaySeen(delay, 75.0f);
    for (int frame = 1; frame < 30; frame++) {
        EXPECT_FALSE(GaugeDelayStep(delay, 75.0f, 30)) << frame;
        GaugeDelaySeen(delay, 75.0f);
    }
    EXPECT_TRUE(GaugeDelayStep(delay, 75.0f, 30));
    GaugeDelaySeen(delay, 76.0f); // refilled
    EXPECT_TRUE(GaugeDelayStep(delay, 76.0f, 30)); // rising doesn't restart it
    GaugeDelaySeen(delay, 77.0f);
    EXPECT_FALSE(GaugeDelayStep(delay, 60.0f, 30)); // spent again
    EXPECT_TRUE(GaugeDelayStep(delay, 50.0f, 0));   // no delay set: always refills
}

TEST(GuardRules, PoiseTakesHitsUntilSpentThenStaggers) {
    PoiseState state;
    EXPECT_FALSE(PoiseTakes(state, 0, false, 10)); // no poise: every hit staggers
    EXPECT_TRUE(PoiseTakes(state, 3, false, 20));  // 1 of 3
    EXPECT_TRUE(PoiseTakes(state, 3, true, 30));   // a heavy one: 3 of 3
    EXPECT_FALSE(PoiseTakes(state, 3, false, 40)); // past it: a stagger
    EXPECT_TRUE(PoiseTakes(state, 3, false, 50));  // and whole again for the next
    EXPECT_FALSE(PoiseTakes(state, 1, true, 60 + POISE_RESET_FRAMES)); // a heavy hit is more than 1 point
}

TEST(GuardRules, PoiseIsWholeAgainAfterAPause) {
    PoiseState state;
    EXPECT_TRUE(PoiseTakes(state, 2, true, 100));
    EXPECT_FALSE(PoiseTakes(state, 2, false, 100 + POISE_RESET_FRAMES - 1)); // still spent
    EXPECT_TRUE(PoiseTakes(state, 2, true, 400));                            // a pause: whole
}

} // namespace

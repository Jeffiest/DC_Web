#include <gtest/gtest.h>

#include "gauge_rules.hpp"
#include "itemdata.hpp"

namespace {

using namespace dc::game;

// The game's own refill, as gameloop.cpp had it before the rules moved out.
float RetailGain(int chara, int speed, bool goo, bool power, bool swinging, int gun, bool exhausted) {
    float gain = static_cast<float>(speed) / 30.0f;
    if (goo) gain /= 2.0f;
    if (power) gain *= 2.0f;
    gain = std::max(gain, 1.0f);
    if (chara == CHARA_TOAN || chara == CHARA_UNGAGA) gain = swinging ? 0.0f : gain * 0.25f;
    if (chara == CHARA_OSMOND) gain = gun != 1 ? (exhausted ? gain * 0.5f : 0.0f) : gain * 1.5f;
    if (chara == CHARA_XIAO || chara == CHARA_RUBY) gain *= 1.5f;
    return gain;
}

TEST(GaugeRules, WithNothingSetTheRefillIsTheGames) {
    for (int chara : {CHARA_TOAN, CHARA_XIAO, CHARA_GORO, CHARA_RUBY, CHARA_UNGAGA, CHARA_OSMOND}) {
        for (int speed : {5, 20, 40, 70, 99}) {
            for (int bits = 0; bits < 32; bits++) {
                const bool goo = bits & 1, power = bits & 2, swinging = bits & 4, exhausted = bits & 8;
                const int  gun = bits & 16 ? 1 : 0;
                const float gain = GaugeRefillGain({.chara = chara, .weapon_speed = speed, .goo = goo, .stamina_ailment = power,
                                                    .swinging = swinging, .gun_type = gun, .exhausted = exhausted});
                EXPECT_FLOAT_EQ(gain, RetailGain(chara, speed, goo, power, swinging, gun, exhausted))
                    << "chara " << chara << " speed " << speed << " bits " << bits;
            }
        }
    }
}

TEST(GaugeRules, StaminaIsTheCharactersOwnRefillWhateverTheWeapon) {
    // 30 a second is half a point a tick, the same with a dagger or a club, and no quarter for Toan.
    EXPECT_FLOAT_EQ(GaugeRefillGain({.chara = CHARA_TOAN, .weapon_speed = 70, .stamina = 30.0f}), 0.5f);
    EXPECT_FLOAT_EQ(GaugeRefillGain({.chara = CHARA_TOAN, .weapon_speed = 20, .stamina = 30.0f}), 0.5f);
    EXPECT_FLOAT_EQ(GaugeRefillGain({.chara = CHARA_XIAO, .weapon_speed = 40, .stamina = 60.0f}), 1.0f);
    // A swing frame still refills nothing, and Goo still halves it.
    EXPECT_FLOAT_EQ(GaugeRefillGain({.chara = CHARA_TOAN, .stamina = 30.0f, .swinging = true}), 0.0f);
    EXPECT_FLOAT_EQ(GaugeRefillGain({.chara = CHARA_GORO, .stamina = 30.0f, .goo = true}), 0.25f);
}

TEST(GaugeRules, LightnessRefillsEveryWeaponAlike) {
    const float dagger = GaugeRefillGain({.chara = CHARA_TOAN, .weapon_speed = 70, .lightness = true});
    const float club = GaugeRefillGain({.chara = CHARA_TOAN, .weapon_speed = 20, .lightness = true});
    EXPECT_FLOAT_EQ(dagger, club);
    EXPECT_FLOAT_EQ(dagger, static_cast<float>(LIGHTNESS_SPEED) / 30.0f * 0.25f);
}

TEST(GaugeRules, ALightWeaponSwingsCheaperAndAHeavyOneDearer) {
    EXPECT_FLOAT_EQ(LightnessSwingScale(LIGHTNESS_SPEED), 1.0f);
    EXPECT_FLOAT_EQ(LightnessSwingScale(30), 2.0f);
    EXPECT_FLOAT_EQ(LightnessSwingScale(5), 2.0f);
    EXPECT_FLOAT_EQ(LightnessSwingScale(80), 0.75f);
    EXPECT_FLOAT_EQ(LightnessSwingScale(99), static_cast<float>(LIGHTNESS_SPEED) / 99.0f);
    EXPECT_FLOAT_EQ(LightnessSwingScale(0), 2.0f);
}

} // namespace

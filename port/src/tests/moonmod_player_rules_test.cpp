#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string_view>

#include "btactstatus.hpp"
#include "gear.hpp"
#include "dun/gameloop.hpp"
#include "main.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/game_hooks.hpp"
#include "moonmod/runtime.hpp"
#include "savedata.hpp"
#include "userstatus.hpp"

using namespace dc::game;

namespace {

using moonmod::ApiCallStatus;
using moonmod::Json;

struct PlayerRulesApi {
    moonmod::RuntimeConfig config;

    PlayerRulesApi() {
        moonmod::ResetChroniclePlayerRulesForTests();
        moonmod::ResetChroniclePlayerTuningForTests();
        config.game_id = "chronicle";
        moonmod::ConfigureChronicleApi(config);
    }
    ~PlayerRulesApi() {
        moonmod::ResetChroniclePlayerRulesForTests();
        moonmod::ResetChroniclePlayerTuningForTests();
    }
    moonmod::ApiCallResult call(std::string_view path, std::string_view owner, const Json &args) {
        const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation(path);
        EXPECT_NE(operation, nullptr) << path;
        return config.api_handler(owner, *operation, args);
    }
};

TEST(MoonModPlayerRules, TheOperationsAreCataloguedAndSupported) {
    PlayerRulesApi api;
    for (const char *path : {"player.guard_rules_set", "player.guard_rules_clear", "player.gauge_rules_set",
                             "player.gauge_rules_clear", "player.gauge_get", "player.gauge_set", "player.state"}) {
        const auto capability = std::ranges::find(api.config.api_capabilities, path, &moonmod::ApiCapability::path);
        ASSERT_NE(capability, api.config.api_capabilities.end()) << path;
        EXPECT_TRUE(capability->supported) << path;
    }
}

TEST(MoonModPlayerRules, GuardRulesValidateHaveOneOwnerAndGoOnUnload) {
    PlayerRulesApi api;
    for (const Json &bad : {Json{{"arc", 361}}, Json{{"arc", "front"}}, Json{{"cost_base", -1}}, Json{{"chip", 1.5}},
                            Json{{"cost_per_damage", 11}}, Json{{"break_recoil", 601}}, Json{{"parry_frames", 2.5}},
                            Json{{"parry_frames", 61}}, Json{{"without_lock", 1}}, Json{{"riposte", true}}}) {
        const moonmod::ApiCallResult rejected = api.call("player.guard_rules_set", "test.vigil", Json::array({bad}));
        EXPECT_EQ(rejected.status, ApiCallStatus::InvalidArgument) << bad.dump();
        EXPECT_FALSE(rejected.message.empty()) << bad.dump();
    }
    EXPECT_FALSE(moonmod::ChroniclePlayerGuardWithoutLock());

    moonmod::ApiCallResult set = api.call("player.guard_rules_set", "test.vigil",
                                          Json::array({Json{{"arc", 120}, {"cost_base", 10}, {"cost_per_damage", 1.5},
                                                            {"chip", 0.2}, {"break_recoil", 200}, {"parry_frames", 6},
                                                            {"without_lock", true}}}));
    ASSERT_EQ(set.status, ApiCallStatus::Ok) << set.message;
    EXPECT_TRUE(set.value["on"].get<bool>());
    EXPECT_EQ(set.value["owner"], "test.vigil");
    EXPECT_FLOAT_EQ(set.value["arc"].get<float>(), 120.0f);
    EXPECT_EQ(set.value["parry_frames"].get<int>(), 6);
    EXPECT_TRUE(moonmod::ChroniclePlayerGuardWithoutLock());

    EXPECT_EQ(api.call("player.guard_rules_set", "test.other", Json::array({Json::object()})).status, ApiCallStatus::Failed);
    EXPECT_FALSE(api.call("player.guard_rules_clear", "test.other", Json::array()).value["cleared"].get<bool>());
    EXPECT_TRUE(moonmod::ChroniclePlayerGuardWithoutLock());

    // The owner may replace its own rules; fields left out are the defaults.
    set = api.call("player.guard_rules_set", "test.vigil", Json::array({Json{{"arc", 180}}}));
    ASSERT_EQ(set.status, ApiCallStatus::Ok) << set.message;
    EXPECT_FALSE(set.value["without_lock"].get<bool>());

    ASSERT_TRUE(api.config.api_owner_unloaded);
    api.config.api_owner_unloaded("test.vigil");
    const moonmod::ApiCallResult after = api.call("player.gauge_rules_clear", "test.vigil", Json::array());
    EXPECT_FALSE(after.value["cleared"].get<bool>());
    EXPECT_EQ(api.call("player.guard_rules_set", "test.other", Json::array({Json::object()})).status, ApiCallStatus::Ok);
    const moonmod::ApiCallResult cleared = api.call("player.guard_rules_clear", "test.other", Json::array());
    EXPECT_TRUE(cleared.value["cleared"].get<bool>());
    EXPECT_FALSE(cleared.value["rules"]["on"].get<bool>());
}

TEST(MoonModPlayerRules, AHitFollowsTheRules) {
    PlayerRulesApi api;
    const BT_ACT_STATUS saved = BtActStatus;

    // No rules: the game's guard and a gauge filled to 100.
    moonmod::ChroniclePlayerHitResult hit = moonmod::ChroniclePlayerHitResolve({.kind = 2, .damage = 20, .guarding = true, .angle = 170.0f, .gauge = 30.0f});
    EXPECT_TRUE(hit.blocked);
    EXPECT_EQ(hit.damage, 0);
    EXPECT_FLOAT_EQ(hit.gauge, 100.0f);

    ASSERT_EQ(api.call("player.guard_rules_set", "test.vigil",
                       Json::array({Json{{"arc", 120}, {"cost_base", 10}, {"cost_per_damage", 1.5}, {"chip", 0.2},
                                         {"break_recoil", 200}, {"parry_frames", 6}}})).status, ApiCallStatus::Ok);
    ASSERT_EQ(api.call("player.gauge_rules_set", "test.vigil", Json::array({Json{{"hit_refill", -15}}})).status, ApiCallStatus::Ok);

    // From behind: lands, and the hit drains instead of filling.
    hit = moonmod::ChroniclePlayerHitResolve({.kind = 2, .damage = 20, .guarding = true, .angle = 170.0f, .gauge = 30.0f});
    EXPECT_FALSE(hit.blocked);
    EXPECT_EQ(hit.damage, 20);
    EXPECT_FLOAT_EQ(hit.gauge, 15.0f);

    // In front, held long past the parry window: blocked for 40 gauge with 4 chip.
    BtActStatus.action_on = 6;
    for (int frame = 0; frame < 10; frame++) moonmod::ChroniclePlayerGaugeTick();
    hit = moonmod::ChroniclePlayerHitResolve({.kind = 3, .damage = 20, .guarding = true, .held = true, .angle = 10.0f, .gauge = 80.0f});
    EXPECT_TRUE(hit.blocked);
    EXPECT_FALSE(hit.parried);
    EXPECT_EQ(hit.damage, 4);
    EXPECT_FLOAT_EQ(hit.gauge, 40.0f);

    // Too little gauge: the guard breaks, and the break's recoil wins over recoil_scale.
    hit = moonmod::ChroniclePlayerHitResolve({.kind = 3, .damage = 20, .guarding = true, .held = true, .angle = 10.0f, .gauge = 39.0f});
    EXPECT_TRUE(hit.broke);
    EXPECT_EQ(hit.damage, 20);
    EXPECT_FLOAT_EQ(hit.gauge, 0.0f);
    ASSERT_EQ(api.call("player.tuning_set", "test.vigil", Json::array({Json{{"recoil_scale", 0.4}}})).status, ApiCallStatus::Ok);
    BtActStatus.recoil_frames = 160;
    moonmod::ChroniclePlayerHitRecoil(true);
    EXPECT_EQ(BtActStatus.recoil_frames, 200);

    // Just up: a parry.
    BtActStatus.action_on = 0;
    moonmod::ChroniclePlayerGaugeTick();
    BtActStatus.action_on = 6;
    moonmod::ChroniclePlayerGaugeTick();
    hit = moonmod::ChroniclePlayerHitResolve({.kind = 3, .damage = 20, .guarding = true, .angle = 10.0f, .gauge = 50.0f});
    EXPECT_TRUE(hit.parried);
    EXPECT_EQ(hit.damage, 0);
    EXPECT_FLOAT_EQ(hit.gauge, 50.0f);

    BtActStatus = saved;
}

TEST(MoonModPlayerRules, TuningRecoilAndInvincibilityApplyToALandedHit) {
    PlayerRulesApi api;
    const BT_ACT_STATUS saved = BtActStatus;
    BtActStatus.recoil_frames = 160;
    BtActStatus.invincible_frames = 0;
    moonmod::ChroniclePlayerHitRecoil(false); // nothing set: the game's
    EXPECT_EQ(BtActStatus.recoil_frames, 160);
    EXPECT_EQ(BtActStatus.invincible_frames, 0);

    ASSERT_EQ(api.call("player.tuning_set", "test.vigil",
                       Json::array({Json{{"recoil_scale", 0.4}, {"invincible_bonus_frames", 12}}})).status, ApiCallStatus::Ok);
    BtActStatus.recoil_frames = 160;
    moonmod::ChroniclePlayerHitRecoil(false);
    EXPECT_EQ(BtActStatus.recoil_frames, 64);
    EXPECT_EQ(BtActStatus.invincible_frames, 76);
    BtActStatus.recoil_frames = 1;
    moonmod::ChroniclePlayerHitRecoil(false);
    EXPECT_EQ(BtActStatus.recoil_frames, 1); // never below 1
    BtActStatus = saved;
}

TEST(MoonModPlayerRules, GaugeRulesGateAndChargeTheFirstSwingAndDelayTheRefill) {
    PlayerRulesApi api;
    const BT_ACT_STATUS saved = BtActStatus;
    for (const Json &bad : {Json{{"hit_refill", -101}}, Json{{"first_swing_cost", 101}}, Json{{"min_to_swing", -1}},
                            Json{{"swing_cost_scale", 5}}, Json{{"refill_delay", 0.5}}, Json{{"refill_delay", 601}},
                            Json{{"stamina", 1}}}) {
        EXPECT_EQ(api.call("player.gauge_rules_set", "test.vigil", Json::array({bad})).status, ApiCallStatus::InvalidArgument) << bad.dump();
    }

    BtActStatus.action_gauge = 10.0f;
    EXPECT_TRUE(moonmod::ChroniclePlayerSwingAllowed(true)); // no rules: the game's free first swing
    BtActStatus.action_on = 1;
    moonmod::ChroniclePlayerSwingStarted(true);
    EXPECT_FLOAT_EQ(BtActStatus.action_gauge, 10.0f);

    ASSERT_EQ(api.call("player.gauge_rules_set", "test.vigil",
                       Json::array({Json{{"first_swing_cost", 25}, {"min_to_swing", 20}, {"swing_cost_scale", 2},
                                         {"refill_delay", 3}}})).status, ApiCallStatus::Ok);
    EXPECT_EQ(api.call("player.gauge_rules_set", "test.other", Json::array({Json::object()})).status, ApiCallStatus::Failed);

    EXPECT_FALSE(moonmod::ChroniclePlayerSwingAllowed(true));
    EXPECT_TRUE(moonmod::ChroniclePlayerSwingAllowed(false)); // follow-ups keep the game's own test
    BtActStatus.action_gauge = 30.0f;
    EXPECT_TRUE(moonmod::ChroniclePlayerSwingAllowed(true));
    BtActStatus.action_on = 1;
    moonmod::ChroniclePlayerSwingStarted(true);
    EXPECT_FLOAT_EQ(BtActStatus.action_gauge, 5.0f);
    moonmod::ChroniclePlayerSwingStarted(false); // a follow-up: not charged again here
    EXPECT_FLOAT_EQ(BtActStatus.action_gauge, 5.0f);

    // The refill waits three ticks after the gauge last went down.
    BtActStatus.action_on = 0;
    BtActStatus.action_gauge = 50.0f;
    moonmod::ChroniclePlayerGaugeTick();
    moonmod::ChroniclePlayerGaugeSeen();
    BtActStatus.action_gauge = 40.0f;
    EXPECT_FALSE(moonmod::ChroniclePlayerGaugeTick());
    moonmod::ChroniclePlayerGaugeSeen();
    EXPECT_FALSE(moonmod::ChroniclePlayerGaugeTick());
    moonmod::ChroniclePlayerGaugeSeen();
    EXPECT_FALSE(moonmod::ChroniclePlayerGaugeTick());
    moonmod::ChroniclePlayerGaugeSeen();
    EXPECT_TRUE(moonmod::ChroniclePlayerGaugeTick());

    api.config.api_owner_unloaded("test.vigil");
    BtActStatus.action_gauge = 10.0f;
    EXPECT_TRUE(moonmod::ChroniclePlayerSwingAllowed(true));
    BtActStatus = saved;
}

TEST(MoonModPlayerRules, AWornTunicsPoiseTakesHitsWithoutAStagger) {
    PlayerRulesApi api;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    CSaveData *previous_save = SaveData;
    CUserStatus *previous_user = UserStatus;
    SaveData = save.get();
    UserStatus = save->GetDngStatus();
    UserStatus->cur_chara = CHARA_TOAN;
    UserStatus->hp[CHARA_TOAN] = 100;
    ClearGear();

    // No poise: every hit staggers.
    EXPECT_FALSE(moonmod::ChroniclePlayerHitResolve({.kind = 2, .damage = 10}).poised);

    GearEffects tunic;
    tunic.poise = 2;
    EquipGear(CHARA_TOAN, AddGear({.kind = GEAR_TUNIC, .name = "Stalwart Tunic", .effects = tunic}));
    EXPECT_TRUE(moonmod::ChroniclePlayerHitResolve({.kind = 2, .damage = 10}).poised);
    EXPECT_TRUE(moonmod::ChroniclePlayerHitResolve({.kind = 2, .damage = 10}).poised);
    EXPECT_FALSE(moonmod::ChroniclePlayerHitResolve({.kind = 2, .damage = 10}).poised); // spent: a stagger
    EXPECT_TRUE(moonmod::ChroniclePlayerHitResolve({.kind = 3, .damage = 10}).poised);  // whole again; heavy is 2
    moonmod::ResetChroniclePlayerRulesForTests();
    // Never a trap, a hit that would empty the life bar, or a blocked one.
    EXPECT_FALSE(moonmod::ChroniclePlayerHitResolve({.kind = 4, .damage = 10}).poised);
    EXPECT_FALSE(moonmod::ChroniclePlayerHitResolve({.kind = 2, .damage = 100}).poised);
    const moonmod::ChroniclePlayerHitResult blocked =
        moonmod::ChroniclePlayerHitResolve({.kind = 2, .damage = 10, .guarding = true, .gauge = 100.0f});
    EXPECT_TRUE(blocked.blocked);
    EXPECT_FALSE(blocked.poised);

    ClearGear();
    UserStatus = previous_user;
    SaveData = previous_save;
}

TEST(MoonModPlayerRules, GaugeAndStateReadAndWriteInADungeon) {
    PlayerRulesApi api;
    const BT_ACT_STATUS saved = BtActStatus;
    const s32 previous_mode = mode;

    mode = -1;
    EXPECT_EQ(api.call("player.gauge_get", "test.vigil", Json::array()).status, ApiCallStatus::Failed);
    EXPECT_EQ(api.call("player.gauge_set", "test.vigil", Json::array({Json{{"gauge", 50}}})).status, ApiCallStatus::Failed);
    moonmod::ApiCallResult state = api.call("player.state", "test.vigil", Json::array());
    ASSERT_EQ(state.status, ApiCallStatus::Ok) << state.message;
    EXPECT_FALSE(state.value["active"].get<bool>());

    mode = GAME_MODE_DUNGEON;
    for (const Json &bad : {Json{{"gauge", 101}}, Json{{"add", -101}}, Json{{"gauge", 5}, {"add", 5}}, Json::object(),
                            Json{{"gauge", "full"}}, Json{{"gauge", 5}, {"max", 5}}}) {
        EXPECT_EQ(api.call("player.gauge_set", "test.vigil", Json::array({bad})).status, ApiCallStatus::InvalidArgument) << bad.dump();
    }
    BtActStatus.action_gauge = 30.0f;
    moonmod::ApiCallResult result = api.call("player.gauge_set", "test.vigil", Json::array({Json{{"add", 25}}}));
    ASSERT_EQ(result.status, ApiCallStatus::Ok) << result.message;
    EXPECT_FLOAT_EQ(result.value["gauge"].get<float>(), 55.0f);
    EXPECT_FLOAT_EQ(result.value["previous"].get<float>(), 30.0f);
    result = api.call("player.gauge_set", "test.vigil", Json::array({Json{{"add", -90}}}));
    EXPECT_FLOAT_EQ(BtActStatus.action_gauge, 0.0f);
    result = api.call("player.gauge_set", "test.vigil", Json::array({Json{{"gauge", 100}}}));
    EXPECT_FLOAT_EQ(BtActStatus.action_gauge, 100.0f);

    result = api.call("player.gauge_get", "test.vigil", Json::array());
    ASSERT_EQ(result.status, ApiCallStatus::Ok) << result.message;
    EXPECT_FLOAT_EQ(result.value["gauge"].get<float>(), 100.0f);
    EXPECT_FALSE(result.value["guard_rules"]["on"].get<bool>());
    EXPECT_EQ(api.call("player.gauge_get", "test.vigil", Json::array({1})).status, ApiCallStatus::InvalidArgument);
    EXPECT_EQ(api.call("player.state", "test.vigil", Json::array({1})).status, ApiCallStatus::InvalidArgument);

    mode = previous_mode;
    BtActStatus = saved;
}

} // namespace

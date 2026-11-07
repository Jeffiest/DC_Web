#include <gtest/gtest.h>

#include <algorithm>
#include <memory>

#include "dun/gameloop.hpp"
#include "main.hpp"
#include "monstorunit.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/combat_formula.hpp"
#include "moonmod/game_hooks.hpp"
#include "savedata.hpp"

namespace dc::game {}
using namespace dc::game;

TEST(MoonModCombatApi, AppliesBoundedRankedSkillDamageBonuses) {
    const moonmod::Json bonuses = moonmod::Json::array({{{"rank", 2}, {"per_rank", 0.1}}});
    EXPECT_FLOAT_EQ(moonmod::ApplyCombatDamageBonuses(10.0f, bonuses), 12.0f);
    EXPECT_FLOAT_EQ(moonmod::ApplyCombatDamageBonuses(0.0f, bonuses), 0.0f);
    EXPECT_FLOAT_EQ(moonmod::ApplyCombatDamageBonuses(10.0f, moonmod::Json::object()), 10.0f);
    EXPECT_FLOAT_EQ(moonmod::ApplyCombatDamageBonuses(
                        10.0f, moonmod::Json::array({{{"rank", 100}, {"per_rank", 0.5}}})), 10.0f);
}


TEST(MoonModCombatApi, RegistersAndOrdersOwnerScopedDamageFormulas) {
    moonmod::ResetChronicleCombatFormulasForTests();
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation("combat.register_formula");
    ASSERT_NE(operation, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "combat.register_formula" && capability.supported;
    }));
    EXPECT_FLOAT_EQ(moonmod::ApplyChronicleCombatDamageFormula(10.0f), 10.0f);

    const std::string owner = "test.moonmod.formula-probe";
    const moonmod::Json definition = {{{"id", "double"}, {"operation", "multiply"}, {"value", 2.0}, {"priority", 10}}};
    const moonmod::ApiCallResult registered = config.api_handler(owner, *operation, definition);
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    EXPECT_FALSE(registered.value.at("idempotent").get<bool>());
    EXPECT_EQ(registered.value.at("id"), owner + ":double");
    EXPECT_FLOAT_EQ(moonmod::ApplyChronicleCombatDamageFormula(10.0f), 20.0f);

    const moonmod::ApiCallResult repeated = config.api_handler(owner, *operation, definition);
    ASSERT_EQ(repeated.status, moonmod::ApiCallStatus::Ok) << repeated.message;
    EXPECT_TRUE(repeated.value.at("idempotent").get<bool>());
    // the same id with a new definition replaces it (a mod updates its own formula), then back
    const moonmod::ApiCallResult replaced = config.api_handler(
        owner, *operation, moonmod::Json::array({{{"id", "double"}, {"operation", "add"}, {"value", 1}}}));
    ASSERT_EQ(replaced.status, moonmod::ApiCallStatus::Ok) << replaced.message;
    EXPECT_FALSE(replaced.value.at("idempotent").get<bool>());
    EXPECT_FLOAT_EQ(moonmod::ApplyChronicleCombatDamageFormula(10.0f), 11.0f);
    ASSERT_EQ(config.api_handler(owner, *operation, definition).status, moonmod::ApiCallStatus::Ok);
    EXPECT_FLOAT_EQ(moonmod::ApplyChronicleCombatDamageFormula(10.0f), 20.0f);
    EXPECT_EQ(config.api_handler(owner, *operation,
                                 moonmod::Json::array({{{"id", "bad"}, {"operation", "multiply"}, {"value", 101}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);

    const std::string earlier_owner = "test.moonmod.formula-earlier";
    const moonmod::Json earlier_formula = {{{"id", "bonus"}, {"operation", "add"}, {"value", 3.0}, {"priority", 5}}};
    ASSERT_EQ(config.api_handler(earlier_owner, *operation, earlier_formula).status, moonmod::ApiCallStatus::Ok);
    EXPECT_FLOAT_EQ(moonmod::ApplyChronicleCombatDamageFormula(10.0f), 26.0f);

    config.api_owner_unloaded(owner);
    EXPECT_FLOAT_EQ(moonmod::ApplyChronicleCombatDamageFormula(10.0f), 13.0f);
    config.api_owner_unloaded(earlier_owner);
    EXPECT_FLOAT_EQ(moonmod::ApplyChronicleCombatDamageFormula(10.0f), 10.0f);
    EXPECT_FLOAT_EQ(moonmod::ApplyChronicleCombatDamageFormula(0.0f), 0.0f);

    const std::string alpha_owner = "test.moonmod.formula-alpha";
    const std::string zeta_owner = "test.moonmod.formula-zeta";
    const moonmod::Json zeta_add = {{{"id", "add"}, {"operation", "add"}, {"value", 1.0}, {"priority", 3}}};
    const moonmod::Json alpha_multiply = {{{"id", "multiply"}, {"operation", "multiply"}, {"value", 2.0}, {"priority", 3}}};
    ASSERT_EQ(config.api_handler(zeta_owner, *operation, zeta_add).status, moonmod::ApiCallStatus::Ok);
    ASSERT_EQ(config.api_handler(alpha_owner, *operation, alpha_multiply).status, moonmod::ApiCallStatus::Ok);
    EXPECT_FLOAT_EQ(moonmod::ApplyChronicleCombatDamageFormula(10.0f), 21.0f);
    config.api_owner_unloaded(alpha_owner);
    EXPECT_FLOAT_EQ(moonmod::ApplyChronicleCombatDamageFormula(10.0f), 11.0f);
    config.api_owner_unloaded(zeta_owner);
    EXPECT_FLOAT_EQ(moonmod::ApplyChronicleCombatDamageFormula(10.0f), 10.0f);
    moonmod::ResetChronicleCombatFormulasForTests();
}

TEST(MoonModCombatApi, ReadsLiveDungeonMonsterHealthStatusAndEmptyCombatState) {
    CMonstorUnit *previous_unit = NowMonstorUnit;
    CSaveData *previous_save = SaveData;
    const s32 previous_mode = mode;
    const s32 previous_dungeon = selectMapNo;
    struct Restore {
        CMonstorUnit *value;
        CSaveData *save;
        s32 game_mode;
        s32 dungeon;
        ~Restore() {
            NowMonstorUnit = value;
            SaveData = save;
            mode = game_mode;
            selectMapNo = dungeon;
        }
    } restore{previous_unit, previous_save, previous_mode, previous_dungeon};

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation("combat.get");
    ASSERT_NE(operation, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "combat.get" && capability.supported;
    }));

    auto unit = std::make_unique<CMonstorUnit>();
    NowMonstorUnit = unit.get();
    SaveData = nullptr;
    mode = GAME_MODE_TITLE;
    const moonmod::ApiCallResult no_dungeon = config.api_handler("test.moonmod.combat-probe", *operation,
                                                                 moonmod::Json::array());
    EXPECT_EQ(no_dungeon.status, moonmod::ApiCallStatus::Failed);

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    SaveData->GetDngStatus()->cur_georama = 0;
    selectMapNo = 0;
    mode = GAME_MODE_DUNGEON;

    unit->monster[0].state = 2;
    unit->monster[0].hp = 45;
    unit->monster[0].max_hp = 80;
    unit->monster[0].attachment_kind = 3;
    unit->monster[0].name_no = 17;
    unit->monster[0].poison_timer = 11;
    unit->monster[0].slow_timer = 0;
    unit->monster[0].stop_timer = 4;
    unit->monster[0].revealed = 1;
    unit->monster[1].state = 1;
    unit->monster[1].hp = 90;
    unit->monster[1].max_hp = 90;
    unit->monster[2].state = 2;
    unit->monster[2].hp = 0;
    unit->monster[2].max_hp = 30;
    NowMonstorUnit = unit.get();

    const moonmod::ApiCallResult current = config.api_handler("test.moonmod.combat-probe", *operation,
                                                              moonmod::Json::array());
    ASSERT_EQ(current.status, moonmod::ApiCallStatus::Ok) << current.message;
    EXPECT_TRUE(current.value.at("in_combat").get<bool>());
    ASSERT_EQ(current.value.at("enemy_count").get<int>(), 1);
    ASSERT_EQ(current.value.at("enemies").size(), 1u);
    EXPECT_EQ(current.value["enemies"][0].at("id"), "chronicle:monster-slot:0");
    EXPECT_EQ(current.value["enemies"][0].at("hp"), 45);
    EXPECT_EQ(current.value["enemies"][0].at("max_hp"), 80);
    EXPECT_EQ(current.value["enemies"][0].at("family"), 3);
    EXPECT_EQ(current.value["enemies"][0].at("poison_frames"), 11);
    EXPECT_EQ(current.value["enemies"][0].at("stun_frames"), 4);

    unit->monster[0].hp = 0;
    const moonmod::ApiCallResult cleared = config.api_handler("test.moonmod.combat-probe", *operation,
                                                             moonmod::Json::array());
    ASSERT_EQ(cleared.status, moonmod::ApiCallStatus::Ok) << cleared.message;
    EXPECT_FALSE(cleared.value.at("in_combat").get<bool>());
    EXPECT_EQ(cleared.value.at("enemy_count").get<int>(), 0);
    EXPECT_TRUE(cleared.value.at("enemies").empty());

    const moonmod::ApiCallResult invalid = config.api_handler(
        "test.moonmod.combat-probe", *operation, moonmod::Json::array({moonmod::Json::object()}));
    EXPECT_EQ(invalid.status, moonmod::ApiCallStatus::InvalidArgument);
}

TEST(MoonModCombatApi, AppliesGenerationCheckedDamageAndQueuesNativeHitOrDeathEvents) {
    CMonstorUnit *previous_unit = NowMonstorUnit;
    CSaveData *previous_save = SaveData;
    const s32 previous_mode = mode;
    const s32 previous_dungeon = selectMapNo;
    struct Restore {
        CMonstorUnit *unit;
        CSaveData *save;
        s32 game_mode;
        s32 dungeon;
        ~Restore() {
            NowMonstorUnit = unit;
            SaveData = save;
            mode = game_mode;
            selectMapNo = dungeon;
        }
    } restore{previous_unit, previous_save, previous_mode, previous_dungeon};

    moonmod::ResetChronicleCharactersForTests();
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *damage_operation = moonmod::FindApiOperation("combat.damage");
    const moonmod::ApiOperationSpec *heal_operation = moonmod::FindApiOperation("combat.heal");
    const moonmod::ApiOperationSpec *combat_operation = moonmod::FindApiOperation("combat.get");
    ASSERT_NE(damage_operation, nullptr);
    ASSERT_NE(heal_operation, nullptr);
    ASSERT_NE(combat_operation, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return (capability.path == "combat.damage" || capability.path == "combat.heal") && capability.supported;
    }));

    auto unit = std::make_unique<CMonstorUnit>();
    NowMonstorUnit = unit.get();
    moonmod::ChronicleMonstersCleared(unit.get());
    unit->monster[0].state = 2;
    unit->monster[0].hp = 40;
    unit->monster[0].max_hp = 60;
    unit->monster[0].invincible_timer = 0;
    unit->monster[0].revealed = -1;
    unit->alive_count = 1;
    moonmod::ChronicleMonsterActorCreated(unit.get(), 0);

    SaveData = nullptr;
    mode = GAME_MODE_TITLE;
    const moonmod::ApiCallResult no_dungeon = config.api_handler(
        "test.moonmod.combat-probe", *damage_operation,
        moonmod::Json::array({moonmod::Json{{"entity", "chronicle:monster:0:1"}, {"amount", 5}}}));
    EXPECT_EQ(no_dungeon.status, moonmod::ApiCallStatus::Failed);

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    SaveData->GetDngStatus()->cur_georama = 0;
    selectMapNo = 0;
    SaveData->GetDngStatus()->cur_floor = 3;
    mode = GAME_MODE_DUNGEON;

    const moonmod::ApiCallResult combat = config.api_handler(
        "test.moonmod.combat-probe", *combat_operation, moonmod::Json::array());
    ASSERT_EQ(combat.status, moonmod::ApiCallStatus::Ok) << combat.message;
    const std::string handle = combat.value.at("enemies")[0].at("entity_id").get<std::string>();
    const moonmod::Json damage_args = {{"entity", handle}, {"amount", 15}};
    const moonmod::ApiCallResult hit = config.api_handler(
        "test.moonmod.combat-probe", *damage_operation, moonmod::Json::array({damage_args}));
    ASSERT_EQ(hit.status, moonmod::ApiCallStatus::Ok) << hit.message;
    EXPECT_EQ(hit.value.at("hp_before"), 40);
    EXPECT_EQ(hit.value.at("hp_after"), 25);
    EXPECT_EQ(hit.value.at("damage"), 15);
    EXPECT_FALSE(hit.value.at("blocked").get<bool>());
    EXPECT_FALSE(hit.value.at("death_queued").get<bool>());
    EXPECT_EQ(unit->monster[0].last_attacker, -1);
    unit->current_monster = 0;
    EXPECT_EQ(unit->CheckDmg(), MonsterDamageResult::Hit);

    unit->monster[0].invincible_timer = 20;
    const moonmod::ApiCallResult blocked = config.api_handler(
        "test.moonmod.combat-probe", *damage_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}, {"amount", 5}}}));
    ASSERT_EQ(blocked.status, moonmod::ApiCallStatus::Ok) << blocked.message;
    EXPECT_TRUE(blocked.value.at("blocked").get<bool>());
    EXPECT_EQ(unit->monster[0].hp, 25);

    // entities.set_stats vulnerable: the script's invincibility (MUTEKI) no longer turns the blow away
    const moonmod::ApiOperationSpec *stats_operation = moonmod::FindApiOperation("entities.set_stats");
    ASSERT_NE(stats_operation, nullptr);
    EXPECT_FALSE(moonmod::ChronicleMonsterVulnerable(unit.get(), 0));
    EXPECT_EQ(config.api_handler("test.moonmod.combat-probe", *stats_operation,
                                 moonmod::Json::array({moonmod::Json{{"entity", handle}, {"vulnerable", 1}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult made_vulnerable = config.api_handler(
        "test.moonmod.combat-probe", *stats_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}, {"vulnerable", true}}}));
    ASSERT_EQ(made_vulnerable.status, moonmod::ApiCallStatus::Ok) << made_vulnerable.message;
    EXPECT_TRUE(made_vulnerable.value.at("current").at("vulnerable").get<bool>());
    EXPECT_TRUE(moonmod::ChronicleMonsterVulnerable(unit.get(), 0));
    const moonmod::ApiCallResult landed = config.api_handler(
        "test.moonmod.combat-probe", *damage_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}, {"amount", 5}}}));
    ASSERT_EQ(landed.status, moonmod::ApiCallStatus::Ok) << landed.message;
    EXPECT_FALSE(landed.value.at("blocked").get<bool>());
    EXPECT_EQ(unit->monster[0].hp, 20);
    unit->monster[0].hp = 25;
    unit->monster[0].invincible_timer = 0;

    const moonmod::ApiCallResult healed = config.api_handler(
        "test.moonmod.combat-probe", *heal_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}, {"amount", 100}}}));
    ASSERT_EQ(healed.status, moonmod::ApiCallStatus::Ok) << healed.message;
    EXPECT_EQ(healed.value.at("hp_before"), 25);
    EXPECT_EQ(healed.value.at("hp_after"), 60);
    EXPECT_EQ(healed.value.at("restored"), 35);

    moonmod::ChronicleMonsterActorCreated(unit.get(), 0);
    EXPECT_FALSE(moonmod::ChronicleMonsterVulnerable(unit.get(), 0)); // a new actor in the slot starts without it
    unit->monster[0].hp = 6;
    unit->alive_count = 1;
    const moonmod::ApiCallResult refreshed = config.api_handler(
        "test.moonmod.combat-probe", *combat_operation, moonmod::Json::array());
    ASSERT_EQ(refreshed.status, moonmod::ApiCallStatus::Ok) << refreshed.message;
    const std::string fresh_handle = refreshed.value.at("enemies")[0].at("entity_id").get<std::string>();
    ASSERT_EQ(config.api_handler("test.moonmod.combat-probe", *stats_operation,
                                 moonmod::Json::array({moonmod::Json{{"entity", fresh_handle}, {"vulnerable", true}}})).status,
              moonmod::ApiCallStatus::Ok);
    const moonmod::ApiCallResult stale = config.api_handler(
        "test.moonmod.combat-probe", *damage_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}, {"amount", 2}}}));
    EXPECT_EQ(stale.status, moonmod::ApiCallStatus::Failed);

    const moonmod::ApiCallResult killed = config.api_handler(
        "test.moonmod.combat-probe", *damage_operation,
        moonmod::Json::array({moonmod::Json{{"entity", fresh_handle}, {"amount", 12}}}));
    ASSERT_EQ(killed.status, moonmod::ApiCallStatus::Ok) << killed.message;
    EXPECT_EQ(killed.value.at("hp_after"), 0);
    EXPECT_TRUE(killed.value.at("death_queued").get<bool>());
    EXPECT_EQ(unit->monster[0].last_attacker, -1);
    EXPECT_EQ(moonmod::TakeChronicleMonsterDamageResult(unit.get(), 0), static_cast<int>(MonsterDamageResult::Killed));

    const moonmod::ApiCallResult malformed = config.api_handler(
        "test.moonmod.combat-probe", *damage_operation,
        moonmod::Json::array({moonmod::Json{{"entity", fresh_handle}, {"amount", 0}}}));
    EXPECT_EQ(malformed.status, moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult stale_heal = config.api_handler(
        "test.moonmod.combat-probe", *heal_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}, {"amount", 5}}}));
    EXPECT_EQ(stale_heal.status, moonmod::ApiCallStatus::Failed);
    EXPECT_TRUE(moonmod::ChronicleMonsterVulnerable(unit.get(), 0));
    moonmod::ChronicleMonstersCleared(unit.get());
    // a floor change clears it, so no later monster in the slot inherits it
    EXPECT_FALSE(moonmod::ChronicleMonsterVulnerable(unit.get(), 0));
    moonmod::ResetChronicleCharactersForTests();
}

TEST(MoonModCombatApi, ChargedFilterMatchesOnlyHitsThatKnowTheyWereCharged) {
    moonmod::CombatDamageFormulaRegistry registry;
    const auto add = [&registry](moonmod::Json formula) { return registry.Register("test.charged", moonmod::Json::array({formula})); };
    ASSERT_EQ(add({{"id", "fury"}, {"operation", "multiply"}, {"value", 1.3}, {"charged", true}}).status, moonmod::ApiCallStatus::Ok);
    ASSERT_EQ(add({{"id", "plain"}, {"operation", "add"}, {"value", 1.0}, {"charged", false}}).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(add({{"id", "bad"}, {"operation", "multiply"}, {"value", 1.0}, {"charged", 1}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_FLOAT_EQ(registry.Apply(10.0f, -1, -1, 0, 5, 10, 0, 1), 13.0f); // charged: x1.3 only
    EXPECT_FLOAT_EQ(registry.Apply(10.0f, -1, -1, 0, 5, 10, 0, 0), 11.0f); // plain: +1 only
    EXPECT_FLOAT_EQ(registry.Apply(10.0f), 10.0f);                         // unknown: neither
    // re-registering the same definition is idempotent; changing the filter replaces it
    EXPECT_EQ(add({{"id", "fury"}, {"operation", "multiply"}, {"value", 1.3}, {"charged", true}}).value["idempotent"], true);
    EXPECT_EQ(add({{"id", "fury"}, {"operation", "multiply"}, {"value", 1.3}, {"charged", false}}).value["idempotent"], false);

    // Chronicle: no charged move yet, so a real hit is plain.
    moonmod::ResetChronicleChargedForTests();
    EXPECT_FALSE(moonmod::ChronicleHitIsCharged());
}

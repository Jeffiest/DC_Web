#include <gtest/gtest.h>

#include <algorithm>
#include <cstring>
#include <memory>

#include "dun/gameloop.hpp"
#include "character.hpp"
#include "collisiondata.hpp"
#include "main.hpp"
#include "monstorunit.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "shot_effect_pack.hpp"
#include "savedata.hpp"
#include "userstatus.hpp"

namespace dc::game {}
using namespace dc::game;


TEST(MoonModEntitiesApi, KeepsModOwnedEnemyActiveWithinNativeViewBudget) {
    CMonstorUnit *const previous_unit = NowMonstorUnit;
    CSaveData *const previous_save = SaveData;
    CUserStatus *const previous_user_status = UserStatus;
    const s32 previous_mode = mode;
    const s32 previous_dungeon = selectMapNo;
    float previous_player_position[4]{};
    CharaMain.GetPosition(previous_player_position);
    struct Restore {
        CMonstorUnit *unit;
        CSaveData *save;
        CUserStatus *user_status;
        s32 game_mode;
        s32 dungeon;
        float player_position[4];
        ~Restore() {
            CharaMain.SetPosition(player_position);
            NowMonstorUnit = unit;
            SaveData = save;
            UserStatus = user_status;
            mode = game_mode;
            selectMapNo = dungeon;
        }
    } restore{previous_unit, previous_save, previous_user_status, previous_mode, previous_dungeon,
              {previous_player_position[0], previous_player_position[1], previous_player_position[2],
               previous_player_position[3]}};

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->cur_georama = 0;
    status->cur_floor = 0;
    UserStatus = static_cast<CUserStatus *>(status);
    selectMapNo = 0;
    mode = GAME_MODE_DUNGEON;
    CharaMain.SetPosition(0.0f, 0.0f, 0.0f);

    auto unit = std::make_unique<CMonstorUnit>();
    NowMonstorUnit = unit.get();
    for (int slot = 0; slot < 5; ++slot) {
        unit->monster[slot].state = 2;
        unit->monster[slot].hp = 10;
        unit->monster[slot].max_hp = 10;
        unit->monster[slot].kind = 0;
        unit->monster[slot].revealed = -1;
        unit->monster[slot].clip_distance = 300.0f;
        unit->chara[slot][0].SetPosition(0.0f, 0.0f, 0.0f);
        moonmod::ChronicleMonsterActorCreated(unit.get(), slot);
    }
    moonmod::ChronicleMonsterActorOwned(unit.get(), 4, "test.moonmod.view-budget", "probe:sentinel", 0);

    unit->CheckViewLevel();

    EXPECT_EQ(unit->monster[4].state, 2);
    EXPECT_EQ(unit->monster[4].view_held, 0);
    int native_active_count = 0;
    int native_held_count = 0;
    for (int slot = 0; slot < 4; ++slot) {
        if (unit->monster[slot].state == 2) ++native_active_count;
        if (unit->monster[slot].view_held != 0) ++native_held_count;
    }
    EXPECT_EQ(native_active_count, 3);
    EXPECT_EQ(native_held_count, 1);
    EXPECT_TRUE(moonmod::ChronicleMonsterActorIsModOwned(unit.get(), 4));
    EXPECT_FALSE(moonmod::ChronicleMonsterActorIsModOwned(unit.get(), 0));
    moonmod::ChronicleMonstersCleared(unit.get());
}

TEST(MoonModEntitiesApi, RaisedActiveLimitKeepsHundredsOfMonstersAwake) {
    ASSERT_GE(MONSTOR_SLOT_MAX, 250);
    CMonstorUnit *const previous_unit = NowMonstorUnit;
    CSaveData *const previous_save = SaveData;
    CUserStatus *const previous_user_status = UserStatus;
    const s32 previous_mode = mode;
    const s32 previous_dungeon = selectMapNo;
    const int previous_limit = MonstorActiveLimit;
    struct Restore {
        CMonstorUnit *unit;
        CSaveData *save;
        CUserStatus *user_status;
        s32 game_mode;
        s32 dungeon;
        int limit;
        ~Restore() {
            NowMonstorUnit = unit;
            SaveData = save;
            UserStatus = user_status;
            mode = game_mode;
            selectMapNo = dungeon;
            MonstorActiveLimit = limit;
        }
    } restore{previous_unit, previous_save, previous_user_status, previous_mode, previous_dungeon, previous_limit};

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *set_limit = moonmod::FindApiOperation("entities.set_active_limit");
    ASSERT_NE(set_limit, nullptr);
    const moonmod::ApiCallResult too_many = config.api_handler(
        "test.moonmod.horde", *set_limit, moonmod::Json::array({{{"active", MONSTOR_SLOT_MAX + 1}}}));
    EXPECT_EQ(too_many.status, moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult raised = config.api_handler(
        "test.moonmod.horde", *set_limit, moonmod::Json::array({{{"active", 250}}}));
    ASSERT_EQ(raised.status, moonmod::ApiCallStatus::Ok) << raised.message;
    EXPECT_EQ(raised.value.at("slots").get<int>(), MONSTOR_SLOT_MAX);
    EXPECT_EQ(raised.value.at("previous").get<int>(), previous_limit);

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->cur_georama = 0;
    status->cur_floor = 0;
    UserStatus = static_cast<CUserStatus *>(status);
    selectMapNo = 0;
    mode = GAME_MODE_DUNGEON;
    CharaMain.SetPosition(0.0f, 0.0f, 0.0f);

    auto unit = std::make_unique<CMonstorUnit>();
    NowMonstorUnit = unit.get();
    for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) unit->monster[slot].state = -1;
    for (int slot = 0; slot < 250; ++slot) {
        unit->monster[slot].state = 2;
        unit->monster[slot].hp = 10;
        unit->monster[slot].max_hp = 10;
        unit->monster[slot].kind = 0;
        unit->monster[slot].revealed = -1;
        unit->monster[slot].clip_distance = 300.0f;
        unit->chara[slot][0].SetPosition(static_cast<float>(slot % 16), 0.0f, static_cast<float>(slot / 16));
    }
    unit->CheckViewLevel();
    int awake = 0;
    for (int slot = 0; slot < 250; ++slot) awake += unit->monster[slot].state == 2 ? 1 : 0;
    EXPECT_EQ(awake, 250);

    MonstorActiveLimit = 4;
    unit->CheckViewLevel();
    awake = 0;
    for (int slot = 0; slot < 250; ++slot) awake += unit->monster[slot].state == 2 ? 1 : 0;
    EXPECT_EQ(awake, 4);
}

TEST(MoonModEntitiesApi, QueriesLiveDungeonMonstersAndRejectsUnsupportedFilters) {
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

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation("entities.query");
    const moonmod::ApiOperationSpec *get_operation = moonmod::FindApiOperation("entities.get");
    const moonmod::ApiOperationSpec *physics_query_operation = moonmod::FindApiOperation("physics.query");
    const moonmod::ApiOperationSpec *velocity_operation = moonmod::FindApiOperation("physics.velocity_get");
    const moonmod::ApiOperationSpec *set_velocity_operation = moonmod::FindApiOperation("physics.velocity_set");
    ASSERT_NE(operation, nullptr);
    ASSERT_NE(get_operation, nullptr);
    ASSERT_NE(physics_query_operation, nullptr);
    ASSERT_NE(velocity_operation, nullptr);
    ASSERT_NE(set_velocity_operation, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return (capability.path == "entities.query" || capability.path == "entities.get") && capability.supported;
    }));
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return (capability.path == "physics.velocity_get" || capability.path == "physics.velocity_set") &&
               capability.supported;
    }));
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "physics.query" && capability.supported;
    }));

    auto unit = std::make_unique<CMonstorUnit>();
    NowMonstorUnit = unit.get();
    moonmod::ChronicleMonstersCleared(unit.get());
    SaveData = nullptr;
    mode = GAME_MODE_TITLE;
    const moonmod::ApiCallResult outside_dungeon = config.api_handler(
        "test.moonmod.entities-probe", *operation, moonmod::Json::array());
    EXPECT_EQ(outside_dungeon.status, moonmod::ApiCallStatus::Failed);
    const moonmod::ApiCallResult physics_query_outside_dungeon = config.api_handler(
        "test.moonmod.entities-probe", *physics_query_operation,
        moonmod::Json::array({moonmod::Json{{"shape", "sphere"}, {"center", {0.0, 0.0, 0.0}}, {"radius", 5.0}}}));
    EXPECT_EQ(physics_query_outside_dungeon.status, moonmod::ApiCallStatus::Failed);

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    status->cur_georama = 0;
    selectMapNo = 0;
    status->cur_floor = 3;
    mode = GAME_MODE_DUNGEON;

    unit->monster[2].state = 2;
    unit->monster[2].hp = 45;
    unit->monster[2].max_hp = 80;
    unit->monster[2].kind = 7;
    unit->monster[2].attachment_kind = 3;
    unit->monster[2].name_no = 17;
    unit->monster[2].poison_timer = 11;
    unit->chara[2][0].SetPosition(3.0f, 4.0f, 0.0f);
    moonmod::ChronicleMonsterActorCreated(unit.get(), 2);
    unit->monster[3].state = 2;
    unit->monster[3].hp = 0;
    unit->monster[3].max_hp = 20;
    unit->monster[4].state = 1;
    unit->monster[4].hp = 20;
    unit->monster[4].max_hp = 20;

    const moonmod::ApiCallResult queried = config.api_handler(
        "test.moonmod.entities-probe", *operation,
        moonmod::Json::array({moonmod::Json{{"type", "monster"}}}));
    ASSERT_EQ(queried.status, moonmod::ApiCallStatus::Ok) << queried.message;
    EXPECT_EQ(queried.value.at("count").get<int>(), 1);
    ASSERT_EQ(queried.value.at("entities").size(), 1u);
    const moonmod::Json &entity = queried.value.at("entities")[0];
    EXPECT_EQ(entity.at("id"), "chronicle:monster-slot:2");
    const std::string handle = entity.at("entity_id").get<std::string>();
    EXPECT_TRUE(handle.starts_with("chronicle:monster:2:"));
    EXPECT_EQ(entity.at("type"), "monster");
    EXPECT_EQ(entity.at("definition").at("kind"), 7);
    EXPECT_EQ(entity.at("definition").at("family"), 3);
    EXPECT_EQ(entity.at("health").at("current"), 45);
    EXPECT_EQ(entity.at("health").at("maximum"), 80);
    EXPECT_EQ(entity.at("status").at("poison_frames"), 11);

    const moonmod::ApiCallResult spatial_query = config.api_handler(
        "test.moonmod.entities-probe", *physics_query_operation,
        moonmod::Json::array({moonmod::Json{{"shape", "sphere"}, {"center", {0.0, 0.0, 0.0}}, {"radius", 5.0}}}));
    ASSERT_EQ(spatial_query.status, moonmod::ApiCallStatus::Ok) << spatial_query.message;
    EXPECT_EQ(spatial_query.value.value("semantics", ""),
              "live monster actor origins inside or on the sphere; no collider or world geometry test");
    EXPECT_EQ(spatial_query.value.at("count").get<int>(), 1);
    ASSERT_EQ(spatial_query.value.at("matches").size(), 1u);
    EXPECT_EQ(spatial_query.value.at("matches")[0].at("entity"), entity.at("entity_id"));
    EXPECT_NEAR(spatial_query.value.at("matches")[0].at("distance").get<double>(), 5.0, 0.00001);
    const moonmod::ApiCallResult malformed_spatial_query = config.api_handler(
        "test.moonmod.entities-probe", *physics_query_operation,
        moonmod::Json::array({moonmod::Json{{"shape", "ray"}, {"center", {0.0, 0.0, 0.0}}, {"radius", 5.0}}}));
    EXPECT_EQ(malformed_spatial_query.status, moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult oversized_spatial_query = config.api_handler(
        "test.moonmod.entities-probe", *physics_query_operation,
        moonmod::Json::array({moonmod::Json{{"shape", "sphere"}, {"center", {0.0, 0.0, 0.0}}, {"radius", 10001.0}}}));
    EXPECT_EQ(oversized_spatial_query.status, moonmod::ApiCallStatus::InvalidArgument);

    const moonmod::ApiCallResult resolved = config.api_handler(
        "test.moonmod.entities-probe", *get_operation,
        moonmod::Json::array({moonmod::Json{{"id", handle}}}));
    ASSERT_EQ(resolved.status, moonmod::ApiCallStatus::Ok) << resolved.message;
    EXPECT_EQ(resolved.value.value("status", ""), "alive");
    EXPECT_EQ(resolved.value.at("health").at("current"), 45);

    unit->monster[2].movement[0] = 0.6f;
    unit->monster[2].movement[1] = 0.2f;
    unit->monster[2].movement[2] = 0.8f;
    unit->monster[2].movement_speed = 2.5f;
    const moonmod::ApiCallResult native_velocity = config.api_handler(
        "test.moonmod.entities-probe", *velocity_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}}}));
    ASSERT_EQ(native_velocity.status, moonmod::ApiCallStatus::Ok) << native_velocity.message;
    EXPECT_EQ(native_velocity.value.value("entity", ""), handle);
    EXPECT_EQ(native_velocity.value.value("source", ""), "native_movement");
    EXPECT_EQ(native_velocity.value.value("units", ""), "game_units_per_frame");
    EXPECT_EQ(native_velocity.value.value("semantics", ""), "native movement intent before collision resolution");
    EXPECT_FLOAT_EQ(native_velocity.value.at("velocity").at("x").get<float>(), 1.5f);
    EXPECT_FLOAT_EQ(native_velocity.value.at("velocity").at("y").get<float>(), 0.5f);
    EXPECT_FLOAT_EQ(native_velocity.value.at("velocity").at("z").get<float>(), 2.0f);

    unit->monster[2].knockback_direction[0] = -1.0f;
    unit->monster[2].knockback[0] = 3.0f;
    unit->monster[2].falls = 1;
    unit->monster[2].ground_distance = 2.0f;
    const moonmod::ApiCallResult knockback_velocity = config.api_handler(
        "test.moonmod.entities-probe", *velocity_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}}}));
    ASSERT_EQ(knockback_velocity.status, moonmod::ApiCallStatus::Ok) << knockback_velocity.message;
    EXPECT_EQ(knockback_velocity.value.value("source", ""), "knockback");
    EXPECT_FLOAT_EQ(knockback_velocity.value.at("velocity").at("x").get<float>(), -3.0f);
    EXPECT_FLOAT_EQ(knockback_velocity.value.at("velocity").at("y").get<float>(), -0.2f);
    EXPECT_FLOAT_EQ(knockback_velocity.value.at("velocity").at("z").get<float>(), 0.0f);

    unit->monster[2].stop_timer = 1;
    const moonmod::ApiCallResult stopped_velocity = config.api_handler(
        "test.moonmod.entities-probe", *velocity_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}}}));
    ASSERT_EQ(stopped_velocity.status, moonmod::ApiCallStatus::Ok) << stopped_velocity.message;
    EXPECT_EQ(stopped_velocity.value.value("source", ""), "stopped");
    EXPECT_FLOAT_EQ(stopped_velocity.value.at("velocity").at("x").get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(stopped_velocity.value.at("velocity").at("y").get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(stopped_velocity.value.at("velocity").at("z").get<float>(), 0.0f);
    const moonmod::ApiCallResult stopped_set = config.api_handler(
        "test.moonmod.entities-probe", *set_velocity_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}, {"velocity", moonmod::Json{{"x", 2.0}, {"y", 0.0}, {"z", -1.0}}}}}));
    EXPECT_EQ(stopped_set.status, moonmod::ApiCallStatus::Failed);
    unit->monster[2].stop_timer = 0;

    const moonmod::ApiCallResult set_velocity = config.api_handler(
        "test.moonmod.entities-probe", *set_velocity_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}, {"velocity", moonmod::Json{{"x", 2.0}, {"y", 0.0}, {"z", -1.0}}}}}));
    ASSERT_EQ(set_velocity.status, moonmod::ApiCallStatus::Ok) << set_velocity.message;
    EXPECT_TRUE(set_velocity.value.at("changed").get<bool>());
    EXPECT_EQ(set_velocity.value.value("source", ""), "native_movement");
    EXPECT_EQ(set_velocity.value.value("units", ""), "game_units_per_frame");
    EXPECT_FLOAT_EQ(set_velocity.value.at("previous_velocity").at("x").get<float>(), -3.0f);
    EXPECT_FLOAT_EQ(set_velocity.value.at("previous_velocity").at("y").get<float>(), -0.2f);
    EXPECT_FLOAT_EQ(set_velocity.value.at("velocity").at("x").get<float>(), 2.0f);
    EXPECT_FLOAT_EQ(set_velocity.value.at("velocity").at("y").get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(set_velocity.value.at("velocity").at("z").get<float>(), -1.0f);
    const moonmod::ApiCallResult velocity_readback = config.api_handler(
        "test.moonmod.entities-probe", *velocity_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}}}));
    ASSERT_EQ(velocity_readback.status, moonmod::ApiCallStatus::Ok) << velocity_readback.message;
    EXPECT_EQ(velocity_readback.value.value("source", ""), "native_movement");
    EXPECT_FLOAT_EQ(velocity_readback.value.at("velocity").at("x").get<float>(), 2.0f);
    EXPECT_FLOAT_EQ(velocity_readback.value.at("velocity").at("y").get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(velocity_readback.value.at("velocity").at("z").get<float>(), -1.0f);
    const moonmod::ApiCallResult stop_velocity = config.api_handler(
        "test.moonmod.entities-probe", *set_velocity_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}, {"velocity", moonmod::Json{{"x", 0.0}, {"y", 0.0}, {"z", 0.0}}}}}));
    ASSERT_EQ(stop_velocity.status, moonmod::ApiCallStatus::Ok) << stop_velocity.message;
    EXPECT_FLOAT_EQ(stop_velocity.value.at("velocity").at("x").get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(stop_velocity.value.at("velocity").at("y").get<float>(), 0.0f);
    EXPECT_FLOAT_EQ(stop_velocity.value.at("velocity").at("z").get<float>(), 0.0f);

    const moonmod::ApiCallResult malformed_velocity = config.api_handler(
        "test.moonmod.entities-probe", *velocity_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}, {"axis", "x"}}}));
    EXPECT_EQ(malformed_velocity.status, moonmod::ApiCallStatus::InvalidArgument);

    const moonmod::ApiCallResult malformed_set_velocity = config.api_handler(
        "test.moonmod.entities-probe", *set_velocity_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}, {"velocity", moonmod::Json{{"x", 1.0}, {"y", 2.0}}}}}));
    EXPECT_EQ(malformed_set_velocity.status, moonmod::ApiCallStatus::InvalidArgument);

    moonmod::ChronicleMonsterActorCreated(unit.get(), 2);
    const moonmod::ApiCallResult reused = config.api_handler(
        "test.moonmod.entities-probe", *get_operation,
        moonmod::Json::array({moonmod::Json{{"id", handle}}}));
    ASSERT_EQ(reused.status, moonmod::ApiCallStatus::Ok) << reused.message;
    EXPECT_EQ(reused.value.value("status", ""), "expired");
    const moonmod::ApiCallResult stale_set_velocity = config.api_handler(
        "test.moonmod.entities-probe", *set_velocity_operation,
        moonmod::Json::array({moonmod::Json{{"entity", handle}, {"velocity", moonmod::Json{{"x", 1.0}, {"y", 0.0}, {"z", 0.0}}}}}));
    EXPECT_EQ(stale_set_velocity.status, moonmod::ApiCallStatus::Failed);

    const moonmod::ApiCallResult unsupported_type = config.api_handler(
        "test.moonmod.entities-probe", *operation,
        moonmod::Json::array({moonmod::Json{{"type", "npc"}}}));
    EXPECT_EQ(unsupported_type.status, moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult unsupported_filter = config.api_handler(
        "test.moonmod.entities-probe", *operation,
        moonmod::Json::array({moonmod::Json{{"tag", "quest"}}}));
    EXPECT_EQ(unsupported_filter.status, moonmod::ApiCallStatus::InvalidArgument);

    const moonmod::ApiCallResult malformed_handle = config.api_handler(
        "test.moonmod.entities-probe", *get_operation,
        moonmod::Json::array({moonmod::Json{{"id", "chronicle:monster:2:bogus"}}}));
    EXPECT_EQ(malformed_handle.status, moonmod::ApiCallStatus::InvalidArgument);

    unit->monster[2].hp = 0;
    const moonmod::ApiCallResult empty = config.api_handler(
        "test.moonmod.entities-probe", *operation, moonmod::Json::array());
    ASSERT_EQ(empty.status, moonmod::ApiCallStatus::Ok) << empty.message;
    EXPECT_EQ(empty.value.at("count").get<int>(), 0);
    EXPECT_TRUE(empty.value.at("entities").empty());
    moonmod::ChronicleMonstersCleared(unit.get());
}

TEST(MoonModEntitiesApi, DespawnsOwnedLiveEnemyAndClearsNativeReferencesWithoutKillCredit) {
    CMonstorUnit *previous_unit = NowMonstorUnit;
    CCollisionData *previous_collision = NowColData;
    CSHOT_EFFECT_PACK *previous_shot_effect = NowShotEffect;
    CSaveData *previous_save = SaveData;
    const s32 previous_mode = mode;
    const s32 previous_dungeon = selectMapNo;
    struct Restore {
        CMonstorUnit *unit;
        CCollisionData *collision;
        CSHOT_EFFECT_PACK *shot_effect;
        CSaveData *save;
        s32 game_mode;
        s32 dungeon;
        ~Restore() {
            NowMonstorUnit = unit;
            NowColData = collision;
            NowShotEffect = shot_effect;
            SaveData = save;
            mode = game_mode;
            selectMapNo = dungeon;
        }
    } restore{previous_unit, previous_collision, previous_shot_effect, previous_save, previous_mode,
              previous_dungeon};

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *query = moonmod::FindApiOperation("entities.query");
    const moonmod::ApiOperationSpec *despawn = moonmod::FindApiOperation("entities.despawn");
    const moonmod::ApiOperationSpec *get = moonmod::FindApiOperation("entities.get");
    ASSERT_NE(query, nullptr);
    ASSERT_NE(despawn, nullptr);
    ASSERT_NE(get, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "entities.despawn" && capability.supported;
    }));

    auto unit = std::make_unique<CMonstorUnit>();
    auto save = std::make_unique<CSaveData>();
    auto collision = std::make_unique<CCollisionData>();
    auto shot_effect = std::make_unique<CSHOT_EFFECT_PACK>();
    NowMonstorUnit = unit.get();
    NowColData = collision.get();
    NowShotEffect = shot_effect.get();
    SaveData = save.get();
    save->Initialize();
    save->GetDngStatus()->cur_georama = 0;
    selectMapNo = 0;
    s16 kills_before[6][100]{};
    std::memcpy(kills_before, save->GetDngStatus()->kills, sizeof(kills_before));
    mode = GAME_MODE_DUNGEON;
    moonmod::ChronicleMonstersCleared(unit.get());

    constexpr int owned_slot = 5;
    constexpr int other_slot = 6;
    constexpr int mimic_slot = 7;
    for (int slot : {owned_slot, other_slot, mimic_slot}) {
        unit->monster[slot].state = 2;
        unit->monster[slot].hp = 40;
        unit->monster[slot].max_hp = 40;
        unit->monster[slot].attachment_kind = slot == mimic_slot ? 8 : 3;
        unit->monster[slot].revealed = -1;
        moonmod::ChronicleMonsterActorCreated(unit.get(), slot);
        moonmod::ChronicleMonsterActorOwned(unit.get(), slot, "test.moonmod.despawn-probe",
                                            "test.moonmod.despawn-probe:sentinel", 0x400);
    }
    unit->alive_count = 3;
    unit->script_state[owned_slot] = 1;
    unit->effect[owned_slot].timer[0] = 1;
    unit->effect[owned_slot].motion_start[0] = 1.0f;
    unit->effect2[owned_slot].active[0] = 1;
    unit->effect3[owned_slot].timer[0] = 1;
    unit->effect3[owned_slot].count = 1;
    unit->event[owned_slot].timer = 1;
    unit->event2[owned_slot].timer = 1;
    unit->sound[owned_slot].id[0] = 25;
    unit->sound[owned_slot].sequence_id = 41;
    collision->active[0] = 1;
    collision->hit[0].owner = owned_slot * 5 + 200;
    collision->hit[0].monster_no = -1;
    collision->active[1] = 1;
    collision->hit[1].owner = 1;
    collision->hit[1].monster_no = owned_slot;
    collision->active[2] = 1;
    collision->hit[2].owner = 1;
    collision->hit[2].monster_no = -1;
    shot_effect->effect[0].active[0] = 1;
    shot_effect->effect[0].user_id_2[0] = owned_slot;
    shot_effect->effect[0].active[1] = 1;
    shot_effect->effect[0].user_id_2[1] = other_slot;

    const moonmod::ApiCallResult listed = config.api_handler(
        "test.moonmod.despawn-probe", *query, moonmod::Json::array());
    ASSERT_EQ(listed.status, moonmod::ApiCallStatus::Ok) << listed.message;
    ASSERT_EQ(listed.value.at("count"), 3);
    std::array<std::string, 8> handles{};
    for (const moonmod::Json &entity : listed.value.at("entities")) {
        const int slot = entity.at("slot").get<int>();
        handles[slot] = entity.at("entity_id").get<std::string>();
    }
    ASSERT_FALSE(handles[owned_slot].empty());
    ASSERT_FALSE(handles[other_slot].empty());
    ASSERT_FALSE(handles[mimic_slot].empty());

    const moonmod::ApiCallResult foreign_owner = config.api_handler(
        "test.moonmod.other", *despawn, moonmod::Json::array({moonmod::Json{{"id", handles[other_slot]}}}));
    EXPECT_EQ(foreign_owner.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(unit->monster[other_slot].state, 2);

    const moonmod::ApiCallResult mimic = config.api_handler(
        "test.moonmod.despawn-probe", *despawn, moonmod::Json::array({moonmod::Json{{"id", handles[mimic_slot]}}}));
    EXPECT_EQ(mimic.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(unit->monster[mimic_slot].state, 2);

    const moonmod::ApiCallResult removed = config.api_handler(
        "test.moonmod.despawn-probe", *despawn, moonmod::Json::array({moonmod::Json{{"id", handles[owned_slot]}}}));
    ASSERT_EQ(removed.status, moonmod::ApiCallStatus::Ok) << removed.message;
    EXPECT_EQ(removed.value.value("status", ""), "despawned");
    EXPECT_TRUE(removed.value.value("changed", false));
    EXPECT_EQ(unit->monster[owned_slot].state, -1);
    EXPECT_EQ(unit->alive_count, 2);
    EXPECT_EQ(unit->script_state[owned_slot], 0);
    EXPECT_EQ(unit->effect[owned_slot].timer[0], 0);
    EXPECT_EQ(unit->effect2[owned_slot].active[0], 0);
    EXPECT_EQ(unit->effect3[owned_slot].timer[0], 0);
    EXPECT_EQ(unit->effect3[owned_slot].count, 0);
    EXPECT_EQ(unit->event[owned_slot].timer, 0);
    EXPECT_EQ(unit->event2[owned_slot].timer, 0);
    EXPECT_EQ(unit->sound[owned_slot].id[0], -1);
    EXPECT_EQ(collision->active[0], 0);
    EXPECT_EQ(collision->active[1], 0);
    EXPECT_EQ(collision->active[2], 1);
    EXPECT_EQ(shot_effect->effect[0].active[0], 0);
    EXPECT_EQ(shot_effect->effect[0].active[1], 1);
    const moonmod::ApiCallResult expired = config.api_handler(
        "test.moonmod.despawn-probe", *get, moonmod::Json::array({moonmod::Json{{"id", handles[owned_slot]}}}));
    ASSERT_EQ(expired.status, moonmod::ApiCallStatus::Ok) << expired.message;
    EXPECT_EQ(expired.value.value("status", ""), "expired");
    EXPECT_EQ(std::memcmp(kills_before, save->GetDngStatus()->kills, sizeof(kills_before)), 0);

    ASSERT_TRUE(static_cast<bool>(config.api_owner_unloaded));
    config.api_owner_unloaded("test.moonmod.despawn-probe");
    EXPECT_EQ(unit->monster[other_slot].state, -1);
    EXPECT_EQ(unit->alive_count, 1);
    EXPECT_EQ(unit->monster[mimic_slot].state, 2);
    EXPECT_EQ(std::memcmp(kills_before, save->GetDngStatus()->kills, sizeof(kills_before)), 0);
    moonmod::ChronicleMonstersCleared(unit.get());
}

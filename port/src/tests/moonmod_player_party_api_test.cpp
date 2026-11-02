#include <gtest/gtest.h>

#include <memory>

#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/runtime.hpp"
#include "dun/gameloop.hpp"
#include "character.hpp"
#include "btactstatus.hpp"
#include "main.hpp"
#include "savedata.hpp"
#include "userstatus.hpp"

namespace dc::game {}
using namespace dc::game;


namespace {

int g_native_party_switch_target = -1;
int g_native_party_switch_calls = 0;

void FakeNativePartySwitch(int character_id, int reload) {
    EXPECT_EQ(reload, 0);
    g_native_party_switch_target = character_id;
    ++g_native_party_switch_calls;
    SaveData->GetDngStatus()->cur_chara = static_cast<s8>(character_id);
}

} // namespace

TEST(MoonModPlayerPartyApi, ReadsHealsAndDamagesActiveMemberAndListsJoinedRoster) {
    const s32 previous_mode = mode;
    CSaveData *previous_save = SaveData;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->party_size = 3;
    status->cur_chara = CHARA_XIAO;
    status->max_hp[CHARA_XIAO] = 60;
    status->hp[CHARA_XIAO] = 34;

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *player_get = moonmod::FindApiOperation("player.get");
    const moonmod::ApiOperationSpec *player_heal = moonmod::FindApiOperation("player.heal");
    const moonmod::ApiOperationSpec *player_damage = moonmod::FindApiOperation("player.damage");
    const moonmod::ApiOperationSpec *party_list = moonmod::FindApiOperation("party.list");
    const moonmod::ApiOperationSpec *party_active = moonmod::FindApiOperation("party.active");
    const moonmod::ApiOperationSpec *characters_list = moonmod::FindApiOperation("characters.list");
    const moonmod::ApiOperationSpec *characters_get = moonmod::FindApiOperation("characters.get");
    const moonmod::ApiOperationSpec *world_get = moonmod::FindApiOperation("world.get");
    ASSERT_NE(player_get, nullptr);
    ASSERT_NE(player_heal, nullptr);
    ASSERT_NE(player_damage, nullptr);
    ASSERT_NE(party_list, nullptr);
    ASSERT_NE(party_active, nullptr);
    ASSERT_NE(characters_list, nullptr);
    ASSERT_NE(characters_get, nullptr);
    ASSERT_NE(world_get, nullptr);

    const moonmod::ApiCallResult player = config.api_handler("test.moonmod.chronicle-player", *player_get,
                                                             moonmod::Json::array());
    ASSERT_EQ(player.status, moonmod::ApiCallStatus::Ok) << player.message;
    EXPECT_EQ(player.value.value("character", ""), "xiao");
    EXPECT_EQ(player.value.value("hp", -1), 34);
    EXPECT_EQ(player.value.value("max_hp", -1), 60);

    const moonmod::ApiCallResult healed = config.api_handler(
        "test.moonmod.chronicle-player", *player_heal, moonmod::Json::array({{{"amount", 25}}}));
    ASSERT_EQ(healed.status, moonmod::ApiCallStatus::Ok) << healed.message;
    EXPECT_EQ(healed.value.value("healed", -1), 25);
    EXPECT_EQ(status->hp[CHARA_XIAO], 59);

    status->hp[CHARA_XIAO] = 59;
    const moonmod::ApiCallResult clamped = config.api_handler(
        "test.moonmod.chronicle-player", *player_heal, moonmod::Json::array({{{"amount", 5}}}));
    ASSERT_EQ(clamped.status, moonmod::ApiCallStatus::Ok) << clamped.message;
    EXPECT_EQ(clamped.value.value("healed", -1), 1);
    EXPECT_EQ(status->hp[CHARA_XIAO], 60);

    const moonmod::ApiCallResult invalid = config.api_handler(
        "test.moonmod.chronicle-player", *player_heal,
        moonmod::Json::array({{{"amount", 5}, {"character", "townsfolk"}}}));
    EXPECT_EQ(invalid.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(status->hp[CHARA_XIAO], 60);

    status->hp[CHARA_XIAO] = 35;
    const moonmod::ApiCallResult damaged = config.api_handler(
        "test.moonmod.chronicle-player", *player_damage, moonmod::Json::array({{{"amount", 25}}}));
    ASSERT_EQ(damaged.status, moonmod::ApiCallStatus::Ok) << damaged.message;
    EXPECT_EQ(damaged.value.value("damaged", -1), 25);
    EXPECT_EQ(status->hp[CHARA_XIAO], 10);

    const moonmod::ApiCallResult clamped_damage = config.api_handler(
        "test.moonmod.chronicle-player", *player_damage, moonmod::Json::array({{{"amount", 100}}}));
    ASSERT_EQ(clamped_damage.status, moonmod::ApiCallStatus::Ok) << clamped_damage.message;
    EXPECT_EQ(clamped_damage.value.value("damaged", -1), 10);
    EXPECT_EQ(status->hp[CHARA_XIAO], 0);

    status->max_hp[CHARA_GORO] = 80;
    status->hp[CHARA_GORO] = 20;
    const moonmod::ApiCallResult bench_heal = config.api_handler(
        "test.moonmod.chronicle-player", *player_heal,
        moonmod::Json::array({{{"amount", 10}, {"character", "goro"}}}));
    ASSERT_EQ(bench_heal.status, moonmod::ApiCallStatus::Ok) << bench_heal.message;
    EXPECT_EQ(bench_heal.value.value("character", ""), "goro");
    EXPECT_EQ(bench_heal.value.value("healed", -1), 10);
    EXPECT_EQ(status->hp[CHARA_GORO], 30);
    EXPECT_EQ(status->cur_chara, CHARA_XIAO);

    status->max_hp[CHARA_TOAN] = 70;
    status->hp[CHARA_TOAN] = 25;
    const moonmod::ApiCallResult bench_damage = config.api_handler(
        "test.moonmod.chronicle-player", *player_damage,
        moonmod::Json::array({{{"amount", 12}, {"character", "toan"}}}));
    ASSERT_EQ(bench_damage.status, moonmod::ApiCallStatus::Ok) << bench_damage.message;
    EXPECT_EQ(bench_damage.value.value("character", ""), "toan");
    EXPECT_EQ(bench_damage.value.value("damaged", -1), 12);
    EXPECT_EQ(status->hp[CHARA_TOAN], 13);
    EXPECT_EQ(status->cur_chara, CHARA_XIAO);

    const moonmod::ApiCallResult invalid_damage = config.api_handler(
        "test.moonmod.chronicle-player", *player_damage,
        moonmod::Json::array({{{"amount", 0}, {"character", "goro"}}}));
    EXPECT_EQ(invalid_damage.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(status->hp[CHARA_XIAO], 0);

    const moonmod::ApiCallResult roster = config.api_handler("test.moonmod.chronicle-player", *party_list,
                                                             moonmod::Json::array());
    ASSERT_EQ(roster.status, moonmod::ApiCallStatus::Ok) << roster.message;
    ASSERT_EQ(roster.value.size(), 3u);
    EXPECT_EQ(roster.value[0].value("id", ""), "toan");
    EXPECT_EQ(roster.value[1].value("id", ""), "xiao");
    EXPECT_TRUE(roster.value[1].value("active", false));
    EXPECT_EQ(roster.value[2].value("id", ""), "goro");

    const moonmod::ApiCallResult active = config.api_handler("test.moonmod.chronicle-player", *party_active,
                                                             moonmod::Json::array());
    ASSERT_EQ(active.status, moonmod::ApiCallStatus::Ok) << active.message;
    EXPECT_EQ(active.value.value("id", ""), "xiao");
    EXPECT_EQ(active.value.value("character_id", -1), CHARA_XIAO);

    status->hp[CHARA_XIAO] = 34;
    const moonmod::ApiCallResult characters = config.api_handler(
        "test.moonmod.chronicle-player", *characters_list, moonmod::Json::array());
    ASSERT_EQ(characters.status, moonmod::ApiCallStatus::Ok) << characters.message;
    ASSERT_EQ(characters.value.size(), 6u);
    EXPECT_EQ(characters.value[0].value("id", ""), "toan");
    EXPECT_TRUE(characters.value[0].value("in_party", false));
    EXPECT_EQ(characters.value[1].value("id", ""), "xiao");
    EXPECT_TRUE(characters.value[1].value("active", false));
    EXPECT_EQ(characters.value[1].value("hp", -1), 34);
    EXPECT_EQ(characters.value[1].value("max_hp", -1), 60);
    EXPECT_FALSE(characters.value[4].value("in_party", true));

    const moonmod::ApiCallResult character = config.api_handler(
        "test.moonmod.chronicle-player", *characters_get, moonmod::Json::array({{{"id", "xiao"}}}));
    ASSERT_EQ(character.status, moonmod::ApiCallStatus::Ok) << character.message;
    EXPECT_EQ(character.value.value("id", ""), "xiao");
    EXPECT_EQ(character.value.value("character_id", -1), CHARA_XIAO);
    const moonmod::ApiCallResult invalid_character = config.api_handler(
        "test.moonmod.chronicle-player", *characters_get, moonmod::Json::array({"enemy:unknown"}));
    EXPECT_EQ(invalid_character.status, moonmod::ApiCallStatus::InvalidArgument);

    status->cur_georama = 3;
    status->cur_floor = -1;
    mode = GAME_MODE_EDIT;
    const moonmod::ApiCallResult town_world = config.api_handler("test.moonmod.chronicle-player", *world_get,
                                                                 moonmod::Json::array());
    ASSERT_EQ(town_world.status, moonmod::ApiCallStatus::Ok) << town_world.message;
    EXPECT_EQ(town_world.value.value("context", ""), "georama");
    EXPECT_EQ(town_world.value.value("area_id", -1), 3);
    EXPECT_EQ(town_world.value.value("map_id", -1), 3);
    EXPECT_TRUE(town_world.value.at("dungeon_id").is_null());
    EXPECT_TRUE(town_world.value.at("floor_id").is_null());

    const s32 previous_dungeon = selectMapNo;
    selectMapNo = 4;
    mode = GAME_MODE_DUNGEON;
    status->cur_georama = 0;
    status->cur_floor = 9;
    const moonmod::ApiCallResult dungeon_world = config.api_handler("test.moonmod.chronicle-player", *world_get,
                                                                    moonmod::Json::array());
    ASSERT_EQ(dungeon_world.status, moonmod::ApiCallStatus::Ok) << dungeon_world.message;
    EXPECT_EQ(dungeon_world.value.value("context", ""), "dungeon");
    EXPECT_EQ(dungeon_world.value.value("map_id", -1), 4);
    EXPECT_EQ(dungeon_world.value.value("dungeon_id", -1), 4);
    EXPECT_EQ(dungeon_world.value.value("floor_id", -1), 9);
    EXPECT_EQ(dungeon_world.value.value("area_id", -1), 0);
    selectMapNo = previous_dungeon;
    mode = previous_mode;

    SaveData = previous_save;
}

TEST(MoonModPlayerPartyApi, SwitchesThroughNativeRoutineAndEnforcesDungeonPartyRules) {
    const s32 previous_mode = mode;
    const s32 previous_map = selectMapNo;
    CSaveData *previous_save = SaveData;
    const BT_ACT_STATUS previous_action = BtActStatus;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->party_size = 3;
    status->cur_georama = 0;
    status->cur_floor = 1;
    status->cur_chara = CHARA_TOAN;
    status->hp[CHARA_XIAO] = 20;
    status->res_limit_zone_current = -1;
    mode = GAME_MODE_DUNGEON;
    selectMapNo = 0;
    BtActStatus = {};
    g_native_party_switch_target = -1;
    g_native_party_switch_calls = 0;
    moonmod::SetChronicleCharacterSwitchRoutineForTests(FakeNativePartySwitch);

    struct RestoreState {
        s32 mode;
        s32 map;
        CSaveData *save;
        BT_ACT_STATUS action;
        ~RestoreState() {
            moonmod::SetChronicleCharacterSwitchRoutineForTests(nullptr);
            ::mode = this->mode;
            selectMapNo = this->map;
            SaveData = this->save;
            BtActStatus = this->action;
        }
    } restore{previous_mode, previous_map, previous_save, previous_action};

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *set_active = moonmod::FindApiOperation("party.set_active");
    ASSERT_NE(set_active, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "party.set_active" && capability.supported && capability.reason.empty();
    }));

    const moonmod::ApiCallResult switched = config.api_handler(
        "test.moonmod.party-switch", *set_active, moonmod::Json::array({{{"id", "xiao"}}}));
    ASSERT_EQ(switched.status, moonmod::ApiCallStatus::Ok) << switched.message;
    EXPECT_TRUE(switched.value.value("changed", false));
    EXPECT_EQ(switched.value.value("previous", ""), "toan");
    EXPECT_EQ(switched.value.value("active", ""), "xiao");
    EXPECT_EQ(status->cur_chara, CHARA_XIAO);
    EXPECT_EQ(g_native_party_switch_target, CHARA_XIAO);
    EXPECT_EQ(g_native_party_switch_calls, 1);

    const moonmod::ApiCallResult unchanged = config.api_handler(
        "test.moonmod.party-switch", *set_active, moonmod::Json::array({{{"id", "xiao"}}}));
    ASSERT_EQ(unchanged.status, moonmod::ApiCallStatus::Ok) << unchanged.message;
    EXPECT_FALSE(unchanged.value.value("changed", true));
    EXPECT_EQ(g_native_party_switch_calls, 1);

    const moonmod::ApiCallResult unjoined = config.api_handler(
        "test.moonmod.party-switch", *set_active, moonmod::Json::array({{{"id", "ruby"}}}));
    EXPECT_EQ(unjoined.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(status->cur_chara, CHARA_XIAO);
    EXPECT_EQ(g_native_party_switch_calls, 1);

    status->ailments[CHARA_GORO] = AILMENT_KNOCKED_OUT;
    const moonmod::ApiCallResult knocked_out = config.api_handler(
        "test.moonmod.party-switch", *set_active, moonmod::Json::array({{{"id", "goro"}}}));
    EXPECT_EQ(knocked_out.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(g_native_party_switch_calls, 1);
    status->ailments[CHARA_GORO] = 0;

    status->res_limit_zone_current = 2;
    const moonmod::ApiCallResult restricted_zone = config.api_handler(
        "test.moonmod.party-switch", *set_active, moonmod::Json::array({{{"id", "goro"}}}));
    EXPECT_EQ(restricted_zone.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(g_native_party_switch_calls, 1);
    status->res_limit_zone_current = -1;

    BtActStatus.action_on = 1;
    const moonmod::ApiCallResult busy = config.api_handler(
        "test.moonmod.party-switch", *set_active, moonmod::Json::array({{{"id", "goro"}}}));
    EXPECT_EQ(busy.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(g_native_party_switch_calls, 1);
    BtActStatus.action_on = 0;

    mode = GAME_MODE_EDIT;
    const moonmod::ApiCallResult outside_dungeon = config.api_handler(
        "test.moonmod.party-switch", *set_active, moonmod::Json::array({{{"id", "goro"}}}));
    EXPECT_EQ(outside_dungeon.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(g_native_party_switch_calls, 1);

    const moonmod::ApiCallResult malformed = config.api_handler(
        "test.moonmod.party-switch", *set_active, moonmod::Json::array({{{"id", "not-a-character"}}}));
    EXPECT_EQ(malformed.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(g_native_party_switch_calls, 1);
}

TEST(MoonModPlayerPartyApi, IncludesNativePositionInActiveDungeonPlayerSnapshot) {
    struct Restore {
        s32 mode;
        s32 dungeon;
        CSaveData *save;
        float position[4];
        ~Restore() {
            CharaMain.SetPosition(position);
            SaveData = save;
            ::mode = mode;
            selectMapNo = dungeon;
        }
    } restore{mode, selectMapNo, SaveData, {}};
    CharaMain.GetPosition(restore.position);

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->cur_georama = 0;
    selectMapNo = 0;
    mode = GAME_MODE_DUNGEON;
    float expected_position[4] = {11.0f, 22.0f, 33.0f, 1.0f};
    CharaMain.SetPosition(expected_position);

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *get = moonmod::FindApiOperation("player.get");
    ASSERT_NE(get, nullptr);
    const moonmod::ApiCallResult player = config.api_handler(
        "test.moonmod.chronicle-player-position", *get, moonmod::Json::array());
    ASSERT_EQ(player.status, moonmod::ApiCallStatus::Ok) << player.message;
    ASSERT_TRUE(player.value.contains("position"));
    EXPECT_FLOAT_EQ(player.value.at("position").at("x").get<float>(), expected_position[0]);
    EXPECT_FLOAT_EQ(player.value.at("position").at("y").get<float>(), expected_position[1]);
    EXPECT_FLOAT_EQ(player.value.at("position").at("z").get<float>(), expected_position[2]);

}

TEST(MoonModPlayerPartyApi, SetsExactHpForActiveAndNamedCharactersWithinNativeRange) {
    CSaveData *previous_save = SaveData;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->party_size = 3;
    status->cur_chara = CHARA_XIAO;
    status->max_hp[CHARA_XIAO] = 60;
    status->hp[CHARA_XIAO] = 34;
    status->max_hp[CHARA_TOAN] = 80;
    status->hp[CHARA_TOAN] = 20;
    status->water_max[CHARA_XIAO] = 120.0f;
    status->water_now[CHARA_XIAO] = 30.0f;

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *set = moonmod::FindApiOperation("player.set");
    ASSERT_NE(set, nullptr);

    status->next_hp[CHARA_XIAO] = 42;
    status->life_step[CHARA_XIAO] = 1;
    const moonmod::ApiCallResult exact = config.api_handler(
        "test.moonmod.chronicle-player-set", *set, moonmod::Json::array({{{"hp", 0}}}));
    ASSERT_EQ(exact.status, moonmod::ApiCallStatus::Ok) << exact.message;
    EXPECT_EQ(exact.value.value("previous_hp", -1), 42);
    EXPECT_EQ(exact.value.value("hp", -1), 0);
    EXPECT_EQ(status->hp[CHARA_XIAO], 0);
    EXPECT_EQ(status->life_step[CHARA_XIAO], 0);

    const moonmod::ApiCallResult named = config.api_handler(
        "test.moonmod.chronicle-player-set", *set,
        moonmod::Json::array({{{"hp", 77}, {"character", "toan"}}}));
    ASSERT_EQ(named.status, moonmod::ApiCallStatus::Ok) << named.message;
    EXPECT_EQ(named.value.value("character", ""), "toan");
    EXPECT_EQ(status->hp[CHARA_TOAN], 77);
    EXPECT_EQ(status->cur_chara, CHARA_XIAO);

    const moonmod::ApiCallResult too_high = config.api_handler(
        "test.moonmod.chronicle-player-set", *set,
        moonmod::Json::array({{{"hp", 81}, {"character", "toan"}}}));
    EXPECT_EQ(too_high.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(status->hp[CHARA_TOAN], 77);
    const moonmod::ApiCallResult unsupported_field = config.api_handler(
        "test.moonmod.chronicle-player-set", *set,
        moonmod::Json::array({{{"max_water", 90}}}));
    EXPECT_EQ(unsupported_field.status, moonmod::ApiCallStatus::InvalidArgument);
    // max_hp is its own setter: it raises the maximum, and lowering it clamps current HP
    const int toan_max = status->max_hp[CHARA_TOAN];
    const moonmod::ApiCallResult raised = config.api_handler(
        "test.moonmod.chronicle-player-set", *set,
        moonmod::Json::array({{{"max_hp", 90}, {"character", "toan"}}}));
    ASSERT_EQ(raised.status, moonmod::ApiCallStatus::Ok) << raised.message;
    EXPECT_EQ(status->max_hp[CHARA_TOAN], 90);
    EXPECT_EQ(status->hp[CHARA_TOAN], 77);
    ASSERT_EQ(config.api_handler("test.moonmod.chronicle-player-set", *set,
                                 moonmod::Json::array({{{"max_hp", 50}, {"character", "toan"}}})).status,
              moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(status->hp[CHARA_TOAN], 50);
    status->max_hp[CHARA_TOAN] = static_cast<s16>(toan_max);
    status->hp[CHARA_TOAN] = 77;
    const moonmod::ApiCallResult fractional = config.api_handler(
        "test.moonmod.chronicle-player-set", *set,
        moonmod::Json::array({{{"hp", 10.5}}}));
    EXPECT_EQ(fractional.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(status->hp[CHARA_TOAN], 77);

    status->drink_next[CHARA_XIAO] = 50;
    status->drink_step[CHARA_XIAO] = 1;
    const moonmod::ApiCallResult water = config.api_handler(
        "test.moonmod.chronicle-player-set", *set, moonmod::Json::array({{{"water", 75}}}));
    ASSERT_EQ(water.status, moonmod::ApiCallStatus::Ok) << water.message;
    EXPECT_EQ(water.value.value("character", ""), "xiao");
    EXPECT_FLOAT_EQ(water.value.value("previous_water", -1.0f), 30.0f);
    EXPECT_FLOAT_EQ(water.value.value("water", -1.0f), 75.0f);
    EXPECT_FLOAT_EQ(status->water_now[CHARA_XIAO], 75.0f);
    EXPECT_EQ(status->drink_next[CHARA_XIAO], 75);
    EXPECT_EQ(status->drink_step[CHARA_XIAO], 0);

    const moonmod::ApiCallResult too_much_water = config.api_handler(
        "test.moonmod.chronicle-player-set", *set, moonmod::Json::array({{{"water", 121}}}));
    EXPECT_EQ(too_much_water.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_FLOAT_EQ(status->water_now[CHARA_XIAO], 75.0f);
    const moonmod::ApiCallResult fractional_water = config.api_handler(
        "test.moonmod.chronicle-player-set", *set, moonmod::Json::array({{{"water", 75.5}}}));
    EXPECT_EQ(fractional_water.status, moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult multiple_gauges = config.api_handler(
        "test.moonmod.chronicle-player-set", *set, moonmod::Json::array({{{"hp", 50}, {"water", 50}}}));
    EXPECT_EQ(multiple_gauges.status, moonmod::ApiCallStatus::InvalidArgument);
    const auto *get = moonmod::FindApiOperation("player.get");
    ASSERT_NE(get, nullptr);
    const moonmod::ApiCallResult current_player = config.api_handler(
        "test.moonmod.chronicle-player-set", *get, moonmod::Json::array());
    ASSERT_EQ(current_player.status, moonmod::ApiCallStatus::Ok) << current_player.message;
    EXPECT_FLOAT_EQ(current_player.value.value("water", -1.0f), 75.0f);
    EXPECT_FLOAT_EQ(current_player.value.value("max_water", -1.0f), 120.0f);

    SaveData = previous_save;
}

TEST(MoonModPlayerPartyApi, AppliesNativeStatusRulesAndRemovesAilments) {
    CSaveData *previous_save = SaveData;
    CUserStatus *previous_user_status = UserStatus;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->cur_chara = CHARA_TOAN;
    UserStatus = static_cast<CUserStatus *>(status);

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *add = moonmod::FindApiOperation("player.status_add");
    const moonmod::ApiOperationSpec *remove = moonmod::FindApiOperation("player.status_remove");
    ASSERT_NE(add, nullptr);
    ASSERT_NE(remove, nullptr);

    const moonmod::ApiCallResult applied = config.api_handler(
        "test.moonmod.status", *add, moonmod::Json::array({{{"status", "stamina"}}}));
    ASSERT_EQ(applied.status, moonmod::ApiCallStatus::Ok) << applied.message;
    EXPECT_EQ(applied.value.value("character", ""), "toan");
    EXPECT_EQ(applied.value.value("duration_frames", -1), 1800);
    EXPECT_NE(UserStatus->ailments[CHARA_TOAN] & AILMENT_STAMINA, 0);

    const moonmod::ApiCallResult blocked = config.api_handler(
        "test.moonmod.status", *add, moonmod::Json::array({{{"status", "poison"}}}));
    EXPECT_EQ(blocked.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(UserStatus->ailments[CHARA_TOAN] & AILMENT_POISON, 0);

    const moonmod::ApiCallResult invalid = config.api_handler(
        "test.moonmod.status", *add, moonmod::Json::array({{{"status", "knockout"}}}));
    EXPECT_EQ(invalid.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_NE(UserStatus->ailments[CHARA_TOAN] & AILMENT_STAMINA, 0);

    const moonmod::ApiCallResult removed = config.api_handler(
        "test.moonmod.status", *remove, moonmod::Json::array({{{"status", "stamina"}}}));
    ASSERT_EQ(removed.status, moonmod::ApiCallStatus::Ok) << removed.message;
    EXPECT_TRUE(removed.value.value("removed", false));
    EXPECT_FALSE(removed.value.value("active", true));
    EXPECT_EQ(UserStatus->ailments[CHARA_TOAN], 0);
    EXPECT_EQ(UserStatus->ailment_frames[CHARA_TOAN], 0);

    UserStatus = previous_user_status;
    SaveData = previous_save;
}

TEST(MoonModPlayerPartyApi, ReportsTheEquippedWeaponWithItsEnduranceAndDurability) {
    CSaveData *previous_save = SaveData;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->party_size = 1;
    status->cur_chara = CHARA_TOAN;
    status->equipped_weapon_slot[CHARA_TOAN] = 0;
    WEAPON_HAVE &weapon = status->chara_weapons[CHARA_TOAN][0];
    weapon.item_no = ITEM_WEAPON_START;
    weapon.attack = 12;
    weapon.endurance = 7;
    weapon.durability = 30;
    weapon.durability_f = 4.5f;

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *player_get = moonmod::FindApiOperation("player.get");
    ASSERT_NE(player_get, nullptr);
    const moonmod::ApiCallResult player = config.api_handler("test.moonmod.chronicle-player", *player_get,
                                                             moonmod::Json::array());
    ASSERT_EQ(player.status, moonmod::ApiCallStatus::Ok) << player.message;
    ASSERT_TRUE(player.value.contains("weapon"));
    const moonmod::Json &reported = player.value.at("weapon");
    EXPECT_EQ(reported.value("attack", -1), 12);
    // The Gauntlet's Shatter needs endurance to know how hard the next hit can wear.
    EXPECT_EQ(reported.value("endurance", -1), 7);
    EXPECT_FLOAT_EQ(reported.value("durability", -1.0f), 4.5f);
    EXPECT_EQ(reported.value("durability_max", -1), 30);

    SaveData = previous_save;
}

#include <gtest/gtest.h>

#include <memory>
#include <ranges>

#include "app/gameloop.hpp"
#include "dun/gameloop.hpp"
#include "gamemode.hpp"
#include "main.hpp"
#include "mainselect.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/game_hooks.hpp"
#include "savedata.hpp"

namespace dc::game {}
using namespace dc::game;


namespace {

struct SavedGameState {
    CSaveData *save = SaveData;
    int current_mode = mode;
    int map_no = MapNo;
    int next_map_no = NextMapNo;
    int start_event_no = StartEventNo;
    int selected_dungeon = selectMapNo;

    ~SavedGameState() {
        SaveData = save;
        mode = current_mode;
        MapNo = map_no;
        NextMapNo = next_map_no;
        StartEventNo = start_event_no;
        selectMapNo = selected_dungeon;
    }
};

} // namespace

TEST(MoonModWorldTransitionApi, QueuesOnlyNativeSettlementRoutesFromPlayableScenes) {
    SavedGameState saved_state;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation("world.transition");
    ASSERT_NE(operation, nullptr);
    const auto capability = std::ranges::find(config.api_capabilities, "world.transition",
                                               &moonmod::ApiCapability::path);
    ASSERT_NE(capability, config.api_capabilities.end());
    EXPECT_TRUE(capability->supported);
    EXPECT_TRUE(capability->reason.empty());

    const moonmod::Json norune = moonmod::Json::array({{{"id", "chronicle:location:norune-village"}}});
    mode = GAME_MODE_TITLE;
    MapNo = 800;
    NextMapNo = -1;
    const moonmod::ApiCallResult title = config.api_handler("test.moonmod.transition", *operation, norune);
    EXPECT_EQ(title.status, moonmod::ApiCallStatus::Failed);
    EXPECT_NE(title.message.find("active town or dungeon"), std::string::npos);
    EXPECT_EQ(NextMapNo, -1);

    mode = GAME_MODE_DUNGEON;
    MapNo = 200;
    const moonmod::ApiCallResult queued = config.api_handler("test.moonmod.transition", *operation, norune);
    ASSERT_EQ(queued.status, moonmod::ApiCallStatus::Ok) << queued.message;
    EXPECT_TRUE(queued.value.at("queued").get<bool>());
    EXPECT_TRUE(queued.value.at("changed").get<bool>());
    EXPECT_EQ(queued.value.at("native_map_id"), 0);
    EXPECT_EQ(NextMapNo, 0);
    EXPECT_EQ(StartEventNo, -1);

    NextMapNo = -1;
    mode = GAME_MODE_EDIT;
    MapNo = 0;
    const moonmod::ApiCallResult current_town = config.api_handler("test.moonmod.transition", *operation, norune);
    ASSERT_EQ(current_town.status, moonmod::ApiCallStatus::Ok) << current_town.message;
    EXPECT_FALSE(current_town.value.at("queued").get<bool>());
    EXPECT_FALSE(current_town.value.at("changed").get<bool>());
    EXPECT_EQ(NextMapNo, -1);

    MapNo = 11;
    const moonmod::ApiCallResult field = config.api_handler("test.moonmod.transition", *operation, norune);
    EXPECT_EQ(field.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(NextMapNo, -1);

    mode = GAME_MODE_DUNGEON;
    const moonmod::ApiCallResult dungeon = config.api_handler(
        "test.moonmod.transition", *operation,
        moonmod::Json::array({{{"id", "chronicle:location:divine-beast-cave"}}}));
    EXPECT_EQ(dungeon.status, moonmod::ApiCallStatus::Failed);
    EXPECT_NE(dungeon.message.find("registered mod dungeon floors"), std::string::npos);

    NextMapNo = 2;
    const moonmod::ApiCallResult pending = config.api_handler("test.moonmod.transition", *operation, norune);
    EXPECT_EQ(pending.status, moonmod::ApiCallStatus::Failed);
    EXPECT_NE(pending.message.find("already pending"), std::string::npos);
    EXPECT_EQ(NextMapNo, 2);
}

TEST(MoonModWorldTransitionApi, QueuesRegisteredDungeonFloorForTheNativeEntranceMenu) {
    SavedGameState saved_state;
    moonmod::ResetChronicleDungeonsForTests();
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *register_operation = moonmod::FindApiOperation("dungeons.register");
    const auto *transition_operation = moonmod::FindApiOperation("world.transition");
    ASSERT_NE(register_operation, nullptr);
    ASSERT_NE(transition_operation, nullptr);
    const moonmod::ApiCallResult registered = config.api_handler(
        "test.moonmod.transition",
        *register_operation,
        moonmod::Json::array({{{"id", "moon-vault"}, {"name", "Moon Vault"}, {"dungeon", 0},
                              {"floors", {{{"id", "annex"}, {"source_floor", 2}}}}}}));
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    const std::string floor_id = registered.value.at("floors").at(0).at("id").get<std::string>();

    mode = GAME_MODE_EDIT;
    MapNo = 0;
    NextMapNo = -1;
    const moonmod::ApiCallResult queued = config.api_handler(
        "test.moonmod.transition", *transition_operation, moonmod::Json::array({{{"id", floor_id}}}));
    ASSERT_EQ(queued.status, moonmod::ApiCallStatus::Ok) << queued.message;
    EXPECT_EQ(queued.value.at("id"), floor_id);
    EXPECT_EQ(queued.value.at("context"), "dungeon_floor");
    EXPECT_EQ(queued.value.at("native_dungeon_id"), 0);
    EXPECT_EQ(queued.value.at("native_floor_id"), 15);
    EXPECT_TRUE(queued.value.at("queued").get<bool>());
    EXPECT_EQ(NextMapNo, 200);

    mode = GAME_MODE_DUNGEON;
    selectMapNo = 0;
    ASSERT_TRUE(moonmod::PreparePendingChronicleDungeonEntry());
    EXPECT_EQ(moonmod::TakePendingChronicleDungeonFloor(1), -1);
    EXPECT_EQ(moonmod::TakePendingChronicleDungeonFloor(0), 15);
    save->GetDngStatus()->SetNowFloor(15);
    save->GetDngStatus()->prev_floor = -1;
    save->GetDngStatus()->cur_georama = 0;

    const auto *world_operation = moonmod::FindApiOperation("world.get");
    ASSERT_NE(world_operation, nullptr);
    const moonmod::ApiCallResult world = config.api_handler(
        "test.moonmod.transition", *world_operation, moonmod::Json::array());
    ASSERT_EQ(world.status, moonmod::ApiCallStatus::Ok) << world.message;
    EXPECT_EQ(world.value.at("context"), "dungeon");
    EXPECT_EQ(world.value.at("dungeon_id"), 0);
    EXPECT_EQ(world.value.at("floor_id"), 15);

    moonmod::ResetChronicleDungeonsForTests();
}

TEST(MoonModWorldTransitionApi, TheExitGateChainsRegisteredFloorsAndStopsAtTheLast) {
    moonmod::ResetChronicleDungeonsForTests();
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *register_operation = moonmod::FindApiOperation("dungeons.register");
    ASSERT_NE(register_operation, nullptr);

    // Retail: the exit's next floor is never past the boss floor, so nothing changes.
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 3), 3);
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 14), 14);

    const moonmod::ApiCallResult registered = config.api_handler(
        "test.moonmod.transition",
        *register_operation,
        moonmod::Json::array({{{"id", "moon-vault"}, {"name", "Moon Vault"}, {"dungeon", 0},
                              {"floors", {{{"id", "annex"}, {"source_floor", 2}}, {{"id", "depths"}, {"source_floor", 5}}}}}}));
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;

    // Floor 15's exit goes on to the registered floor 16; floor 16's exit stays on 16 instead of leaving the tables.
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 16), 16);
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 17), 16);
    // Other dungeons keep their own last floor.
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(1, 16), 16);
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(-1, 40), 40);

    moonmod::ResetChronicleDungeonsForTests();
}

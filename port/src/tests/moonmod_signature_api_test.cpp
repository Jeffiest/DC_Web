#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/runtime.hpp"
#include "dun/gameloop.hpp"
#include "dungeonmap.hpp"
#include "gamemode.hpp"
#include "itemdata.hpp"
#include "main.hpp"
#include "mainselect.hpp"
#include "monstorunit.hpp"
#include "savedata.hpp"
#include "userstatus.hpp"

namespace dc::game {}
using namespace dc::game;


namespace {

namespace fs = std::filesystem;

void WriteFile(const fs::path &path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(output.good()) << path.string();
    output << content;
}

struct TempRoot {
    fs::path path;
    TempRoot() {
        path = fs::temp_directory_path() /
               ("moonmod-chronicle-signature-" + std::to_string(reinterpret_cast<std::uintptr_t>(this)));
        fs::create_directories(path);
    }
    ~TempRoot() {
        std::error_code error;
        fs::remove_all(path, error);
    }
};

moonmod::ApiCallResult CallApi(moonmod::RuntimeConfig &config, std::string_view path, const moonmod::Json &args) {
    const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation(path);
    EXPECT_NE(operation, nullptr) << path;
    if (operation == nullptr) return {moonmod::ApiCallStatus::Failed, nullptr, "missing operation"};
    return config.api_handler("test.moonmod.chronicle-signature", *operation, args);
}

} // namespace

TEST(MoonModChronicleSignature, ReadsSetsThirstAndReportsDrainRate) {
    const s32 previous_mode = mode;
    CSaveData *previous_save = SaveData;
    CUserStatus *previous_user = UserStatus;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    UserStatus = save->GetDngStatus();
    ASSERT_NE(UserStatus, nullptr);
    mode = GAME_MODE_DUNGEON;
    CDngStatusData *status = save->GetDngStatus();
    status->cur_chara = CHARA_TOAN;
    status->cur_georama = 2;
    status->water_max[CHARA_TOAN] = 100.0f;
    status->water_now[CHARA_TOAN] = 40.0f;
    status->res_limit_zone_current = -1;
    status->water_drain_disable = 0;
    status->step_disable = 0;
    moonmod::ResetChronicleSignatureForTests();

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);

    const moonmod::ApiCallResult before = CallApi(config, "player.thirst_get", moonmod::Json::array());
    ASSERT_EQ(before.status, moonmod::ApiCallStatus::Ok) << before.message;
    EXPECT_EQ(before.value.value("character", ""), "toan");
    EXPECT_FLOAT_EQ(before.value.value("water", -1.0f), 40.0f);
    EXPECT_FLOAT_EQ(before.value.value("max_water", -1.0f), 100.0f);
    EXPECT_GT(before.value.value("drain_per_frame", 0.0f), 0.0f);
    EXPECT_TRUE(before.value.value("drain_enabled", false));

    const moonmod::ApiCallResult clamped = CallApi(
        config, "player.thirst_set", moonmod::Json::array({{{"water", 150}}}));
    ASSERT_EQ(clamped.status, moonmod::ApiCallStatus::Ok) << clamped.message;
    EXPECT_FLOAT_EQ(clamped.value.value("water", -1.0f), 100.0f);
    EXPECT_TRUE(clamped.value.value("changed", false));
    EXPECT_FLOAT_EQ(status->water_now[CHARA_TOAN], 100.0f);

    const moonmod::ApiCallResult restored = CallApi(
        config, "player.thirst_set", moonmod::Json::array({{{"restore", -250}}}));
    ASSERT_EQ(restored.status, moonmod::ApiCallStatus::Ok) << restored.message;
    EXPECT_FLOAT_EQ(restored.value.value("water", -1.0f), 0.0f);
    EXPECT_TRUE(restored.value.value("dried_out", false));
    EXPECT_FLOAT_EQ(status->water_now[CHARA_TOAN], 0.0f);

    const moonmod::ApiCallResult invalid = CallApi(
        config, "player.thirst_set", moonmod::Json::array({{{"water", 1}, {"restore", 1}}}));
    EXPECT_EQ(invalid.status, moonmod::ApiCallStatus::InvalidArgument);

    status->water_max[CHARA_GORO] = 50.0f;
    status->water_now[CHARA_GORO] = 10.0f;
    const moonmod::ApiCallResult named = CallApi(
        config, "player.thirst_set", moonmod::Json::array({{{"water", 20}, {"character", "goro"}}}));
    ASSERT_EQ(named.status, moonmod::ApiCallStatus::Ok) << named.message;
    EXPECT_FLOAT_EQ(status->water_now[CHARA_GORO], 20.0f);
    EXPECT_FLOAT_EQ(status->water_now[CHARA_TOAN], 0.0f);

    UserStatus = previous_user;
    SaveData = previous_save;
    mode = previous_mode;
}

TEST(MoonModChronicleSignature, ListsAndCollectsAtlaAndDerivesSatisfaction) {
    const s32 previous_mode = mode;
    const s32 previous_map = selectMapNo;
    const s32 previous_task = gameTask;
    const s32 previous_ura = BtUraDongeon;
    CSaveData *previous_save = SaveData;
    CUserStatus *previous_user = UserStatus;
    CDungeonMap *previous_map_object = NowDngMap;

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    UserStatus = status;
    ASSERT_NE(status, nullptr);
    status->cur_chara = CHARA_TOAN;
    status->cur_georama = 0;
    status->cur_floor = 1;
    mode = GAME_MODE_DUNGEON;
    selectMapNo = 0;
    BtUraDongeon = 0;
    auto map = std::make_unique<CDungeonMap>();
    NowDngMap = map.get();
    NowDngMap->atra_num = 1;
    NowDngMap->atra[0].used = 1;
    NowDngMap->atra[0].atra_no = 7;
    NowDngMap->atra[0].pos[0] = 1.0f;
    NowDngMap->atra[0].pos[1] = 2.0f;
    NowDngMap->atra[0].pos[2] = 3.0f;
    status->atra_registry[0][7].id = 3;
    status->atra_registry[0][7].floor = 1;
    status->atra_registry[0][7].refcount = 1;
    status->atra_grid[0][1][0] = 7;
    for (int index = 1; index < 8; ++index) status->atra_grid[0][1][index] = -1;
    SV_EDIT_PARTS_INFO *plot = save->GetEditPartsInfo(0, 3);
    ASSERT_NE(plot, nullptr);
    plot->obtained = 0;
    plot->stock = 0;
    plot->placed = 0;
    SaveData->GetGrdData(0)->request_count = 2;
    SaveData->GetGrdData(0)->request_complete[0] = 1;
    SaveData->GetGrdData(0)->request_complete[1] = 0;
    moonmod::ResetChronicleSignatureForTests();

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);

    const moonmod::ApiCallResult listed = CallApi(config, "georama.atla_list", moonmod::Json::array());
    ASSERT_EQ(listed.status, moonmod::ApiCallStatus::Ok) << listed.message;
    ASSERT_EQ(listed.value.at("spheres").size(), 1u);
    EXPECT_EQ(listed.value.at("spheres")[0].value("slot", -1), 0);
    EXPECT_EQ(listed.value.at("spheres")[0].value("atra_id", -1), 3);
    EXPECT_EQ(listed.value.at("spheres")[0].at("contents").value("kind", ""), "part");
    EXPECT_EQ(listed.value.at("spheres")[0].at("contents").value("part_id", -1), 3);

    const moonmod::ApiCallResult collected = CallApi(
        config, "georama.atla_collect", moonmod::Json::array({{{"slot", 0}}}));
    ASSERT_EQ(collected.status, moonmod::ApiCallStatus::Ok) << collected.message;
    EXPECT_EQ(collected.value.value("atra_id", -1), 3);
    EXPECT_TRUE(collected.value.value("collected", false));
    EXPECT_EQ(NowDngMap->atra[0].used, 0);
    EXPECT_NE(plot->obtained, 0);

    const moonmod::ApiCallResult repeated = CallApi(
        config, "georama.atla_collect", moonmod::Json::array({{{"slot", 0}}}));
    EXPECT_EQ(repeated.status, moonmod::ApiCallStatus::Failed);

    const moonmod::ApiCallResult satisfaction = CallApi(
        config, "georama.satisfaction_get", moonmod::Json::array({{{"town_id", 0}}}));
    ASSERT_EQ(satisfaction.status, moonmod::ApiCallStatus::Ok) << satisfaction.message;
    ASSERT_EQ(satisfaction.value.at("towns").size(), 1u);
    EXPECT_EQ(satisfaction.value.at("towns")[0].value("name", ""), "Norune Village");
    EXPECT_EQ(satisfaction.value.at("towns")[0].value("completed_requests", -1), 1);
    EXPECT_EQ(satisfaction.value.at("towns")[0].value("satisfaction_percent", -1), 50);
    EXPECT_EQ(satisfaction.value.at("towns")[0].at("plots").size(), 24u);

    NowDngMap = previous_map_object;
    UserStatus = previous_user;
    SaveData = previous_save;
    selectMapNo = previous_map;
    gameTask = previous_task;
    BtUraDongeon = previous_ura;
    mode = previous_mode;
}

TEST(MoonModChronicleSignature, ReadsBackFloorAndQueuesTransitionWithoutConsumingKey) {
    const s32 previous_mode = mode;
    const s32 previous_map = selectMapNo;
    const s32 previous_task = gameTask;
    const s32 previous_ura = BtUraDongeon;
    CSaveData *previous_save = SaveData;
    CDungeonMap *previous_map_object = NowDngMap;
    BT_EVENT_INFO previous_event = BtEventInfo;
    BT_ENEMY_FLOOR *previous_back_layout = BtUraEnemyLayoutList[0];

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->cur_floor = 4;
    mode = GAME_MODE_DUNGEON;
    selectMapNo = 0;
    gameTask = GAME_TASK_PLAY;
    BtUraDongeon = 0;
    auto map = std::make_unique<CDungeonMap>();
    NowDngMap = map.get();
    NowDngMap->gate_key_no = ITEM_SUN_DEW;
    BT_ENEMY_FLOOR layout{};
    BtUraEnemyLayoutList[0] = &layout;
    moonmod::ResetChronicleSignatureForTests();

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);

    const moonmod::ApiCallResult read = CallApi(config, "dungeons.back_floor_get", moonmod::Json::array());
    ASSERT_EQ(read.status, moonmod::ApiCallStatus::Ok) << read.message;
    EXPECT_TRUE(read.value.value("available", false));
    EXPECT_FALSE(read.value.value("active", true));
    EXPECT_FALSE(read.value.value("unlocked", true));
    EXPECT_EQ(read.value.at("gate_key").value("native_item_no", -1), ITEM_SUN_DEW);
    EXPECT_EQ(read.value.at("gate_key").value("held", true), false);

    const moonmod::ApiCallResult queued = CallApi(
        config, "dungeons.back_floor_enter", moonmod::Json::array());
    ASSERT_EQ(queued.status, moonmod::ApiCallStatus::Ok) << queued.message;
    EXPECT_TRUE(queued.value.value("queued", false));
    EXPECT_FALSE(queued.value.value("key_consumed", true));
    EXPECT_EQ(BtEventInfo.request, previous_event.request);

    gameTask = GAME_TASK_SCRIPT_RUN;
    moonmod::ChronicleTick();
    EXPECT_EQ(BtEventInfo.request, BT_REQUEST_URA_DUNGEON);

    BtUraEnemyLayoutList[0] = previous_back_layout;
    NowDngMap = previous_map_object;
    SaveData = previous_save;
    BtEventInfo = previous_event;
    selectMapNo = previous_map;
    gameTask = previous_task;
    BtUraDongeon = previous_ura;
    mode = previous_mode;
}

TEST(MoonModChronicleSignature, PublishesThirstChangesFromApiAndNativeTicks) {
    moonmod::SharedRuntime().Stop();
    const s32 previous_mode = mode;
    CSaveData *previous_save = SaveData;
    CUserStatus *previous_user = UserStatus;
    struct Restore {
        s32 previous_mode;
        CSaveData *save;
        CUserStatus *user;
        ~Restore() {
            moonmod::SharedRuntime().Stop();
            mode = previous_mode;
            SaveData = this->save;
            UserStatus = this->user;
        }
    } restore{previous_mode, previous_save, previous_user};

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    UserStatus = save->GetDngStatus();
    mode = GAME_MODE_DUNGEON;
    CDngStatusData *status = save->GetDngStatus();
    status->cur_chara = CHARA_TOAN;
    status->water_max[CHARA_TOAN] = 100.0f;
    status->water_now[CHARA_TOAN] = 50.0f;
    moonmod::ResetChronicleSignatureForTests();

    TempRoot root;
    const fs::path package = root.path / "mods" / "test.moonmod.chronicle-signature";
    WriteFile(package / "mod.json", R"json({
      "schema_version":1,"id":"test.moonmod.chronicle-signature","name":"Signature Event Probe","version":"1.0.0",
      "framework_api":1,"games":["chronicle"],"entry":"main.lua",
      "permissions":["game.read","game.write"]
    })json");
    WriteFile(package / "main.lua", R"lua(
moonmod.on("game.player.thirst_changed", function(event)
  assert(type(event.water) == "number" and type(event.previous_water) == "number")
  moonmod.log("info", "SIGNATURE_EVENT:thirst:" .. event.source .. ":" .. tostring(event.dried_out))
end)
)lua");
    WriteFile(root.path / "save" / "moonmod" / "profile.json",
              R"json({"format_version":1,"enabled":{"test.moonmod.chronicle-signature":true},"settings":{}})json");

    std::vector<std::string> logs;
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    moonmod::ConfigureChronicleApi(config);
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    config.world_location_reader = [] {
        return moonmod::ApiCallResult{moonmod::ApiCallStatus::Failed, nullptr, "no active world"};
    };
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };
    std::string error;
    const auto api_handler = config.api_handler;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(std::move(config), error)) << error;
    ASSERT_EQ(moonmod::SharedRuntime().Mods().size(), 1u);
    moonmod::SharedRuntime().Pump();

    const moonmod::ApiOperationSpec *thirst_set = moonmod::FindApiOperation("player.thirst_set");
    ASSERT_NE(thirst_set, nullptr);
    const moonmod::ApiCallResult set = api_handler("test.moonmod.chronicle-signature", *thirst_set,
                                                 moonmod::Json::array({{{"water", 0}}}));
    ASSERT_EQ(set.status, moonmod::ApiCallStatus::Ok) << set.message;
    moonmod::SharedRuntime().Pump();
    EXPECT_EQ(std::ranges::count_if(logs, [](const std::string &message) {
        return message == "SIGNATURE_EVENT:thirst:api:true";
    }), 1) << moonmod::Json(logs).dump();

    status->water_now[CHARA_TOAN] = 0.0f;
    moonmod::ChronicleTick();
    status->water_now[CHARA_TOAN] = 30.0f;
    moonmod::ChronicleTick();
    moonmod::SharedRuntime().Pump();
    EXPECT_EQ(std::ranges::count_if(logs, [](const std::string &message) {
        return message == "SIGNATURE_EVENT:thirst:native:false";
    }), 1) << moonmod::Json(logs).dump();
    EXPECT_EQ(std::ranges::count_if(logs, [](const std::string &message) {
        return message.starts_with("SIGNATURE_EVENT:thirst:");
    }), 2) << "API writes must update the native observation baseline: " << moonmod::Json(logs).dump();
}




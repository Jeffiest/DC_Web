#include <gtest/gtest.h>
#include <libpad.h>

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "battlemenu.hpp"
#include "btactstatus.hpp"
#include "btitem.hpp"
#include "dngstatusdata.hpp"
#include "dun/gameloop.hpp"
#include "framevu1.hpp"
#include "gamepad.hpp"
#include "gear.hpp"
#include "itemdata.hpp"
#include "motionmodel.hpp"
#include "platform/input.hpp"
#include "userstatus.hpp"
#include "menu_inventory.hpp"
#include "menu_misc.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/target_hud.hpp"
#include "savedata.hpp"

namespace dc::game {}
using namespace dc::game;

namespace {

namespace fs = std::filesystem;

struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-item-event-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void WriteFile(const fs::path &path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    ASSERT_TRUE(output.good()) << path.string();
}

} // namespace

TEST(MoonModItemsApi, ListsAndReadsBuiltInChronicleItemDefinitions) {
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *list_operation = moonmod::FindApiOperation("items.list");
    const moonmod::ApiOperationSpec *get_operation = moonmod::FindApiOperation("items.get");
    const moonmod::ApiOperationSpec *give_operation = moonmod::FindApiOperation("items.give");
    const moonmod::ApiOperationSpec *remove_operation = moonmod::FindApiOperation("items.remove");
    const moonmod::ApiOperationSpec *use_operation = moonmod::FindApiOperation("items.use");
    ASSERT_NE(list_operation, nullptr);
    ASSERT_NE(get_operation, nullptr);
    ASSERT_NE(give_operation, nullptr);
    ASSERT_NE(remove_operation, nullptr);
    ASSERT_NE(use_operation, nullptr);

    const moonmod::ApiCallResult listing = config.api_handler(
        "test.moonmod.chronicle-items", *list_operation, moonmod::Json::array());
    ASSERT_EQ(listing.status, moonmod::ApiCallStatus::Ok) << listing.message;
    ASSERT_TRUE(listing.value.is_array());
    ASSERT_FALSE(listing.value.empty());

    const int bread_no = ITEM_BREAD;
    const COM_ITEM_INFO *bread_common = GetCommonItemInfo(bread_no);
    const ITEM_DATA *bread_data = GetItemData(bread_no);
    ASSERT_NE(bread_common, nullptr);
    ASSERT_NE(bread_data, nullptr);

    const std::string bread_id = "chronicle:item:" + std::to_string(bread_no);
    const auto bread = std::find_if(listing.value.begin(), listing.value.end(), [&](const moonmod::Json &item) {
        return item.value("id", "") == bread_id;
    });
    ASSERT_NE(bread, listing.value.end());
    EXPECT_EQ(bread->value("item_no", -1), bread_no);
    EXPECT_EQ(bread->value("message_id", -1), bread_common->msg);
    EXPECT_EQ(bread->value("icon_index", -1), bread_common->icon_index);
    EXPECT_EQ(bread->value("use_flags", 0u), bread_data->use_flags);
    EXPECT_EQ(bread->value("kind_flags", -1), bread_data->kind_flags);
    EXPECT_EQ(bread->value("initial_volume", -1), bread_data->vol);
    EXPECT_EQ(bread->value("stack_kind", -1), bread_data->stack_kind);

    const moonmod::ApiCallResult by_stable_id = config.api_handler(
        "test.moonmod.chronicle-items", *get_operation, moonmod::Json::array({bread_id}));
    ASSERT_EQ(by_stable_id.status, moonmod::ApiCallStatus::Ok) << by_stable_id.message;
    EXPECT_EQ(by_stable_id.value, *bread);

    const moonmod::ApiCallResult by_native_number = config.api_handler(
        "test.moonmod.chronicle-items", *get_operation,
        moonmod::Json::array({moonmod::Json{{"item_no", bread_no}}}));
    ASSERT_EQ(by_native_number.status, moonmod::ApiCallStatus::Ok) << by_native_number.message;
    EXPECT_EQ(by_native_number.value, *bread);

    const moonmod::ApiCallResult attachment = config.api_handler(
        "test.moonmod.chronicle-items", *get_operation, moonmod::Json::array({ITEM_ATTACH_FIRE}));
    EXPECT_EQ(attachment.status, moonmod::ApiCallStatus::Failed);

    const moonmod::ApiCallResult invalid_number = config.api_handler(
        "test.moonmod.chronicle-items", *get_operation, moonmod::Json::array({377}));
    EXPECT_EQ(invalid_number.status, moonmod::ApiCallStatus::InvalidArgument);

    struct SaveDataScope {
        CSaveData *previous = SaveData;
        std::unique_ptr<CSaveData> owned = std::make_unique<CSaveData>();
        ~SaveDataScope() { SaveData = previous; }
    } save_scope;
    save_scope.owned->Initialize();
    SaveData = save_scope.owned.get();
    CDngStatusData *status = SaveData->GetDngStatus();
    ASSERT_NE(status, nullptr);
    EXPECT_EQ(GetNowItemNum(static_cast<short>(bread_no), &status->item_pack), 0);

    const moonmod::Json bread_request = {{"id", bread_id}, {"count", 3}};
    const moonmod::ApiCallResult given = config.api_handler(
        "test.moonmod.chronicle-items", *give_operation, moonmod::Json::array({bread_request}));
    ASSERT_EQ(given.status, moonmod::ApiCallStatus::Ok) << given.message;
    EXPECT_EQ(given.value.value("added", -1), 3);
    EXPECT_EQ(GetNowItemNum(static_cast<short>(bread_no), &status->item_pack), 3);

    const moonmod::ApiCallResult partially_removed = config.api_handler(
        "test.moonmod.chronicle-items", *remove_operation,
        moonmod::Json::array({{{"id", bread_id}, {"count", 2}}}));
    ASSERT_EQ(partially_removed.status, moonmod::ApiCallStatus::Ok) << partially_removed.message;
    EXPECT_EQ(partially_removed.value.value("removed", -1), 2);
    EXPECT_EQ(partially_removed.value.value("remaining", -1), 1);
    EXPECT_EQ(GetNowItemNum(static_cast<short>(bread_no), &status->item_pack), 1);

    const moonmod::ApiCallResult remove_remaining = config.api_handler(
        "test.moonmod.chronicle-items", *remove_operation,
        moonmod::Json::array({{{"id", bread_id}, {"count", 4}}}));
    ASSERT_EQ(remove_remaining.status, moonmod::ApiCallStatus::Ok) << remove_remaining.message;
    EXPECT_EQ(remove_remaining.value.value("removed", -1), 1);
    EXPECT_EQ(remove_remaining.value.value("remaining", -1), 0);
    EXPECT_EQ(GetNowItemNum(static_cast<short>(bread_no), &status->item_pack), 0);

    const moonmod::ApiCallResult give_for_use = config.api_handler(
        "test.moonmod.chronicle-items", *give_operation, moonmod::Json::array({{{"id", bread_id}}}));
    ASSERT_EQ(give_for_use.status, moonmod::ApiCallStatus::Ok) << give_for_use.message;
    EXPECT_EQ(give_for_use.value.value("added", -1), 1);

    status->cur_chara = CHARA_TOAN;
    status->party_size = 1;
    status->hp[CHARA_TOAN] = 10;
    status->max_hp[CHARA_TOAN] = 100;
    status->life_step[CHARA_TOAN] = 0;
    status->next_hp[CHARA_TOAN] = 10;
    ASSERT_EQ(bread_data->kind_flags, ITEMKINDF_CONSUMABLE);
    ASSERT_EQ(bread_data->use_flags, ITEMUSE_HEAL_HP);

    const moonmod::ApiCallResult used = config.api_handler(
        "test.moonmod.chronicle-items", *use_operation,
        moonmod::Json::array({{{"id", bread_id}, {"character", "active"}}}));
    ASSERT_EQ(used.status, moonmod::ApiCallStatus::Ok) << used.message;
    EXPECT_EQ(used.value.value("character", ""), "toan");
    EXPECT_EQ(used.value.value("effect", ""), "heal");
    EXPECT_EQ(used.value.value("hp", -1), 10);
    EXPECT_EQ(used.value.value("hp_target", -1), 60);
    EXPECT_EQ(used.value.value("healing", -1), 50);
    EXPECT_EQ(used.value.value("consumed", -1), 1);
    EXPECT_EQ(used.value.value("remaining", -1), 0);
    EXPECT_EQ(GetNowItemNum(static_cast<short>(bread_no), &status->item_pack), 0);

    const int water_item_no = ITEM_REGULAR_WATER;
    const ITEM_DATA *water_item = GetItemData(water_item_no);
    ASSERT_NE(water_item, nullptr);
    ASSERT_EQ(water_item->kind_flags, ITEMKINDF_CONSUMABLE);
    ASSERT_EQ(water_item->use_flags, ITEMUSE_DRINK);
    const std::string water_id = "chronicle:item:" + std::to_string(water_item_no);
    const moonmod::ApiCallResult give_water = config.api_handler(
        "test.moonmod.chronicle-items", *give_operation,
        moonmod::Json::array({{{"id", water_id}}}));
    ASSERT_EQ(give_water.status, moonmod::ApiCallStatus::Ok) << give_water.message;
    EXPECT_EQ(give_water.value.value("added", -1), 1);
    status->water_max[CHARA_TOAN] = 100.0f;
    status->water_now[CHARA_TOAN] = 10.0f;

    const moonmod::ApiCallResult drank = config.api_handler(
        "test.moonmod.chronicle-items", *use_operation,
        moonmod::Json::array({{{"id", water_id}}}));
    ASSERT_EQ(drank.status, moonmod::ApiCallStatus::Ok) << drank.message;
    EXPECT_EQ(drank.value.value("effect", ""), "water");
    EXPECT_FLOAT_EQ(drank.value.value("water", -1.0f), 40.0f);
    EXPECT_FLOAT_EQ(drank.value.value("water_restored", -1.0f), 30.0f);
    EXPECT_EQ(drank.value.value("consumed", -1), 1);
    EXPECT_EQ(drank.value.value("remaining", -1), 0);

    const moonmod::ApiCallResult give_water_for_event = config.api_handler(
        "test.moonmod.chronicle-items", *give_operation,
        moonmod::Json::array({{{"id", water_id}}}));
    ASSERT_EQ(give_water_for_event.status, moonmod::ApiCallStatus::Ok) << give_water_for_event.message;
    status->water_now[CHARA_TOAN] = 10.0f;

    TempRoot event_root;
    constexpr std::string_view event_mod_id = "test.chronicle-item-event";
    WriteFile(event_root.path / "mods" / event_mod_id / "mod.json",
              R"json({"schema_version":1,"id":"test.chronicle-item-event","name":"Item Event Probe","version":"1.0.0","framework_api":1,"games":["chronicle"],"entry":"main.lua","permissions":["content.register","game.read","game.write"]})json");
    const std::string event_script =
        std::string(R"lua(local water_id = "chronicle:item:)lua") + std::to_string(water_item_no) + R"lua("
local callbacks = 0
local story_flag_callbacks = 0
assert(game.quests.register({
  id = "water-use", title = "Use water",
  objectives = {
    {id = "drink", title = "Drink water", target = 1,
     event = "game.item_used", match = {effect = "water"}},
    {id = "flag", title = "Set story flag", target = 1,
     event = "game.story_flag_changed", match = {id = 1234, value = true}}
  }
}))
assert(assert(game.quests.start({id = "water-use"})).status == "active")
game.events.subscribe("game.story_flag_changed", function(event)
  if event.id == 1234 then
    assert(event.source_mod == "test.chronicle-item-event" and event.value and not event.previous)
    local quest = assert(game.quests.get({id = "water-use"}))
    assert(quest.status == "active" and quest.objectives[1].progress == 0 and
           quest.objectives[2].progress == 1)
    story_flag_callbacks = story_flag_callbacks + 1
    moonmod.log("info", "MOONMOD_CHRONICLE_STORY_FLAG_EVENT_OK")
  end
end)
game.events.subscribe("game.item_used", function(event)
  if event.effect == "water" then
    assert(event.id == water_id and event.source_mod == "test.chronicle-item-event")
    assert(event.character == "toan")
    local quest = assert(game.quests.get({id = "water-use"}))
    assert(quest.status == "completed" and quest.objectives[1].progress == 1)
    callbacks = callbacks + 1
    moonmod.log("info", "MOONMOD_CHRONICLE_ITEM_USED_EVENT_OK")
  end
end)
game.events.subscribe("quests.completed", function(event)
  if event.id == "test.chronicle-item-event:water-use" then
    local reward = assert(game.items.give({id = water_id, count = 1}))
    assert(reward.added == 1)
    moonmod.log("info", "MOONMOD_CHRONICLE_QUEST_REWARD_OK")
  end
end)
game.events.subscribe("tests.use_water", function()
  local flag = assert(game.world.story_flag_set({id = 1234, value = true}))
  assert(flag.changed and flag.value and story_flag_callbacks == 0)
  local used = assert(game.items.use({id = water_id, character = "active"}))
  assert(used.effect == "water" and used.water_restored == 30)
  assert(callbacks == 0, "item-use event must wait until the runtime pump")
end)
game.events.subscribe("game.item_picked_up", function(event)
  if event.id == water_id and event.kind == "item" and event.native_item_no == )lua" +
        std::to_string(water_item_no) + R"lua( and event.count == 2 and event.source == "native" then
    moonmod.log("info", "MOONMOD_CHRONICLE_ITEM_PICKED_UP_EVENT_OK")
  elseif event.id == "chronicle:weapon:)lua" +
        std::to_string(ITEM_WEAPON_START) + R"lua(" and event.kind == "weapon" and event.native_item_no == )lua" +
        std::to_string(ITEM_WEAPON_START) + R"lua( and event.count == 1 and event.source == "native" then
    moonmod.log("info", "MOONMOD_CHRONICLE_WEAPON_PICKED_UP_EVENT_OK")
  end
end)
return {}
)lua";
    WriteFile(event_root.path / "mods" / event_mod_id / "main.lua", event_script);
    WriteFile(event_root.path / "save" / "moonmod" / "profile.json",
              R"json({"format_version":1,"enabled":{"test.chronicle-item-event":true},"settings":{}})json");
    moonmod::RuntimeConfig event_config;
    event_config.game_id = "chronicle";
    event_config.game_name = "Chronicle";
    event_config.mod_directories = {event_root.path / "mods"};
    event_config.save_root = event_root.path / "save";
    moonmod::ConfigureChronicleApi(event_config);
    std::vector<std::string> event_logs;
    event_config.log = [&event_logs](std::string_view, std::string_view, std::string_view message) {
        event_logs.emplace_back(message);
    };
    moonmod::Runtime &event_runtime = moonmod::SharedRuntime();
    event_runtime.Stop();
    std::string runtime_error;
    ASSERT_TRUE(event_runtime.Start(std::move(event_config), runtime_error)) << runtime_error;
    ASSERT_EQ(event_runtime.Mods().size(), 1u);
    ASSERT_EQ(event_runtime.Mods()[0].state, moonmod::ModState::Loaded) << event_runtime.Mods()[0].status_message;
    event_runtime.Publish("tests.use_water", moonmod::Json::object());
    moonmod::ChronicleNativeItemPickedUp(water_item_no, 2);
    moonmod::ChronicleNativeItemPickedUp(ITEM_WEAPON_START, 1);
    EXPECT_EQ(std::find(event_logs.begin(), event_logs.end(), "MOONMOD_CHRONICLE_ITEM_USED_EVENT_OK"), event_logs.end());
    EXPECT_EQ(std::find(event_logs.begin(), event_logs.end(), "MOONMOD_CHRONICLE_STORY_FLAG_EVENT_OK"), event_logs.end());
    EXPECT_EQ(std::find(event_logs.begin(), event_logs.end(), "MOONMOD_CHRONICLE_QUEST_REWARD_OK"), event_logs.end());
    EXPECT_EQ(std::find(event_logs.begin(), event_logs.end(), "MOONMOD_CHRONICLE_ITEM_PICKED_UP_EVENT_OK"), event_logs.end());
    EXPECT_EQ(std::find(event_logs.begin(), event_logs.end(), "MOONMOD_CHRONICLE_WEAPON_PICKED_UP_EVENT_OK"), event_logs.end());
    event_runtime.Pump();
    EXPECT_NE(std::find(event_logs.begin(), event_logs.end(), "MOONMOD_CHRONICLE_ITEM_USED_EVENT_OK"), event_logs.end());
    EXPECT_NE(std::find(event_logs.begin(), event_logs.end(), "MOONMOD_CHRONICLE_STORY_FLAG_EVENT_OK"), event_logs.end());
    EXPECT_NE(std::find(event_logs.begin(), event_logs.end(), "MOONMOD_CHRONICLE_QUEST_REWARD_OK"), event_logs.end());
    EXPECT_NE(std::find(event_logs.begin(), event_logs.end(), "MOONMOD_CHRONICLE_ITEM_PICKED_UP_EVENT_OK"), event_logs.end());
    EXPECT_NE(std::find(event_logs.begin(), event_logs.end(), "MOONMOD_CHRONICLE_WEAPON_PICKED_UP_EVENT_OK"), event_logs.end());
    event_runtime.Stop();
    EXPECT_EQ(GetNowItemNum(static_cast<short>(water_item_no), &status->item_pack), 1);

    const moonmod::ApiCallResult give_full_water = config.api_handler(
        "test.moonmod.chronicle-items", *give_operation,
        moonmod::Json::array({{{"id", water_id}}}));
    ASSERT_EQ(give_full_water.status, moonmod::ApiCallStatus::Ok) << give_full_water.message;
    status->water_now[CHARA_TOAN] = status->water_max[CHARA_TOAN];
    const moonmod::ApiCallResult refused_full_water = config.api_handler(
        "test.moonmod.chronicle-items", *use_operation,
        moonmod::Json::array({{{"id", water_id}}}));
    EXPECT_EQ(refused_full_water.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(GetNowItemNum(static_cast<short>(water_item_no), &status->item_pack), 2);

    const moonmod::ApiCallResult invalid_target = config.api_handler(
        "test.moonmod.chronicle-items", *use_operation,
        moonmod::Json::array({{{"id", bread_id}, {"character", "xiao"}}}));
    EXPECT_EQ(invalid_target.status, moonmod::ApiCallStatus::Failed);
    EXPECT_NE(invalid_target.message.find("not in the joined party"), std::string::npos);
}

namespace {

// Presses or releases the first pad and lets CGamePad see it, so the chest loops read a fresh
// Down() edge each time.
void PressPad(std::uint16_t buttons) {
    InputPadState state;
    state.connected = true;
    state.buttons = buttons;
    InputSetOverride(0, &state);
    GamePad.UpDate();
}

} // namespace

// Opening a chest is retail's BtGetTreasurebox*_Loop at the "take it" step: the item goes into the
// pack through GetItem, and the mod layer must hear about it. The port replaces both loops, so the
// notification has to live in the port copies, not only in ps2/.
TEST(MoonModItemsApi, OpeningTreasureBoxesPublishesItemPickedUp) {
    struct SaveDataScope {
        CSaveData *previous_save = SaveData;
        CUserStatus *previous_status = UserStatus;
        std::unique_ptr<CSaveData> owned = std::make_unique<CSaveData>();
        ~SaveDataScope() {
            SaveData = previous_save;
            UserStatus = previous_status;
        }
    } save_scope;
    save_scope.owned->Initialize();
    SaveData = save_scope.owned.get();
    CDngStatusData *status = SaveData->GetDngStatus();
    ASSERT_NE(status, nullptr);
    UserStatus = status;

    const int chest_item_no = ITEM_BREAD;
    const int chest_weapon_no = ITEM_WEAPON_START;
    ASSERT_EQ(GetNowItemNum(static_cast<short>(chest_item_no), &status->item_pack), 0);
    for (const WEAPON_HAVE &weapon : status->chara_weapons[CHARA_TOAN]) {
        ASSERT_NE(weapon.item_no, chest_weapon_no);
    }

    TempRoot root;
    constexpr std::string_view mod_id = "test.chronicle-chest-pickup";
    WriteFile(root.path / "mods" / mod_id / "mod.json",
              R"json({"schema_version":1,"id":"test.chronicle-chest-pickup","name":"Chest Pickup Probe","version":"1.0.0","framework_api":1,"games":["chronicle"],"entry":"main.lua","permissions":["game.read"]})json");
    WriteFile(root.path / "mods" / mod_id / "main.lua", R"lua(
game.events.subscribe("game.item_picked_up", function(event)
  local line = "MOONMOD_CHEST_PICKUP id=" .. tostring(event.id) .. " kind=" .. tostring(event.kind) ..
    " native=" .. tostring(event.native_item_no) .. " count=" .. tostring(event.count) ..
    " source=" .. tostring(event.source)
  if event.character ~= nil then
    line = line .. " character=" .. tostring(event.character) .. " slot=" .. tostring(event.slot)
  end
  moonmod.log("info", line)
end)
return {}
)lua");
    WriteFile(root.path / "save" / "moonmod" / "profile.json",
              R"json({"format_version":1,"enabled":{"test.chronicle-chest-pickup":true},"settings":{}})json");
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    moonmod::ConfigureChronicleApi(config);
    std::vector<std::string> logs;
    config.log = [&logs](std::string_view, std::string_view, std::string_view message) {
        logs.emplace_back(message);
    };
    moonmod::Runtime &runtime = moonmod::SharedRuntime();
    runtime.Stop();
    std::string runtime_error;
    ASSERT_TRUE(runtime.Start(std::move(config), runtime_error)) << runtime_error;
    ASSERT_EQ(runtime.Mods().size(), 1u);
    ASSERT_EQ(runtime.Mods()[0].state, moonmod::ModState::Loaded) << runtime.Mods()[0].status_message;

    // The chest-opening effect model: the loops read its "cam" and "int" frames for the camera.
    CFrameVu1 effect_root;
    CFrameVu1 camera_frame;
    CFrameVu1 interest_frame;
    std::strcpy(camera_frame.name, "cam");
    std::strcpy(interest_frame.name, "int");
    effect_root.SetChild(&camera_frame);
    effect_root.SetChild(&interest_frame);
    struct ChestStateScope {
        CFrame *big_frame = itemOpenBigFx.frame;
        CFrame *small_frame = itemOpenSmallFx.frame;
        CCameraFollow *camera = NowCamera__3;
        int sled = BtGetTreasurebox_Sled;
        int big_item = TreasureboxBig_itemNo;
        int small_item = BtGetTreasureboxSmall_itemNo;
        int small_volume = BtGetTreasureboxSmall_itemVolume;
        BT_ACT_STATUS action = BtActStatus;
        ~ChestStateScope() {
            itemOpenBigFx.frame = big_frame;
            itemOpenSmallFx.frame = small_frame;
            NowCamera__3 = camera;
            BtGetTreasurebox_Sled = sled;
            TreasureboxBig_itemNo = big_item;
            BtGetTreasureboxSmall_itemNo = small_item;
            BtGetTreasureboxSmall_itemVolume = small_volume;
            BtActStatus = action;
            InputSetOverride(0, nullptr);
        }
    } chest_scope;
    itemOpenBigFx.frame = &effect_root;
    itemOpenSmallFx.frame = &effect_root;
    NowCamera__3 = &SubCamera;

    static unsigned char dma_buffer[1024];
    ASSERT_EQ(scePadInit(0), 1);
    ASSERT_EQ(scePadPortOpen(0, 0, dma_buffer), 1);
    for (int i = 0; i < 4; ++i) {
        PressPad(0);
    }

    // The big chest, waiting for the player to take the item.
    BtGetTreasurebox_Sled = 3;
    TreasureboxBig_itemNo = chest_item_no;
    PressPad(PAD_CROSS);
    ASSERT_NE(GamePad.Down(PAD_CIRCLE | PAD_CROSS), 0);
    EXPECT_EQ(BtGetTreasureboxBig_Loop(), 1);
    EXPECT_EQ(GetNowItemNum(static_cast<short>(chest_item_no), &status->item_pack), 1);
    EXPECT_TRUE(logs.empty());
    runtime.Pump();
    const std::string item_line = "MOONMOD_CHEST_PICKUP id=chronicle:item:" + std::to_string(chest_item_no) +
                                  " kind=item native=" + std::to_string(chest_item_no) +
                                  " count=1 source=native";
    EXPECT_NE(std::find(logs.begin(), logs.end(), item_line), logs.end())
        << (logs.empty() ? std::string("no log") : logs.back());
    logs.clear();

    // The small chest handing over a weapon names the owner and the weapon slot it landed in.
    PressPad(0);
    BtGetTreasurebox_Sled = 3;
    BtGetTreasureboxSmall_itemNo = chest_weapon_no;
    BtGetTreasureboxSmall_itemVolume = 0;
    PressPad(PAD_CROSS);
    ASSERT_NE(GamePad.Down(PAD_CIRCLE | PAD_CROSS), 0);
    EXPECT_EQ(BtGetTreasureboxSmall_Loop(), 1);
    int weapon_slot = -1;
    for (int slot = 0; slot < 11; ++slot) {
        if (status->chara_weapons[CHARA_TOAN][slot].item_no == chest_weapon_no) {
            weapon_slot = slot;
            break;
        }
    }
    ASSERT_GE(weapon_slot, 0);
    EXPECT_TRUE(logs.empty());
    runtime.Pump();
    const std::string weapon_line = "MOONMOD_CHEST_PICKUP id=chronicle:weapon:" + std::to_string(chest_weapon_no) +
                                    " kind=weapon native=" + std::to_string(chest_weapon_no) +
                                    " count=1 source=native character=" + std::to_string(CHARA_TOAN) +
                                    " slot=" + std::to_string(weapon_slot);
    EXPECT_NE(std::find(logs.begin(), logs.end(), weapon_line), logs.end())
        << (logs.empty() ? std::string("no log") : logs.back());

    runtime.Stop();
}

TEST(MoonModItemsApi, GearPageOpensWithEachKindsIconAndCloses) {
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *page = moonmod::FindApiOperation("items.gear_page_set");
    ASSERT_NE(page, nullptr);
    const auto call = [&](moonmod::Json args) { return config.api_handler("test.moonmod.gear-page", *page, std::move(args)); };
    const moonmod::Json icons = {{"hat", "gear_hat"}, {"tunic", "gear_tunic"}, {"gloves", "gear_gloves"}, {"boots", "gear_boots"}};

    const moonmod::ApiCallResult opened = call(moonmod::Json::array({{{"icons", icons}}}));
    ASSERT_EQ(opened.status, moonmod::ApiCallStatus::Ok) << opened.message;
    EXPECT_TRUE(GearPageOpen());
    EXPECT_EQ(GearKindIcon(GEAR_TUNIC), "icons/gear_tunic.png");
    EXPECT_TRUE(GearKindIcon(GEAR_POUCH).empty()); // no pouch named, no pouch slot

    // The pouch is the mod's to name; then its slot shows, and a page of four after it hides it again.
    moonmod::Json with_pouch = icons;
    with_pouch["pouch"] = "gear_pouch";
    ASSERT_EQ(call(moonmod::Json::array({{{"icons", with_pouch}}})).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(GearKindIcon(GEAR_POUCH), "icons/gear_pouch.png");
    ASSERT_EQ(call(moonmod::Json::array({{{"icons", icons}}})).status, moonmod::ApiCallStatus::Ok);
    EXPECT_TRUE(GearKindIcon(GEAR_POUCH).empty());

    // The first four kinds, each a bare stem.
    moonmod::Json missing = icons;
    missing.erase("boots");
    EXPECT_EQ(call(moonmod::Json::array({{{"icons", missing}}})).status, moonmod::ApiCallStatus::InvalidArgument);
    moonmod::Json pathed = icons;
    pathed["hat"] = "../gear_hat";
    EXPECT_EQ(call(moonmod::Json::array({{{"icons", pathed}}})).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(moonmod::Json::array({{{"tabs", 4}}})).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(GearKindIcon(GEAR_HAT), "icons/gear_hat.png"); // a refused call changes nothing

    const moonmod::ApiCallResult closed = call(moonmod::Json::array({moonmod::Json::object()}));
    ASSERT_EQ(closed.status, moonmod::ApiCallStatus::Ok) << closed.message;
    EXPECT_FALSE(GearPageOpen());
}

TEST(MoonModItemsApi, GearStockSetsAShopsGoodsPricedAndTakesThemAway) {
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *stock = moonmod::FindApiOperation("items.gear_stock_set");
    ASSERT_NE(stock, nullptr);
    const auto call = [&](moonmod::Json args) { return config.api_handler("test.moonmod.gear-stock", *stock, std::move(args)); };
    ClearGearStock();
    const moonmod::Json cap = {{"kind", "hat"}, {"name", "Shop Cap"}, {"rarity", 1}, {"value", 60}, {"price", 150},
                               {"effects", {{"guard", {{"poison", 20}}}}}};
    const moonmod::Json boots = {{"kind", "boots"}, {"name", "Shop Boots"}, {"value", 40}};

    const moonmod::ApiCallResult set = call(moonmod::Json::array({{{"shop", 3}, {"goods", {cap, boots}}}}));
    ASSERT_EQ(set.status, moonmod::ApiCallStatus::Ok) << set.message;
    EXPECT_EQ(set.value["goods"], 2);
    std::vector<GearGood> *goods = GearStock(3);
    ASSERT_NE(goods, nullptr);
    ASSERT_EQ(goods->size(), 2u);
    EXPECT_EQ((*goods)[0].piece.name, "Shop Cap");
    EXPECT_EQ((*goods)[0].price, 150);
    EXPECT_EQ((*goods)[0].piece.effects.guard[GearGuardFromName("poison")], 20);
    EXPECT_EQ((*goods)[1].price, 40); // no price: its value
    EXPECT_EQ(GearStock(4), nullptr);

    // A good is a piece as gear_give takes it, with a price; a bad one changes nothing.
    moonmod::Json dear = cap;
    dear["price"] = 70000;
    EXPECT_EQ(call(moonmod::Json::array({{{"shop", 3}, {"goods", {dear}}}})).status, moonmod::ApiCallStatus::InvalidArgument);
    moonmod::Json odd = boots;
    odd["kind"] = "ring";
    EXPECT_EQ(call(moonmod::Json::array({{{"shop", 3}, {"goods", {odd}}}})).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(moonmod::Json::array({{{"shop", 3}, {"goods", moonmod::Json::array({boots, boots, boots, boots, boots, boots, boots, boots, boots, boots, boots})}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(GearStock(3)->size(), 2u);
    // The Gauntlet's five rarities: 4 is legendary, and there is no sixth.
    moonmod::Json legendary = cap;
    legendary["rarity"] = 4;
    moonmod::Json beyond = cap;
    beyond["rarity"] = 5;
    EXPECT_EQ(call(moonmod::Json::array({{{"shop", 3}, {"goods", {beyond}}}})).status, moonmod::ApiCallStatus::InvalidArgument);
    ASSERT_EQ(call(moonmod::Json::array({{{"shop", 3}, {"goods", {legendary}}}})).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(GearStock(3)->front().piece.rarity, 4);

    ASSERT_EQ(call(moonmod::Json::array({{{"shop", 3}, {"goods", moonmod::Json::array()}}})).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(GearStock(3), nullptr);
}

TEST(MoonModItemsApi, StatLabelsTakeAnIconPerStatAndGiveTheGamesBack) {
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *labels = moonmod::FindApiOperation("weapons.stat_label_set");
    ASSERT_NE(labels, nullptr);
    const auto call = [&](moonmod::Json args) { return config.api_handler("test.moonmod.stat-labels", *labels, std::move(args)); };

    const moonmod::ApiCallResult set = call(moonmod::Json::array({{{"speed", "label_lightness"}}}));
    ASSERT_EQ(set.status, moonmod::ApiCallStatus::Ok) << set.message;
    EXPECT_EQ(set.value["labels"]["speed"], "icons/label_lightness.png");
    EXPECT_EQ(WeaponStatLabel(WEAPON_STAT_SPEED), "icons/label_lightness.png");
    EXPECT_TRUE(WeaponStatLabel(WEAPON_STAT_ATTACK).empty());

    // A stat the cards don't show, or a path for a stem, changes nothing.
    EXPECT_EQ(call(moonmod::Json::array({{{"weight", "x"}}})).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(moonmod::Json::array({{{"magic", "icons/x.png"}}})).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(WeaponStatLabel(WEAPON_STAT_SPEED), "icons/label_lightness.png");

    ASSERT_EQ(call(moonmod::Json::array({moonmod::Json::object()})).status, moonmod::ApiCallStatus::Ok);
    EXPECT_TRUE(WeaponStatLabel(WEAPON_STAT_SPEED).empty());
}

TEST(MoonModItemsApi, GearPageTakesEachHerosPartsAndForgetsThemClosed) {
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *page = moonmod::FindApiOperation("items.gear_page_set");
    ASSERT_NE(page, nullptr);
    const auto call = [&](moonmod::Json args) { return config.api_handler("test.moonmod.gear-parts", *page, std::move(args)); };
    using moonmod::Json;
    const Json icons = {{"hat", "gear_hat"}, {"tunic", "gear_tunic"}, {"gloves", "gear_gloves"}, {"boots", "gear_boots"}};
    const auto names = [](std::initializer_list<const char *> list) {
        Json out = Json::array();
        for (const char *name : list) out.push_back(name);
        return out;
    };
    const auto hero = [](Json kinds) {
        Json out = Json::object();
        out[CharaIdName(0)] = std::move(kinds);
        return out;
    };
    Json tunic = Json::object();
    tunic["draw"] = names({"skin", "beruto"});
    tunic["fit"] = names({"beruto"});
    tunic["turn"] = 90;
    tunic["tilt"] = -45;
    tunic["bare"] = names({"cloth2", "cloth3"});
    tunic["below"] = names({"l_f"});
    Json kinds = Json::object();
    kinds["hat"] = names({"boushi"});
    kinds["tunic"] = tunic;

    const moonmod::ApiCallResult opened = call(Json::array({Json{{"icons", icons}, {"parts", hero(kinds)}}}));
    ASSERT_EQ(opened.status, moonmod::ApiCallStatus::Ok) << opened.message;
    EXPECT_EQ(GearPartsFor(0, GEAR_HAT).draw, std::vector<std::string>{"boushi"});
    EXPECT_TRUE(GearPartsFor(0, GEAR_HAT).fit.empty());
    EXPECT_EQ(GearPartsFor(0, GEAR_TUNIC).draw, (std::vector<std::string>{"skin", "beruto"}));
    EXPECT_EQ(GearPartsFor(0, GEAR_TUNIC).fit, std::vector<std::string>{"beruto"});
    EXPECT_NEAR(GearPartsFor(0, GEAR_TUNIC).turn, 1.5708f, 0.001f);
    EXPECT_NEAR(GearPartsFor(0, GEAR_TUNIC).tilt, -0.7854f, 0.001f);
    EXPECT_EQ(GearPartsFor(0, GEAR_TUNIC).bare, (std::vector<std::string>{"cloth2", "cloth3"}));
    EXPECT_EQ(GearPartsFor(0, GEAR_TUNIC).below, std::vector<std::string>{"l_f"});
    EXPECT_TRUE(GearPartsFor(0, GEAR_HAT).bare.empty()); // nothing left out unless named
    EXPECT_TRUE(GearPartsFor(0, GEAR_BOOTS).draw.empty()); // a kind left out keeps its icon
    EXPECT_TRUE(GearPartsFor(1, GEAR_HAT).draw.empty());   // as does a hero left out

    // Names, kinds and fields as the page takes them; a refused call changes nothing.
    std::vector<Json> bad;
    bad.push_back(Json{{"cape", names({"boushi"})}});
    bad.push_back(Json{{"hat", Json::array()}});
    bad.push_back(Json{{"hat", Json::array({std::string(32, 'a')})}});
    bad.push_back(Json{{"hat", Json{{"fit", names({"boushi"})}}}});
    bad.push_back(Json{{"hat", Json{{"draw", names({"boushi"})}, {"turn", 200}}}});
    bad.push_back(Json{{"hat", Json{{"draw", names({"boushi"})}, {"tilt", 100}}}});
    bad.push_back(Json{{"hat", Json{{"draw", names({"boushi"})}, {"spin", 1}}}});
    bad.push_back(Json{{"hat", Json{{"draw", names({"boushi"})}, {"bare", "boushi"}}}});
    bad.push_back(Json{{"hat", Json{{"draw", names({"boushi"})}, {"below", Json::array()}}}});
    bad.push_back(Json{{"hat", "boushi"}});

    for (const Json &kind : bad) {
        EXPECT_EQ(call(Json::array({Json{{"icons", icons}, {"parts", hero(kind)}}})).status, moonmod::ApiCallStatus::InvalidArgument) << kind.dump();
    }
    EXPECT_EQ(call(Json::array({Json{{"icons", icons}, {"parts", Json{{CharaIdName(0), "boushi"}}}}})).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(GearPartsFor(0, GEAR_HAT).draw, std::vector<std::string>{"boushi"});

    ASSERT_EQ(call(Json::array({Json::object()})).status, moonmod::ApiCallStatus::Ok);
    EXPECT_TRUE(GearPartsFor(0, GEAR_HAT).draw.empty());
}

TEST(MoonModItemsApi, TargetBarTurnsOnAndItsMeterChecksTheMonster) {
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *bar = moonmod::FindApiOperation("render.target_bar_set");
    const moonmod::ApiOperationSpec *meter = moonmod::FindApiOperation("render.target_meter_set");
    ASSERT_NE(bar, nullptr);
    ASSERT_NE(meter, nullptr);
    using moonmod::Json;
    const auto call = [&](const moonmod::ApiOperationSpec &op, Json spec) {
        return config.api_handler("test.moonmod.target-bar", op, Json::array({std::move(spec)}));
    };

    const moonmod::ApiCallResult on = call(*bar, {{"on", true}});
    ASSERT_EQ(on.status, moonmod::ApiCallStatus::Ok) << on.message;
    EXPECT_TRUE(MoonModTargetBarOn());
    EXPECT_EQ(call(*bar, {{"on", 1}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(*bar, Json::object()).status, moonmod::ApiCallStatus::InvalidArgument);

    // With no life on the gauge there is nothing to draw, so the game's gauge keeps its turn.
    EnemyLifeGage.life_max = 0;
    EXPECT_FALSE(MoonModTargetBarDraw(0));

    // The meter takes a monster handle, a fill of 0..1, a colour and seconds, and nothing else.
    EXPECT_EQ(call(*meter, {{"entity", "chronicle:player:0"}, {"fill", 0.5}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(*meter, {{"entity", "chronicle:monster:0:1"}, {"fill", 1.5}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(*meter, {{"entity", "chronicle:monster:0:1"}, {"fill", 0.5}, {"why", 1}}).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(*meter, {{"entity", "chronicle:monster:0:1"}, {"fill", 0.5}, {"seconds", 900}}).status,
              moonmod::ApiCallStatus::InvalidArgument);
    // A handle from another generation is a monster that has gone.
    EXPECT_NE(call(*meter, {{"entity", "chronicle:monster:0:4000000"}, {"fill", 0.5}}).status, moonmod::ApiCallStatus::Ok);

    ASSERT_EQ(call(*bar, {{"on", false}}).status, moonmod::ApiCallStatus::Ok);
    EXPECT_FALSE(MoonModTargetBarOn());
    EnemyLifeGage.life_max = 100;
    EXPECT_FALSE(MoonModTargetBarDraw(0));
    EnemyLifeGage.life_max = 0;
}

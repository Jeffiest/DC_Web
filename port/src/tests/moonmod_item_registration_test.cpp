#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "itemdata.hpp"
#include "dun/gameloop.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/manager_ui.hpp"
#include "moonmod/runtime.hpp"
#include "savedata.hpp"
#include "userstatus.hpp"

namespace dc::game {}
using namespace dc::game;

namespace {

namespace fs = std::filesystem;

struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-chronicle-item-e2e-" +
                     std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

struct SaveDataScope {
    CSaveData *previous = SaveData;
    std::unique_ptr<CSaveData> owned = std::make_unique<CSaveData>();
    ~SaveDataScope() { SaveData = previous; }
};

struct ModItemRestore {
    ~ModItemRestore() { moonmod::ResetChronicleItemsForTests(); }
};

struct UserStatusRestore {
    CUserStatus *previous = UserStatus;
    ~UserStatusRestore() { UserStatus = previous; }
};

void Write(const fs::path &path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    ASSERT_TRUE(out.good()) << path.string();
}

moonmod::RuntimeConfig MakeConfig(const TempRoot &root) {
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    moonmod::ConfigureChronicleApi(config);
    return config;
}

} // namespace

TEST(MoonModItemRegistration, LuaAddsAndUsesDataOnlyItemWithStableNativeSlot) {
    moonmod::ResetChronicleItemsForTests();
    ModItemRestore restore_mod_items;
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    SaveDataScope save_scope;
    save_scope.owned->Initialize();
    SaveData = save_scope.owned.get();
    CDngStatusData *status = SaveData->GetDngStatus();
    ASSERT_NE(status, nullptr);
    UserStatusRestore restore_user_status;
    UserStatus = static_cast<CUserStatus *>(status);
    status->cur_chara = CHARA_TOAN;
    status->party_size = 2;
    status->hp[CHARA_TOAN] = 40;
    status->max_hp[CHARA_TOAN] = 100;
    status->life_step[CHARA_TOAN] = 0;
    status->next_hp[CHARA_TOAN] = 40;
    status->hp[CHARA_XIAO] = 40;
    status->max_hp[CHARA_XIAO] = 100;
    status->life_step[CHARA_XIAO] = 0;
    status->next_hp[CHARA_XIAO] = 40;
    status->water_max[CHARA_TOAN] = 100.0f;
    status->water_now[CHARA_TOAN] = 10.0f;

    TempRoot root;
    const fs::path package = root.path / "mods" / "test.chronicle-new-item";
    Write(package / "mod.json",
          R"json({"schema_version":1,"id":"test.chronicle-new-item","name":"Chronicle New Item","version":"1.0.0","framework_api":1,"games":["chronicle"],"entry":"main.lua","permissions":["content.register","game.read","game.write"]})json");
    Write(package / "main.lua", R"lua(
local item = assert(game.content.register({
  type = "item",
  definition = {id = "starlight-tonic", name = "Starlight Tonic", kind = "consumable",
    template_item = 148, assets = {}, effect = {kind = "heal", amount = 25}}
}))
assert(item.id == "test.chronicle-new-item:starlight-tonic")
assert(item.status == "registered" and item.mod_owned and item.name == "Starlight Tonic")
assert(item.native_item_no ~= item.template_item)
assert(next(item.assets) == nil)

local water_item = assert(game.items.register({
  id = "moon-dew", name = "Moon Dew", kind = "consumable",
  template_item = 145, assets = {}, effect = {kind = "restore_water", amount = 15}
}))
assert(water_item.id == "test.chronicle-new-item:moon-dew")
assert(water_item.effect.kind == "restore_water" and water_item.effect.amount == 15)

local remedies = {}
for _, status in ipairs({"freeze", "poison", "curse", "goo"}) do
  local remedy = assert(game.items.register({
    id = "remedy-" .. status, name = "Remedy " .. status, kind = "consumable",
    template_item = 154, assets = {}, effect = {kind = "cure_status", status = status}
  }))
  assert(remedy.effect.kind == "cure_status" and remedy.effect.status == status)
  remedies[#remedies + 1] = remedy
end
local panacea = assert(game.items.register({
  id = "panacea", name = "Panacea", kind = "consumable",
  template_item = 154, assets = {}, effect = {kind = "cure_all"}
}))
assert(panacea.effect.kind == "cure_all" and panacea.status == "registered")
local unsupported_remedy, unsupported_remedy_error = game.items.register({
  id = "remedy-stamina", name = "Remedy stamina", kind = "consumable",
  template_item = 154, assets = {}, effect = {kind = "cure_status", status = "stamina"}
})
assert(unsupported_remedy == nil and unsupported_remedy_error.code == "invalid_argument")

local listed = assert(game.items.list())
local found = false
for _, entry in ipairs(listed) do
  if entry.id == item.id then found = true end
end
assert(found, "the registered item must be visible in the item catalog")
local content = assert(game.content.list())
local found_in_content = false
for _, entry in ipairs(content.entries) do
  if entry.id == item.id and entry.content_type == "item" then found_in_content = true end
end
assert(found_in_content and content.count == #content.entries)
local loaded = assert(game.items.get(item.id))
assert(loaded.native_item_no == item.native_item_no and loaded.name == item.name)

local grant = assert(game.items.give({id = item.id, count = 2}))
assert(grant.added == 2 and grant.id == item.id)
assert(assert(game.player.get()).hp == 40)
local non_party_use, non_party_use_error = game.items.use({id = item.id, character = "ruby"})
assert(non_party_use == nil and non_party_use_error.code == "failed")
local use = assert(game.items.use({id = item.id}))
assert(use.effect == "heal" and use.healing == 25 and use.consumed == 1 and use.remaining == 1)
local removal = assert(game.items.remove({id = item.id, count = 1}))
assert(removal.removed == 1 and removal.remaining == 0)
assert(assert(game.items.give({id = water_item.id})).added == 1)
local drink = assert(game.items.use({id = water_item.id}))
assert(drink.effect == "water" and drink.water_restored == 15 and drink.consumed == 1)
for _, remedy in ipairs(remedies) do
  assert(assert(game.items.give({id = remedy.id})).added == 1)
  local cured = assert(game.items.use({id = remedy.id}))
  assert(cured.effect == "cure_status" and cured.cured_status == remedy.effect.status)
  assert(cured.removed and not cured.active and cured.consumed == 1)
end
game.events.subscribe("tests.use.panacea", function()
  assert(assert(game.items.give({id = panacea.id})).added == 1)
  local cured_all = assert(game.items.use({id = panacea.id}))
  assert(cured_all.effect == "cure_all" and cured_all.removed and cured_all.remaining_ailments == 0)
  assert(cured_all.consumed == 1 and cured_all.remaining == 0)
  moonmod.log("info", "MOONMOD_CHRONICLE_CURE_ALL_OK")
end)
assert(assert(game.items.give({id = remedies[1].id})).added == 1)
local bench_cure = assert(game.items.use({id = remedies[1].id, character = "xiao"}))
assert(bench_cure.character == "xiao" and bench_cure.cured_status == "freeze")
assert(bench_cure.removed and bench_cure.consumed == 1)
local invalid_field, invalid_field_error = game.items.use({id = remedies[1].id, target = "xiao"})
assert(invalid_field == nil and invalid_field_error.code == "invalid_argument")
moonmod.log("info", "MOONMOD_CHRONICLE_NATIVE_STATUS_CURES_OK")
moonmod.log("info", "MOONMOD_CHRONICLE_BENCH_ITEM_USE_OK")
moonmod.log("info", "MOONMOD_CHRONICLE_NEW_WATER_ITEM_OK:" .. water_item.native_item_no)
moonmod.log("info", "MOONMOD_CHRONICLE_NEW_ITEM_OK:" .. item.native_item_no)
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.chronicle-new-item":true},"settings":{}})json");

    std::vector<std::string> logs;
    std::vector<std::string> errors;
    for (int launch = 0; launch < 2; ++launch) {
        status->water_now[CHARA_TOAN] = 10.0f;
        status->ailments[CHARA_TOAN] = AILMENT_FREEZE | AILMENT_POISON | AILMENT_CURSE | AILMENT_GOO;
        status->ailment_frames[CHARA_TOAN] = 90;
        status->ailments[CHARA_XIAO] = AILMENT_FREEZE;
        status->ailment_frames[CHARA_XIAO] = 90;
        moonmod::RuntimeConfig config = MakeConfig(root);
        config.log = [&logs, &errors](std::string_view level, std::string_view, std::string_view message) {
            logs.emplace_back(message);
            if (level == "error") errors.emplace_back(message);
        };
        moonmod::Runtime runtime;
        std::string error;
        ASSERT_TRUE(runtime.Start(std::move(config), error)) << error;
        ASSERT_EQ(runtime.Mods().size(), 1u);
        ASSERT_EQ(runtime.Mods()[0].state, moonmod::ModState::Loaded) << runtime.Mods()[0].status_message;
        status->ailments[CHARA_TOAN] = AILMENT_FREEZE | AILMENT_POISON | AILMENT_CURSE | AILMENT_GOO;
        ASSERT_TRUE(runtime.PublishToMod("test.chronicle-new-item", "tests.use.panacea", moonmod::Json::object()));
        EXPECT_EQ(status->ailments[CHARA_TOAN], 0);
        EXPECT_EQ(status->ailments[CHARA_XIAO], 0);
        runtime.Stop();
    }

    const fs::path registry_path = root.path / "save" / "moonmod" / "game-content" / "chronicle-item-slots.json";
    ASSERT_TRUE(fs::is_regular_file(registry_path));
    std::ifstream registry_file(registry_path, std::ios::binary);
    const moonmod::Json registry = moonmod::Json::parse(registry_file);
    const int item_no = registry.at("items").at("test.chronicle-new-item:starlight-tonic").get<int>();
    const int water_item_no = registry.at("items").at("test.chronicle-new-item:moon-dew").get<int>();
    const int panacea_item_no = registry.at("items").at("test.chronicle-new-item:panacea").get<int>();
    EXPECT_NE(item_no, ITEM_BREAD);
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string &message) {
                  return message.starts_with("MOONMOD_CHRONICLE_NEW_ITEM_OK:");
              }),
              2);
    EXPECT_EQ(std::count_if(logs.begin(), logs.end(), [](const std::string &message) {
                  return message.starts_with("MOONMOD_CHRONICLE_NEW_WATER_ITEM_OK:");
              }),
              2);
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_CHRONICLE_NATIVE_STATUS_CURES_OK"), 2);
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_CHRONICLE_CURE_ALL_OK"), 2);
    const ITEM_DATA *panacea_data = GetItemData(panacea_item_no);
    ASSERT_NE(panacea_data, nullptr);
    EXPECT_EQ(panacea_data->kind_flags, ITEMKINDF_USE_ON_CHARACTER);
    EXPECT_EQ(panacea_data->use_flags, ITEMUSE_CURE_FREEZE | ITEMUSE_CURE_POISON | ITEMUSE_CURE_CURSE | ITEMUSE_CURE_GOO);
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_CHRONICLE_BENCH_ITEM_USE_OK"), 2);
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());

    const COM_ITEM_INFO *common = GetCommonItemInfo(item_no);
    const ITEM_DATA *item = GetItemData(item_no);
    ASSERT_NE(common, nullptr);
    ASSERT_NE(item, nullptr);
    EXPECT_EQ(common->icon_index, GetCommonItemInfo(ITEM_BREAD)->icon_index);
    EXPECT_EQ(item->use_flags, ITEMUSE_HEAL_HP);
    EXPECT_EQ(item->kind_flags, ITEMKINDF_CONSUMABLE);
    EXPECT_EQ(item->vol, 25);
    const COM_ITEM_INFO *water_common = GetCommonItemInfo(water_item_no);
    const ITEM_DATA *water_data = GetItemData(water_item_no);
    ASSERT_NE(water_common, nullptr);
    ASSERT_NE(water_data, nullptr);
    EXPECT_EQ(water_common->icon_index, GetCommonItemInfo(ITEM_REGULAR_WATER)->icon_index);
    EXPECT_EQ(water_data->use_flags, ITEMUSE_DRINK);
    EXPECT_EQ(water_data->kind_flags, ITEMKINDF_CONSUMABLE);
    EXPECT_EQ(water_data->vol, 15);
    for (const std::string status_name : {"freeze", "poison", "curse", "goo"}) {
        const int native_item = registry.at("items").at("test.chronicle-new-item:remedy-" + status_name).get<int>();
        const ITEM_DATA *remedy = GetItemData(native_item);
        ASSERT_NE(remedy, nullptr);
        EXPECT_EQ(remedy->kind_flags, ITEMKINDF_USE_ON_CHARACTER);
        if (status_name == "freeze") EXPECT_EQ(remedy->use_flags, ITEMUSE_CURE_FREEZE);
        else if (status_name == "poison") EXPECT_EQ(remedy->use_flags, ITEMUSE_CURE_POISON);
        else if (status_name == "curse") EXPECT_EQ(remedy->use_flags, ITEMUSE_CURE_CURSE);
        else EXPECT_EQ(remedy->use_flags, ITEMUSE_CURE_GOO);
    }
}

TEST(MoonModItemRegistration, RefusesItemsAfterTheReservedNativeSlotsAreAllocated) {
    moonmod::ResetChronicleItemsForTests();
    ModItemRestore restore_mod_items;
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    TempRoot root;
    const fs::path package = root.path / "mods" / "test.chronicle-item-capacity";
    Write(package / "mod.json",
          R"json({"schema_version":1,"id":"test.chronicle-item-capacity","name":"Item Capacity Probe","version":"1.0.0","framework_api":1,"games":["chronicle"],"entry":"main.lua","permissions":["content.register"]})json");
    Write(package / "main.lua", R"lua(
local registered = 0
for index = 1, 250 do
  local item, failure = game.items.register({
    id = "reserved-" .. index, name = "Reserved Item " .. index, kind = "consumable",
    template_item = 148, assets = {}, effect = {kind = "heal", amount = index}
  })
  if item == nil then
    moonmod.log("info", "MOONMOD_CHRONICLE_ITEM_SLOT_FAILURE:" .. index .. ":" .. failure.message)
    break
  end
  registered = registered + 1
  assert(item.status == "registered")
end
assert(registered > 0)
local overflow, failure = game.items.register({
  id = "overflow", name = "Overflow Item", kind = "consumable",
  template_item = 148, assets = {}, effect = {kind = "heal", amount = 1}
})
assert(overflow == nil and failure.code == "failed")
moonmod.log("info", "MOONMOD_CHRONICLE_ITEM_CAPACITY:" .. registered)
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.chronicle-item-capacity":true},"settings":{}})json");

    std::vector<std::string> logs;
    std::vector<std::string> errors;
    moonmod::RuntimeConfig config = MakeConfig(root);
    config.log = [&logs, &errors](std::string_view level, std::string_view, std::string_view message) {
        logs.emplace_back(message);
        if (level == "error") errors.emplace_back(message);
    };
    moonmod::Runtime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Start(std::move(config), error)) << error;
    ASSERT_EQ(runtime.Mods().size(), 1u);
    ASSERT_EQ(runtime.Mods()[0].state, moonmod::ModState::Loaded) << runtime.Mods()[0].status_message;
    runtime.Stop();

    const auto capacity_log = std::find_if(logs.begin(), logs.end(), [](const std::string &message) {
        return message.starts_with("MOONMOD_CHRONICLE_ITEM_CAPACITY:");
    });
    ASSERT_NE(capacity_log, logs.end());
    const int capacity = std::stoi(capacity_log->substr(std::string("MOONMOD_CHRONICLE_ITEM_CAPACITY:").size()));
    EXPECT_GT(capacity, 11);
    EXPECT_TRUE(errors.empty()) << (errors.empty() ? "" : errors.front());
    std::ifstream registry_file(root.path / "save" / "moonmod" / "game-content" / "chronicle-item-slots.json",
                                std::ios::binary);
    ASSERT_TRUE(registry_file.good());
    const moonmod::Json registry = moonmod::Json::parse(registry_file);
    EXPECT_EQ(registry.at("items").size(), static_cast<std::size_t>(capacity));
}

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "btmisc.hpp"
#include "dun/gameloop.hpp"
#include "dataread.hpp"
#include "itemdata.hpp"
#include "menu_inventory.hpp"
#include "moonmod/native_resource_overrides.hpp"
#include "main.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/runtime.hpp"
#include "savedata.hpp"
#include "userstatus.hpp"

namespace dc::game {}
using namespace dc::game;


namespace {

namespace fs = std::filesystem;


struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-weapon-event-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void Write(const fs::path &path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << content;
    ASSERT_TRUE(output.good()) << path.string();
}

} // namespace

TEST(MoonModWeaponsApi, ListsAndReadsNativeWeaponDefinitionsAndOwnedState) {
    CSaveData *previous_save = SaveData;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);

    constexpr int dagger_no = ITEM_WEAPON_DAGGER;
    COM_ITEM_INFO *common = GetCommonItemInfo(dagger_no);
    WEAPON_DATA *definition = GetWeaponData(dagger_no);
    ASSERT_NE(common, nullptr);
    ASSERT_NE(definition, nullptr);
    ASSERT_EQ(common->kind, ITEMKIND_WEAPON);

    WEAPON_HAVE &owned = status->chara_weapons[CHARA_TOAN][0];
    owned.item_no = dagger_no;
    owned.level = 3;
    owned.attack = 41;
    owned.endurance = 7;
    owned.speed = 8;
    owned.magic = 9;
    owned.durability = 23;
    owned.experience = 12;
    owned.synthesis_count = 2;
    status->equipped_weapon_slot[CHARA_TOAN] = 0;

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *list_operation = moonmod::FindApiOperation("weapons.list");
    const moonmod::ApiOperationSpec *get_operation = moonmod::FindApiOperation("weapons.get");
    const moonmod::ApiOperationSpec *upgrade_operation = moonmod::FindApiOperation("weapons.upgrade");
    ASSERT_NE(list_operation, nullptr);
    ASSERT_NE(get_operation, nullptr);
    ASSERT_NE(upgrade_operation, nullptr);
    const auto upgrade_capability = std::ranges::find(config.api_capabilities, "weapons.upgrade",
                                                       &moonmod::ApiCapability::path);
    ASSERT_NE(upgrade_capability, config.api_capabilities.end());
    EXPECT_TRUE(upgrade_capability->supported);
    EXPECT_TRUE(upgrade_capability->reason.empty());

    CSaveData *active_test_save = SaveData;
    SaveData = nullptr;
    const moonmod::ApiCallResult no_save = config.api_handler(
        "test.moonmod.chronicle-weapons", *get_operation, moonmod::Json::array({dagger_no}));
    ASSERT_EQ(no_save.status, moonmod::ApiCallStatus::Ok) << no_save.message;
    EXPECT_TRUE(no_save.value.at("owned_count").is_null());
    EXPECT_TRUE(no_save.value.at("instances").is_null());
    const moonmod::ApiCallResult upgrade_no_save = config.api_handler(
        "test.moonmod.chronicle-weapons", *upgrade_operation,
        moonmod::Json::array({moonmod::Json{{"item_no", dagger_no}, {"character", "toan"}, {"slot", 0}}}));
    EXPECT_EQ(upgrade_no_save.status, moonmod::ApiCallStatus::Failed);
    SaveData = active_test_save;

    const moonmod::ApiCallResult listing = config.api_handler(
        "test.moonmod.chronicle-weapons", *list_operation, moonmod::Json::array());
    ASSERT_EQ(listing.status, moonmod::ApiCallStatus::Ok) << listing.message;
    ASSERT_TRUE(listing.value.is_array());
    const std::string dagger_id = "chronicle:weapon:" + std::to_string(dagger_no);
    const auto dagger = std::find_if(listing.value.begin(), listing.value.end(), [&](const moonmod::Json &entry) {
        return entry.value("id", "") == dagger_id;
    });
    ASSERT_NE(dagger, listing.value.end());
    EXPECT_EQ(dagger->value("item_no", -1), dagger_no);
    EXPECT_EQ(dagger->value("message_id", -1), common->msg);
    EXPECT_EQ(dagger->value("icon_index", -1), common->icon_index);
    EXPECT_EQ(dagger->at("stats").value("attack", -1), definition->attack);
    EXPECT_EQ(dagger->at("stats").value("durability", -1), definition->durability);
    EXPECT_EQ(dagger->at("socket_count").get<int>(), definition->hole_num);
    EXPECT_FALSE(dagger->contains("owned_count"));

    const moonmod::ApiCallResult by_stable_id = config.api_handler(
        "test.moonmod.chronicle-weapons", *get_operation, moonmod::Json::array({dagger_id}));
    ASSERT_EQ(by_stable_id.status, moonmod::ApiCallStatus::Ok) << by_stable_id.message;
    EXPECT_EQ(by_stable_id.value.value("owned_count", -1), 1);
    ASSERT_EQ(by_stable_id.value.at("owned_by").size(), 1u);
    EXPECT_EQ(by_stable_id.value.at("owned_by")[0], "toan");
    ASSERT_EQ(by_stable_id.value.at("equipped_by").size(), 1u);
    EXPECT_EQ(by_stable_id.value.at("equipped_by")[0], "toan");
    ASSERT_EQ(by_stable_id.value.at("instances").size(), 1u);
    EXPECT_TRUE(by_stable_id.value.at("instances")[0].value("equipped", false));
    EXPECT_EQ(by_stable_id.value.at("instances")[0].value("level", -1), 3);
    EXPECT_EQ(by_stable_id.value.at("instances")[0].at("stats").value("attack", -1), 41);
    EXPECT_EQ(by_stable_id.value.at("instances")[0].value("experience", -1), 12);

    const moonmod::ApiCallResult by_native_selector = config.api_handler(
        "test.moonmod.chronicle-weapons", *get_operation,
        moonmod::Json::array({moonmod::Json{{"item_no", dagger_no}}}));
    ASSERT_EQ(by_native_selector.status, moonmod::ApiCallStatus::Ok) << by_native_selector.message;
    EXPECT_EQ(by_native_selector.value, by_stable_id.value);

    const moonmod::ApiCallResult mismatched_selector = config.api_handler(
        "test.moonmod.chronicle-weapons", *get_operation,
        moonmod::Json::array({moonmod::Json{{"id", dagger_id}, {"item_no", dagger_no + 1}}}));
    EXPECT_EQ(mismatched_selector.status, moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult nonweapon = config.api_handler(
        "test.moonmod.chronicle-weapons", *get_operation, moonmod::Json::array({ITEM_BREAD}));
    EXPECT_EQ(nonweapon.status, moonmod::ApiCallStatus::InvalidArgument);

    const moonmod::ApiCallResult not_ready = config.api_handler(
        "test.moonmod.chronicle-weapons", *upgrade_operation,
        moonmod::Json::array({moonmod::Json{{"id", dagger_id}, {"character", "toan"}, {"slot", 0}}}));
    ASSERT_EQ(not_ready.status, moonmod::ApiCallStatus::Ok) << not_ready.message;
    EXPECT_FALSE(not_ready.value.value("ready", true));
    EXPECT_FALSE(not_ready.value.value("upgraded", true));
    EXPECT_FALSE(not_ready.value.value("changed", true));
    EXPECT_EQ(not_ready.value.value("level", -1), 3);

    const int required_experience = GetWeaponMaxExp(&owned);
    owned.experience = static_cast<s16>(required_experience);
    owned.attack = std::max(0, static_cast<int>(definition->attack_max) - 8);
    const auto socket = std::find_if(std::begin(definition->hole), std::end(definition->hole),
                                     [](s8 size) { return size > 0; });
    ASSERT_NE(socket, std::end(definition->hole));
    const std::size_t socket_index = static_cast<std::size_t>(socket - std::begin(definition->hole));
    owned.attach[socket_index].item_no = ITEM_ATTACH_ATTACK;
    owned.attach[socket_index].status[0] = 5;
    owned.synthesis_count = 2;
    std::size_t synthesis_socket = 0;
    while (synthesis_socket == socket_index) ++synthesis_socket;
    ASSERT_LT(synthesis_socket, std::size(definition->hole));
    ASSERT_GT(definition->hole[synthesis_socket], 0);
    owned.attach[synthesis_socket].item_no = ITEM_ATTACH_SYNTHESIS_SPHERE;
    owned.attach[synthesis_socket].sphere_weapon_no = dagger_no;
    const int attack_before_levelup = owned.attack;
    const moonmod::ApiCallResult upgraded = config.api_handler(
        "test.moonmod.chronicle-weapons", *upgrade_operation,
        moonmod::Json::array({moonmod::Json{{"id", dagger_id}, {"character", "toan"}, {"slot", 0}}}));
    ASSERT_EQ(upgraded.status, moonmod::ApiCallStatus::Ok) << upgraded.message;
    EXPECT_TRUE(upgraded.value.value("upgraded", false));
    EXPECT_TRUE(upgraded.value.value("changed", false));
    EXPECT_TRUE(upgraded.value.value("ready", false));
    EXPECT_EQ(upgraded.value.value("previous_level", -1), 3);
    EXPECT_EQ(upgraded.value.value("level", -1), 4);
    EXPECT_EQ(upgraded.value.at("stats").value("attack", -1),
              std::min(attack_before_levelup + 6, static_cast<int>(definition->attack_max)));
    EXPECT_EQ(upgraded.value.at("stats").value("endurance", -1), 8);
    EXPECT_EQ(upgraded.value.at("stats").value("speed", -1), 9);
    EXPECT_EQ(upgraded.value.at("stats").value("magic", -1), 10);
    EXPECT_GE(upgraded.value.at("stats").value("durability", -1), 24);
    EXPECT_LE(upgraded.value.at("stats").value("durability", -1), 26);
    EXPECT_EQ(owned.experience, required_experience);
    EXPECT_GT(GetWeaponMaxExp(&owned), required_experience);
    EXPECT_EQ(owned.synthesis_count, 3);
    EXPECT_TRUE(std::ranges::all_of(owned.attach, [](const ATTACH_LIST &attachment) {
        return attachment.item_no == 0;
    }));

    const moonmod::ApiCallResult repeated = config.api_handler(
        "test.moonmod.chronicle-weapons", *upgrade_operation,
        moonmod::Json::array({moonmod::Json{{"id", dagger_id}, {"character", "toan"}, {"slot", 0}}}));
    ASSERT_EQ(repeated.status, moonmod::ApiCallStatus::Ok) << repeated.message;
    EXPECT_FALSE(repeated.value.value("upgraded", true));
    EXPECT_FALSE(repeated.value.value("changed", true));

    status->chara_weapons[CHARA_TOAN][1] = owned;
    const moonmod::ApiCallResult ambiguous = config.api_handler(
        "test.moonmod.chronicle-weapons", *upgrade_operation, moonmod::Json::array({moonmod::Json{dagger_id}}));
    EXPECT_EQ(ambiguous.status, moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult invalid_slot = config.api_handler(
        "test.moonmod.chronicle-weapons", *upgrade_operation,
        moonmod::Json::array({moonmod::Json{{"id", dagger_id}, {"character", "toan"}, {"slot", 11}}}));
    EXPECT_EQ(invalid_slot.status, moonmod::ApiCallStatus::InvalidArgument);

    SaveData = previous_save;
}

TEST(MoonModWeaponsApi, EquipsOwnedCompatibleWeaponOnlyFromTownAndReportsChanges) {
    CSaveData *previous_save = SaveData;
    const s32 previous_mode = mode;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->party_size = 2;
    status->cur_chara = CHARA_TOAN;
    status->cur_georama = 0;
    status->cur_floor = -1;
    status->chara_weapons[CHARA_TOAN][0].item_no = ITEM_WEAPON_DAGGER_BROKEN;
    status->chara_weapons[CHARA_TOAN][2].item_no = ITEM_WEAPON_DAGGER;
    status->equipped_weapon_slot[CHARA_TOAN] = 0;

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *equip = moonmod::FindApiOperation("weapons.equip");
    ASSERT_NE(equip, nullptr);
    const auto capability = std::ranges::find(config.api_capabilities, "weapons.equip",
                                               &moonmod::ApiCapability::path);
    ASSERT_NE(capability, config.api_capabilities.end());
    EXPECT_TRUE(capability->supported);
    EXPECT_TRUE(capability->reason.empty());

    const moonmod::Json selector{{"id", "chronicle:weapon:" + std::to_string(ITEM_WEAPON_DAGGER)},
                                 {"character", "toan"}, {"slot", 2}};
    mode = GAME_MODE_TITLE;
    const moonmod::ApiCallResult title_rejected = config.api_handler(
        "test.moonmod.chronicle-weapons", *equip, moonmod::Json::array({selector}));
    EXPECT_EQ(title_rejected.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(status->equipped_weapon_slot[CHARA_TOAN], 0);

    mode = GAME_MODE_EDIT;
    const moonmod::ApiCallResult equipped = config.api_handler(
        "test.moonmod.chronicle-weapons", *equip, moonmod::Json::array({selector}));
    ASSERT_EQ(equipped.status, moonmod::ApiCallStatus::Ok) << equipped.message;
    EXPECT_EQ(equipped.value.value("character", ""), "toan");
    EXPECT_EQ(equipped.value.value("previous_slot", -1), 0);
    EXPECT_EQ(equipped.value.value("slot", -1), 2);
    EXPECT_EQ(equipped.value.value("previous_item_no", -1), ITEM_WEAPON_DAGGER_BROKEN);
    EXPECT_TRUE(equipped.value.value("equipped", false));
    EXPECT_TRUE(equipped.value.value("changed", false));
    EXPECT_EQ(status->equipped_weapon_slot[CHARA_TOAN], 2);

    const moonmod::ApiCallResult repeated = config.api_handler(
        "test.moonmod.chronicle-weapons", *equip, moonmod::Json::array({selector}));
    ASSERT_EQ(repeated.status, moonmod::ApiCallStatus::Ok) << repeated.message;
    EXPECT_FALSE(repeated.value.value("changed", true));
    EXPECT_EQ(status->equipped_weapon_slot[CHARA_TOAN], 2);

    const moonmod::ApiCallResult missing_weapon = config.api_handler(
        "test.moonmod.chronicle-weapons", *equip,
        moonmod::Json::array({moonmod::Json{{"id", "chronicle:weapon:" + std::to_string(ITEM_WEAPON_DAGGER)},
                                           {"character", "xiao"}}}));
    EXPECT_EQ(missing_weapon.status, moonmod::ApiCallStatus::Failed);
    const moonmod::ApiCallResult incompatible = config.api_handler(
        "test.moonmod.chronicle-weapons", *equip,
        moonmod::Json::array({moonmod::Json{{"id", "chronicle:weapon:" + std::to_string(ITEM_WEAPON_DAGGER)},
                                           {"character", "xiao"}, {"slot", 2}}}));
    EXPECT_EQ(incompatible.status, moonmod::ApiCallStatus::Failed);
    const moonmod::ApiCallResult invalid_slot = config.api_handler(
        "test.moonmod.chronicle-weapons", *equip,
        moonmod::Json::array({moonmod::Json{{"id", "chronicle:weapon:" + std::to_string(ITEM_WEAPON_DAGGER)},
                                           {"character", "toan"}, {"slot", 11}}}));
    EXPECT_EQ(invalid_slot.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(status->equipped_weapon_slot[CHARA_TOAN], 2);

    status->chara_weapons[CHARA_TOAN][0].item_no = ITEM_WEAPON_DAGGER_BROKEN;
    status->equipped_weapon_slot[CHARA_TOAN] = 0;
    mode = GAME_MODE_DUNGEON;
    const s32 previous_dungeon = selectMapNo;
    selectMapNo = -1; // a dungeon mode with no dungeon scene loaded
    const moonmod::ApiCallResult dungeon_rejected = config.api_handler(
        "test.moonmod.chronicle-weapons", *equip, moonmod::Json::array({selector}));
    EXPECT_EQ(dungeon_rejected.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(status->equipped_weapon_slot[CHARA_TOAN], 0);

    selectMapNo = previous_dungeon;

    SaveData = previous_save;
    mode = previous_mode;
}

TEST(MoonModWeaponsApi, RegistersPersistsAndGrantsNativeWeaponDefinitions) {
    CSaveData *previous_save = SaveData;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->party_size = 1;

    TempRoot root;
    moonmod::RuntimeConfig config;
    config.save_root = root.path / "save";
    moonmod::ResetChronicleWeaponsForTests();
    moonmod::ConfigureChronicleApi(config);
    const auto *register_weapon = moonmod::FindApiOperation("weapons.register");
    const auto *give_weapon = moonmod::FindApiOperation("weapons.give");
    const auto *get_weapon = moonmod::FindApiOperation("weapons.get");
    const auto *list_weapons = moonmod::FindApiOperation("weapons.list");
    ASSERT_NE(register_weapon, nullptr);
    ASSERT_NE(give_weapon, nullptr);
    ASSERT_NE(get_weapon, nullptr);
    ASSERT_NE(list_weapons, nullptr);
    const auto capability = std::ranges::find(config.api_capabilities, "weapons.register",
                                               &moonmod::ApiCapability::path);
    ASSERT_NE(capability, config.api_capabilities.end());
    EXPECT_TRUE(capability->supported);
    EXPECT_TRUE(capability->reason.empty());

    const std::string mod_id = "test.moonmod.weapon-registration";
    const moonmod::Json definition{{"id", "annex-blade"}, {"name", "Annex Blade"},
                                   {"template_weapon", ITEM_WEAPON_DAGGER}, {"owner", "toan"},
                                   {"stats", {{"attack", 37}, {"durability", 81}}},
                                   {"stat_caps", {{"attack", 99}, {"magic", 40}}}};
    const auto registered = config.api_handler(mod_id, *register_weapon, moonmod::Json::array({definition}));
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    EXPECT_EQ(registered.value.value("id", ""), mod_id + ":annex-blade");
    EXPECT_EQ(registered.value.value("name", ""), "Annex Blade");
    EXPECT_TRUE(registered.value.value("mod_owned", false));
    EXPECT_EQ(registered.value.value("template_weapon", ""), "chronicle:weapon:258");
    const int native_item_no = registered.value.value("native_item_no", -1);
    ASSERT_GE(native_item_no, 377);
    ASSERT_LT(native_item_no, 409);

    const COM_ITEM_INFO *common = GetCommonItemInfo(native_item_no);
    const WEAPON_DATA *data = GetWeaponData(native_item_no);
    ASSERT_NE(common, nullptr);
    ASSERT_NE(data, nullptr);
    EXPECT_EQ(common->kind, ITEMKIND_WEAPON);
    EXPECT_EQ(common->index, native_item_no - ITEM_WEAPON_START);
    EXPECT_EQ(data->owner, CHARA_TOAN);
    EXPECT_EQ(data->attack, 37);
    EXPECT_EQ(data->durability, 81);
    EXPECT_EQ(data->attack_max, 99);
    EXPECT_EQ(data->magic_max, 40);
    EXPECT_EQ(status->CheckWeaponUser(native_item_no), CHARA_TOAN);

    const auto listed = config.api_handler(mod_id, *list_weapons, moonmod::Json::array());
    ASSERT_EQ(listed.status, moonmod::ApiCallStatus::Ok) << listed.message;
    EXPECT_EQ(std::ranges::count_if(listed.value, [&](const moonmod::Json &entry) {
        return entry.value("id", "") == mod_id + ":annex-blade";
    }), 1);
    const auto got = config.api_handler(mod_id, *get_weapon, moonmod::Json::array({registered.value.at("id")}));
    ASSERT_EQ(got.status, moonmod::ApiCallStatus::Ok) << got.message;
    EXPECT_EQ(got.value.value("name", ""), "Annex Blade");
    EXPECT_EQ(got.value.at("stats").value("attack", -1), 37);

    const auto invalid_assets = config.api_handler(
        mod_id, *register_weapon,
        moonmod::Json::array({moonmod::Json{{"id", "asset-blade"}, {"name", "Asset Blade"},
                                             {"template_weapon", ITEM_WEAPON_DAGGER},
                                             {"assets", {{"model", "new-model"}}}}}));
    EXPECT_EQ(invalid_assets.status, moonmod::ApiCallStatus::InvalidArgument);
    const auto invalid_stat = config.api_handler(
        mod_id, *register_weapon,
        moonmod::Json::array({moonmod::Json{{"id", "invalid-blade"}, {"name", "Invalid Blade"},
                                             {"template_weapon", ITEM_WEAPON_DAGGER},
                                             {"stats", {{"attack", 32768}}}}}));
    EXPECT_EQ(invalid_stat.status, moonmod::ApiCallStatus::InvalidArgument);

    // Exercise Chronicle's native inventory grant path for IDs past the retail owner table.
    const int native_grant_slot = status->GetItem(native_item_no, 0);
    ASSERT_GE(native_grant_slot, 0);
    EXPECT_EQ(status->chara_weapons[CHARA_TOAN][native_grant_slot].item_no, native_item_no);
    const auto given = config.api_handler(
        mod_id, *give_weapon, moonmod::Json::array({moonmod::Json{{"id", registered.value.at("id")}}}));
    ASSERT_EQ(given.status, moonmod::ApiCallStatus::Ok) << given.message;
    EXPECT_TRUE(given.value.value("added", false));
    EXPECT_EQ(status->chara_weapons[CHARA_TOAN][given.value.value("slot", -1)].item_no, native_item_no);

    const auto registry_path = root.path / "save" / "moonmod" / "game-content" / "chronicle-weapon-slots.json";
    ASSERT_TRUE(fs::exists(registry_path));
    moonmod::ResetChronicleWeaponsForTests();
    const auto reloaded = config.api_handler(mod_id, *register_weapon, moonmod::Json::array({definition}));
    ASSERT_EQ(reloaded.status, moonmod::ApiCallStatus::Ok) << reloaded.message;
    EXPECT_EQ(reloaded.value.value("native_item_no", -1), native_item_no);
    EXPECT_EQ(GetWeaponData(native_item_no)->attack, 37);
    moonmod::ResetChronicleWeaponsForTests();
    SaveData = previous_save;
}

TEST(MoonModWeaponsApi, RegisteredWeaponDrawsItsOwnModelAndIcon) {
    TempRoot root;
    const fs::path package = root.path / "package";
    Write(package / "resources" / "annexmdl.chr", std::string(1024, '\x51'));
    Write(package / "resources" / "annexicon.png", "not checked until drawn");
    moonmod::ResetNativeResourceOverridesForTests();
    moonmod::RuntimeConfig config;
    config.save_root = root.path / "save";
    moonmod::ResetChronicleWeaponsForTests();
    moonmod::ConfigureChronicleApi(config);
    const auto *register_weapon = moonmod::FindApiOperation("weapons.register");
    ASSERT_NE(register_weapon, nullptr);
    const std::string mod_id = "test.moonmod.weapon-model";
    auto define = [](const char *id, const char *model) {
        moonmod::Json definition{{"id", id}, {"name", "Annex"}, {"template_weapon", ITEM_WEAPON_DAGGER}, {"owner", "toan"}};
        if (model != nullptr) definition["model"] = model;
        return moonmod::Json::array({definition});
    };

    // The model and icon must be this mod's own overrides.
    EXPECT_EQ(config.api_handler(mod_id, *register_weapon, define("annex", "annexmdl")).status,
              moonmod::ApiCallStatus::InvalidArgument);
    for (const auto &[resource, game_path] : {std::pair{"resources/annexmdl.chr", "commenu/weapon/annexmdl.chr"},
                                              std::pair{"resources/annexicon.png", "icons/annexicon.png"}}) {
        ASSERT_EQ(moonmod::RegisterNativeResourceOverride(
                      mod_id, package, moonmod::Json::array({{{"path", resource}, {"game_path", game_path}}})).status,
                  moonmod::ApiCallStatus::Ok);
    }
    EXPECT_EQ(config.api_handler(mod_id, *register_weapon, define("native", "c01w05")).status,
              moonmod::ApiCallStatus::InvalidArgument);

    moonmod::Json with_icon = define("annex", "annexmdl");
    with_icon[0]["icon"] = "annexicon";
    const auto registered = config.api_handler(mod_id, *register_weapon, with_icon);
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    const int item_no = registered.value.value("native_item_no", -1);
    EXPECT_EQ(registered.value.at("assets").value("model", ""), "commenu/weapon/annexmdl.chr");
    EXPECT_EQ(registered.value.at("assets").value("icon", ""), "icons/annexicon.png");
    EXPECT_EQ(MoonModWeaponTemplate(item_no), ITEM_WEAPON_DAGGER);

    // Every loader names the weapon's files through BtGetWeaponNamePath2/3: the mod's model, the template's .cfg.
    char chr[64] = {};
    char cfg[64] = {};
    BtGetWeaponNamePath3(chr, cfg, item_no);
    EXPECT_STREQ(chr, "annexmdl.chr");
    char template_chr[64] = {};
    char template_cfg[64] = {};
    BtGetWeaponNamePath3(template_chr, template_cfg, ITEM_WEAPON_DAGGER);
    EXPECT_STREQ(cfg, template_cfg);
    EXPECT_TRUE(moonmod::FindNativeResourceOverride("commenu/weapon/annexmdl.chr").has_value());

    // Without a model the weapon draws its template's.
    const auto plain = config.api_handler(mod_id, *register_weapon, define("plain", nullptr));
    ASSERT_EQ(plain.status, moonmod::ApiCallStatus::Ok) << plain.message;
    BtGetWeaponNamePath3(chr, cfg, plain.value.value("native_item_no", -1));
    EXPECT_STREQ(chr, template_chr);
    EXPECT_STREQ(cfg, template_cfg);

    moonmod::ResetChronicleWeaponsForTests();
    moonmod::ResetNativeResourceOverridesForTests();
}

TEST(MoonModWeaponsApi, PublishesNativeUpgradeForQuestObjectivesOnNextPump) {
    moonmod::SharedRuntime().Stop();
    CSaveData *previous_save = SaveData;
    const s32 previous_mode = mode;
    struct Restore {
        CSaveData *save;
        s32 previous_mode;
        ~Restore() {
            moonmod::SharedRuntime().Stop();
            SaveData = save;
            mode = previous_mode;
        }
    } restore{previous_save, previous_mode};

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->party_size = 1;
    status->cur_chara = CHARA_TOAN;
    status->cur_georama = 0;
    status->cur_floor = -1;
    status->chara_weapons[CHARA_TOAN][0].item_no = ITEM_WEAPON_DAGGER_BROKEN;
    status->equipped_weapon_slot[CHARA_TOAN] = 0;
    mode = GAME_MODE_EDIT;
    constexpr int weapon_no = ITEM_WEAPON_DAGGER;
    WEAPON_HAVE &weapon = status->chara_weapons[CHARA_TOAN][2];
    weapon.item_no = weapon_no;
    weapon.level = 2;
    weapon.attack = 15;
    weapon.endurance = 4;
    weapon.speed = 5;
    weapon.magic = 6;
    weapon.durability = 22;
    weapon.experience = static_cast<s16>(GetWeaponMaxExp(&weapon));

    TempRoot root;
    const fs::path package = root.path / "mods" / "test.moonmod.weapon-event";
    Write(package / "mod.json", R"json({
      "schema_version":1,"id":"test.moonmod.weapon-event","name":"Weapon Upgrade Event Probe","version":"1.0.0",
      "framework_api":1,"games":["chronicle"],"entry":"main.lua",
      "permissions":["content.register","game.read","game.write"]
    })json");
    Write(package / "main.lua", R"lua(
assert(game.quests.register({
  id = "upgrade", title = "Upgrade a dagger",
    objectives = {{id = "level", title = "Raise the dagger", target = 1,
    event = "game.weapon.upgraded", match = {item_no = 258, character = "toan", previous_level = 2, level = 3}}}
}))
assert(game.quests.register({
  id = "equip", title = "Equip the dagger",
    objectives = {{id = "loadout", title = "Select the dagger", target = 1,
    event = "game.weapon.equipped", match = {item_no = 258, character = "toan", previous_slot = 0, slot = 2}}}
}))
local changed = false
moonmod.on("game.weapon.upgraded", function(event)
  assert(event.item_no == 258 and event.character == "toan" and event.slot == 2)
  assert(event.previous_level == 2 and event.level == 3 and event.changed == true)
  assert(event.source_mod == "test.moonmod.weapon-event")
  moonmod.log("info", "WEAPON_UPGRADE_EVENT:" .. tostring(event.level))
end)
moonmod.on("game.weapon.equipped", function(event)
  assert(event.item_no == 258 and event.character == "toan")
  assert(event.previous_slot == 0 and event.slot == 2 and event.changed == true)
  assert(event.previous_item_no == 257)
  assert(event.source_mod == "test.moonmod.weapon-event")
  moonmod.log("info", "WEAPON_EQUIP_EVENT:" .. tostring(event.slot))
end)
moonmod.on("game.tick", function()
  if changed then return end
  moonmod.log("info", "WEAPON_UPGRADE_TICK")
  assert(game.quests.start({id = "upgrade"}))
  assert(game.quests.start({id = "equip"}))
  local result, failure = game.weapons.upgrade({id = "chronicle:weapon:258", character = "toan", slot = 2})
  if not result then
    moonmod.log("error", "WEAPON_UPGRADE_FAILURE:" .. tostring(failure and failure.message or failure))
    changed = true
    return
  end
  moonmod.log("info", "WEAPON_UPGRADE_RESULT:" .. tostring(result.upgraded) .. ":" .. tostring(result.level))
  assert(result.upgraded and result.changed and result.level == 3)
  local repeated, repeated_failure = game.weapons.upgrade({id = "chronicle:weapon:258", character = "toan", slot = 2})
  if not repeated then
    moonmod.log("error", "WEAPON_UPGRADE_REPEAT_FAILURE:" .. tostring(repeated_failure and repeated_failure.message or repeated_failure))
    changed = true
    return
  end
  assert(not repeated.changed)
  local equipped, equip_failure = game.weapons.equip({id = "chronicle:weapon:258", character = "toan", slot = 2})
  if not equipped then
    moonmod.log("error", "WEAPON_EQUIP_FAILURE:" .. tostring(equip_failure and equip_failure.message or equip_failure))
    changed = true
    return
  end
  assert(equipped.changed and equipped.equipped and equipped.previous_slot == 0)
  changed = true
end)
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.moonmod.weapon-event":true},"settings":{}})json");

    std::vector<std::string> logs;
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    moonmod::ConfigureChronicleApi(config);
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(std::move(config), error)) << error;
    ASSERT_EQ(moonmod::SharedRuntime().Mods().size(), 1u);
    ASSERT_EQ(moonmod::SharedRuntime().Mods()[0].state, moonmod::ModState::Loaded)
        << moonmod::SharedRuntime().Mods()[0].status_message;

    moonmod::SharedRuntime().Pump();
    EXPECT_TRUE(std::ranges::none_of(logs, [](const std::string &message) {
        return message.find("WEAPON_UPGRADE_EVENT:") != std::string::npos;
    }));
    moonmod::SharedRuntime().Pump();
    std::string probe_logs;
    for (const std::string &message : logs) probe_logs += message + "\n";
    EXPECT_EQ(std::ranges::count_if(logs, [](const std::string &message) {
        return message.find("WEAPON_UPGRADE_EVENT:3") != std::string::npos;
    }), 1) << probe_logs;
    EXPECT_EQ(std::ranges::count_if(logs, [](const std::string &message) {
        return message.find("WEAPON_EQUIP_EVENT:2") != std::string::npos;
    }), 1) << probe_logs;
    const moonmod::ApiCallResult quests = moonmod::SharedRuntime().Quests("test.moonmod.weapon-event");
    ASSERT_EQ(quests.status, moonmod::ApiCallStatus::Ok) << quests.message;
    ASSERT_EQ(quests.value.at("quests").size(), 2u);
    const auto completed_upgrade = std::ranges::find_if(quests.value.at("quests"), [](const moonmod::Json &quest) {
        return quest.value("id", "") == "test.moonmod.weapon-event:upgrade";
    });
    ASSERT_NE(completed_upgrade, quests.value.at("quests").end());
    EXPECT_EQ(completed_upgrade->at("status"), "completed");
    EXPECT_EQ(completed_upgrade->at("objectives").at(0).at("progress"), 1);
    const auto completed_equip = std::ranges::find_if(quests.value.at("quests"), [](const moonmod::Json &quest) {
        return quest.value("id", "") == "test.moonmod.weapon-event:equip";
    });
    ASSERT_NE(completed_equip, quests.value.at("quests").end());
    EXPECT_EQ(completed_equip->at("status"), "completed");
    EXPECT_EQ(completed_equip->at("objectives").at(0).at("progress"), 1);
}

namespace {

int g_weapon_reload_character = -1;
int g_weapon_reload_flag = -1;

void RecordWeaponReload(int character_id, int reload) {
    g_weapon_reload_character = character_id;
    g_weapon_reload_flag = reload;
}

} // namespace

TEST(MoonModWeaponsApi, EquipsInADungeonWithAModelReloadAndRemovesTheSpare) {
    CSaveData *previous_save = SaveData;
    const s32 previous_mode = mode;
    const s32 previous_dungeon = selectMapNo;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    CDngStatusData *status = save->GetDngStatus();
    ASSERT_NE(status, nullptr);
    status->party_size = 2;
    status->cur_chara = CHARA_TOAN;
    status->cur_georama = 0;
    status->cur_floor = 3;
    status->chara_weapons[CHARA_TOAN][0].item_no = ITEM_WEAPON_DAGGER;
    status->chara_weapons[CHARA_TOAN][1].item_no = ITEM_WEAPON_DAGGER + 1;
    status->equipped_weapon_slot[CHARA_TOAN] = 0;
    mode = GAME_MODE_DUNGEON;
    selectMapNo = 0;
    g_weapon_reload_character = -1;
    g_weapon_reload_flag = -1;
    moonmod::SetChronicleCharacterSwitchRoutineForTests(RecordWeaponReload);

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *equip = moonmod::FindApiOperation("weapons.equip");
    const auto *remove = moonmod::FindApiOperation("weapons.remove");
    ASSERT_NE(equip, nullptr);
    ASSERT_NE(remove, nullptr);
    const std::string old_id = "chronicle:weapon:" + std::to_string(ITEM_WEAPON_DAGGER);
    const std::string new_id = "chronicle:weapon:" + std::to_string(ITEM_WEAPON_DAGGER + 1);

    // The equipped weapon cannot be removed.
    const moonmod::ApiCallResult equipped_kept = config.api_handler(
        "test.moonmod.chronicle-weapons", *remove,
        moonmod::Json::array({moonmod::Json{{"id", old_id}, {"character", "toan"}, {"slot", 0}}}));
    EXPECT_EQ(equipped_kept.status, moonmod::ApiCallStatus::Failed);

    // Equipping the active character's other weapon in a dungeon reloads its model like a character switch.
    const moonmod::ApiCallResult swapped = config.api_handler(
        "test.moonmod.chronicle-weapons", *equip,
        moonmod::Json::array({moonmod::Json{{"id", new_id}, {"character", "toan"}, {"slot", 1}}}));
    ASSERT_EQ(swapped.status, moonmod::ApiCallStatus::Ok) << swapped.message;
    EXPECT_TRUE(swapped.value.value("changed", false));
    EXPECT_EQ(status->equipped_weapon_slot[CHARA_TOAN], 1);
    EXPECT_EQ(g_weapon_reload_character, CHARA_TOAN);
    EXPECT_EQ(g_weapon_reload_flag, 1);

    // The old weapon, now spare, goes.
    const moonmod::ApiCallResult removed = config.api_handler(
        "test.moonmod.chronicle-weapons", *remove,
        moonmod::Json::array({moonmod::Json{{"id", old_id}, {"character", "toan"}, {"slot", 0}}}));
    ASSERT_EQ(removed.status, moonmod::ApiCallStatus::Ok) << removed.message;
    EXPECT_TRUE(removed.value.value("removed", false));
    EXPECT_LT(status->chara_weapons[CHARA_TOAN][0].item_no, ITEM_WEAPON_START);
    EXPECT_EQ(status->chara_weapons[CHARA_TOAN][1].item_no, ITEM_WEAPON_DAGGER + 1);

    moonmod::SetChronicleCharacterSwitchRoutineForTests(nullptr);
    selectMapNo = previous_dungeon;
    mode = previous_mode;
    SaveData = previous_save;
}

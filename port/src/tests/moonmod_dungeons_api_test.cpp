#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>

#include "dun/gameloop.hpp"
#include "dungeonmap.hpp"
#include "itemdata.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/dungeonmap_layout.hpp"
#include "moonmod/game_hooks.hpp"
#include "monstorunit.hpp"
#include "savedata.hpp"
#include "userstatus.hpp"

namespace dc::game {}
using namespace dc::game;

namespace {

struct ResetDungeons {
    ResetDungeons() { moonmod::ResetChronicleDungeonsForTests(); }
    ~ResetDungeons() { moonmod::ResetChronicleDungeonsForTests(); }
};

struct ResetContentRegistries {
    ResetContentRegistries() { Reset(); }
    ~ResetContentRegistries() { Reset(); }

    static void Reset() {
        moonmod::ResetChronicleItemsForTests();
        moonmod::ResetChronicleWeaponsForTests();
        moonmod::ResetChronicleCharactersForTests();
        moonmod::ResetChronicleDungeonsForTests();
        moonmod::ResetChronicleGeoramaForTests();
    }
};

} // namespace

TEST(MoonModDungeonsApi, ListsNativeDungeonCatalogAndResolvesStableAndLocalIds) {
    ResetDungeons reset_dungeons;
    CSaveData *previous_save = SaveData;
    struct RestoreSave {
        CSaveData *save;
        ~RestoreSave() { SaveData = save; }
    } restore{previous_save};
    SaveData = nullptr;

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *list_operation = moonmod::FindApiOperation("dungeons.list");
    const moonmod::ApiOperationSpec *get_operation = moonmod::FindApiOperation("dungeons.get");
    ASSERT_NE(list_operation, nullptr);
    ASSERT_NE(get_operation, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "dungeons.list" && capability.supported;
    }));
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return capability.path == "dungeons.get" && capability.supported;
    }));

    const moonmod::ApiCallResult listed = config.api_handler("test.moonmod.dungeons", *list_operation,
                                                              moonmod::Json::array());
    ASSERT_EQ(listed.status, moonmod::ApiCallStatus::Ok) << listed.message;
    ASSERT_EQ(listed.value.size(), 7);

    constexpr std::array<const char *, 7> ids = {
        "chronicle:dungeon:divine-beast-cave", "chronicle:dungeon:wise-owl-forest", "chronicle:dungeon:shipwreck",
        "chronicle:dungeon:sun-and-moon-temple", "chronicle:dungeon:moon-sea", "chronicle:dungeon:gallery-of-time",
        "chronicle:dungeon:demon-shaft"};
    constexpr std::array<int, 7> floor_counts = {15, 17, 18, 18, 15, 25, 100};
    for (std::size_t index = 0; index < ids.size(); ++index) {
        const moonmod::Json &entry = listed.value.at(index);
        EXPECT_EQ(entry.at("id"), ids[index]);
        EXPECT_EQ(entry.at("native_id"), index);
        EXPECT_EQ(entry.at("floor_count"), floor_counts[index]);
        EXPECT_FALSE(entry.at("mod_owned").get<bool>());
        EXPECT_EQ(entry.at("status"), "native");
        EXPECT_TRUE(entry.at("deepest_floor_reached").is_null());

        const moonmod::ApiCallResult by_stable_id = config.api_handler(
            "test.moonmod.dungeons", *get_operation, moonmod::Json::array({ids[index]}));
        ASSERT_EQ(by_stable_id.status, moonmod::ApiCallStatus::Ok) << by_stable_id.message;
        EXPECT_EQ(by_stable_id.value, entry);

        const std::string stable_id(ids[index]);
        const std::string suffix = stable_id.substr(std::string("chronicle:dungeon:").size());
        const moonmod::ApiCallResult by_local_id = config.api_handler(
            "test.moonmod.dungeons", *get_operation, moonmod::Json::array({suffix}));
        ASSERT_EQ(by_local_id.status, moonmod::ApiCallStatus::Ok) << by_local_id.message;
        EXPECT_EQ(by_local_id.value, entry);
    }

    EXPECT_EQ(config.api_handler("test.moonmod.dungeons", *list_operation,
                                 moonmod::Json::array({"unexpected"})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("test.moonmod.dungeons", *get_operation, moonmod::Json::array()).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("test.moonmod.dungeons", *get_operation,
                                 moonmod::Json::array({42})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("test.moonmod.dungeons", *get_operation,
                                 moonmod::Json::array({"made-up-dungeon"})).status,
              moonmod::ApiCallStatus::Failed);

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    save->GetDngStatus()->floor_reached[6] = 12;
    SaveData = save.get();
    const moonmod::ApiCallResult progress = config.api_handler(
        "test.moonmod.dungeons", *get_operation, moonmod::Json::array({"demon-shaft"}));
    ASSERT_EQ(progress.status, moonmod::ApiCallStatus::Ok) << progress.message;
    EXPECT_EQ(progress.value.at("deepest_floor_reached"), 12);
}

TEST(MoonModDungeonsApi, RegistersProceduralFloorsAndExtendsNativeEnemyLayouts) {
    ResetContentRegistries reset_content;
    CSaveData *previous_save = SaveData;
    struct RestoreSave {
        CSaveData *save;
        ~RestoreSave() { SaveData = save; }
    } restore{previous_save};
    SaveData = nullptr;

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *register_operation = moonmod::FindApiOperation("dungeons.register");
    const auto *character_register_operation = moonmod::FindApiOperation("characters.register");
    const auto *content_register_operation = moonmod::FindApiOperation("content.register");
    const auto *content_list_operation = moonmod::FindApiOperation("content.list");
    const auto *content_unregister_operation = moonmod::FindApiOperation("content.unregister");
    const auto *alias_operation = moonmod::FindApiOperation("world.register_dungeon");
    const auto *list_operation = moonmod::FindApiOperation("dungeons.list");
    const auto *get_operation = moonmod::FindApiOperation("dungeons.get");
    ASSERT_NE(register_operation, nullptr);
    ASSERT_NE(character_register_operation, nullptr);
    ASSERT_NE(content_register_operation, nullptr);
    ASSERT_NE(content_list_operation, nullptr);
    ASSERT_NE(content_unregister_operation, nullptr);
    ASSERT_NE(alias_operation, nullptr);
    ASSERT_NE(list_operation, nullptr);
    ASSERT_NE(get_operation, nullptr);
    for (const std::string_view path : {"dungeons.register", "world.register_dungeon"}) {
        const auto capability = std::ranges::find(config.api_capabilities, path, &moonmod::ApiCapability::path);
        ASSERT_NE(capability, config.api_capabilities.end()) << path;
        EXPECT_TRUE(capability->supported) << path;
    }

    const moonmod::ApiCallResult character = config.api_handler(
        "test.moonmod.dungeons", *character_register_operation,
        moonmod::Json::array({{{"id", "dungeon-sentinel"}, {"kind", "enemy"}, {"name", "Dungeon Sentinel"},
                               {"template_monster_id", 0}, {"stats", {{"hp", 450}}}}}));
    ASSERT_EQ(character.status, moonmod::ApiCallStatus::Ok) << character.message;
    const int native_character_id = character.value.at("native_monster_id").get<int>();

    const moonmod::Json definition = {
        {"id", "moon-vault"}, {"name", "Moon Vault"}, {"dungeon", 0}, {"assets", moonmod::Json::array()},
        {"floors", moonmod::Json::array({
            {{"id", "annex"}, {"name", "Moon Vault Annex"}, {"source_floor", 2},
             {"enemies", moonmod::Json::array({"test.moonmod.dungeons:dungeon-sentinel"})}},
            {{"id", "mirror"}, {"source_floor", "annex"}},
        })},
    };
    const moonmod::ApiCallResult registered = config.api_handler(
        "test.moonmod.dungeons", *content_register_operation,
        moonmod::Json::array({{{"type", "dungeon"}, {"definition", definition}}}));
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    EXPECT_EQ(registered.value.at("id"), "test.moonmod.dungeons:moon-vault");
    EXPECT_EQ(registered.value.at("base_dungeon_id"), "chronicle:dungeon:divine-beast-cave");
    EXPECT_EQ(registered.value.at("native_floor_count"), 15);
    EXPECT_EQ(registered.value.at("floor_count"), 17);
    ASSERT_EQ(registered.value.at("floors").size(), 2u);
    EXPECT_EQ(registered.value.at("floors").at(0).at("native_floor_id"), 15);
    EXPECT_EQ(registered.value.at("floors").at(0).at("source_floor"), 2);
    EXPECT_EQ(registered.value.at("floors").at(1).at("native_floor_id"), 16);
    EXPECT_EQ(registered.value.at("floors").at(1).at("source_floor"), 15);
    EXPECT_EQ(maxFloorTbl__3[0], 17);
    EXPECT_EQ(BtEnemyLayoutList[0][15].monster[0].monster_no, native_character_id);
    EXPECT_EQ(BtEnemyLayoutList[0][15].monster[1].monster_no, -1);
    EXPECT_EQ(BtEnemyLayoutList[0][16].monster[0].monster_no, native_character_id);
    EXPECT_EQ(BtEnemyLayoutList[0][16].monster[1].monster_no, -1);
    EXPECT_EQ(BtUraEnemyLayoutList[0][15].monster[0].monster_no, native_character_id);
    EXPECT_EQ(BtUraEnemyLayoutList[0][15].monster[1].monster_no, -1);
    EXPECT_EQ(BtUraEnemyLayoutList[0][16].monster[0].monster_no, native_character_id);
    EXPECT_EQ(BtUraEnemyLayoutList[0][16].monster[1].monster_no, -1);
    ASSERT_EQ(registered.value.at("floors")[0].at("enemies").size(), 1u);
    EXPECT_EQ(registered.value.at("floors")[0].at("enemies")[0], "test.moonmod.dungeons:dungeon-sentinel");
    const moonmod::ApiCallResult referenced_character_removal = config.api_handler(
        "test.moonmod.dungeons", *content_unregister_operation,
        moonmod::Json::array({{{"type", "character"}, {"id", "dungeon-sentinel"}}}));
    EXPECT_EQ(referenced_character_removal.status, moonmod::ApiCallStatus::Failed);
    EXPECT_NE(referenced_character_removal.message.find("registered dungeon floor uses this character variant"),
              std::string::npos);
    EXPECT_NE(GetMonstorTable(native_character_id), nullptr);

    const moonmod::ApiCallResult by_local_id = config.api_handler(
        "test.moonmod.dungeons", *get_operation, moonmod::Json::array({"moon-vault"}));
    ASSERT_EQ(by_local_id.status, moonmod::ApiCallStatus::Ok) << by_local_id.message;
    EXPECT_EQ(by_local_id.value, registered.value);
    const moonmod::ApiCallResult listed = config.api_handler(
        "test.moonmod.dungeons", *list_operation, moonmod::Json::array());
    ASSERT_EQ(listed.status, moonmod::ApiCallStatus::Ok) << listed.message;
    ASSERT_EQ(listed.value.size(), 8u);
    EXPECT_EQ(listed.value.at(0).at("floor_count"), 17);
    EXPECT_EQ(listed.value.at(7).at("id"), "test.moonmod.dungeons:moon-vault");
    const moonmod::ApiCallResult content_listed = config.api_handler(
        "test.moonmod.dungeons", *content_list_operation, moonmod::Json::array());
    ASSERT_EQ(content_listed.status, moonmod::ApiCallStatus::Ok) << content_listed.message;
    ASSERT_EQ(content_listed.value.value("count", 0), 2);
    ASSERT_EQ(content_listed.value.at("entries").size(), 2u);
    EXPECT_TRUE(std::ranges::any_of(content_listed.value.at("entries"), [](const moonmod::Json &entry) {
        return entry.value("content_type", "") == "dungeon" &&
               entry.value("id", "") == "test.moonmod.dungeons:moon-vault";
    }));
    EXPECT_TRUE(std::ranges::any_of(content_listed.value.at("entries"), [](const moonmod::Json &entry) {
        return entry.value("content_type", "") == "character" &&
               entry.value("id", "") == "test.moonmod.dungeons:dungeon-sentinel";
    }));

    const moonmod::Json alias_definition = {
        {"id", "owl-annex"}, {"name", "Owl Annex"}, {"dungeon", 1},
        {"floors", moonmod::Json::array({{{"id", "annex"}, {"source_floor", 3}}})},
    };
    const moonmod::ApiCallResult alias_registered = config.api_handler(
        "test.moonmod.dungeons", *alias_operation, moonmod::Json::array({alias_definition}));
    ASSERT_EQ(alias_registered.status, moonmod::ApiCallStatus::Ok) << alias_registered.message;
    EXPECT_EQ(alias_registered.value.at("base_dungeon_id"), "chronicle:dungeon:wise-owl-forest");
    EXPECT_EQ(maxFloorTbl__3[1], 18);

    moonmod::Json invalid_source = definition;
    invalid_source["id"] = "bad-source";
    invalid_source["floors"] = moonmod::Json::array({{{"id", "broken"}, {"source_floor", 40}}});
    const moonmod::ApiCallResult rejected = config.api_handler(
        "test.moonmod.dungeons", *register_operation, moonmod::Json::array({invalid_source}));
    EXPECT_EQ(rejected.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(maxFloorTbl__3[0], 17);

    moonmod::Json fill_to_limit = definition;
    fill_to_limit["id"] = "deep-annex";
    fill_to_limit["floors"] = moonmod::Json::array();
    for (int index = 0; index < 8; ++index) {
        fill_to_limit["floors"].push_back({{"id", "floor-" + std::to_string(index)}, {"source_floor", 0}});
    }
    const moonmod::ApiCallResult filled = config.api_handler(
        "test.moonmod.dungeons", *register_operation, moonmod::Json::array({fill_to_limit}));
    ASSERT_EQ(filled.status, moonmod::ApiCallStatus::Ok) << filled.message;
    EXPECT_EQ(maxFloorTbl__3[0], 25);
    moonmod::Json beyond_limit = definition;
    beyond_limit["id"] = "overflow";
    beyond_limit["floors"] = moonmod::Json::array({{{"id", "too-far"}, {"source_floor", 0}}});
    const moonmod::ApiCallResult overflow = config.api_handler(
        "test.moonmod.dungeons", *register_operation, moonmod::Json::array({beyond_limit}));
    EXPECT_EQ(overflow.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(maxFloorTbl__3[0], 25);
}

TEST(MoonModDungeonsApi, SetLayoutBoundsMazeSettingsAndRestoresNativeLayout) {
    ASSERT_GE(DNG_GRID, 48);
    ASSERT_GE(DNG_ROOM_MAX, 50);
    struct Restore {
        ~Restore() { DngLayoutRooms = DngLayoutArea = DngLayoutRoomMin = DngLayoutRoomMax = 0; }
    } restore;

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *set_layout = moonmod::FindApiOperation("dungeons.set_layout");
    ASSERT_NE(set_layout, nullptr);
    const auto call = [&](const moonmod::Json &request) {
        return config.api_handler("test.moonmod.maze", *set_layout, moonmod::Json::array({request}));
    };

    EXPECT_EQ(call({{"rooms", DNG_ROOM_MAX + 1}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call({{"rooms", 50}, {"area", DNG_GRID - 1}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call({{"rooms", 50}, {"room_min", 4}, {"room_max", 3}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call({{"rooms", 50}, {"walls", 1}}).status, moonmod::ApiCallStatus::InvalidArgument);
    // 2-cell rooms put their door on a corner, which Chronicle has no piece for.
    EXPECT_EQ(call({{"rooms", 50}, {"room_min", 2}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(DngLayoutRooms, 0);

    const moonmod::ApiCallResult maze = call({{"rooms", 50}, {"area", 46}, {"room_min", 3}, {"room_max", 4}});
    ASSERT_EQ(maze.status, moonmod::ApiCallStatus::Ok) << maze.message;
    EXPECT_EQ(maze.value.at("rooms").get<int>(), 50);
    EXPECT_EQ(maze.value.at("area").get<int>(), 46);
    EXPECT_EQ(maze.value.at("room_min").get<int>(), 3);
    EXPECT_EQ(maze.value.at("room_max").get<int>(), 4);
    EXPECT_EQ(DNG_BUILD_AREA, 46);
    EXPECT_EQ(DNG_BUILD_ROOM_SPAN, 2);
    // The minimap's tile size is not the layout's: the map fits itself to the floor when it is drawn
    // (port/src/dungeonmap.cpp, game.fit_dungeon_map).

    const moonmod::ApiCallResult native = call(moonmod::Json::object());
    ASSERT_EQ(native.status, moonmod::ApiCallStatus::Ok) << native.message;
    EXPECT_EQ(native.value.at("rooms").get<std::string>(), "native");
    EXPECT_EQ(DNG_BUILD_AREA, 16);
    EXPECT_EQ(DNG_BUILD_ROOM_MIN, 3);
}

TEST(MoonModDungeonsApi, SetSeedHoldsOneSeedForTheNextFloor) {
    struct Restore {
        ~Restore() { DngNextSeed = -1; }
    } restore;
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *set_seed = moonmod::FindApiOperation("dungeons.set_seed");
    ASSERT_NE(set_seed, nullptr);
    const auto call = [&](const moonmod::Json &request) {
        return config.api_handler("test.moonmod.seed", *set_seed, moonmod::Json::array({request}));
    };
    EXPECT_EQ(call({{"seed", -1}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call({{"seed", 1.5}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call({{"seed", 2147483648LL}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call({{"floor", 3}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(DngNextSeed, -1);

    const moonmod::ApiCallResult set = call({{"seed", 9001}});
    ASSERT_EQ(set.status, moonmod::ApiCallStatus::Ok) << set.message;
    EXPECT_EQ(set.value.at("seed").get<int>(), 9001);
    EXPECT_EQ(set.value.at("previous").get<std::string>(), "random");
    EXPECT_EQ(DngNextSeed, 9001);

    const moonmod::ApiCallResult cleared = call(moonmod::Json::object());
    ASSERT_EQ(cleared.status, moonmod::ApiCallStatus::Ok) << cleared.message;
    EXPECT_EQ(cleared.value.at("seed").get<std::string>(), "random");
    EXPECT_EQ(cleared.value.at("previous").get<int>(), 9001);
    EXPECT_EQ(DngNextSeed, -1);
}

TEST(MoonModDungeonsApi, SeamlessConcurrentFloorTraversal) {
    struct RestoreState {
        ~RestoreState() {
            moonmod::ResetSeamlessDungeonForTests();
            moonmod::SetSeamlessDungeonEnabled(false);
        }
    } restore;

    // Off by default: the exit keeps the native continue/leave window.
    EXPECT_FALSE(moonmod::IsSeamlessDungeonEnabled());
    moonmod::SetSeamlessDungeonEnabled(true);
    moonmod::ResetSeamlessDungeonForTests();
    EXPECT_TRUE(moonmod::IsSeamlessDungeonEnabled());
    EXPECT_FALSE(moonmod::IsExitDoorPhysicallyOpened());

    // Exit gate unlocked: physical door opens, 0 menus, sets up antechamber
    moonmod::TriggerExitDoorPhysicalOpen();
    EXPECT_TRUE(moonmod::IsExitDoorPhysicallyOpened());

    moonmod::SetupSeamlessAntechamber(10, 10);
    const auto &info = moonmod::GetSeamlessAntechamberInfo();
    EXPECT_TRUE(info.active);
    EXPECT_EQ(info.exit_cell_x, 10);
    EXPECT_EQ(info.exit_cell_z, 10);

    const float center_x = 160.0f * 10;
    const float center_z = 160.0f * 10;

    // Inside chamber, before portals
    float inside_pos[3] = {center_x, 0.0f, center_z};
    EXPECT_EQ(moonmod::CheckSeamlessPortalCrossing(inside_pos), 0);

    // Cross Door A: Next Floor portal (world +Z)
    float door_a_pos[3] = {center_x, 0.0f, center_z + 45.0f};
    EXPECT_EQ(moonmod::CheckSeamlessPortalCrossing(door_a_pos), 1);

    // Cross Door B: Village portal (world +X)
    float door_b_pos[3] = {center_x + 45.0f, 0.0f, center_z};
    EXPECT_EQ(moonmod::CheckSeamlessPortalCrossing(door_b_pos), 2);

    // Test toggle disabling seamless traversal
    moonmod::SetSeamlessDungeonEnabled(false);
    EXPECT_FALSE(moonmod::IsSeamlessDungeonEnabled());
    moonmod::SetSeamlessDungeonEnabled(true);
    EXPECT_TRUE(moonmod::IsSeamlessDungeonEnabled());
}


TEST(MoonModDungeonsApi, FloorRedirectSendsAnExitGatePastASkippedFloor) {
    ResetContentRegistries reset_content;
    moonmod::ResetChronicleFloorRedirectsForTests();
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto call = [&](std::string_view owner, std::string_view path, const moonmod::Json &request) {
        return config.api_handler(owner, *moonmod::FindApiOperation(path), moonmod::Json::array({request}));
    };
    // Five added floors on Divine Beast Cave: 15..19 (the Gauntlet's I, II, III, IV and Warden).
    moonmod::Json floors = moonmod::Json::array();
    for (const char *id : {"i", "ii", "iii", "iv", "warden"}) floors.push_back({{"id", id}, {"source_floor", 2}});
    ASSERT_EQ(call("test.maze", "dungeons.register", {{"id", "maze"}, {"name", "Maze"}, {"dungeon", 0}, {"floors", floors}}).status,
              moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 18), 18); // III's gate leads to IV
    const auto set = call("test.maze", "dungeons.floor_redirect", {{"dungeon_id", 0}, {"floor_id", 18}, {"to_floor_id", 19}});
    ASSERT_EQ(set.status, moonmod::ApiCallStatus::Ok) << set.message;
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 18), 19); // ...now to the Warden
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 17), 17);
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 20), 19); // still clamped to the last floor
    EXPECT_EQ(call("test.other", "dungeons.floor_redirect", {{"dungeon_id", 0}, {"floor_id", 18}, {"to_floor_id", 15}}).status,
              moonmod::ApiCallStatus::Failed);
    for (const moonmod::Json &bad : {moonmod::Json{{"dungeon_id", 0}, {"floor_id", 3}, {"to_floor_id", 19}},
                                     moonmod::Json{{"dungeon_id", 0}, {"floor_id", 18}, {"to_floor_id", 20}},
                                     moonmod::Json{{"dungeon_id", 5}, {"floor_id", 18}},
                                     moonmod::Json{{"dungeon_id", 0}, {"floor_id", 18}, {"to", 19}}}) {
        EXPECT_EQ(call("test.maze", "dungeons.floor_redirect", bad).status, moonmod::ApiCallStatus::InvalidArgument) << bad.dump();
    }
    // Leaving out to_floor_id takes the redirect back; unloading the owner does too.
    EXPECT_EQ(call("test.maze", "dungeons.floor_redirect", {{"dungeon_id", 0}, {"floor_id", 18}}).value["removed"], true);
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 18), 18);
    ASSERT_EQ(call("test.maze", "dungeons.floor_redirect", {{"dungeon_id", 0}, {"floor_id", 18}, {"to_floor_id", 19}}).status,
              moonmod::ApiCallStatus::Ok);
    config.api_owner_unloaded("test.maze");
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 18), std::min(18, maxFloorTbl__3[0] - 1));
    moonmod::ResetChronicleFloorRedirectsForTests();
}

TEST(MoonModDungeonsApi, ARegionsExitGateLeadsPastItsLastFloor) {
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    CSaveData *previous_save = SaveData;
    CUserStatus *previous_user = UserStatus;
    SaveData = save.get();
    UserStatus = save->GetDngStatus();
    const int previous_select = selectMapNo;
    selectMapNo = 0;
    auto *previous_map = NowDngMap;
    const int previous_type = MainDungeonMap.map_type;
    NowDngMap = &MainDungeonMap;
    MainDungeonMap.map_type = 1; // a generated floor
    struct Restore {
        CSaveData *save;
        CUserStatus *user;
        int select;
        decltype(NowDngMap) map;
        int type;
        ~Restore() {
            DngZoneFloorCount = 0;
            DngZoneFloorExit = -1;
            SaveData = save;
            UserStatus = user;
            selectMapNo = select;
            NowDngMap = map;
            MainDungeonMap.map_type = type;
        }
    } restore{previous_save, previous_user, previous_select, previous_map, previous_type};
    UserStatus->cur_floor = 1; // came in by floor 2 of a region of floors 1-4
    DngZoneFloorCount = 4;
    DngZoneFloorSeed = MainDungeonMap.map_seed;
    DngZoneFloorExit = 4;
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 2), 4);  // the gate: past floor 4
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 1), 1);  // any other floor change is left alone
    MainDungeonMap.map_type = 4;                             // a story floor of the game's own map leads on by itself
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 2), 2);
    MainDungeonMap.map_type = 1;
    DngZoneFloorSeed = MainDungeonMap.map_seed + 1;          // a floor built without the region
    EXPECT_EQ(moonmod::ChronicleScriptFloorLevel(0, 2), 2);
}

TEST(MoonModDungeonsApi, ShortcutDoorRequestsCombineByOwnerAndUnload) {
    ResetContentRegistries reset_content;
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto call = [&](std::string_view owner, std::string_view path, const moonmod::Json &request) {
        return config.api_handler(owner, *moonmod::FindApiOperation(path), moonmod::Json::array({request}));
    };
    ASSERT_EQ(call("test.a", "dungeons.set_shortcuts", {{"count", 2}}).status, moonmod::ApiCallStatus::Ok);
    const auto both = call("test.b", "dungeons.set_shortcuts", {{"count", 3}});
    ASSERT_EQ(both.status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(both.value["effective"], 3); // the most any owner asks for
    EXPECT_EQ(DngShortcutDoors, 3);
    for (const moonmod::Json &bad : {moonmod::Json{{"count", 8}}, moonmod::Json{{"count", -1}}, moonmod::Json{{"doors", 1}}}) {
        EXPECT_EQ(call("test.a", "dungeons.set_shortcuts", bad).status, moonmod::ApiCallStatus::InvalidArgument) << bad.dump();
    }
    // open: the key items of doors a floor built again builds open; owners' lists join, and go with the owner.
    ASSERT_EQ(call("test.b", "dungeons.set_shortcuts", {{"count", 3}, {"open", {ITEM_BONE_KEY, ITEM_BONE_KEY + 2}}}).status,
              moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(DngShortcutOpenKeys, 0b101);
    for (const moonmod::Json &bad : {moonmod::Json{{"count", 2}, {"open", {ITEM_BONE_KEY - 1}}},
                                     moonmod::Json{{"count", 2}, {"open", {ITEM_BONE_KEY + 7}}}, moonmod::Json{{"count", 2}, {"open", 3}}}) {
        EXPECT_EQ(call("test.a", "dungeons.set_shortcuts", bad).status, moonmod::ApiCallStatus::InvalidArgument) << bad.dump();
    }
    // illusory: up to 3 walls a blow opens; open_walls builds those numbers open.
    ASSERT_EQ(call("test.b", "dungeons.set_shortcuts", {{"count", 3}, {"illusory", 2}, {"open_walls", {1}}}).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(DngIllusoryWalls, 2);
    EXPECT_EQ(DngIllusoryOpen, 0b10);
    for (const moonmod::Json &bad : {moonmod::Json{{"count", 2}, {"illusory", 4}}, moonmod::Json{{"count", 2}, {"illusory", 1}, {"open_walls", {7}}}}) {
        EXPECT_EQ(call("test.a", "dungeons.set_shortcuts", bad).status, moonmod::ApiCallStatus::InvalidArgument) << bad.dump();
    }
    config.api_owner_unloaded("test.b");
    EXPECT_EQ(DngShortcutDoors, 2);
    EXPECT_EQ(DngShortcutOpenKeys, 0);
    EXPECT_EQ(DngIllusoryWalls, 0);
    EXPECT_EQ(DngIllusoryOpen, 0);
    EXPECT_EQ(call("test.a", "dungeons.set_shortcuts", {{"count", 0}}).value["effective"], 0);
    EXPECT_EQ(DngShortcutDoors, 0);

    // shortcuts_get lists the floor's doors and keys.
    DngShortcutDoorTable[0] = {2, 13, ITEM_BONE_KEY, 37, 10, 0, 1};
    DngShortcutDoorCount = 1;
    const auto doors = call("test.a", "dungeons.shortcuts_get", moonmod::Json::object());
    ASSERT_EQ(doors.status, moonmod::ApiCallStatus::Ok);
    ASSERT_EQ(doors.value["doors"].size(), 1u);
    EXPECT_EQ(doors.value["doors"][0]["key"], "chronicle:item:216");
    EXPECT_EQ(doors.value["doors"][0]["locked"], true);
    DngShortcutDoorCount = 0;

    // Each key has its own colour.
    float bone[3], pitchdark[3];
    DngShortcutDoorTint(ITEM_BONE_KEY, bone);
    DngShortcutDoorTint(ITEM_PITCHDARK_KEY, pitchdark);
    EXPECT_NE(bone[0], pitchdark[0]);
}

TEST(MoonModDungeonsApi, BuiltinItemIconsBelongToOneModAndGoBackOnUnload) {
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto call = [&](std::string_view owner, const moonmod::Json &request) {
        return config.api_handler(owner, *moonmod::FindApiOperation("items.icon_set"), moonmod::Json::array({request}));
    };
    const auto set = call("test.keys", {{"id", "chronicle:item:216"}, {"icon", "key_bone"}});
    ASSERT_EQ(set.status, moonmod::ApiCallStatus::Ok) << set.message;
    EXPECT_EQ(set.value["icon"], "icons/key_bone.png");
    EXPECT_EQ(call("test.other", {{"id", "chronicle:item:216"}, {"icon", "mine"}}).status, moonmod::ApiCallStatus::Failed);
    for (const moonmod::Json &bad : {moonmod::Json{{"id", "chronicle:item:216"}, {"icon", "../x"}},
                                     moonmod::Json{{"id", "chronicle:item:216"}, {"icon", "key.png"}},
                                     moonmod::Json{{"id", "chronicle:weapon:259"}, {"icon", "x"}},
                                     moonmod::Json{{"id", 5}, {"icon", "x"}}}) {
        EXPECT_EQ(call("test.keys", bad).status, moonmod::ApiCallStatus::InvalidArgument) << bad.dump();
    }
    // The owner can put the native icon back; unloading does it too.
    EXPECT_EQ(call("test.keys", {{"id", "chronicle:item:216"}}).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(call("test.other", {{"id", "chronicle:item:216"}, {"icon", "mine"}}).status, moonmod::ApiCallStatus::Ok);
    config.api_owner_unloaded("test.other");
    EXPECT_EQ(call("test.keys", {{"id", "chronicle:item:216"}, {"icon", "key_bone"}}).status, moonmod::ApiCallStatus::Ok);
    config.api_owner_unloaded("test.keys");
}

TEST(MoonModDungeonsApi, SetOpenedChestsOpensThemOnTheNextBuildOnly) {
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *op = moonmod::FindApiOperation("dungeons.set_opened_chests");
    ASSERT_NE(op, nullptr);
    const auto call = [&](const moonmod::Json &request) {
        return config.api_handler("test.moonmod.chests", *op, moonmod::Json::array({request}));
    };
    EXPECT_EQ(call({{"chests", moonmod::Json::array({-1})}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call({{"chests", moonmod::Json::array({DNG_BOX_MAX})}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call({{"chests", 2}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call({{"rooms", moonmod::Json::array({1})}}).status, moonmod::ApiCallStatus::InvalidArgument);

    auto map = std::make_unique<CDungeonMap>();
    const auto place = [&] {
        for (auto &event : map->events) event.kind = DNG_EVENT_NONE;
        for (auto &box : map->boxes) box.used = 0;
        map->box_num = 0;
        for (int i = 0; i < 3; ++i) {
            float pos[4] = {40.0f * i, 0.0f, 0.0f, 1.0f};
            ASSERT_GE(map->SetTreasureBox(pos, 100 + i, TREASURE_BOX_SMALL, 0), 0);
        }
    };
    const auto opened = [&](int chest) {
        for (const auto &event : map->events) {
            if (event.kind == DNG_EVENT_TREASURE_BOX && event.index == chest) return false;
        }
        return map->boxes[chest].closed == 0;
    };

    const moonmod::ApiCallResult set = call({{"chests", moonmod::Json::array({1})}, {"back_chests", moonmod::Json::array({0})}});
    ASSERT_EQ(set.status, moonmod::ApiCallStatus::Ok) << set.message;
    EXPECT_EQ(set.value.at("chests").get<int>(), 1);
    place();
    moonmod::ChronicleFloorEventsBuilt(map.get());
    EXPECT_FALSE(opened(0));
    EXPECT_TRUE(opened(1));
    EXPECT_FALSE(opened(2));

    place(); // held for one build only
    moonmod::ChronicleFloorEventsBuilt(map.get());
    EXPECT_FALSE(opened(1));

    ASSERT_EQ(call(moonmod::Json::object()).status, moonmod::ApiCallStatus::Ok); // clears the back floor's too
}

TEST(MoonModDungeonsApi, SetZonesAsksForAChainOfZones) {
    struct Restore {
        ~Restore() { DngZoneCount = 0; DngZoneOpenMask = 0; DngZoneShrines = 0; }
    } restore;
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *op = moonmod::FindApiOperation("dungeons.set_zones");
    ASSERT_NE(op, nullptr);
    const auto call = [&](const std::string &owner, const moonmod::Json &request) {
        return config.api_handler(owner, *op, moonmod::Json::array({request}));
    };
    const auto zone = [](int rooms) { return moonmod::Json{{"rooms", rooms}}; };
    EXPECT_EQ(call("a", {{"zones", moonmod::Json::array({zone(5)})}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call("a", {{"zones", moonmod::Json::array({zone(5), zone(1)})}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call("a", {{"zones", moonmod::Json::array({zone(5), zone(13)})}}).status, moonmod::ApiCallStatus::InvalidArgument);
    moonmod::Json nine = moonmod::Json::array();
    for (int z = 0; z < DNG_ZONE_MAX + 1; ++z) nine.push_back(zone(5));
    EXPECT_EQ(call("a", {{"zones", nine}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call("a", {{"zones", moonmod::Json::array({zone(5), zone(5)})}, {"open", moonmod::Json::array({2})}}).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call("a", {{"zones", moonmod::Json::array({zone(5), zone(5)})}, {"rooms", 3}}).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(DngZoneCount, 0);

    const moonmod::ApiCallResult set = call("a", {{"zones", moonmod::Json::array({zone(5), zone(6), zone(4)})}, {"open", moonmod::Json::array({2})}});
    ASSERT_EQ(set.status, moonmod::ApiCallStatus::Ok) << set.message;
    EXPECT_EQ(DngZoneCount, 3);
    EXPECT_EQ(DngZoneRooms[1], 6);
    EXPECT_EQ(DngZoneOpenMask, 1 << 2);
    EXPECT_EQ(set.value.at("open").size(), 1u);
    EXPECT_EQ(call("b", {{"zones", moonmod::Json::array({zone(5), zone(5)})}}).status, moonmod::ApiCallStatus::Failed);

    ASSERT_EQ(call("a", moonmod::Json::object()).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(DngZoneCount, 0);
    EXPECT_EQ(call("b", {{"zones", moonmod::Json::array({zone(5), zone(5)})}}).status, moonmod::ApiCallStatus::Ok);
    ASSERT_EQ(call("b", moonmod::Json::object()).status, moonmod::ApiCallStatus::Ok);

    // shrines: true or false, and {} turns them off with the zones
    EXPECT_EQ(call("a", {{"zones", moonmod::Json::array({zone(5), zone(5)})}, {"shrines", 1}}).status, moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult shrined = call("a", {{"zones", moonmod::Json::array({zone(5), zone(5)})}, {"shrines", true}});
    ASSERT_EQ(shrined.status, moonmod::ApiCallStatus::Ok) << shrined.message;
    EXPECT_EQ(DngZoneShrines, 1);
    EXPECT_TRUE(shrined.value.at("shrines").get<bool>());
    ASSERT_EQ(call("a", moonmod::Json::object()).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(DngZoneShrines, 0);

    // sanctuary: true or false, off by default and with {}
    EXPECT_EQ(call("a", {{"zones", moonmod::Json::array({zone(5), zone(5)})}, {"sanctuary", "yes"}}).status,
              moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult safe = call("a", {{"zones", moonmod::Json::array({zone(5), zone(5)})}, {"shrines", true}, {"sanctuary", true}});
    ASSERT_EQ(safe.status, moonmod::ApiCallStatus::Ok) << safe.message;
    EXPECT_EQ(DngZoneSanctuary, 1);
    EXPECT_TRUE(safe.value.at("sanctuary").get<bool>());
    ASSERT_EQ(call("a", moonmod::Json::object()).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(DngZoneSanctuary, 0);
    ASSERT_EQ(call("a", {{"zones", moonmod::Json::array({zone(5), zone(5)})}}).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(DngZoneSanctuary, 0);
    ASSERT_EQ(call("a", moonmod::Json::object()).status, moonmod::ApiCallStatus::Ok);

    // a whole run on one map: 25 zones, each zone quiet, healing or an arena, and no way down to the back side
    moonmod::Json run = moonmod::Json::array();
    for (int z = 0; z < 25; ++z) run.push_back(zone(3));
    run[3]["quiet"] = true;
    run[3]["arena"] = true;
    run[4]["quiet"] = true;
    run[4]["heal"] = true;
    EXPECT_EQ(call("a", {{"zones", moonmod::Json::array({{{"rooms", 3}, {"arena", 1}}, zone(3)})}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call("a", {{"zones", moonmod::Json::array({zone(3), zone(3)})}, {"vaults", "no"}}).status, moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult whole = call("a", {{"zones", run}, {"vaults", false}});
    ASSERT_EQ(whole.status, moonmod::ApiCallStatus::Ok) << whole.message;
    EXPECT_EQ(DngZoneCount, 25);
    EXPECT_EQ(DngZoneQuietMask, (1 << 3) | (1 << 4));
    EXPECT_EQ(DngZoneHealMask, 1 << 4);
    EXPECT_EQ(DngZoneArenaMask, 1 << 3);
    EXPECT_EQ(DngZoneVaults, 0);
    EXPECT_EQ(whole.value.at("arena"), moonmod::Json::array({3}));
    EXPECT_FALSE(whole.value.at("vaults").get<bool>());
    ASSERT_EQ(call("a", moonmod::Json::object()).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(DngZoneQuietMask | DngZoneHealMask | DngZoneArenaMask, 0);
    EXPECT_EQ(DngZoneVaults, 1);
    moonmod::Json too_many = moonmod::Json::array();
    for (int z = 0; z <= DNG_ZONE_MAX; ++z) too_many.push_back(zone(2));
    EXPECT_EQ(call("a", {{"zones", too_many}}).status, moonmod::ApiCallStatus::InvalidArgument);
}

TEST(MoonModDungeonsApi, AZoneOfAnotherDungeonIsBuiltFromItsParts) {
    struct Restore {
        ~Restore() {
            DngZoneCount = 0;
            DngZoneFloorCount = 0;
            std::fill(std::begin(DngZoneSourceDungeon), std::end(DngZoneSourceDungeon), -1);
            std::fill(std::begin(DngZoneFloorDungeon), std::end(DngZoneFloorDungeon), -1);
        }
    } restore;
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *op = moonmod::FindApiOperation("dungeons.set_zones");
    ASSERT_NE(op, nullptr);
    const auto call = [&](const moonmod::Json &request) { return config.api_handler("a", *op, moonmod::Json::array({request})); };
    const auto zone = [](int rooms, int dungeon) {
        moonmod::Json z = {{"rooms", rooms}};
        if (dungeon >= 0) z["source_dungeon"] = dungeon;
        return z;
    };
    // a dungeon from 0 to 6
    EXPECT_EQ(call({{"zones", moonmod::Json::array({zone(4, -1), zone(4, 7)})}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call({{"zones", moonmod::Json::array({zone(4, -1), {{"rooms", 4}, {"source_dungeon", "forest"}}})}}).status,
              moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult set = call({{"zones", moonmod::Json::array({zone(4, -1), zone(4, 1), zone(4, 2), zone(4, 0)})}});
    ASSERT_EQ(set.status, moonmod::ApiCallStatus::Ok) << set.message;
    EXPECT_EQ(set.value.at("source_dungeons"), moonmod::Json::array({-1, 1, 2, 0}));
    // banks for the other dungeons only: a zone of the floor's own dungeon needs none
    EXPECT_EQ(DngPartBanksWanted(0), (1 << 1) | (1 << 2));
    EXPECT_EQ(DngPartBanksWanted(1), (1 << 0) | (1 << 2));

    // With no bank loaded every cell is the map's own part, as before.
    CDungeonMap &map = MainDungeonMap;
    const int    saved_seed = map.map_seed;
    const MAP_CELL saved_cell = map.cells[0];
    const unsigned char saved_zone = DngZoneOfCell[0];
    map.cells[0].parts_no = MAP_PARTS_ROOM_FLOOR;
    DngZoneOfCell[0] = 1;
    DngZoneFloorCount = 4;
    DngZoneFloorSeed = map.map_seed;
    DngZoneFloorDungeon[1] = 1;
    EXPECT_EQ(DngPartBankCount, 0);
    EXPECT_EQ(DngCellPart(&map, 0), &map.parts[MAP_PARTS_ROOM_FLOOR]);
    EXPECT_FALSE(DngZoneDungeonsShown(map.map_seed));
    map.cells[0] = saved_cell;
    DngZoneOfCell[0] = saved_zone;
    map.map_seed = saved_seed;

    ASSERT_EQ(call(moonmod::Json::object()).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(DngPartBanksWanted(0), 0);
    EXPECT_EQ(DngZoneSourceDungeon[1], -1);
}

TEST(MoonModDungeonsApi, NpcModelsMarksAndEventsSayWhatTheyTake) {
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *models = moonmod::FindApiOperation("dungeons.set_npc_models");
    const moonmod::ApiOperationSpec *marks = moonmod::FindApiOperation("dungeons.set_npc_marks");
    const moonmod::ApiOperationSpec *play = moonmod::FindApiOperation("dungeons.play_event");
    ASSERT_NE(models, nullptr);
    ASSERT_NE(marks, nullptr);
    ASSERT_NE(play, nullptr);
    const auto call = [&](const moonmod::ApiOperationSpec *op, const std::string &owner, const moonmod::Json &request) {
        return config.api_handler(owner, *op, moonmod::Json::array({request}));
    };
    const auto king = [](const std::string &pack) {
        return moonmod::Json{{"models", moonmod::Json::array({{{"dungeon_id", 0}, {"npc", 1}, {"pack", pack}}})}};
    };
    // a game pack under dun/ or gedit/, a slot the floors' parts leave free (1-3)
    EXPECT_EQ(call(models, "a", king("../save/x.chr")).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(models, "a", king("dun/d01//e28c03c.chr")).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(models, "a", king("dun/d01/event/e28c03c.mds")).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(models, "a", {{"models", moonmod::Json::array({{{"dungeon_id", 0}, {"npc", 0}, {"pack", "dun/d01/event/e28c03c.chr"}}})}}).status,
              moonmod::ApiCallStatus::InvalidArgument);
    ASSERT_EQ(call(models, "a", king("dun/d01/event/e28c03c.chr")).status, moonmod::ApiCallStatus::Ok);
    ASSERT_EQ(DngNpcModelCount, 1);
    EXPECT_STREQ(DngNpcModels[0].cfg, "e28c03c.cfg");
    EXPECT_EQ(call(models, "b", king("dun/d01/event/e28c03c.chr")).status, moonmod::ApiCallStatus::Failed);
    ASSERT_EQ(call(models, "a", king("dun/mainchara/c07a.chr")).status, moonmod::ApiCallStatus::Ok);
    EXPECT_STREQ(DngNpcModels[0].cfg, "base.cfg");
    ASSERT_EQ(call(models, "a", moonmod::Json::object()).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(DngNpcModelCount, 0);

    // marks and scenes want a dungeon floor
    EXPECT_EQ(call(marks, "a", {{"marks", moonmod::Json::array({{{"npc", 9}, {"x", 1}, {"y", 1}}})}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(marks, "a", {{"marks", moonmod::Json::array({{{"npc", 1}, {"x", 1}, {"y", 1}, {"facing", 4}}})}}).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(marks, "a", {{"marks", moonmod::Json::array({{{"npc", 1}, {"x", 1}, {"y", 1}}})}}).status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(call(marks, "a", moonmod::Json::object()).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(call(play, "a", {{"program", -1}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(play, "a", {{"program", 191}, {"x", 1}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(play, "a", {{"program", 191}}).status, moonmod::ApiCallStatus::Failed);
}

TEST(MoonModDungeonsApi, SetExitsKeepsTownBehindTheShrines) {
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *op = moonmod::FindApiOperation("dungeons.set_exits");
    ASSERT_NE(op, nullptr);
    const auto call = [&](const std::string &owner, const moonmod::Json &request) {
        return config.api_handler(owner, *op, moonmod::Json::array({request}));
    };
    EXPECT_EQ(call("a", {{"town", "never"}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call("a", {{"gate", "shrines"}}).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_FALSE(moonmod::ChronicleTownExitRefused("menu"));
    ASSERT_EQ(call("a", {{"town", "shrines"}}).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(call("b", {{"town", "shrines"}}).status, moonmod::ApiCallStatus::Failed);
    ASSERT_EQ(call("a", {{"town", "any"}}).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(call("b", {{"town", "shrines"}}).status, moonmod::ApiCallStatus::Ok);
    ASSERT_EQ(call("b", moonmod::Json::object()).status, moonmod::ApiCallStatus::Ok);

    // warp and enter_floor want a dungeon floor, and say what they take
    const moonmod::ApiOperationSpec *warp = moonmod::FindApiOperation("dungeons.warp");
    const moonmod::ApiOperationSpec *enter = moonmod::FindApiOperation("dungeons.enter_floor");
    ASSERT_NE(warp, nullptr);
    ASSERT_NE(enter, nullptr);
    EXPECT_EQ(config.api_handler("a", *warp, moonmod::Json::array({{{"x", 3}}})).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("a", *warp, moonmod::Json::array({{{"x", 3}, {"y", 99}}})).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("a", *warp, moonmod::Json::array({{{"x", 3}, {"y", 4}}})).status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(config.api_handler("a", *enter, moonmod::Json::array({{{"floor_id", 1}, {"x", 3}}})).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("a", *enter, moonmod::Json::array({{{"floor_id", 1}, {"room", 3}}})).status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("a", *enter, moonmod::Json::array({{{"floor_id", 1}}})).status, moonmod::ApiCallStatus::Failed);
}

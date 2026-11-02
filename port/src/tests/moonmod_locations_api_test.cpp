#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>

#include "dataread.hpp"
#include "data/data_read.hpp"
#include "app/gameloop.hpp"
#include "gamemode.hpp"
#include "main.hpp"
#include "mainselect.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/native_resource_overrides.hpp"
#include "platform/paths.hpp"
#include "savedata.hpp"

namespace dc::game {}
using namespace dc::game;


TEST(MoonModLocationsApi, ListsAndResolvesNativeWorldMapDestinations) {
    CSaveData *previous_save = SaveData;
    struct RestoreSave {
        CSaveData *save;
        ~RestoreSave() { SaveData = save; }
    } restore{previous_save};

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    save->GetDngStatus()->cur_georama = 0;
    SaveData = save.get();
    ASSERT_EQ(SaveData->VisitMap(0, 1), 1);
    ASSERT_EQ(SaveData->QuestDungeon(0, 1), 1);

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *list_operation = moonmod::FindApiOperation("locations.list");
    const moonmod::ApiOperationSpec *get_operation = moonmod::FindApiOperation("locations.get");
    ASSERT_NE(list_operation, nullptr);
    ASSERT_NE(get_operation, nullptr);
    for (std::string_view path : {"locations.list", "locations.get"}) {
        EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [path](const moonmod::ApiCapability &capability) {
            return capability.path == path && capability.supported;
        }));
    }

    const moonmod::ApiCallResult listed = config.api_handler("test.moonmod.locations", *list_operation,
                                                              moonmod::Json::array());
    ASSERT_EQ(listed.status, moonmod::ApiCallStatus::Ok) << listed.message;
    ASSERT_EQ(listed.value.size(), 16u);
    EXPECT_EQ(listed.value[0].at("id"), "chronicle:location:norune-village");
    EXPECT_EQ(listed.value[0].at("kind"), "settlement");
    EXPECT_EQ(listed.value[0].at("native_map_id"), 0);
    EXPECT_EQ(listed.value[0].at("frame"), "e01");
    EXPECT_TRUE(listed.value[0].at("visited").get<bool>());
    EXPECT_EQ(listed.value[0].at("native_progress"), 1);
    EXPECT_EQ(listed.value[1].at("id"), "chronicle:location:divine-beast-cave");
    EXPECT_EQ(listed.value[1].at("kind"), "dungeon");
    EXPECT_EQ(listed.value[1].at("native_map_id"), 200);
    EXPECT_TRUE(listed.value[1].at("visited").get<bool>());
    EXPECT_EQ(listed.value[1].at("native_progress"), 1);
    EXPECT_EQ(listed.value[15].at("kind"), "challenge");

    const moonmod::ApiCallResult stable_id = config.api_handler(
        "test.moonmod.locations", *get_operation,
        moonmod::Json::array({{{"id", "chronicle:location:divine-beast-cave"}}}));
    ASSERT_EQ(stable_id.status, moonmod::ApiCallStatus::Ok) << stable_id.message;
    EXPECT_EQ(stable_id.value, listed.value[1]);

    const moonmod::ApiCallResult local_id = config.api_handler(
        "test.moonmod.locations", *get_operation, moonmod::Json::array({{{"id", "divine-beast-cave"}}}));
    ASSERT_EQ(local_id.status, moonmod::ApiCallStatus::Ok) << local_id.message;
    EXPECT_EQ(local_id.value, listed.value[1]);

    const moonmod::ApiCallResult missing_id = config.api_handler(
        "test.moonmod.locations", *get_operation, moonmod::Json::array({{{"id", "new-mod-location"}}}));
    EXPECT_EQ(missing_id.status, moonmod::ApiCallStatus::Failed);

    const moonmod::ApiCallResult malformed = config.api_handler(
        "test.moonmod.locations", *get_operation, moonmod::Json::array({{{"name", "Norune"}}}));
    EXPECT_EQ(malformed.status, moonmod::ApiCallStatus::InvalidArgument);
}

TEST(MoonModLocationsApi, RegistersNamespacedFieldAliasAndTransitionsIntoAndBackOut) {
    namespace fs = std::filesystem;
    const fs::path root = fs::temp_directory_path() /
                          ("moonmod_chronicle_location_" +
                           std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    const fs::path package = root / "mods" / "test.location";
    ASSERT_TRUE(fs::create_directories(root / "data" / "gedit" / "s13"));
    ASSERT_TRUE(fs::create_directories(package / "resources"));
    const std::string source_bytes = "native-field-resource";
    const std::string replacement_bytes = "mod-map-data";
    {
        std::ofstream source(root / "data" / "gedit" / "s13" / "field.bin", std::ios::binary);
        ASSERT_TRUE(source.good());
        source.write(source_bytes.data(), static_cast<std::streamsize>(source_bytes.size()));
    }
    {
        std::ofstream replacement(package / "resources" / "mapinfo.cfb", std::ios::binary);
        ASSERT_TRUE(replacement.good());
        replacement.write(replacement_bytes.data(), static_cast<std::streamsize>(replacement_bytes.size()));
    }

    struct SavedGameState {
        explicit SavedGameState(fs::path temporary_root) : root(std::move(temporary_root)) {}
        fs::path root;
        CSaveData *save = SaveData;
        int current_mode = mode;
        int map_no = MapNo;
        int next_map_no = NextMapNo;
        int start_event_no = StartEventNo;
        ~SavedGameState() {
            SaveData = save;
            mode = current_mode;
            MapNo = map_no;
            NextMapNo = next_map_no;
            StartEventNo = start_event_no;
            moonmod::ResetChronicleLocationsForTests();
            moonmod::ResetNativeResourceOverridesForTests();
            PathsReset();
            dc::data::DataReadReset();
            std::error_code error;
            fs::remove_all(root, error);
        }
    } saved_state(root);

    moonmod::ResetChronicleLocationsForTests();
    moonmod::ResetNativeResourceOverridesForTests();
    PathsSetDataRoot(root / "data");
    PathsSetSaveRoot(root / "save");
    InitCDFile();
    ASSERT_TRUE(dc::data::DataDirectoryIndexed("gedit/s13"));
    EXPECT_FALSE(dc::data::DataDirectoryIndexed("gedit/s60"));
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    save->GetDngStatus()->cur_georama = 0;
    SaveData = save.get();

    const moonmod::ApiCallResult asset = moonmod::RegisterNativeResourceOverride(
        "test.location", package,
        moonmod::Json::array({{{"path", "resources/mapinfo.cfb"}, {"game_path", "gedit/s60/mapinfo.cfb"}}}));
    ASSERT_EQ(asset.status, moonmod::ApiCallStatus::Ok) << asset.message;

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *register_operation = moonmod::FindApiOperation("locations.register");
    const auto *list_operation = moonmod::FindApiOperation("locations.list");
    const auto *get_operation = moonmod::FindApiOperation("locations.get");
    const auto *transition_operation = moonmod::FindApiOperation("world.transition");
    const auto *world_operation = moonmod::FindApiOperation("world.get");
    ASSERT_NE(register_operation, nullptr);
    ASSERT_NE(list_operation, nullptr);
    ASSERT_NE(get_operation, nullptr);
    ASSERT_NE(transition_operation, nullptr);
    ASSERT_NE(world_operation, nullptr);
    const auto capability = std::ranges::find(config.api_capabilities, "locations.register",
                                               &moonmod::ApiCapability::path);
    ASSERT_NE(capability, config.api_capabilities.end());
    EXPECT_TRUE(capability->supported);
    EXPECT_TRUE(capability->reason.empty());

    const moonmod::Json definition = moonmod::Json::array({{{"id", "outpost"}, {"name", "Moon Outpost"},
                                                            {"source_map_id", 23}}});
    const moonmod::ApiCallResult registered = config.api_handler("test.location", *register_operation, definition);
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    EXPECT_EQ(registered.value.at("id"), "chronicle:location:test.location:outpost");
    EXPECT_EQ(registered.value.at("native_map_id"), 70);
    EXPECT_EQ(registered.value.at("source_map_id"), 23);
    EXPECT_EQ(registered.value.at("status"), "registered");
    EXPECT_EQ(moonmod::FindNativeResourcePathAlias("gedit/s60/field.bin"), "gedit/s13/field.bin");

    alignas(64) char buffer[16384]{};
    int size = 0;
    std::string aliased_path = "gedit/s60/field.bin";
    ASSERT_EQ(LoadFile2(aliased_path.data(), buffer, &size, 0), 1);
    EXPECT_EQ(size, static_cast<int>(source_bytes.size()));
    EXPECT_EQ(std::string(buffer, buffer + size), source_bytes);
    std::string overridden_path = "gedit/s60/mapinfo.cfb";
    std::memset(buffer, 0, sizeof(buffer));
    ASSERT_EQ(LoadFile2(overridden_path.data(), buffer, &size, 0), 1);
    EXPECT_EQ(size, static_cast<int>(replacement_bytes.size()));
    EXPECT_EQ(std::string(buffer, buffer + size), replacement_bytes);

    const moonmod::ApiCallResult listed = config.api_handler("test.location", *list_operation,
                                                               moonmod::Json::array());
    ASSERT_EQ(listed.status, moonmod::ApiCallStatus::Ok) << listed.message;
    ASSERT_EQ(listed.value.size(), 17u);
    EXPECT_EQ(listed.value[16].at("id"), registered.value.at("id"));
    EXPECT_EQ(listed.value[16].at("source"), "mod_package");
    const moonmod::ApiCallResult queried = config.api_handler(
        "test.location", *get_operation, moonmod::Json::array({{{"id", registered.value.at("id")}}}));
    ASSERT_EQ(queried.status, moonmod::ApiCallStatus::Ok) << queried.message;
    EXPECT_EQ(queried.value.at("native_map_id"), 70);

    mode = GAME_MODE_TITLE;
    MapNo = 800;
    NextMapNo = -1;
    const auto inactive = config.api_handler("test.location", *transition_operation,
                                              moonmod::Json::array({{{"id", registered.value.at("id")}}}));
    EXPECT_EQ(inactive.status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(NextMapNo, -1);

    mode = GAME_MODE_EDIT;
    MapNo = 0;
    const auto entered = config.api_handler("test.location", *transition_operation,
                                             moonmod::Json::array({{{"id", registered.value.at("id")}}}));
    ASSERT_EQ(entered.status, moonmod::ApiCallStatus::Ok) << entered.message;
    EXPECT_EQ(entered.value.at("context"), "mod_location");
    EXPECT_TRUE(entered.value.at("queued").get<bool>());
    EXPECT_EQ(NextMapNo, 70);

    NextMapNo = -1;
    MapNo = 70;
    save->GetDngStatus()->cur_georama = -1;
    const auto world = config.api_handler("test.location", *world_operation, moonmod::Json::array());
    ASSERT_EQ(world.status, moonmod::ApiCallStatus::Ok) << world.message;
    EXPECT_EQ(world.value.at("context"), "location");
    EXPECT_EQ(world.value.at("map_id"), 70);
    EXPECT_EQ(world.value.at("location_id"), registered.value.at("id"));
    EXPECT_TRUE(world.value.at("area_id").is_null());

    const auto returned = config.api_handler(
        "test.location", *transition_operation,
        moonmod::Json::array({{{"id", "chronicle:location:norune-village"}}}));
    ASSERT_EQ(returned.status, moonmod::ApiCallStatus::Ok) << returned.message;
    EXPECT_EQ(NextMapNo, 0);
    EXPECT_TRUE(moonmod::NativeResourceOverrideOwnedBy("test.location", "gedit/s60/mapinfo.cfb"));
}

#include <gtest/gtest.h>

#include <memory>
#include <ranges>

#include "main.hpp"
#include "mainselect.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "savedata.hpp"

namespace dc::game {}
using namespace dc::game;


TEST(MoonModDebugInspect, ReturnsBoundedNativeSceneSnapshot) {
    CSaveData *previous_save = SaveData;
    const int previous_mode = mode;
    const int previous_map = MapNo;
    const int previous_local_map = LocalMapNo;
    struct RestoreState {
        CSaveData *save;
        int mode;
        int map;
        int local_map;
        ~RestoreState() {
            SaveData = save;
            ::mode = mode;
            MapNo = map;
            LocalMapNo = local_map;
        }
    } restore{previous_save, previous_mode, previous_map, previous_local_map};

    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    save->GetDngStatus()->cur_floor = 9;
    mode = GAME_MODE_DUNGEON;
    MapNo = 204;
    LocalMapNo = 4;

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation("debug.inspect");
    ASSERT_NE(operation, nullptr);
    const auto capability = std::ranges::find(config.api_capabilities, "debug.inspect",
                                               &moonmod::ApiCapability::path);
    ASSERT_NE(capability, config.api_capabilities.end());
    EXPECT_TRUE(capability->supported);
    EXPECT_TRUE(capability->reason.empty());

    const moonmod::ApiCallResult snapshot = config.api_handler("test.moonmod.debug", *operation,
                                                                moonmod::Json::array());
    ASSERT_EQ(snapshot.status, moonmod::ApiCallStatus::Ok) << snapshot.message;
    EXPECT_EQ(snapshot.value.at("scene_id"), "dungeon");
    EXPECT_EQ(snapshot.value.at("native_mode"), GAME_MODE_DUNGEON);
    EXPECT_EQ(snapshot.value.at("native_map_id"), 204);
    EXPECT_EQ(snapshot.value.at("native_floor_id"), 9);

    mode = GAME_MODE_TITLE;
    MapNo = 800;
    const moonmod::ApiCallResult title = config.api_handler("test.moonmod.debug", *operation,
                                                             moonmod::Json::array());
    ASSERT_EQ(title.status, moonmod::ApiCallStatus::Ok) << title.message;
    EXPECT_EQ(title.value.at("scene_id"), "title");
    EXPECT_TRUE(title.value.at("native_map_id").is_null());
    EXPECT_TRUE(title.value.at("native_floor_id").is_null());

    const moonmod::ApiCallResult malformed = config.api_handler(
        "test.moonmod.debug", *operation, moonmod::Json::array({{{"target", "memory"}}}));
    EXPECT_EQ(malformed.status, moonmod::ApiCallStatus::InvalidArgument);
}

TEST(MoonModDebugInspect, RegistersAndUnloadsOwnedOverlay) {
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation("debug.overlay_register");
    ASSERT_NE(operation, nullptr);
    const auto capability = std::ranges::find(config.api_capabilities, "debug.overlay_register",
                                               &moonmod::ApiCapability::path);
    ASSERT_NE(capability, config.api_capabilities.end());
    EXPECT_TRUE(capability->supported);
    EXPECT_TRUE(capability->reason.empty());

    constexpr std::string_view owner = "test.moonmod.debug-overlay";
    struct Cleanup {
        moonmod::RuntimeConfig &config;
        ~Cleanup() { if (config.api_owner_unloaded) config.api_owner_unloaded("test.moonmod.debug-overlay"); }
    } cleanup{config};

    const moonmod::ApiCallResult registered = config.api_handler(
        owner, *operation, moonmod::Json::array({{{"id", "status"}, {"text", "PROBE READY"}}}));
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    EXPECT_EQ(registered.value.at("id"), "test.moonmod.debug-overlay:status");
    EXPECT_EQ(moonmod::ChronicleDebugOverlayText(), "PROBE READY");

    const moonmod::ApiCallResult updated = config.api_handler(
        owner, *operation, moonmod::Json::array({{{"id", "status"}, {"text", "UPDATED"}}}));
    ASSERT_EQ(updated.status, moonmod::ApiCallStatus::Ok) << updated.message;
    EXPECT_EQ(moonmod::ChronicleDebugOverlayText(), "UPDATED");

    const moonmod::ApiCallResult malformed = config.api_handler(
        owner, *operation, moonmod::Json::array({{{"id", "status"}, {"text", "BAD\nTEXT"}}}));
    EXPECT_EQ(malformed.status, moonmod::ApiCallStatus::InvalidArgument);
}

TEST(MoonModRenderApi, RegistersUpdatesAndRemovesOwnedHudTextLayer) {
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *register_operation = moonmod::FindApiOperation("render.layer_register");
    const moonmod::ApiOperationSpec *remove_operation = moonmod::FindApiOperation("render.layer_remove");
    ASSERT_NE(register_operation, nullptr);
    ASSERT_NE(remove_operation, nullptr);
    for (const std::string_view path : {"render.layer_register", "render.layer_remove"}) {
        const auto capability = std::ranges::find(config.api_capabilities, path, &moonmod::ApiCapability::path);
        ASSERT_NE(capability, config.api_capabilities.end());
        EXPECT_TRUE(capability->supported) << path;
        EXPECT_TRUE(capability->reason.empty()) << path;
    }

    constexpr std::string_view owner = "test.moonmod.hud-layer";
    struct Cleanup {
        moonmod::RuntimeConfig &config;
        ~Cleanup() { if (config.api_owner_unloaded) config.api_owner_unloaded("test.moonmod.hud-layer"); }
    } cleanup{config};

    const moonmod::ApiCallResult registered = config.api_handler(
        owner, *register_operation, moonmod::Json::array({{{"id", "health"}, {"text", "HP 24/30"}}}));
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    EXPECT_EQ(registered.value.at("id"), "test.moonmod.hud-layer:health");
    EXPECT_EQ(moonmod::ChronicleDebugOverlayText(), "HP 24/30");

    const moonmod::ApiCallResult updated = config.api_handler(
        owner, *register_operation, moonmod::Json::array({{{"id", "health"}, {"text", "HP 25/30"}}}));
    ASSERT_EQ(updated.status, moonmod::ApiCallStatus::Ok) << updated.message;
    EXPECT_EQ(moonmod::ChronicleDebugOverlayText(), "HP 25/30");

    const moonmod::ApiCallResult wrong_owner = config.api_handler(
        "test.moonmod.other", *remove_operation, moonmod::Json::array({{{"id", "health"}}}));
    EXPECT_EQ(wrong_owner.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(moonmod::ChronicleDebugOverlayText(), "HP 25/30");

    const moonmod::ApiCallResult malformed = config.api_handler(
        owner, *register_operation, moonmod::Json::array({{{"id", "health"}, {"text", "BAD\nTEXT"}}}));
    EXPECT_EQ(malformed.status, moonmod::ApiCallStatus::InvalidArgument);

    const moonmod::ApiCallResult removed = config.api_handler(
        owner, *remove_operation, moonmod::Json::array({{{"id", "health"}}}));
    ASSERT_EQ(removed.status, moonmod::ApiCallStatus::Ok) << removed.message;
    EXPECT_TRUE(removed.value.at("removed").get<bool>());
    EXPECT_TRUE(moonmod::ChronicleDebugOverlayText().empty());
}

TEST(MoonModRenderApi, SetsHudIconsAndTheFloorPlateLabel) {
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *icon_set = moonmod::FindApiOperation("render.hud_icon_set");
    const auto *icon_remove = moonmod::FindApiOperation("render.hud_icon_remove");
    const auto *label_set = moonmod::FindApiOperation("render.floor_label_set");
    const auto *label_clear = moonmod::FindApiOperation("render.floor_label_clear");
    ASSERT_NE(icon_set, nullptr);
    ASSERT_NE(icon_remove, nullptr);
    ASSERT_NE(label_set, nullptr);
    ASSERT_NE(label_clear, nullptr);

    constexpr std::string_view owner = "test.moonmod.hud-icons";
    struct Cleanup {
        moonmod::RuntimeConfig &config;
        ~Cleanup() { if (config.api_owner_unloaded) config.api_owner_unloaded("test.moonmod.hud-icons"); }
    } cleanup{config};

    // A native badge, blinking.
    const moonmod::ApiCallResult curse = config.api_handler(
        owner, *icon_set, moonmod::Json::array({{{"id", "curse"}, {"icon", "curse"}, {"blink", true}}}));
    ASSERT_EQ(curse.status, moonmod::ApiCallStatus::Ok) << curse.message;
    auto icons = moonmod::ChronicleHudIcons();
    ASSERT_EQ(icons.size(), 1u);
    EXPECT_EQ(icons[0].native, 3);
    EXPECT_TRUE(icons[0].blink);

    // Unknown badges, both kinds at once, and a png the mod never registered are refused.
    EXPECT_EQ(config.api_handler(owner, *icon_set, moonmod::Json::array({{{"id", "x"}, {"icon", "sparkle"}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler(owner, *icon_set,
                                 moonmod::Json::array({{{"id", "x"}, {"icon", "curse"}, {"image", "hud/x.png"}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler(owner, *icon_set, moonmod::Json::array({{{"id", "x"}, {"image", "hud/chest.png"}}})).status,
              moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(moonmod::ChronicleHudIcons().size(), 1u);

    const moonmod::ApiCallResult removed = config.api_handler(owner, *icon_remove, moonmod::Json::array({{{"id", "curse"}}}));
    ASSERT_EQ(removed.status, moonmod::ApiCallStatus::Ok) << removed.message;
    EXPECT_TRUE(removed.value.at("removed").get<bool>());
    EXPECT_TRUE(moonmod::ChronicleHudIcons().empty());

    // The floor plate: one owner at a time, six characters at most.
    ASSERT_EQ(config.api_handler(owner, *label_set, moonmod::Json::array({{{"text", "2-III"}}})).status,
              moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(moonmod::ChronicleFloorLabel(), "2-III");
    EXPECT_EQ(moonmod::ChronicleFloorNumber(), 0); // the floor card keeps the native number
    ASSERT_EQ(config.api_handler(owner, *label_set, moonmod::Json::array({{{"text", "2-3"}, {"number", 7}}})).status,
              moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(moonmod::ChronicleFloorNumber(), 7);
    EXPECT_EQ(config.api_handler(owner, *label_set, moonmod::Json::array({{{"text", "2-3"}, {"number", 0}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(config.api_handler("test.moonmod.other", *label_set, moonmod::Json::array({{{"text", "9"}}})).status,
              moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(config.api_handler(owner, *label_set, moonmod::Json::array({{{"text", "TOOLONG"}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(moonmod::ChronicleFloorLabel(), "2-3");
    ASSERT_EQ(config.api_handler(owner, *label_clear, moonmod::Json::array()).status, moonmod::ApiCallStatus::Ok);
    EXPECT_TRUE(moonmod::ChronicleFloorLabel().empty());
    EXPECT_EQ(moonmod::ChronicleFloorNumber(), 0);

    // Unloading takes the owner's icons and label away.
    ASSERT_EQ(config.api_handler(owner, *icon_set, moonmod::Json::array({{{"id", "curse"}, {"icon", "curse"}}})).status,
              moonmod::ApiCallStatus::Ok);
    ASSERT_EQ(config.api_handler(owner, *label_set, moonmod::Json::array({{{"text", "1-I"}}})).status,
              moonmod::ApiCallStatus::Ok);
    config.api_owner_unloaded(owner);
    EXPECT_TRUE(moonmod::ChronicleHudIcons().empty());
    EXPECT_TRUE(moonmod::ChronicleFloorLabel().empty());
}

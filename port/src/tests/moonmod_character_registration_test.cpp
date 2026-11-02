#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <cstdio>

#include "dun/gameloop.hpp"
#include "main.hpp"
#include "editloop.hpp"
#include "monstorunit.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/native_resource_overrides.hpp"
#include "moonmod/runtime.hpp"
#include "savedata.hpp"

namespace dc::game {}
using namespace dc::game;


namespace {

namespace fs = std::filesystem;

struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-npc-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
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

TEST(MoonModCharacterRegistration, RegistersADataOnlyEnemyVariantThroughTheNativeModelResolver) {
    moonmod::ResetChronicleCharactersForTests();
    CMonstorUnit *previous_unit = NowMonstorUnit;
    CSaveData *previous_save = SaveData;
    const s32 previous_mode = mode;
    const s32 previous_dungeon = selectMapNo;
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *register_operation = moonmod::FindApiOperation("characters.register");
    const moonmod::ApiOperationSpec *get_operation = moonmod::FindApiOperation("characters.get");
    const moonmod::ApiOperationSpec *list_operation = moonmod::FindApiOperation("characters.list");
    const moonmod::ApiOperationSpec *spawn_operation = moonmod::FindApiOperation("characters.spawn");
    const moonmod::ApiOperationSpec *entity_spawn_operation = moonmod::FindApiOperation("entities.spawn");
    const moonmod::ApiOperationSpec *model_resolve_operation = moonmod::FindApiOperation("models.resolve");
    ASSERT_NE(register_operation, nullptr);
    ASSERT_NE(get_operation, nullptr);
    ASSERT_NE(list_operation, nullptr);
    ASSERT_NE(spawn_operation, nullptr);
    ASSERT_NE(entity_spawn_operation, nullptr);
    ASSERT_NE(model_resolve_operation, nullptr);
    EXPECT_TRUE(std::ranges::any_of(config.api_capabilities, [](const moonmod::ApiCapability &capability) {
        return (capability.path == "characters.register" || capability.path == "characters.spawn" ||
                capability.path == "entities.spawn" || capability.path == "entities.get" ||
                capability.path == "models.resolve") && capability.supported;
    }));

    const moonmod::Json definition = {
        {"id", "bright-sentinel"}, {"kind", "enemy"}, {"name", "Bright Sentinel"},
        {"template_monster_id", 0},
        {"stats", {{"hp", 500}, {"defense", 23}, {"reward_exp", 17}, {"reward_money", 91}}},
        {"assets", moonmod::Json::object()},
    };
    const moonmod::ApiCallResult registration = config.api_handler(
        "test.moonmod.chronicle-character", *register_operation, moonmod::Json::array({definition}));
    ASSERT_EQ(registration.status, moonmod::ApiCallStatus::Ok) << registration.message;
    EXPECT_EQ(registration.value.value("id", ""), "test.moonmod.chronicle-character:bright-sentinel");
    EXPECT_EQ(registration.value.value("kind", ""), "enemy");
    EXPECT_EQ(registration.value.value("model", ""), "e01a");
    EXPECT_EQ(registration.value.value("script", ""), "e01a");
    EXPECT_EQ(registration.value.at("stats").value("hp", 0), 500);
    EXPECT_EQ(registration.value.at("stats").value("defense", 0), 23);

    const int native_id = registration.value.at("native_monster_id").get<int>();
    ASSERT_EQ(native_id, 0x400);
    MONSTOR_MODEL *native_model = GetMonstorTable(native_id);
    ASSERT_NE(native_model, nullptr);
    EXPECT_STREQ(native_model->model_name[0], "e01a");
    EXPECT_STREQ(native_model->script_name, "e01a");
    EXPECT_EQ(native_model->max_hp, 500);
    EXPECT_EQ(native_model->defense, 23);
    EXPECT_EQ(native_model->exp, 17);
    EXPECT_EQ(native_model->money, 91);

    const moonmod::ApiCallResult resolved_model = config.api_handler(
        "test.moonmod.chronicle-character", *model_resolve_operation,
        moonmod::Json::array({{{"monster_id", native_id}}}));
    ASSERT_EQ(resolved_model.status, moonmod::ApiCallStatus::Ok) << resolved_model.message;
    EXPECT_EQ(resolved_model.value.value("model", ""), "e01a");
    EXPECT_EQ(resolved_model.value.value("source", ""), "native_table");
    EXPECT_EQ(resolved_model.value.value("character_id", ""), "test.moonmod.chronicle-character:bright-sentinel");
    ASSERT_EQ(resolved_model.value.at("assets").size(), 1u);
    EXPECT_EQ(resolved_model.value.at("assets")[0].value("game_path", ""), "dun/monstor/e01a.chr");
    EXPECT_FALSE(resolved_model.value.at("assets")[0].value("overridden", true));

    const moonmod::Json spawn_request = {
        {"character", registration.value.at("id")},
        {"position", {{"x", 1.0}, {"y", 0.0}, {"z", -2.0}}},
    };
    const moonmod::ApiCallResult non_owner = config.api_handler(
        "test.moonmod.other", *spawn_operation, moonmod::Json::array({spawn_request}));
    EXPECT_EQ(non_owner.status, moonmod::ApiCallStatus::Failed);
    const moonmod::ApiCallResult no_dungeon = config.api_handler(
        "test.moonmod.chronicle-character", *spawn_operation, moonmod::Json::array({spawn_request}));
    EXPECT_EQ(no_dungeon.status, moonmod::ApiCallStatus::Failed);

    auto unit = std::make_unique<CMonstorUnit>();
    NowMonstorUnit = unit.get();
    SaveData->GetDngStatus()->cur_georama = 0;
    selectMapNo = 0;
    mode = GAME_MODE_DUNGEON;
    unit->model_count = 0;
    moonmod::ChronicleMonstersCleared(unit.get());
    const moonmod::ApiCallResult native_template_unloaded = config.api_handler(
        "test.moonmod.chronicle-character", *model_resolve_operation,
        moonmod::Json::array({{{"monster_id", 0}}}));
    ASSERT_EQ(native_template_unloaded.status, moonmod::ApiCallStatus::Ok) << native_template_unloaded.message;
    EXPECT_FALSE(native_template_unloaded.value.value("loaded_on_current_floor", true));

    unit->model_count = 1;
    moonmod::ChronicleMonsterModelLoaded(unit.get(), 0, 0);
    const moonmod::ApiCallResult native_template_loaded = config.api_handler(
        "test.moonmod.chronicle-character", *model_resolve_operation,
        moonmod::Json::array({{{"monster_id", 0}}}));
    ASSERT_EQ(native_template_loaded.status, moonmod::ApiCallStatus::Ok) << native_template_loaded.message;
    EXPECT_TRUE(native_template_loaded.value.value("loaded_on_current_floor", false));

    moonmod::ChronicleMonstersCleared(unit.get());
    const moonmod::ApiCallResult native_template_after_actor_clear = config.api_handler(
        "test.moonmod.chronicle-character", *model_resolve_operation,
        moonmod::Json::array({{{"monster_id", 0}}}));
    ASSERT_EQ(native_template_after_actor_clear.status, moonmod::ApiCallStatus::Ok)
        << native_template_after_actor_clear.message;
    EXPECT_TRUE(native_template_after_actor_clear.value.value("loaded_on_current_floor", false));

    unit->model_count = 0;
    moonmod::ChronicleMonstersCleared(unit.get());
    const moonmod::ApiCallResult unloaded_template = config.api_handler(
        "test.moonmod.chronicle-character", *entity_spawn_operation, moonmod::Json::array({spawn_request}));
    EXPECT_EQ(unloaded_template.status, moonmod::ApiCallStatus::Failed);
    EXPECT_NE(unloaded_template.message.find("native template is not loaded on this floor"), std::string::npos);
    moonmod::Json invalid_position = spawn_request;
    invalid_position["position"]["x"] = 1e100;
    const moonmod::ApiCallResult invalid_spawn = config.api_handler(
        "test.moonmod.chronicle-character", *spawn_operation, moonmod::Json::array({invalid_position}));
    EXPECT_EQ(invalid_spawn.status, moonmod::ApiCallStatus::InvalidArgument);

    const moonmod::ApiCallResult found = config.api_handler(
        "test.moonmod.chronicle-character", *get_operation,
        moonmod::Json::array({moonmod::Json{{"id", registration.value.at("id")}}}));
    ASSERT_EQ(found.status, moonmod::ApiCallStatus::Ok) << found.message;
    EXPECT_EQ(found.value.value("native_monster_id", -1), native_id);

    const moonmod::ApiCallResult listed = config.api_handler(
        "test.moonmod.chronicle-character", *list_operation, moonmod::Json::array());
    ASSERT_EQ(listed.status, moonmod::ApiCallStatus::Ok) << listed.message;
    EXPECT_TRUE(std::ranges::any_of(listed.value, [](const moonmod::Json &entry) {
        return entry.value("id", "") == "test.moonmod.chronicle-character:bright-sentinel";
    }));

    const moonmod::ApiCallResult duplicate = config.api_handler(
        "test.moonmod.chronicle-character", *register_operation, moonmod::Json::array({definition}));
    EXPECT_EQ(duplicate.status, moonmod::ApiCallStatus::InvalidArgument);
    moonmod::Json invalid_stats = definition;
    invalid_stats["id"] = "unsupported-attack";
    invalid_stats["stats"]["attack"] = 99;
    const moonmod::ApiCallResult invalid = config.api_handler(
        "test.moonmod.chronicle-character", *register_operation, moonmod::Json::array({invalid_stats}));
    EXPECT_EQ(invalid.status, moonmod::ApiCallStatus::InvalidArgument);

    SaveData = previous_save;
    NowMonstorUnit = previous_unit;
    mode = previous_mode;
    selectMapNo = previous_dungeon;
    moonmod::ResetChronicleCharactersForTests();
}

TEST(MoonModModelRegistration, OwnedCustomAssetBindsToNativeMonsterAndCharacterTables) {
    moonmod::ResetChronicleCharactersForTests();
    moonmod::ResetNativeResourceOverridesForTests();
    const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path package = std::filesystem::temp_directory_path() /
                                          ("moonmod-chronicle-model-registration-" + std::to_string(unique_suffix));
    std::filesystem::create_directories(package / "resources");
    std::ofstream(package / "resources" / "model.chr", std::ios::binary) << "model-bytes";
    std::ofstream(package / "resources" / "brain.stb", std::ios::binary) << "script-bytes";
    struct Cleanup {
        std::filesystem::path package;
        ~Cleanup() {
            moonmod::ResetNativeResourceOverridesForTests();
            moonmod::ResetChronicleCharactersForTests();
            std::error_code error;
            std::filesystem::remove_all(package, error);
        }
    } cleanup{package};

    constexpr std::string_view owner = "test.moonmod.custom-model";
    ASSERT_EQ(moonmod::RegisterNativeResourceOverride(
                  owner, package,
                  moonmod::Json::array({{{"path", "resources/model.chr"},
                                         {"game_path", "dun/monstor/moon_sentinel.chr"}}})).status,
              moonmod::ApiCallStatus::Ok);
    ASSERT_EQ(moonmod::RegisterNativeResourceOverride(
                  owner, package,
                  moonmod::Json::array({{{"path", "resources/brain.stb"},
                                         {"game_path", "dun/monstor/moon_brain.stb"}}})).status,
              moonmod::ApiCallStatus::Ok);

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *model_register = moonmod::FindApiOperation("models.register");
    const auto *model_unregister = moonmod::FindApiOperation("models.unregister");
    const auto *model_replace = moonmod::FindApiOperation("models.replace");
    const auto *model_remove = moonmod::FindApiOperation("models.remove");
    const auto *model_resolve = moonmod::FindApiOperation("models.resolve");
    const auto *character_register = moonmod::FindApiOperation("characters.register");
    ASSERT_NE(model_register, nullptr);
    ASSERT_NE(model_unregister, nullptr);
    ASSERT_NE(model_replace, nullptr);
    ASSERT_NE(model_remove, nullptr);
    ASSERT_NE(model_resolve, nullptr);
    ASSERT_NE(character_register, nullptr);
    const auto supported = std::ranges::find(config.api_capabilities, "models.register",
                                              &moonmod::ApiCapability::path);
    ASSERT_NE(supported, config.api_capabilities.end());
    EXPECT_TRUE(supported->supported) << supported->reason;
    for (const char *path : {"models.replace", "models.remove"}) {
        const auto capability = std::ranges::find(config.api_capabilities, path, &moonmod::ApiCapability::path);
        ASSERT_NE(capability, config.api_capabilities.end());
        EXPECT_TRUE(capability->supported) << capability->reason;
    }

    const moonmod::Json model_definition = {
        {"id", "retail-sentinel-model"}, {"template_monster_id", 0},
        {"model", "moon_sentinel"}, {"script", "moon_brain"},
    };
    const moonmod::ApiCallResult model = config.api_handler(
        owner, *model_register, moonmod::Json::array({model_definition}));
    ASSERT_EQ(model.status, moonmod::ApiCallStatus::Ok) << model.message;
    EXPECT_EQ(model.value.value("id", ""), "test.moonmod.custom-model:retail-sentinel-model");
    EXPECT_EQ(model.value.value("status", ""), "registered");
    EXPECT_EQ(model.value.at("assets")[0].value("game_path", ""), "dun/monstor/moon_sentinel.chr");
    EXPECT_TRUE(model.value.at("assets")[0].value("overridden", false));
    EXPECT_TRUE(model.value.at("assets")[1].value("overridden", false));

    const std::string original_native_model = GetMonstorTable(0)->model_name[0];
    const moonmod::ApiCallResult replaced_native_model = config.api_handler(
        owner, *model_replace, moonmod::Json::array({{{"monster_id", 0}, {"model", "moon_sentinel"}}}));
    ASSERT_EQ(replaced_native_model.status, moonmod::ApiCallStatus::Ok) << replaced_native_model.message;
    EXPECT_EQ(replaced_native_model.value.value("previous_model", ""), original_native_model);
    EXPECT_EQ(replaced_native_model.value.value("game_path", ""), "dun/monstor/moon_sentinel.chr");
    EXPECT_STREQ(GetMonstorTable(0)->model_name[0], "moon_sentinel");
    const moonmod::ApiCallResult native_model_resolve = config.api_handler(
        owner, *model_resolve, moonmod::Json::array({{{"monster_id", 0}}}));
    ASSERT_EQ(native_model_resolve.status, moonmod::ApiCallStatus::Ok) << native_model_resolve.message;
    EXPECT_EQ(native_model_resolve.value.value("model", ""), "moon_sentinel");
    ASSERT_TRUE(static_cast<bool>(config.api_resource_override_removal_guard));
    const moonmod::Json model_resource_path = moonmod::Json::array({{{"game_path", "DUN/MONSTOR/MOON_SENTINEL.CHR"}}});
    EXPECT_NE(config.api_resource_override_removal_guard(owner, model_resource_path).find("registered model"),
              std::string::npos);
    const moonmod::Json custom_script_path = moonmod::Json::array({{{"game_path", "dun/monstor/moon_brain.stb"}}});
    EXPECT_NE(config.api_resource_override_removal_guard(owner, custom_script_path).find("registered model"),
              std::string::npos);
    EXPECT_TRUE(config.api_resource_override_removal_guard("test.moonmod.foreign", model_resource_path).empty());
    const moonmod::ApiCallResult foreign_remove = config.api_handler(
        "test.moonmod.foreign", *model_remove, moonmod::Json::array({{{"monster_id", 0}}}));
    EXPECT_EQ(foreign_remove.status, moonmod::ApiCallStatus::PermissionDenied);
    const moonmod::ApiCallResult removed_native_model = config.api_handler(
        owner, *model_remove, moonmod::Json::array({{{"monster_id", 0}}}));
    ASSERT_EQ(removed_native_model.status, moonmod::ApiCallStatus::Ok) << removed_native_model.message;
    EXPECT_TRUE(removed_native_model.value.value("removed", false));
    EXPECT_STREQ(GetMonstorTable(0)->model_name[0], original_native_model.c_str());
    EXPECT_NE(config.api_resource_override_removal_guard(owner, model_resource_path).find("registered model"),
              std::string::npos);
    const moonmod::ApiCallResult replaced_for_unload = config.api_handler(
        owner, *model_replace, moonmod::Json::array({{{"monster_id", 0}, {"model", "moon_sentinel"}}}));
    ASSERT_EQ(replaced_for_unload.status, moonmod::ApiCallStatus::Ok) << replaced_for_unload.message;
    ASSERT_TRUE(static_cast<bool>(config.api_owner_unloaded));
    config.api_owner_unloaded(owner);
    EXPECT_STREQ(GetMonstorTable(0)->model_name[0], original_native_model.c_str());

    const moonmod::ApiCallResult by_id = config.api_handler(
        owner, *model_resolve,
        moonmod::Json::array({{{"id", "test.moonmod.custom-model:retail-sentinel-model"}}}));
    ASSERT_EQ(by_id.status, moonmod::ApiCallStatus::Ok) << by_id.message;
    EXPECT_EQ(by_id.value.value("model", ""), "moon_sentinel");
    EXPECT_FALSE(by_id.value.value("loaded_on_current_floor", true));

    const moonmod::Json character_definition = {
        {"id", "retail-sentinel"}, {"name", "Retail Sentinel"},
        {"kind", "enemy"}, {"model", "test.moonmod.custom-model:retail-sentinel-model"},
        {"stats", {{"hp", 350}, {"defense", 19}}},
    };
    const moonmod::ApiCallResult character = config.api_handler(
        owner, *character_register, moonmod::Json::array({character_definition}));
    ASSERT_EQ(character.status, moonmod::ApiCallStatus::Ok) << character.message;
    EXPECT_EQ(character.value.value("model_id", ""), "test.moonmod.custom-model:retail-sentinel-model");
    EXPECT_EQ(character.value.value("model", ""), "moon_sentinel");
    EXPECT_EQ(character.value.value("script", ""), "moon_brain");
    const int native_id = character.value.at("native_monster_id").get<int>();
    const MONSTOR_MODEL *native_definition = GetMonstorTable(native_id);
    ASSERT_NE(native_definition, nullptr);
    EXPECT_STREQ(native_definition->model_name[0], "moon_sentinel");
    EXPECT_STREQ(native_definition->script_name, "moon_brain");

    const moonmod::ApiCallResult by_native_id = config.api_handler(
        owner, *model_resolve, moonmod::Json::array({{{"monster_id", native_id}}}));
    ASSERT_EQ(by_native_id.status, moonmod::ApiCallStatus::Ok) << by_native_id.message;
    EXPECT_EQ(by_native_id.value.value("model_id", ""), "test.moonmod.custom-model:retail-sentinel-model");
    EXPECT_EQ(by_native_id.value.at("assets")[0].value("game_path", ""), "dun/monstor/moon_sentinel.chr");
    EXPECT_TRUE(by_native_id.value.at("assets")[0].value("overridden", false));

    const moonmod::ApiCallResult foreign_character = config.api_handler(
        "test.moonmod.foreign", *character_register,
        moonmod::Json::array({{{"id", "foreign-sentinel"}, {"name", "Foreign Sentinel"},
                               {"model", "test.moonmod.custom-model:retail-sentinel-model"}}}));
    EXPECT_EQ(foreign_character.status, moonmod::ApiCallStatus::Failed);
    const moonmod::ApiCallResult referenced_model_removal = config.api_handler(
        owner, *model_unregister,
        moonmod::Json::array({{{"id", "retail-sentinel-model"}}}));
    EXPECT_EQ(referenced_model_removal.status, moonmod::ApiCallStatus::Failed);
    EXPECT_NE(referenced_model_removal.message.find("registered character uses this model binding"),
              std::string::npos);
}

TEST(MoonModAnimationRegistration, BindsOwnedModelMotionAndRejectsPlaybackWithoutOwnedSpawn) {
    moonmod::ResetChronicleCharactersForTests();
    moonmod::ResetNativeResourceOverridesForTests();
    const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path package = std::filesystem::temp_directory_path() /
                                          ("moonmod-chronicle-animation-" + std::to_string(unique_suffix));
    std::filesystem::create_directories(package / "resources");
    std::ofstream(package / "resources" / "motion.chr", std::ios::binary) << "model-with-motion-data";
    struct Cleanup {
        std::filesystem::path package;
        ~Cleanup() {
            moonmod::ResetNativeResourceOverridesForTests();
            moonmod::ResetChronicleCharactersForTests();
            std::error_code error;
            std::filesystem::remove_all(package, error);
        }
    } cleanup{package};

    constexpr std::string_view owner = "test.moonmod.animation-probe";
    ASSERT_EQ(moonmod::RegisterNativeResourceOverride(
                  owner, package,
                  moonmod::Json::array({{{"path", "resources/motion.chr"},
                                         {"game_path", "dun/monstor/moon_motion.chr"}}})).status,
              moonmod::ApiCallStatus::Ok);
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *model_register = moonmod::FindApiOperation("models.register");
    const auto *animation_register = moonmod::FindApiOperation("animations.register");
    const auto *animation_play = moonmod::FindApiOperation("animations.play");
    const auto *animation_stop = moonmod::FindApiOperation("animations.stop");
    ASSERT_NE(model_register, nullptr);
    ASSERT_NE(animation_register, nullptr);
    ASSERT_NE(animation_play, nullptr);
    ASSERT_NE(animation_stop, nullptr);
    for (const char *path : {"animations.register", "animations.play", "animations.stop"}) {
        const auto capability = std::ranges::find(config.api_capabilities, path, &moonmod::ApiCapability::path);
        ASSERT_NE(capability, config.api_capabilities.end());
        EXPECT_TRUE(capability->supported) << capability->reason;
        EXPECT_TRUE(capability->reason.empty());
    }
    const moonmod::ApiCallResult model = config.api_handler(
        owner, *model_register,
        moonmod::Json::array({{{"id", "motion-model"}, {"template_monster_id", 0}, {"model", "moon_motion"}}}));
    ASSERT_EQ(model.status, moonmod::ApiCallStatus::Ok) << model.message;

    const moonmod::Json animation_definition = {
        {"id", "idle"}, {"model", model.value.at("id")}, {"motion_index", 0},
    };
    const moonmod::ApiCallResult registered = config.api_handler(
        owner, *animation_register, moonmod::Json::array({animation_definition}));
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    EXPECT_EQ(registered.value.value("id", ""), "test.moonmod.animation-probe:idle");
    EXPECT_EQ(registered.value.value("model", ""), "test.moonmod.animation-probe:motion-model");
    EXPECT_EQ(registered.value.value("motion_index", -1), 0);
    EXPECT_EQ(registered.value.value("motion_validation", ""), "deferred_until_model_is_loaded");
    EXPECT_EQ(config.api_handler(owner, *animation_register,
                                 moonmod::Json::array({animation_definition})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    moonmod::Json invalid_motion = animation_definition;
    invalid_motion["id"] = "bad-motion";
    invalid_motion["motion_index"] = 256;
    EXPECT_EQ(config.api_handler(owner, *animation_register,
                                 moonmod::Json::array({invalid_motion})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult no_spawn = config.api_handler(
        owner, *animation_play,
        moonmod::Json::array({{{"animation", "idle"}, {"entity", "chronicle:monster:0:1"}}}));
    EXPECT_EQ(no_spawn.status, moonmod::ApiCallStatus::Failed);
    EXPECT_NE(no_spawn.message.find("characters.spawn"), std::string::npos);
    const moonmod::ApiCallResult no_playback = config.api_handler(
        owner, *animation_stop,
        moonmod::Json::array({{{"entity", "chronicle:monster:0:1"}}}));
    EXPECT_EQ(no_playback.status, moonmod::ApiCallStatus::Failed);
    EXPECT_NE(no_playback.message.find("no active MoonMod animation"), std::string::npos);
    ASSERT_TRUE(static_cast<bool>(config.api_owner_unloaded));
    config.api_owner_unloaded(owner);
    EXPECT_EQ(config.api_handler(owner, *animation_register,
                                 moonmod::Json::array({animation_definition})).status,
              moonmod::ApiCallStatus::Ok);
}

TEST(MoonModContentApi, GenericContentRegistrationAndListingUseTheTypedCharacterAdapter) {
    ResetContentRegistries reset_content;
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *register_operation = moonmod::FindApiOperation("content.register");
    const auto *list_operation = moonmod::FindApiOperation("content.list");
    const auto *unregister_operation = moonmod::FindApiOperation("content.unregister");
    ASSERT_NE(register_operation, nullptr);
    ASSERT_NE(list_operation, nullptr);
    ASSERT_NE(unregister_operation, nullptr);

    const moonmod::Json definition = {
        {"id", "content-sentinel"}, {"kind", "enemy"}, {"name", "Content Sentinel"},
        {"template_monster_id", 0}, {"stats", {{"hp", 240}}},
    };
    const moonmod::ApiCallResult registered = config.api_handler(
        "test.moonmod.content-probe", *register_operation,
        moonmod::Json::array({{{"type", "character"}, {"definition", definition}}}));
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    EXPECT_EQ(registered.value.value("id", ""), "test.moonmod.content-probe:content-sentinel");

    const moonmod::ApiCallResult listed = config.api_handler(
        "test.moonmod.content-probe", *list_operation, moonmod::Json::array());
    ASSERT_EQ(listed.status, moonmod::ApiCallStatus::Ok) << listed.message;
    EXPECT_EQ(listed.value.value("count", 0), 1u);
    ASSERT_EQ(listed.value.at("entries").size(), 1u);
    EXPECT_EQ(listed.value.at("entries")[0].value("content_type", ""), "character");
    EXPECT_EQ(listed.value.at("entries")[0].value("id", ""), "test.moonmod.content-probe:content-sentinel");
    const int native_model_no = registered.value.at("native_monster_id").get<int>();
    EXPECT_NE(GetMonstorTable(native_model_no), nullptr);

    const auto wrong_owner = config.api_handler(
        "test.moonmod.other", *unregister_operation,
        moonmod::Json::array({{{"type", "character"}, {"id", "content-sentinel"}}}));
    EXPECT_EQ(wrong_owner.status, moonmod::ApiCallStatus::InvalidArgument);
    const auto unsupported_type = config.api_handler(
        "test.moonmod.content-probe", *unregister_operation,
        moonmod::Json::array({{{"type", "item"}, {"id", "content-sentinel"}}}));
    EXPECT_EQ(unsupported_type.status, moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_NE(unsupported_type.message.find("character and unused Georama variants"), std::string::npos);

    const moonmod::ApiCallResult removed = config.api_handler(
        "test.moonmod.content-probe", *unregister_operation,
        moonmod::Json::array({{{"type", "character"}, {"id", "content-sentinel"}}}));
    ASSERT_EQ(removed.status, moonmod::ApiCallStatus::Ok) << removed.message;
    EXPECT_EQ(removed.value.value("id", ""), "test.moonmod.content-probe:content-sentinel");
    EXPECT_EQ(removed.value.value("type", ""), "character");
    EXPECT_TRUE(removed.value.value("removed", false));
    EXPECT_EQ(GetMonstorTable(native_model_no), nullptr);

    const moonmod::ApiCallResult removed_listing = config.api_handler(
        "test.moonmod.content-probe", *list_operation, moonmod::Json::array());
    ASSERT_EQ(removed_listing.status, moonmod::ApiCallStatus::Ok) << removed_listing.message;
    EXPECT_EQ(removed_listing.value.value("count", 0), 0u);
    EXPECT_EQ(config.api_handler(
        "test.moonmod.content-probe", *unregister_operation,
        moonmod::Json::array({{{"type", "character"}, {"id", "content-sentinel"}}})).status,
        moonmod::ApiCallStatus::InvalidArgument);

    const moonmod::ApiCallResult unknown_type = config.api_handler(
        "test.moonmod.content-probe", *register_operation,
        moonmod::Json::array({{{"type", "model"}, {"definition", definition}}}));
    EXPECT_EQ(unknown_type.status, moonmod::ApiCallStatus::InvalidArgument);
    const auto unregister_capability = std::ranges::find(
        config.api_capabilities, "content.unregister", &moonmod::ApiCapability::path);
    ASSERT_NE(unregister_capability, config.api_capabilities.end());
    EXPECT_TRUE(unregister_capability->supported);
    EXPECT_TRUE(unregister_capability->reason.empty());
    moonmod::ResetChronicleCharactersForTests();
}

TEST(MoonModContentApi, KeepsCharacterVariantUntilRegisteredChildIsRemoved) {
    struct ResetCharacters {
        ~ResetCharacters() { moonmod::ResetChronicleCharactersForTests(); }
    } reset;
    moonmod::ResetChronicleCharactersForTests();
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *register_operation = moonmod::FindApiOperation("content.register");
    const auto *unregister_operation = moonmod::FindApiOperation("content.unregister");
    ASSERT_NE(register_operation, nullptr);
    ASSERT_NE(unregister_operation, nullptr);

    const std::string owner = "test.moonmod.content-dependency-probe";
    const moonmod::Json parent_definition = {
        {"id", "parent"}, {"kind", "enemy"}, {"name", "Parent Variant"}, {"template_monster_id", 0},
    };
    const moonmod::ApiCallResult parent = config.api_handler(
        owner, *register_operation,
        moonmod::Json::array({{{"type", "character"}, {"definition", parent_definition}}}));
    ASSERT_EQ(parent.status, moonmod::ApiCallStatus::Ok) << parent.message;
    const moonmod::Json child_definition = {
        {"id", "child"}, {"kind", "enemy"}, {"name", "Child Variant"},
        {"template_monster_id", parent.value.at("native_monster_id")},
    };
    const moonmod::ApiCallResult child = config.api_handler(
        owner, *register_operation,
        moonmod::Json::array({{{"type", "character"}, {"definition", child_definition}}}));
    ASSERT_EQ(child.status, moonmod::ApiCallStatus::Ok) << child.message;

    const moonmod::ApiCallResult parent_while_referenced = config.api_handler(
        owner, *unregister_operation,
        moonmod::Json::array({{{"type", "character"}, {"id", "parent"}}}));
    EXPECT_EQ(parent_while_referenced.status, moonmod::ApiCallStatus::Failed);
    EXPECT_NE(parent_while_referenced.message.find("inherits this variant"), std::string::npos);
    EXPECT_NE(GetMonstorTable(parent.value.at("native_monster_id").get<int>()), nullptr);

    const moonmod::ApiCallResult remove_child = config.api_handler(
        owner, *unregister_operation,
        moonmod::Json::array({{{"type", "character"}, {"id", "child"}}}));
    ASSERT_EQ(remove_child.status, moonmod::ApiCallStatus::Ok) << remove_child.message;
    const moonmod::ApiCallResult remove_parent = config.api_handler(
        owner, *unregister_operation,
        moonmod::Json::array({{{"type", "character"}, {"id", "parent"}}}));
    ASSERT_EQ(remove_parent.status, moonmod::ApiCallStatus::Ok) << remove_parent.message;
    EXPECT_EQ(GetMonstorTable(parent.value.at("native_monster_id").get<int>()), nullptr);
}

TEST(MoonModContentApi, KeepsCharacterVariantWhileRegisteredModelUsesIt) {
    moonmod::ResetChronicleCharactersForTests();
    moonmod::ResetNativeResourceOverridesForTests();
    const auto unique_suffix = std::chrono::steady_clock::now().time_since_epoch().count();
    const std::filesystem::path package = std::filesystem::temp_directory_path() /
                                          ("moonmod-chronicle-content-model-guard-" +
                                           std::to_string(unique_suffix));
    std::filesystem::create_directories(package / "resources");
    std::ofstream(package / "resources" / "dependent.chr", std::ios::binary) << "dependent-model-bytes";
    struct Cleanup {
        std::filesystem::path package;
        ~Cleanup() {
            moonmod::ResetNativeResourceOverridesForTests();
            moonmod::ResetChronicleCharactersForTests();
            std::error_code error;
            std::filesystem::remove_all(package, error);
        }
    } cleanup{package};

    constexpr std::string_view owner = "test.moonmod.content-model-guard";
    ASSERT_EQ(moonmod::RegisterNativeResourceOverride(
                  owner, package,
                  moonmod::Json::array({{{"path", "resources/dependent.chr"},
                                         {"game_path", "dun/monstor/dependent_model.chr"}}})).status,
              moonmod::ApiCallStatus::Ok);
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *content_register = moonmod::FindApiOperation("content.register");
    const auto *content_unregister = moonmod::FindApiOperation("content.unregister");
    const auto *model_register = moonmod::FindApiOperation("models.register");
    ASSERT_NE(content_register, nullptr);
    ASSERT_NE(content_unregister, nullptr);
    ASSERT_NE(model_register, nullptr);

    const moonmod::ApiCallResult character = config.api_handler(
        owner, *content_register,
        moonmod::Json::array({{{"type", "character"},
                              {"definition", {{"id", "model-parent"}, {"kind", "enemy"},
                                               {"name", "Model Parent"}, {"template_monster_id", 0}}}}}));
    ASSERT_EQ(character.status, moonmod::ApiCallStatus::Ok) << character.message;
    const int native_model_no = character.value.at("native_monster_id").get<int>();
    const moonmod::ApiCallResult model = config.api_handler(
        owner, *model_register,
        moonmod::Json::array({{{"id", "dependent-model"}, {"template_monster_id", native_model_no},
                               {"model", "dependent_model"}}}));
    ASSERT_EQ(model.status, moonmod::ApiCallStatus::Ok) << model.message;

    const moonmod::ApiCallResult removal = config.api_handler(
        owner, *content_unregister,
        moonmod::Json::array({{{"type", "character"}, {"id", "model-parent"}}}));
    EXPECT_EQ(removal.status, moonmod::ApiCallStatus::Failed);
    EXPECT_NE(removal.message.find("registered model binding uses this character"), std::string::npos);
    EXPECT_NE(GetMonstorTable(native_model_no), nullptr);
}

TEST(MoonModContentApi, KeepsCharacterVariantWhileLoadedOrTrackedByActiveDungeon) {
    moonmod::ResetChronicleCharactersForTests();
    struct ResetCharacters {
        ~ResetCharacters() { moonmod::ResetChronicleCharactersForTests(); }
    } reset;
    CMonstorUnit *previous_unit = NowMonstorUnit;
    const s32 previous_mode = mode;
    const s32 previous_dungeon = selectMapNo;
    struct RestoreWorld {
        CMonstorUnit *unit;
        s32 mode;
        s32 dungeon;
        ~RestoreWorld() {
            NowMonstorUnit = unit;
            ::mode = mode;
            selectMapNo = dungeon;
        }
    } restore{previous_unit, previous_mode, previous_dungeon};

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *content_register = moonmod::FindApiOperation("content.register");
    const auto *content_unregister = moonmod::FindApiOperation("content.unregister");
    ASSERT_NE(content_register, nullptr);
    ASSERT_NE(content_unregister, nullptr);
    constexpr std::string_view owner = "test.moonmod.content-active-guard";
    const moonmod::ApiCallResult character = config.api_handler(
        owner, *content_register,
        moonmod::Json::array({{{"type", "character"},
                              {"definition", {{"id", "active-sentinel"}, {"kind", "enemy"},
                                               {"name", "Active Sentinel"}, {"template_monster_id", 0}}}}}));
    ASSERT_EQ(character.status, moonmod::ApiCallStatus::Ok) << character.message;
    const int native_model_no = character.value.at("native_monster_id").get<int>();

    // Heap: the PC unit holds MONSTOR_SLOT_MAX actors, too big for the stack.
    const auto unit_storage = std::make_unique<CMonstorUnit>();
    CMonstorUnit &unit = *unit_storage;
    NowMonstorUnit = &unit;
    mode = GAME_MODE_DUNGEON;
    selectMapNo = 0;
    unit.model_count = 1;
    moonmod::ChronicleMonsterModelLoaded(&unit, 0, native_model_no);
    const moonmod::Json remove_request = moonmod::Json::array(
        {{{"type", "character"}, {"id", "active-sentinel"}}});
    const moonmod::ApiCallResult loaded_removal = config.api_handler(owner, *content_unregister, remove_request);
    EXPECT_EQ(loaded_removal.status, moonmod::ApiCallStatus::Failed);
    EXPECT_NE(loaded_removal.message.find("loaded in the active dungeon"), std::string::npos);

    unit.model_count = 0;
    moonmod::ChronicleMonstersCleared(&unit);
    moonmod::ChronicleMonsterActorCreated(&unit, 0);
    moonmod::ChronicleMonsterActorOwned(&unit, 0, owner, "test.moonmod.content-active-guard:active-sentinel",
                                       native_model_no);
    const moonmod::ApiCallResult actor_removal = config.api_handler(owner, *content_unregister, remove_request);
    EXPECT_EQ(actor_removal.status, moonmod::ApiCallStatus::Failed);
    EXPECT_NE(actor_removal.message.find("live or tracked dungeon actor"), std::string::npos);

    moonmod::ChronicleMonstersCleared(&unit);
    const moonmod::ApiCallResult removed = config.api_handler(owner, *content_unregister, remove_request);
    ASSERT_EQ(removed.status, moonmod::ApiCallStatus::Ok) << removed.message;
    EXPECT_EQ(GetMonstorTable(native_model_no), nullptr);
}

TEST(MoonModCharacterRegistration, RegistersTownVillagersInFreeNativeSlots) {
    TempRoot root;
    const fs::path package = root.path / "package";
    fs::create_directories(package / "resources");
    { std::ofstream(package / "resources" / "moonnpc.chr", std::ios::binary) << std::string(256, 'n'); }
    moonmod::ResetNativeResourceOverridesForTests();
    moonmod::ResetChronicleCharactersForTests();
    moonmod::RuntimeConfig config;
    config.save_root = root.path / "save";
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *register_operation = moonmod::FindApiOperation("characters.register");
    ASSERT_NE(register_operation, nullptr);
    const std::string mod_id = "test.moonmod.npc";
    ASSERT_EQ(moonmod::RegisterNativeResourceOverride(
                  mod_id, package, moonmod::Json::array({{{"path", "resources/moonnpc.chr"},
                                                          {"game_path", "gedit/e02/chara/moonnpc.chr"}}})).status,
              moonmod::ApiCallStatus::Ok);
    auto define = [](const char *id) {
        return moonmod::Json::array({moonmod::Json{{"id", id}, {"kind", "npc"}, {"name", "Moon Trader"}, {"town_id", 1},
                                                   {"model", "moonnpc"}, {"position", {{"x", 10}, {"y", 0}, {"z", -20}}},
                                                   {"facing", 90}, {"talk_event", 300}}});
    };
    const auto registered = config.api_handler(mod_id, *register_operation, define("trader"));
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    EXPECT_EQ(registered.value.at("game_path"), "gedit/e02/chara/moonnpc.chr");
    EXPECT_FALSE(registered.value.at("in_town").get<bool>());
    EXPECT_EQ(config.api_handler(mod_id, *register_operation, define("trader")).status,
              moonmod::ApiCallStatus::InvalidArgument);

    // The town picks six native villagers; the mod's takes the first free slot and keeps its place.
    VILLAGER_INFO villagers[10]{};
    for (int slot = 0; slot < 6; ++slot) std::snprintf(villagers[slot].name, sizeof villagers[slot].name, "v%d", slot);
    const int previous_map = NowEditMap;
    const int previous_map_no = MapNo;
    NowEditMap = 1;
    MapNo = 1;
    ChronicleAddModVillagers(villagers, nullptr);
    EXPECT_STREQ(villagers[6].name, "moonnpc");
    EXPECT_EQ(villagers[6].talk_event_no, 300);
    EXPECT_EQ(villagers[6].talk_event_level, 0);
    EXPECT_FLOAT_EQ(villagers[6].position[2], -20.0f);
    EXPECT_EQ(ChronicleModVillagerInSlot(6), 0);
    EXPECT_EQ(ChronicleModVillagerTalkEvent(0), 300);
    // Another town leaves the slots alone.
    VILLAGER_INFO other[10]{};
    NowEditMap = 2;
    ChronicleAddModVillagers(other, nullptr);
    EXPECT_EQ(other[0].name[0], '\0');
    EXPECT_EQ(ChronicleModVillagerInSlot(0), -1);
    NowEditMap = previous_map;
    MapNo = previous_map_no;

    moonmod::ResetChronicleCharactersForTests();
    moonmod::ResetNativeResourceOverridesForTests();
}

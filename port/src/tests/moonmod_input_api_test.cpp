#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/runtime.hpp"
#include "platform/input.hpp"

namespace dc::game {}
using namespace dc::game;

namespace {

namespace fs = std::filesystem;

struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-input-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void Write(const fs::path &path, std::string_view contents) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << contents;
    ASSERT_TRUE(out.good()) << path.string();
}

} // namespace

TEST(MoonModInputApi, BindsReadsConsumesUnbindsAndCleansUpOnOwnerUnload) {
    TempRoot root;
    const std::string owner = "test.input-probe";
    const fs::path package = root.path / "mods" / owner;
    Write(package / "mod.json",
          R"json({"schema_version":1,"id":"test.input-probe","name":"Input Probe","version":"1.0.0","framework_api":1,"games":["chronicle"],"entry":"main.lua","permissions":["game.read","game.write"]})json");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.input-probe":true},"settings":{}})json");
    Write(package / "main.lua", R"lua(
local bound = assert(game.input.bind({id = "quick_menu", keys = {"K", "Gamepad:back"}}))
assert(bound.id == "test.input-probe:quick_menu")
assert(bound.context == "default")
assert(#bound.keys == 2 and bound.keys[1] == "K" and bound.keys[2] == "Gamepad:back")
local modal = assert(game.input.bind({id = "modal_confirm", keys = {"K"}, context = "menu"}))
assert(modal.context == "test.input-probe:menu")
assert(game.input.bind({id = "cleanup_probe", keys = {"J"}}).id == "test.input-probe:cleanup_probe")
local input_context = assert(game.input.context_get())
assert(input_context.active == "default" and input_context.depth == 0 and #input_context.stack == 0)
local reserved_default, reserved_default_error = game.input.context_push({id = "default"})
assert(reserved_default == nil and reserved_default_error.code == "invalid_argument")
local lower = assert(game.input.context_push({id = "menu"}))
local overlay = assert(game.input.context_push({id = "overlay"}))
assert(overlay.depth == 2 and overlay.active == "test.input-probe:overlay")
assert(not assert(game.input.state({id = "modal_confirm"})).active)
assert(game.input.context_pop({token = lower.token}).active == "test.input-probe:overlay")
assert(game.input.context_pop({token = overlay.token}).active == "default")
local modal_context = assert(game.input.context_push({id = "menu"}))
local invalid_axis, invalid_axis_error = game.input.axis({axis = "trigger"})
assert(invalid_axis == nil and invalid_axis_error.code == "invalid_argument")
local invalid_pad, invalid_pad_error = game.input.axis({axis = "left_x", pad = 2})
assert(invalid_pad == nil and invalid_pad_error.code == "invalid_argument")
local unknown_field, unknown_field_error = game.input.axis({axis = "left_x", source = "raw"})
assert(unknown_field == nil and unknown_field_error.code == "invalid_argument")
local disconnected = assert(game.input.axis({axis = "left_x", pad = 1}))
assert(not disconnected.connected and disconnected.value == 0)
assert(not assert(game.input.state({id = "quick_menu"})).down)
assert(not assert(game.input.consume({id = "quick_menu"})).pressed)
moonmod.on("game.tick", function(tick)
  local state = assert(game.input.state({id = "quick_menu"}))
  if tick.frame == 1 then
    local modal_state = assert(game.input.state({id = "modal_confirm"}))
    assert(not state.active and not state.down)
    assert(modal_state.active and modal_state.down)
    local left_x = assert(game.input.axis({axis = "left_x"}))
    local left_y = assert(game.input.axis({axis = "left_y"}))
    local right_x = assert(game.input.axis({axis = "right_x"}))
    local right_y = assert(game.input.axis({axis = "right_y"}))
    local left_trigger = assert(game.input.axis({axis = "left_trigger"}))
    local right_trigger = assert(game.input.axis({axis = "right_trigger"}))
    local second_pad = assert(game.input.axis({axis = "right_x", pad = 1}))
    local second_pad_trigger = assert(game.input.axis({axis = "left_trigger", pad = 1}))
    assert(left_x.connected and left_x.value == -1)
    assert(left_y.connected and left_y.value == 1)
    assert(right_x.connected and right_x.value == 0)
    assert(right_y.connected and right_y.value == 1)
    assert(left_trigger.connected and left_trigger.value == 0)
    assert(right_trigger.connected and right_trigger.value == 1)
    assert(second_pad.pad == 1 and second_pad.connected and second_pad.value == 1)
    assert(second_pad_trigger.connected and math.abs(second_pad_trigger.value - 64 / 255) < 0.001)
    assert(not game.input.consume({id = "quick_menu"}).pressed)
    assert(game.input.consume({id = "modal_confirm"}).pressed)
    assert(not game.input.consume({id = "modal_confirm"}).pressed)
    moonmod.log("info", "MOONMOD_INPUT_PRESS_OK")
  elseif tick.frame == 2 then
    local modal_state = assert(game.input.state({id = "modal_confirm"}))
    assert(not state.active and not state.down)
    assert(modal_state.active and not modal_state.down)
    local left_x = assert(game.input.axis({axis = "left_x"}))
    local left_y = assert(game.input.axis({axis = "left_y"}))
    local right_x = assert(game.input.axis({axis = "right_x"}))
    local right_y = assert(game.input.axis({axis = "right_y"}))
    local left_trigger = assert(game.input.axis({axis = "left_trigger"}))
    local right_trigger = assert(game.input.axis({axis = "right_trigger"}))
    assert(left_x.value == 1 and left_y.value == -1)
    assert(right_x.value == -1 and right_y.value == 0)
    assert(math.abs(left_trigger.value - 64 / 255) < 0.001)
    assert(math.abs(right_trigger.value - 192 / 255) < 0.001)
    assert(not game.input.consume({id = "quick_menu"}).pressed)
    assert(game.input.context_pop({token = modal_context.token}).active == "default")
    local default_state = assert(game.input.state({id = "quick_menu"}))
    assert(default_state.active and not default_state.down)
    assert(game.input.context_push({id = "unload_cleanup"}).depth == 1)
    assert(game.input.unbind({id = "quick_menu"}).removed)
    moonmod.log("info", "MOONMOD_INPUT_RELEASE_OK")
  end
end)
)lua");

    InputUnbindModActionsByPrefix(owner + ":");
    InputSetScriptedDevices({});
    InputPadState first_sample;
    first_sample.connected = true;
    first_sample.left_x = 0;
    first_sample.left_y = 255;
    first_sample.right_x = 128;
    first_sample.right_y = 255;
    first_sample.right_trigger = 255;
    InputSetOverride(0, &first_sample);
    InputPadState disconnected_pad;
    InputSetOverride(1, &disconnected_pad);
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    moonmod::ConfigureChronicleApi(config);
    const auto context_query_handler = config.api_handler;
    std::vector<std::string> logs;
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };

    moonmod::Runtime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Start(std::move(config), error)) << error;
    ASSERT_TRUE(InputModActionExists(owner + ":quick_menu"));
    ASSERT_TRUE(InputModActionExists(owner + ":cleanup_probe"));
    const moonmod::ApiOperationSpec *context_query = moonmod::FindApiOperation("input.context_get");
    const moonmod::ApiOperationSpec *context_pop = moonmod::FindApiOperation("input.context_pop");
    ASSERT_NE(context_query, nullptr);
    ASSERT_NE(context_pop, nullptr);
    const auto context_snapshot = context_query_handler(owner, *context_query, moonmod::Json::array());
    ASSERT_EQ(context_snapshot.status, moonmod::ApiCallStatus::Ok);
    ASSERT_EQ(context_snapshot.value.at("depth").get<std::size_t>(), 1u);
    const std::string context_token = context_snapshot.value.at("stack").at(0).at("token").get<std::string>();
    const auto foreign_pop = context_query_handler(
        "test.foreign-input-probe", *context_pop, moonmod::Json::array({{{"token", context_token}}}));
    EXPECT_NE(foreign_pop.status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(context_query_handler(owner, *context_query, moonmod::Json::array()).value.at("depth").get<std::size_t>(), 1u);

    const int key = InputScancodeFromName("K");
    ASSERT_GE(key, 0);
    InputPadState second_pad_sample;
    second_pad_sample.connected = true;
    second_pad_sample.right_x = 255;
    second_pad_sample.left_trigger = 64;
    second_pad_sample.right_trigger = 192;
    InputSetOverride(1, &second_pad_sample);
    InputSetScriptedDevices({.keys = {key}});
    runtime.Pump();
    InputPadState second_sample;
    second_sample.connected = true;
    second_sample.left_x = 255;
    second_sample.left_y = 0;
    second_sample.right_x = 0;
    second_sample.right_y = 128;
    second_sample.left_trigger = 64;
    second_sample.right_trigger = 192;
    InputSetOverride(0, &second_sample);
    InputSetScriptedDevices({});
    runtime.Pump();

    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_INPUT_PRESS_OK"), logs.end());
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_INPUT_RELEASE_OK"), logs.end());
    EXPECT_EQ(std::ranges::count_if(logs, [](const std::string &line) {
                  return line.find("event 'game.tick' failed") != std::string::npos;
              }), 0);
    EXPECT_FALSE(InputModActionExists(owner + ":quick_menu"));
    EXPECT_TRUE(InputModActionExists(owner + ":cleanup_probe"));
    runtime.Stop();
    EXPECT_FALSE(InputModActionExists(owner + ":cleanup_probe"));
    const auto after_unload = context_query_handler(owner, *context_query, moonmod::Json::array());
    ASSERT_EQ(after_unload.status, moonmod::ApiCallStatus::Ok) << after_unload.message;
    EXPECT_EQ(after_unload.value.value("active", ""), "default");
    EXPECT_EQ(after_unload.value.value("depth", 1u), 0u);
    InputUnbindModActionsByPrefix(owner + ":");
    InputSetScriptedDevices({});
    InputSetOverride(0, nullptr);
    InputSetOverride(1, nullptr);
}

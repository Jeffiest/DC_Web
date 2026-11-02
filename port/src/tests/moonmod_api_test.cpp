#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "editloop3.hpp"
#include "moonmod/manager_ui.hpp"
#include "moonmod/combat_formula.hpp"
#include "moonmod/native_resource_overrides.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "dungeonmap.hpp"
#include "userstatus.hpp"
#include "moonmod/runtime.hpp"
#include "btactstatus.hpp"
#include "dun/gameloop.hpp"
#include "main.hpp"
#include "language.h"
#include "mainselect.hpp"
#include "dataalloc.hpp"
#include "runscript.hpp"

namespace dc::game {}
using namespace dc::game;


namespace {

namespace fs = std::filesystem;

struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-api-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void Write(const fs::path &path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
    ASSERT_TRUE(out.good()) << path.string();
}

void ExerciseLuaMemoryBudgetIsolation(const std::string &game_id) {
    TempRoot root;
    const fs::path failing_mod = root.path / "mods" / "test.a-memory-overrun";
    const fs::path healthy_mod = root.path / "mods" / "test.b-memory-healthy";
    auto write_manifest = [&](const fs::path &package, const std::string &id, const std::string &name) {
        Write(package / "mod.json",
              "{\"schema_version\":1,\"id\":\"" + id + "\",\"name\":\"" + name +
                  "\",\"version\":\"1.0.0\",\"framework_api\":1,\"games\":[\"" +
                  game_id + "\"],\"entry\":\"main.lua\",\"permissions\":[]}");
    };
    write_manifest(failing_mod, "test.a-memory-overrun", "Memory Overrun");
    write_manifest(healthy_mod, "test.b-memory-healthy", "Memory Healthy");
    Write(failing_mod / "main.lua", R"lua(
local retained = {}
while true do
  retained[#retained + 1] = string.rep("m", 65536)
end
)lua");
    Write(healthy_mod / "main.lua", R"lua(
game.events.subscribe("tests.memory_budget_probe", function()
  moonmod.log("info", "MOONMOD_MEMORY_BUDGET_HEALTHY_MOD_RAN")
end)
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.a-memory-overrun":true,"test.b-memory-healthy":true},"settings":{}})json");

    std::vector<std::string> logs;
    moonmod::RuntimeConfig config;
    config.game_id = game_id;
    config.game_name = game_id == "chronicle" ? "Chronicle" : "Chronicle Two";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    config.lua_memory_limit = 1024 * 1024;
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };

    moonmod::Runtime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Start(std::move(config), error)) << error;
    ASSERT_EQ(runtime.Mods().size(), 2u);
    const auto failed = std::ranges::find(runtime.Mods(), "test.a-memory-overrun", &moonmod::ModRecord::id);
    const auto healthy = std::ranges::find(runtime.Mods(), "test.b-memory-healthy", &moonmod::ModRecord::id);
    ASSERT_NE(failed, runtime.Mods().end());
    ASSERT_NE(healthy, runtime.Mods().end());
    EXPECT_EQ(failed->state, moonmod::ModState::LoadError);
    EXPECT_NE(failed->status_message.find("not enough memory"), std::string::npos);
    EXPECT_EQ(healthy->state, moonmod::ModState::Loaded) << healthy->status_message;

    runtime.Publish("tests.memory_budget_probe", moonmod::Json::object());
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_MEMORY_BUDGET_HEALTHY_MOD_RAN"), 1);
    runtime.Stop();
}

void ExerciseFunctionExportsAcrossReload(const std::string &game_id) {
    TempRoot root;
    const fs::path provider = root.path / "mods" / "test.reload-provider";
    Write(provider / "mod.json",
          "{\"schema_version\":1,\"id\":\"test.reload-provider\",\"name\":\"Reload Provider\"," 
          "\"version\":\"1.0.0\",\"framework_api\":1,\"games\":[\"" + game_id +
              "\"],\"entry\":\"main.lua\",\"permissions\":[\"content.register\",\"game.read\"]}");
    Write(provider / "main.lua", R"lua(
local exported = assert(game.scripts.register_function({id = "ping"}, function(arguments)
  return arguments.value
end))
assert(exported.id == "test.reload-provider:ping")
)lua");
    const fs::path caller = root.path / "mods" / "test.reload-caller";
    Write(caller / "mod.json",
          "{\"schema_version\":1,\"id\":\"test.reload-caller\",\"name\":\"Reload Caller\"," 
          "\"version\":\"1.0.0\",\"framework_api\":1,\"games\":[\"" + game_id +
              "\"],\"entry\":\"main.lua\",\"dependencies\":[\"test.reload-provider\"],"
              "\"permissions\":[\"game.read\",\"game.write\"]}");
    Write(caller / "main.lua", R"lua(
local exports = assert(game.scripts.list_functions()).functions
assert(#exports == 1 and exports[1].id == "test.reload-provider:ping")
local result = assert(game.scripts.call_function({
  id = "test.reload-provider:ping", arguments = {value = "ready"}
}))
assert(result == "ready")
moonmod.log("info", "MOONMOD_FUNCTION_RELOAD_CALLER_READY")
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          "{\"format_version\":1,\"enabled\":{\"test.reload-provider\":true,"
          "\"test.reload-caller\":true},\"settings\":{}}");

    std::vector<std::string> logs;
    moonmod::RuntimeConfig config;
    config.game_id = game_id;
    config.game_name = game_id == "chronicle" ? "Chronicle" : "Chronicle Two";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };

    moonmod::Runtime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Start(config, error)) << error;
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_FUNCTION_RELOAD_CALLER_READY"), 1);

    // A toggle applies at the next launch: the provider is flagged, and nothing unloads now.
    ASSERT_TRUE(runtime.SetEnabled("test.reload-provider", false, error)) << error;
    const auto flagged = std::ranges::find(runtime.Mods(), "test.reload-provider", &moonmod::ModRecord::id);
    ASSERT_NE(flagged, runtime.Mods().end());
    EXPECT_TRUE(flagged->restart_required);
    runtime.Stop();

    // Next launch without the provider: the caller cannot load.
    moonmod::Runtime without;
    ASSERT_TRUE(without.Start(config, error)) << error;
    const auto disabled_caller = std::ranges::find(without.Mods(), "test.reload-caller", &moonmod::ModRecord::id);
    ASSERT_NE(disabled_caller, without.Mods().end());
    EXPECT_EQ(disabled_caller->state, moonmod::ModState::MissingDependency);
    ASSERT_TRUE(without.SetEnabled("test.reload-provider", true, error)) << error;
    without.Stop();

    // And with it back, the caller loads and reaches the provider's function again.
    moonmod::Runtime with;
    ASSERT_TRUE(with.Start(config, error)) << error;
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_FUNCTION_RELOAD_CALLER_READY"), 2);
    const auto loaded_caller = std::ranges::find(with.Mods(), "test.reload-caller", &moonmod::ModRecord::id);
    ASSERT_NE(loaded_caller, with.Mods().end());
    EXPECT_EQ(loaded_caller->state, moonmod::ModState::Loaded) << loaded_caller->status_message;
    with.Stop();
}

void ExerciseNativeScriptHooks(const std::string &game_id) {
    TempRoot root;
    const fs::path package = root.path / "mods" / "test.native-script-hooks";
    Write(package / "mod.json",
          "{\"schema_version\":1,\"id\":\"test.native-script-hooks\",\"name\":\"Native Script Hooks\","
          "\"version\":\"1.0.0\",\"framework_api\":1,\"games\":[\"" + game_id +
              "\"],\"entry\":\"main.lua\",\"permissions\":[\"game.read\",\"game.write\",\"scripts.override\"]}");
    Write(package / "main.lua", R"lua(
local info = assert(game.info.get())
local listed = assert(game.scripts.list_hooks())
assert(listed.game == info.id and #listed.hooks == 6)
assert(listed.hooks[1].id == "native.event.run")
for _, path in ipairs({"scripts.list_hooks", "scripts.call", "scripts.override"}) do
  local supported, reason = moonmod.api.supports(path)
  assert(supported and reason == "", path .. " must be supported")
end
local outside, outside_error = game.scripts.call({id = "native.event.run"})
assert(outside == nil and outside_error.code == "phase_violation")
local json_override, json_override_error = moonmod.api.call("scripts.override", {id = "native.event.run"})
assert(json_override == nil and json_override_error.code == "invalid_argument")
local initial_override = assert(game.scripts.override({id = "native.event.run"}, function(context)
  assert(context.program_no == 77)
  moonmod.log("info", "MOONMOD_NATIVE_SCRIPT_HOOK_REPLACED:" .. context.program_no)
  return {handled = true}
end))
assert(initial_override.replaced == false)
game.events.subscribe("tests.native_script_hook_mode", function(event)
  if event.mode == "call" then
    local replacement = assert(game.scripts.override({id = "native.event.run"}, function(context)
      local called = assert(game.scripts.call({id = context.hook_id}))
      assert(called.run_status == 1)
      local repeated, repeated_error = game.scripts.call({id = context.hook_id})
      assert(repeated == nil and repeated_error.code == "phase_violation")
      moonmod.log("info", "MOONMOD_NATIVE_SCRIPT_HOOK_CALLED:" .. context.program_no)
    end))
    assert(replacement.replaced)
  elseif event.mode == "native_call" then
    assert(game.scripts.override({id = "native.event.run"}, function(context)
      local called = assert(game.scripts.call({id = context.hook_id}))
      assert(called.run_status == 0)
      local repeated, repeated_error = game.scripts.call({id = context.hook_id})
      assert(repeated == nil and repeated_error.code == "phase_violation")
      moonmod.log("info", "MOONMOD_NATIVE_EVENT_ENTRY_CALLED:" .. context.program_no)
    end))
  elseif event.mode == "replace" then
    assert(game.scripts.override({id = "native.event.run"}, function(context)
      assert(context.program_no == 77)
      return {handled = true}
    end))
  elseif event.mode == "pass" then
    assert(game.scripts.override({id = "native.event.run"}, function(context)
      assert(context.program_no == 79)
      return {handled = false}
    end))
  elseif event.mode == "disable" then
    assert(assert(game.scripts.override({id = "native.event.run", enabled = false})).removed)
  end
end)
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.native-script-hooks":true},"settings":{}})json");

    std::vector<std::string> logs;
    moonmod::RuntimeConfig config;
    config.game_id = game_id;
    config.game_name = game_id == "chronicle" ? "Chronicle" : "Chronicle Two";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };

    moonmod::Runtime &runtime = moonmod::SharedRuntime();
    runtime.Stop();
    std::string error;
    ASSERT_TRUE(runtime.Start(std::move(config), error)) << error;
    ASSERT_EQ(runtime.Mods().size(), 1u);
    ASSERT_EQ(runtime.Mods()[0].state, moonmod::ModState::Loaded) << runtime.Mods()[0].status_message;

    if (game_id == "chronicle") {
        struct DiscFuncData {
            std::int32_t  addr;
            std::uint32_t name;
            std::int32_t  local;
            std::int32_t  arg;
        };
        struct EventProgram {
            RS_PROG_HEADER header{};
            RS_PROGDATA    entry{77, static_cast<int>(offsetof(EventProgram, function))};
            DiscFuncData   function{0, 0, 0, 0};
            vmcode_t       code{RS_OP_END, 0, 0};

            EventProgram() {
                header.main = static_cast<int>(offsetof(EventProgram, function));
                header.code = static_cast<int>(offsetof(EventProgram, code));
                header.prog = static_cast<int>(offsetof(EventProgram, entry));
                header.prog_num = 1;
            }
        } program;
        std::array<RS_STACKDATA, 128> event_stack{};
        std::array<RS_CALLDATA, 512> event_calls{};
        EdEventScript.load(&program.header, event_stack.data(), event_stack.size(), event_calls.data(),
                           event_calls.size());
        EdEventScript.end = 0;
        alignas(64) std::array<u_char, 16384> arena_storage{};
        CDataAlloc2<1> arena(1024);
        arena.base = arena_storage.data();
        arena.buffer = arena_storage.data();
        arena.used = 0;
        arena.limit = 1024;

        EXPECT_EQ(PortEdRunEvent(77, &arena), 0);
        EXPECT_EQ(EdEventScript.end, 0) << "handled=true must skip EdEventScript.run";
        EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_NATIVE_SCRIPT_HOOK_REPLACED:77"), 1);

        ASSERT_TRUE(runtime.PublishToMod("test.native-script-hooks", "tests.native_script_hook_mode",
                                         {{"mode", "native_call"}}));
        EdEventScript.end = 0;
        EXPECT_EQ(PortEdRunEvent(77, &arena), 0);
        EXPECT_EQ(EdEventScript.end, 1) << "scripts.call must execute the native event program exactly once";
        EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_NATIVE_EVENT_ENTRY_CALLED:77"), 1);
        ASSERT_TRUE(runtime.PublishToMod("test.native-script-hooks", "tests.native_script_hook_mode",
                                         {{"mode", "replace"}}));
    }

    int original_calls = 0;
    const auto original = [&]() {
        ++original_calls;
        return moonmod::ApiCallResult{moonmod::ApiCallStatus::Ok, {{"run_status", 1}}, {}};
    };
    const auto replaced = runtime.DispatchNativeScriptHook("native.event.run", {{"program_no", 77}}, original);
    ASSERT_EQ(replaced.status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(replaced.value.at("run_status"), 0);
    EXPECT_TRUE(replaced.value.at("overridden"));
    EXPECT_EQ(original_calls, 0);
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_NATIVE_SCRIPT_HOOK_REPLACED:77"), logs.end());

    ASSERT_TRUE(runtime.PublishToMod("test.native-script-hooks", "tests.native_script_hook_mode", {{"mode", "call"}}));
    const auto wrapped = runtime.DispatchNativeScriptHook("native.event.run", {{"program_no", 78}}, original);
    ASSERT_EQ(wrapped.status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(wrapped.value.at("run_status"), 1);
    EXPECT_EQ(original_calls, 1);
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_NATIVE_SCRIPT_HOOK_CALLED:78"), logs.end());

    ASSERT_TRUE(runtime.PublishToMod("test.native-script-hooks", "tests.native_script_hook_mode", {{"mode", "pass"}}));
    const auto fallthrough = runtime.DispatchNativeScriptHook("native.event.run", {{"program_no", 79}}, original);
    ASSERT_EQ(fallthrough.status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(fallthrough.value.at("run_status"), 1);
    EXPECT_EQ(original_calls, 2);

    ASSERT_TRUE(runtime.PublishToMod("test.native-script-hooks", "tests.native_script_hook_mode", {{"mode", "disable"}}));
    const auto passthrough = runtime.DispatchNativeScriptHook("native.event.run", {{"program_no", 80}}, original);
    ASSERT_EQ(passthrough.status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(passthrough.value.at("run_status"), 1);
    EXPECT_EQ(original_calls, 3);
    ASSERT_TRUE(runtime.SetEnabled("test.native-script-hooks", false, error)) << error;
    const auto unloaded = runtime.DispatchNativeScriptHook("native.event.run", {{"program_no", 81}}, original);
    ASSERT_EQ(unloaded.status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(unloaded.value.at("run_status"), 1);
    EXPECT_EQ(original_calls, 4);
    runtime.Stop();
}

void ExerciseApi(const std::string &game_id) {
    moonmod::CloseManager();
    TempRoot root;
    const fs::path package = root.path / "mods" / "test.content-probe";
    const fs::path denied_package = root.path / "mods" / "test.permission-probe";
    const fs::path foreign_package = root.path / "mods" / "test.resource-foreign";
    const fs::path quest_permission_package = root.path / "mods" / "test.quest-reward-permission";
    Write(package / "mod.json",
          "{\"schema_version\":1,\"id\":\"test.content-probe\",\"name\":\"Content Probe\","
          "\"version\":\"1.0.0\",\"framework_api\":1,\"games\":[\"" + game_id +
              "\"],\"entry\":\"main.lua\",\"permissions\":[\"content.register\",\"game.read\","
              "\"game.write\",\"storage.read\",\"storage.write\",\"scripts.override\",\"debug.inspect\"]}");
    const std::string binary_resource("\0\xff\x10\xff", 4);
    Write(package / "resources" / "probe.bin", std::string_view(binary_resource.data(), binary_resource.size()));
    const std::string oversized_resource(256 * 1024 + 1, 'x');
    Write(package / "resources" / "oversized.bin", oversized_resource);
    Write(denied_package / "mod.json",
          "{\"schema_version\":1,\"id\":\"test.permission-probe\",\"name\":\"Permission Probe\","
          "\"version\":\"1.0.0\",\"framework_api\":1,\"games\":[\"" + game_id +
              "\"],\"entry\":\"main.lua\"}");
    Write(denied_package / "main.lua", R"lua(
local result, failure = game.player.heal({amount = 1})
assert(result == nil and failure.code == "permission_denied")
local localized, localization_failure = game.localization.register({id = "denied", translations = {en = "Denied"}})
assert(localized == nil and localization_failure.code == "permission_denied")
local input_binding, input_failure = game.input.bind({id = "denied", keys = {"K"}})
assert(input_binding == nil and input_failure.code == "permission_denied")
local item_list, item_list_failure = game.items.list()
assert(item_list == nil and item_list_failure.code == "permission_denied")
local item_definition, item_definition_failure = game.items.get("chronicle:item:148")
assert(item_definition == nil and item_definition_failure.code == "permission_denied")
local weapons, weapons_failure = game.weapons.list()
assert(weapons == nil and weapons_failure.code == "permission_denied")
local item_use, item_use_failure = game.items.use({id = "test:denied", target = "max"})
assert(item_use == nil and item_use_failure.code == "permission_denied")
local override_denied, override_denied_error = game.resources.override({path = "resources/probe.bin", game_path = "probe/native.bin"})
assert(override_denied == nil and override_denied_error.code == "permission_denied")
local script_override_denied, script_override_denied_error = game.scripts.override({id = "native.event.run"}, function() end)
assert(script_override_denied == nil and script_override_denied_error.code == "permission_denied")
local weapon_registration, weapon_registration_failure = game.weapons.register({id = "denied"})
assert(weapon_registration == nil and weapon_registration_failure.code == "permission_denied")
local resource_load, resource_load_failure = game.resources.load({path = "resources/probe.bin"})
assert(resource_load == nil and resource_load_failure.code == "permission_denied")
local resource_read, resource_read_failure = game.resources.resolve({handle = "test.content-probe:resource:1"})
assert(resource_read == nil and resource_read_failure.code == "permission_denied")
local notice, notice_failure = game.ui.notify({message = "No permission"})
assert(notice == nil and notice_failure.code == "permission_denied")
local ui_action, ui_action_failure = game.ui.register({id = "denied", label = "Denied"})
assert(ui_action == nil and ui_action_failure.code == "permission_denied")
local profile, profile_failure = game.debug.profile()
assert(profile == nil and profile_failure.code == "permission_denied")
local quests, quests_failure = game.quests.get()
assert(quests == nil and quests_failure.code == "permission_denied")
local quest, quest_failure = game.quests.register({id = "denied", title = "Denied", objectives = {{id = "one", title = "One", target = 1}}})
assert(quest == nil and quest_failure.code == "permission_denied")
local quest_start, quest_start_failure = game.quests.start({id = "denied"})
assert(quest_start == nil and quest_start_failure.code == "permission_denied")
local quest_advance, quest_advance_failure = game.quests.advance({id = "denied", objective = "one"})
assert(quest_advance == nil and quest_advance_failure.code == "permission_denied")
local skill_list, skill_list_failure = game.skills.list()
assert(skill_list == nil and skill_list_failure.code == "permission_denied")
local skill_registration, skill_registration_failure = game.skills.register({id = "denied"})
assert(skill_registration == nil and skill_registration_failure.code == "permission_denied")
local skill_unlock, skill_unlock_failure = game.skills.unlock({id = "denied.node"})
assert(skill_unlock == nil and skill_unlock_failure.code == "permission_denied")
local story_flag_read, story_flag_read_failure = game.world.story_flag_get({id = 1})
assert(story_flag_read == nil and story_flag_read_failure.code == "permission_denied")
local story_flag_write, story_flag_write_failure = game.world.story_flag_set({id = 1, value = true})
assert(story_flag_write == nil and story_flag_write_failure.code == "permission_denied")
local story_value_read, story_value_read_failure = game.world.story_value_get({id = 1})
assert(story_value_read == nil and story_value_read_failure.code == "permission_denied")
local story_value_write, story_value_write_failure = game.world.story_value_set({id = 1, value = 5})
assert(story_value_write == nil and story_value_write_failure.code == "permission_denied")
local map_flag_read, map_flag_read_failure = game.world.map_flag_get({map_id = 1, flag_id = 1})
assert(map_flag_read == nil and map_flag_read_failure.code == "permission_denied")
local map_flag_write, map_flag_write_failure = game.world.map_flag_set({map_id = 1, flag_id = 1, value = true})
assert(map_flag_write == nil and map_flag_write_failure.code == "permission_denied")
local georama_request, georama_request_failure = game.georama.request_set({town_id = 0, request_id = 0, completed = true})
assert(georama_request == nil and georama_request_failure.code == "permission_denied")
local georama_condition, georama_condition_failure = game.georama.condition_set({town_id = 0, condition_id = 0, met = true})
assert(georama_condition == nil and georama_condition_failure.code == "permission_denied")
local function_registration, function_registration_failure = game.scripts.register_function({id = "denied"}, function() return true end)
assert(function_registration == nil and function_registration_failure.code == "permission_denied")
local function_call, function_call_failure = game.scripts.call_function({id = "test.function-provider:twice"})
assert(function_call == nil and function_call_failure.code == "permission_denied")
local function_list, function_list_failure = game.scripts.list_functions()
assert(function_list == nil and function_list_failure.code == "permission_denied")
local function_removal, function_removal_failure = game.scripts.unregister_function({id = "denied"})
assert(function_removal == nil and function_removal_failure.code == "permission_denied")
)lua");
    Write(quest_permission_package / "mod.json",
          "{\"schema_version\":1,\"id\":\"test.quest-reward-permission\",\"name\":\"Quest Reward Permission\"," 
          "\"version\":\"1.0.0\",\"framework_api\":1,\"games\":[\"" + game_id +
              "\"],\"entry\":\"main.lua\",\"permissions\":[\"content.register\",\"game.read\"]}");
    Write(quest_permission_package / "main.lua", R"lua(
local quest, failure = game.quests.register({
  id = "reward-permission", title = "Permission Gate",
  objectives = {{id = "one", title = "One", target = 1}},
  rewards = {{kind = "currency", amount = 5}}
})
assert(quest == nil and failure.code == "permission_denied" and failure.message:match("game.write"))
)lua");
    Write(foreign_package / "mod.json",
          "{\"schema_version\":1,\"id\":\"test.resource-foreign\",\"name\":\"Resource Owner Probe\","
          "\"version\":\"1.0.0\",\"framework_api\":1,\"games\":[\"" + game_id +
              "\"],\"entry\":\"main.lua\",\"permissions\":[\"game.read\",\"game.write\"]}");
    const fs::path function_provider = root.path / "mods" / "test.function-provider";
    const fs::path function_caller = root.path / "mods" / "test.function-caller";
    Write(function_provider / "mod.json",
          "{\"schema_version\":1,\"id\":\"test.function-provider\",\"name\":\"Function Provider\","
          "\"version\":\"1.0.0\",\"framework_api\":1,\"games\":[\"" + game_id +
              "\"],\"entry\":\"main.lua\",\"permissions\":[\"content.register\",\"game.read\"]}");
    Write(function_provider / "main.lua", R"lua(
local twice, twice_error = game.scripts.register_function({id = "twice", description = "Double a number"}, function(arguments)
  return {value = arguments.value * 2}
end)
assert(twice_error == nil and twice.id == "test.function-provider:twice" and twice.exposed)
local throws, throws_error = game.scripts.register_function({id = "throws"}, function()
  error("expected callback failure")
end)
assert(throws_error == nil and throws.id == "test.function-provider:throws")
local runaway, runaway_error = game.scripts.register_function({id = "runaway"}, function()
  while true do pcall(function() while true do end end) end
end)
assert(runaway_error == nil and runaway.id == "test.function-provider:runaway")
local exports = assert(game.scripts.list_functions())
assert(#exports.functions == 3)
local found_twice = false
for _, entry in ipairs(exports.functions) do
  if entry.id == "test.function-provider:twice" then
    found_twice = entry.owner_mod_id == "test.function-provider" and entry.description == "Double a number"
  end
end
assert(found_twice)
game.events.subscribe("tests.functions.unregister", function()
  assert(game.scripts.unregister_function({id = "twice"}).removed)
end)
)lua");
    Write(function_caller / "mod.json",
          "{\"schema_version\":1,\"id\":\"test.function-caller\",\"name\":\"Function Caller\","
          "\"version\":\"1.0.0\",\"framework_api\":1,\"games\":[\"" + game_id +
              "\"],\"entry\":\"main.lua\",\"dependencies\":[\"test.function-provider\"],"
              "\"permissions\":[\"content.register\",\"game.read\",\"game.write\"]}");
    Write(function_caller / "main.lua", R"lua(
local initial_exports = assert(game.scripts.list_functions())
assert(#initial_exports.functions == 3)
local listed_twice = false
for _, entry in ipairs(initial_exports.functions) do
  if entry.id == "test.function-provider:twice" then
    listed_twice = entry.owner_mod_id == "test.function-provider" and entry.description == "Double a number"
  end
end
assert(listed_twice)
local doubled, doubled_error = game.scripts.call_function({id = "test.function-provider:twice", arguments = {value = 21}})
assert(doubled_error == nil and doubled.value == 42)
local failed, failed_error = game.scripts.call_function({id = "test.function-provider:throws"})
assert(failed == nil and failed_error.code == "failed" and failed_error.message:match("callback failed"))
local over_budget, over_budget_error = game.scripts.call_function({id = "test.function-provider:runaway"})
assert(over_budget == nil and over_budget_error.code == "failed" and over_budget_error.message:match("instruction budget"))
local repeated, repeated_error = game.scripts.call_function({id = "test.function-provider:twice", arguments = {value = 4}})
assert(repeated_error == nil and repeated.value == 8)
local missing, missing_error = game.scripts.call_function({id = "test.function-provider:missing"})
assert(missing == nil and missing_error.code == "failed")
local local_function, local_function_error = game.scripts.register_function({id = "sum"}, function(arguments)
  return arguments.left + arguments.right
end)
assert(local_function_error == nil and local_function.id == "test.function-caller:sum")
local with_local = assert(game.scripts.list_functions())
assert(#with_local.functions == 4 and with_local.functions[1].id == "test.function-caller:sum")
local sum, sum_error = game.scripts.call_function({id = "sum", arguments = {left = 12, right = 30}})
assert(sum_error == nil and sum == 42)
assert(game.scripts.unregister_function({id = "sum"}).removed)
local removed, removed_error = game.scripts.call_function({id = "sum"})
assert(removed == nil and removed_error.code == "failed")
local after_local_removal = assert(game.scripts.list_functions())
assert(#after_local_removal.functions == 3)
game.events.subscribe("tests.functions.check_cleanup", function()
  local current = assert(game.scripts.list_functions())
  for _, entry in ipairs(current.functions) do
    assert(entry.id ~= "test.function-provider:twice")
  end
  moonmod.log("info", "MOONMOD_FUNCTION_LIST_CLEANUP_OK")
end)
)lua");
    Write(foreign_package / "main.lua", R"lua(
local isolated_exports = assert(game.scripts.list_functions())
assert(#isolated_exports.functions == 0)
local function_result, function_error = game.scripts.call_function({id = "test.function-provider:twice", arguments = {value = 5}})
assert(function_result == nil and function_error.code == "permission_denied")
game.events.subscribe("game.tick", function(frame)
  if frame.frame == 1 then
    local resource, failure = game.resources.resolve({handle = "test.content-probe:resource:1"})
    assert(resource == nil and failure.code == "failed")
    moonmod.log("info", "MOONMOD_RESOURCE_OWNER_ISOLATED")
  end
end)
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          "{\"format_version\":1,\"enabled\":{\"test.content-probe\":true,\"test.permission-probe\":true,"
          "\"test.resource-foreign\":true,\"test.quest-reward-permission\":true,"
          "\"test.function-provider\":true,\"test.function-caller\":true},"
          "\"settings\":{}}");
    Write(package / "main.lua", R"lua(
local info = assert(game.info.get())
assert(info.id == game_id)
local info_by_path = assert(moonmod.api.call("info.get"))
assert(info_by_path.id == info.id)
local item_supported, item_reason = moonmod.api.supports("items.register")
assert(item_supported and item_reason == "")
local resource_supported, resource_reason = moonmod.api.supports("resources.load")
assert(resource_supported and resource_reason == "")
local override_supported, override_reason = moonmod.api.supports("resources.override")
assert(override_supported and override_reason == "")
local flat_function, flat_function_error = moonmod.api.call("scripts.register_function", {id = "flat"}, function() return true end)
assert(flat_function == nil and flat_function_error.code == "invalid_argument")
local resource, resource_error = game.resources.load({path = "resources/probe.bin"})
assert(resource_error == nil and resource.status == "loaded" and resource.size == 4)
assert(resource.path == "resources/probe.bin" and resource.media_type == "application/octet-stream")
local override, override_error = moonmod.api.call("resources.override", {
  path = "resources/probe.bin", game_path = "probe/native.bin"
})
assert(override_error == nil and override.status == "registered" and override.game_path == "probe/native.bin")
assert(game.resources.unoverride({game_path = "PROBE/native.bin"}).removed)
assert(game.resources.override({path = "resources/probe.bin", game_path = "probe/unload.bin"}))
local invalid_override, invalid_override_error = game.resources.override({
  path = "resources/../main.lua", game_path = "probe/native.bin"
})
assert(invalid_override == nil and invalid_override_error.code == "invalid_argument")
local bytes = assert(game.resources.resolve({handle = resource.handle}))
assert(bytes.encoding == "base64" and bytes.data == "AP8Q/w==" and bytes.size == 4)
local by_flat_api = assert(moonmod.api.call("resources.resolve", {handle = resource.handle}))
assert(by_flat_api.data == bytes.data)
local traversal, traversal_error = game.resources.load({path = "resources/../main.lua"})
assert(traversal == nil and traversal_error.code == "invalid_argument")
local oversized, oversized_error = game.resources.load({path = "resources/oversized.bin"})
assert(oversized == nil and oversized_error.code == "invalid_argument")
local missing_resource, missing_resource_error = game.resources.load({path = "resources/missing.bin"})
assert(missing_resource == nil and missing_resource_error.code == "failed")
game.events.subscribe("game.tick", function(frame)
  if frame.frame == 2 then
    assert(game.resources.release({handle = resource.handle}).removed)
    local stale_resource, stale_resource_error = game.resources.resolve({handle = resource.handle})
    assert(stale_resource == nil and stale_resource_error.code == "failed")
    moonmod.log("info", "MOONMOD_RESOURCE_RELEASE_OK")
  end
end)
moonmod.log("info", "MOONMOD_RESOURCES_OK")
local notice, notice_error = game.ui.notify({message = "Resource probe ready", duration_frames = 120})
assert(notice_error == nil and notice.queued and notice.manager_open == false and notice.duration_frames == 120)
local invalid_notice, invalid_notice_error = game.ui.notify({message = "Bad duration", duration_frames = 0})
assert(invalid_notice == nil and invalid_notice_error.code == "invalid_argument")
moonmod.log("info", "MOONMOD_UI_NOTIFY_QUEUED")
local previous_save = game.save.get("api-probe")
assert(previous_save == nil or previous_save.count == 4)
if previous_save == nil then
  local saved, save_error = game.save.set("api-probe", {count = 4})
  assert(saved == true and save_error == nil)
  assert(game.save.get("api-probe").count == 4)
else
  moonmod.log("info", "MOONMOD_SAVE_RELOAD_OK:" .. info.id)
end
local save_version = assert(game.save.get("__moonmod_schema_version"))
assert(save_version == 0 or save_version == 1)
local migration_outside_load, migration_phase_error = game.save.migrate({from_version = 0, to_version = 1})
assert(migration_outside_load == nil and migration_phase_error.code == "phase_violation")
local reserved_version_write, reserved_version_error = game.save.set("__moonmod_schema_version", 9)
assert(reserved_version_write == nil and reserved_version_error.code == "invalid_argument")
local storage_version_write, storage_version_write_error = moonmod.storage.set("__moonmod_schema_version", 9)
assert(storage_version_write == nil and storage_version_write_error:match("reserved"))
local storage_version_delete, storage_version_delete_error = moonmod.storage.delete("__moonmod_schema_version")
assert(storage_version_delete == nil and storage_version_delete_error:match("reserved"))
if save_version == 0 then
  assert(game.save.set("legacy", {coins = 8}))
else
  assert(game.save.get("migration-probe").revision == 1)
end
local tick_token
local tick_error
tick_token, tick_error = game.events.subscribe("game.tick", function(frame)
  moonmod.log("info", "MOONMOD_TICK:" .. tostring(frame.frame))
  assert(game.events.unsubscribe(tick_token))
end)
assert(tick_token ~= nil and tick_error == nil)
game.events.subscribe("game.scene.entered", function(scene)
  if scene.scene_id == "dungeon" then
    assert(scene.native_mode == 2)
    moonmod.log("info", "MOONMOD_SCENE_EVENT:" .. scene.scene_id .. ":" .. tostring(scene.native_mode))
  end
end)
game.events.subscribe("game.scene.will_enter", function(scene)
  assert(scene.scene_id == "dungeon" and scene.native_mode == 2)
  moonmod.log("info", "MOONMOD_SCENE_WILL_ENTER_EVENT:" .. scene.scene_id .. ":" .. tostring(scene.native_mode))
end)
local catalog = game.api.list()
assert(type(catalog) == "table" and #catalog >= 70)
local event_catalog = assert(game.events.list())
local minimum_event_count = info.id == "chronicle" and 31 or 36
assert(event_catalog.game == info.id and type(event_catalog.events) == "table" and #event_catalog.events >= minimum_event_count)
local available_events = {}
for _, event in ipairs(event_catalog.events) do
  assert(type(event.name) == "string" and type(event.domain) == "string" and type(event.summary) == "string")
  assert(not available_events[event.name], "event catalog must not contain duplicates")
  available_events[event.name] = true
  local event_token, subscribe_error = game.events.subscribe(event.name, function() end)
  assert(event_token ~= nil and subscribe_error == nil, "listed event is not subscribable: " .. event.name)
  assert(game.events.unsubscribe(event_token))
end
for _, name in ipairs({"game.scene.entered", "game.item_given", "game.item_removed", "game.item_picked_up",
                       "game.enemy_defeated", "quests.completed", "skills.unlocked", "ui.action"}) do
  assert(available_events[name], "missing shared event: " .. name)
end
if info.id == "chronicle" then
  assert(available_events["game.georama.element_changed"] and available_events["game.player.thirst_changed"] and
         not available_events["game.georama.river_changed"] and not available_events["game.party.recruited"] and
         not available_events["game.photo.taken"] and not available_events["game.invention.created"] and
         not available_events["game.spheda.completed"])
else
  assert(available_events["game.party.recruited"] and available_events["game.georama.river_changed"] and
         available_events["game.photo.taken"] and available_events["game.invention.created"] and
         available_events["game.spheda.completed"] and not available_events["game.georama.element_changed"] and
         not available_events["game.player.thirst_changed"])
end
local invalid_event_args, event_args_error = game.events.list("unexpected")
assert(invalid_event_args == nil and event_args_error.code == "invalid_argument")
local domains = {}
for _, operation in ipairs(catalog) do
  assert(operation.domain_api_major == 1)
  assert(operation.status == (operation.supported and "supported" or "unsupported"))
  domains[operation.domain] = true
end
for _, domain in ipairs({"info", "api", "events", "player", "party", "world", "locations", "dungeons", "georama", "entities", "combat", "items",
                         "skills", "models", "animations",
                         "weapons", "shops", "characters", "quests", "scripts", "content", "photos", "inventions", "save", "input", "audio",
                         "render", "ui", "physics", "resources", "localization", "commands", "compat", "debug"}) do
  assert(domains[domain], "missing API domain: " .. domain)
end
for _, path in ipairs({"locations.list", "locations.get", "dungeons.list", "dungeons.get", "dungeons.register",
                         "georama.get", "georama.request_set", "georama.condition_set", "georama.register", "georama.place", "georama.remove",
                         "combat.styles_list",
                         "combat.styles_get", "combat.styles_register", "combat.styles_set_active"}) do
  local supported, reason = moonmod.api.supports(path)
  assert(not supported and type(reason) == "string" and #reason > 0, path .. " must remain explicitly unsupported until native verification")
end
for _, path in ipairs({"models.replace", "models.remove"}) do
  local supported, reason = moonmod.api.supports(path)
  assert(supported and reason == "", path .. " must be supported by the native model binding adapter")
end

for _, path in ipairs({"skills.list", "skills.get", "skills.register", "skills.unlock", "skills.set_rank"}) do
  local supported, reason = moonmod.api.supports(path)
  assert(supported and reason == "", path .. " must be a supported shared runtime API")
end
for _, path in ipairs({"scripts.register_function", "scripts.list_functions", "scripts.call_function", "scripts.unregister_function"}) do
  local supported, reason = moonmod.api.supports(path)
  assert(supported and reason == "", path .. " must be a supported shared runtime API")
end
local greeting, localization_error = game.localization.register({
  id = "greeting", translations = {en = "Hello", fr = "Bonjour", ["fr-CA"] = "Salut"}
})
assert(localization_error == nil and greeting.id == "test.content-probe:greeting")
assert(game.localization.get({id = "greeting", language = "FR-ca"}) == "Salut")
assert(game.localization.get({id = greeting.id, language = "fr-BE"}) == "Bonjour")
assert(game.localization.get({id = "greeting", language = "de-DE"}) == "Hello")
assert(game.localization.get({id = "greeting"}) == "Bonjour")
local french_only = assert(game.localization.register({id = "french-only", translations = {fr = "Salut"}}))
assert(game.localization.get({id = french_only.id}) == "Salut")
local duplicate, duplicate_error = game.localization.register({id = "greeting", translations = {en = "Again"}})
assert(duplicate == nil and duplicate_error.code == "invalid_argument")
local invalid_tag, invalid_tag_error = game.localization.register({id = "invalid-tag", translations = {["en-"] = "Bad"}})
assert(invalid_tag == nil and invalid_tag_error.code == "invalid_argument")
local missing, missing_error = game.localization.get({id = "missing"})
assert(missing == nil and missing_error.code == "invalid_argument")
local foreign, foreign_error = game.localization.get({id = "other.mod:greeting"})
assert(foreign == nil and foreign_error.code == "invalid_argument")
local profile = assert(game.debug.profile())
assert(profile.lua_memory.used_bytes > 0 and profile.lua_memory.used_bytes <= profile.lua_memory.limit_bytes)
assert(profile.events.subscriptions == 4 and profile.events.event_names == 3)
assert(profile.localization.keys == 2 and profile.localization.translations == 4)
assert(profile.resources.handles == 1 and profile.resources.bytes == 4)
assert(profile.storage.keys >= 2 and profile.storage.keys <= 8 and profile.storage.bytes <= profile.storage.limit_bytes)
local invalid_profile, invalid_profile_error = game.debug.profile({unexpected = true})
assert(invalid_profile == nil and invalid_profile_error.code == "invalid_argument")
local hidden_skills, hidden_skills_error = game.save.get("__moonmod_skills_v1")
assert(hidden_skills == nil and hidden_skills_error.code == "invalid_argument")
local skill_storage_write, skill_storage_write_error = moonmod.storage.set("__moonmod_skills_v1", {})
assert(skill_storage_write == nil and skill_storage_write_error:match("reserved"))
local skill_definition = {
  id = "combat", title = "Combat", description = "A mod-owned progression tree",
  skills = {
    {id = "focus", title = "Focus", max_rank = 2, effects = {enemy_damage_per_rank = 0.1}},
    {id = "power", title = "Power", max_rank = 3, prerequisites = {{id = "focus", rank = 2}}}
  }
}
for _, path in ipairs({"animations.register", "animations.play", "animations.stop"}) do
  local supported, reason = moonmod.api.supports(path)
  assert(supported and reason == "", path .. " must support Chronicle's registered model motions")
end
local prior_focus, prior_focus_error = game.skills.get({id = "combat.focus"})
local skill_tree = assert(game.skills.register(skill_definition))
assert(skill_tree.id == "test.content-probe:combat" and skill_tree.skills[1] == "test.content-probe:combat.focus")
assert(skill_tree.idempotent == (prior_focus ~= nil))
assert(assert(game.skills.register(skill_definition)).idempotent)
local blocked_power, blocked_power_error = game.skills.unlock({id = "combat.power"})
if prior_focus == nil then
  assert(blocked_power == nil and blocked_power_error.code == "failed")
  assert(assert(game.skills.unlock({id = "combat.focus"})).rank == 1)
  assert(assert(game.skills.unlock({id = "combat.focus"})).rank == 2)
  assert(assert(game.skills.unlock({id = "combat.power"})).rank == 1)
else
  assert(prior_focus.rank == 2, "persisted focus rank was " .. tostring(prior_focus.rank))
  assert(blocked_power_error == nil,
         "power unlock failed: " .. tostring(blocked_power_error and blocked_power_error.code) .. ": " ..
           tostring(blocked_power_error and blocked_power_error.message))
  assert(blocked_power.rank == 2, "power unlock rank was " .. tostring(blocked_power.rank))
end
assert(assert(game.skills.get({id = "test.content-probe:combat.power"})).prerequisites_met)
assert(#assert(game.skills.list()).skills == 2)
assert(game.skills.get({id = "combat.focus"}).effects.enemy_damage_per_rank == 0.1)
local invalid_skill_effect, invalid_skill_effect_error = game.skills.register({
  id = "invalid-effect", title = "Invalid Effect",
  skills = {{id = "overpowered", title = "Overpowered", max_rank = 1,
    effects = {enemy_damage_per_rank = 1.01}}}
})
assert(invalid_skill_effect == nil and invalid_skill_effect_error.code == "invalid_argument")
local invalid_rank, invalid_rank_error = game.skills.set_rank({id = "combat.focus", rank = 3})
assert(invalid_rank == nil and invalid_rank_error.code == "invalid_argument")
local cyclic_tree, cyclic_tree_error = game.skills.register({
  id = "cycle", title = "Cycle", skills = {
    {id = "one", title = "One", max_rank = 1, prerequisites = {{id = "two", rank = 1}}},
    {id = "two", title = "Two", max_rank = 1, prerequisites = {{id = "one", rank = 1}}}
  }
})
assert(cyclic_tree == nil and cyclic_tree_error.code == "invalid_argument")
local item, item_error = game.items.register({
  id = "test:starlight-tonic",
  name = "Starlight Tonic",
  kind = "consumable",
  max_stack = 10,
  effect = { kind = "heal", amount = 12 },
  assets = {}
})
assert(item_error == nil and item.id == "test:starlight-tonic")
local dungeon, dungeon_error = game.world.register_dungeon({
  id = "test:moon-vault",
  name = "Moon Vault",
  floors = {{ id = "test:moon-vault:1", source = "builtin:training-room" }}
})
assert(dungeon_error == nil and dungeon.id == "test:moon-vault")
local quests_supported, quests_reason = moonmod.api.supports("quests.register")
assert(quests_supported and quests_reason == "")
local quest_started_event, quest_advanced_event, quest_completed_event = 0, 0, 0
local scene_quest_started_event, scene_quest_advanced_event, scene_quest_completed_event = 0, 0, 0
local item_use_quest_started_event, item_use_quest_advanced_event, item_use_quest_completed_event = 0, 0, 0
local item_given_quest_started_event, item_given_quest_advanced_event, item_given_quest_completed_event = 0, 0, 0
local item_removed_quest_started_event, item_removed_quest_advanced_event, item_removed_quest_completed_event = 0, 0, 0
game.events.subscribe("quests.started", function(event)
  if event.id == "test.content-probe:story" then
    quest_started_event = quest_started_event + 1
  elseif event.id == "test.content-probe:scene-check" then
    scene_quest_started_event = scene_quest_started_event + 1
  elseif event.id == "test.content-probe:item-use" then
    item_use_quest_started_event = item_use_quest_started_event + 1
  elseif event.id == "test.content-probe:item-given" then
    item_given_quest_started_event = item_given_quest_started_event + 1
  elseif event.id == "test.content-probe:item-removed" then
    item_removed_quest_started_event = item_removed_quest_started_event + 1
  else
    error("unexpected quest start " .. tostring(event.id))
  end
end)
game.events.subscribe("quests.objective_advanced", function(event)
  if event.id == "test.content-probe:story" then
    quest_advanced_event = quest_advanced_event + 1
  elseif event.id == "test.content-probe:scene-check" then
    scene_quest_advanced_event = scene_quest_advanced_event + 1
  elseif event.id == "test.content-probe:item-use" then
    item_use_quest_advanced_event = item_use_quest_advanced_event + 1
  elseif event.id == "test.content-probe:item-given" then
    item_given_quest_advanced_event = item_given_quest_advanced_event + 1
  elseif event.id == "test.content-probe:item-removed" then
    item_removed_quest_advanced_event = item_removed_quest_advanced_event + 1
  else
    error("unexpected quest progress " .. tostring(event.id))
  end
end)
game.events.subscribe("quests.completed", function(event)
  if event.id == "test.content-probe:story" then
    quest_completed_event = quest_completed_event + 1
    assert(#event.rewards == 4 and event.rewards[1].kind == "currency" and
           event.rewards[1].status == "granted" and event.rewards[1].requested == 25 and
           event.rewards[2].kind == "item" and event.rewards[2].id == "test.content-probe:moon-cookie" and
           event.rewards[2].status == "granted" and event.rewards[2].requested == 2 and
           event.rewards[3].kind == "weapon" and event.rewards[3].id == "test.content-probe:quest-reward-blade" and
           event.rewards[3].status == "granted" and event.rewards[3].requested == 1 and
           event.rewards[3].result.added == true and
           event.rewards[4].kind == "weapon" and event.rewards[4].id == "test.content-probe:weapon-reward-refused" and
           event.rewards[4].status == "failed" and event.rewards[4].requested == 1 and
           event.rewards[4].result.added == false,
           "quest completion should expose successful and refused boolean weapon reward results")
  elseif event.id == "test.content-probe:scene-check" then
    scene_quest_completed_event = scene_quest_completed_event + 1
  elseif event.id == "test.content-probe:item-use" then
    item_use_quest_completed_event = item_use_quest_completed_event + 1
  elseif event.id == "test.content-probe:item-given" then
    item_given_quest_completed_event = item_given_quest_completed_event + 1
  elseif event.id == "test.content-probe:item-removed" then
    item_removed_quest_completed_event = item_removed_quest_completed_event + 1
  else
    error("unexpected quest completion " .. tostring(event.id))
  end
end)
local quest_definition = {
  id = "story", title = "A Small Story", description = "A quest lifecycle probe",
  objectives = {{id = "find", title = "Find the clue", target = 3},
                {id = "return", title = "Return to town", target = 1}},
  rewards = {{kind = "currency", amount = 25},
             {kind = "item", id = "test.content-probe:moon-cookie", count = 2},
             {kind = "weapon", id = "test.content-probe:quest-reward-blade"},
             {kind = "weapon", id = "test.content-probe:weapon-reward-refused"}}
}
local prior_quest, prior_quest_error = game.quests.get({id = "story"})
local quest_was_saved = prior_quest ~= nil
if not quest_was_saved then assert(prior_quest_error.code == "failed") end
local registered_quest = assert(game.quests.register(quest_definition))
assert(registered_quest.id == "test.content-probe:story" and registered_quest.idempotent == quest_was_saved,
       "quest registration mismatch: " .. tostring(registered_quest.id) .. "/" .. tostring(registered_quest.idempotent) .. "/" .. tostring(quest_was_saved))
local repeated_quest, repeated_quest_error = game.quests.register(quest_definition)
assert(repeated_quest and repeated_quest.id == registered_quest.id,
       repeated_quest_error and repeated_quest_error.message or "repeated quest registration failed")
assert(repeated_quest.idempotent)
if quest_was_saved then
  assert(prior_quest.status == "completed" and prior_quest.objectives[2].progress == 1)
else
  local quest_start = assert(game.quests.start({id = "story"}))
  assert(quest_start.status == "active" and not quest_start.idempotent and quest_started_event == 1)
  local first_progress = assert(game.quests.advance({id = "story", objective = "find", amount = 2}))
  assert(first_progress.progress == 2 and first_progress.advanced == 2 and first_progress.status == "active")
  assert(quest_advanced_event == 1)
  local second_progress = assert(moonmod.api.call("quests.advance", {id = "story", objective = "find"}))
  assert(second_progress.progress == 3 and second_progress.status == "active")
  assert(assert(game.quests.advance({id = "story", objective = "return"})).status == "completed")
  assert(quest_advanced_event == 3 and quest_completed_event == 1)
end
local quest_record = assert(game.quests.get({id = "test.content-probe:story"}))
assert(quest_record.status == "completed" and quest_record.objectives[1].progress == 3)

local scene_quest_definition = {
  id = "scene-check", title = "Scene Check",
  objectives = {
    {id = "enter-dungeon", title = "Enter a dungeon", target = 1,
     event = "game.scene.entered", match = {scene_id = "dungeon"}},
    {id = "return-to-town", title = "Return to town", target = 1,
     event = "game.scene.entered", match = {scene_id = "town"}}
  }
}
local prior_scene_quest, prior_scene_quest_error = game.quests.get({id = "scene-check"})
local registered_scene_quest = assert(game.quests.register(scene_quest_definition))
assert(registered_scene_quest.id == "test.content-probe:scene-check")
assert(registered_scene_quest.idempotent == (prior_scene_quest ~= nil))
assert(assert(game.quests.register(scene_quest_definition)).idempotent)
if prior_scene_quest == nil then
  local scene_start = assert(game.quests.start({id = "scene-check"}))
  assert(scene_start.status == "active" and scene_quest_started_event == 1)
else
  assert(prior_scene_quest.status == "completed" and prior_scene_quest.objectives[2].progress == 1)
end
game.events.subscribe("game.scene.entered", function(scene)
  local scene_quest = assert(game.quests.get({id = "scene-check"}))
  assert(scene_quest.objectives[1].event == "game.scene.entered")
  assert(scene_quest.objectives[1].match.scene_id == "dungeon")
  if scene.scene_id == "dungeon" then
    assert(scene_quest.objectives[1].progress == 1 and scene_quest.objectives[2].progress == 0)
    assert(scene_quest_advanced_event == 1)
  elseif scene.scene_id == "town" then
    assert(scene_quest.status == "completed" and scene_quest.objectives[2].progress == 1)
    assert(scene_quest_advanced_event == 2 and scene_quest_completed_event == 1)
    moonmod.log("info", "MOONMOD_QUEST_EVENT_PROGRESSION_OK")
  end
end)

local item_use_event_count = 0
local item_use_quest_definition = {
  id = "item-use", title = "Use a Mod Item",
  objectives = {
    {id = "use-tonic", title = "Use the tonic", target = 1,
     event = "game.item_used", match = {id = "test:item-used-event"}},
    {id = "set-flag", title = "Set the story flag", target = 1,
     event = "game.story_flag_changed", match = {id = 1911, value = true}}
  }
}
local prior_item_use_quest = game.quests.get({id = "item-use"})
local item_use_quest = assert(game.quests.register(item_use_quest_definition))
assert(item_use_quest.id == "test.content-probe:item-use")
assert(assert(game.quests.register(item_use_quest_definition)).idempotent)
if prior_item_use_quest == nil then
  assert(assert(game.quests.start({id = "item-use"})).status == "active")
  assert(item_use_quest_started_event == 1)
else
  assert(prior_item_use_quest.status == "completed" and prior_item_use_quest.objectives[1].progress == 1)
end
game.events.subscribe("game.item_used", function(event)
  assert(event.id == "test:item-used-event")
  assert(event.source_mod == "test.content-probe" and event.item_no == 777)
  assert(item_use_quest_advanced_event == 2 and item_use_quest_completed_event == 1)
  item_use_event_count = item_use_event_count + 1
  local progress = assert(game.quests.get({id = "item-use"}))
  assert(progress.status == "completed" and progress.objectives[1].progress == 1)
  moonmod.log("info", "MOONMOD_ITEM_USED_EVENT_OK")
end)
local story_flag_event_count = 0
game.events.subscribe("game.story_flag_changed", function(event)
  assert(event.id == 1911 and event.value and not event.previous)
  assert(event.source_mod == "test.content-probe")
  local progress = assert(game.quests.get({id = "item-use"}))
  assert(progress.status == "active" and progress.objectives[1].progress == 0 and
         progress.objectives[2].progress == 1)
  story_flag_event_count = story_flag_event_count + 1
  moonmod.log("info", "MOONMOD_STORY_FLAG_EVENT_OK")
end)
game.events.subscribe("tests.request_story_flag", function()
  local changed = assert(game.world.story_flag_set({id = 1911, value = true}))
  assert(changed.changed and story_flag_event_count == 0,
         "story-flag event must wait for the runtime pump")
end)
game.events.subscribe("tests.request_item_use", function()
  assert(item_use_event_count == 0, "item-use event must not re-enter the active API callback")
  local used = assert(game.items.use({id = "test:item-used-event"}))
  assert(used.id == "test:item-used-event")
  assert(item_use_event_count == 0, "item-use event must wait for the runtime pump")
end)

local item_given_event_count = 0
local item_given_quest_definition = {
  id = "item-given", title = "Receive Mod Content",
  objectives = {
    {id = "receive-item", title = "Receive the tonic", target = 1,
     event = "game.item_given", match = {id = "test:item-given-event", kind = "item"}},
    {id = "receive-weapon", title = "Receive the blade", target = 1,
     event = "game.item_given", match = {id = "test:weapon-given-event", kind = "weapon"}}
  }
}
local prior_item_given_quest = game.quests.get({id = "item-given"})
local item_given_quest = assert(game.quests.register(item_given_quest_definition))
assert(item_given_quest.id == "test.content-probe:item-given")
assert(assert(game.quests.register(item_given_quest_definition)).idempotent)
if prior_item_given_quest == nil then
  assert(assert(game.quests.start({id = "item-given"})).status == "active")
  assert(item_given_quest_started_event == 1)
else
  assert(prior_item_given_quest.status == "completed" and prior_item_given_quest.objectives[2].progress == 1)
end
game.events.subscribe("game.item_given", function(event)
  assert(event.source_mod == "test.content-probe" and event.frame >= 0)
  if event.kind == "item" then
    assert(event.id == "test:item-given-event" and event.native_item_no == 778 and
           event.requested == 2 and event.added == 2)
  else
    assert(event.kind == "weapon" and event.id == "test:weapon-given-event" and
           event.native_item_no == 779 and event.requested == 1 and event.added == true and event.slot == 0)
  end
  item_given_event_count = item_given_event_count + 1
  assert(item_given_event_count <= 2, "a failed grant or one successful grant must not emit extra events")
  assert(item_given_quest_advanced_event == item_given_event_count)
  if item_given_event_count == 2 then
    local progress = assert(game.quests.get({id = "item-given"}))
    assert(progress.status == "completed" and progress.objectives[1].progress == 1 and
           progress.objectives[2].progress == 1)
    assert(item_given_quest_completed_event == 1)
    moonmod.log("info", "MOONMOD_ITEM_GIVEN_QUEST_OK")
  end
end)
game.events.subscribe("tests.request_item_given", function()
  assert(item_given_event_count == 0, "item-given events must not re-enter the requesting callback")
  local item = assert(game.items.give({id = "test:item-given-event", count = 2}))
  assert(item.added == 2 and item_given_event_count == 0)
  local refused = assert(game.items.give({id = "test:item-given-refused"}))
  assert(refused.added == 0 and item_given_event_count == 0)
  local weapon = assert(game.weapons.give({id = "test:weapon-given-event"}))
  assert(weapon.added == true and item_given_event_count == 0)
end)

local item_removed_event_count = 0
local item_removed_quest_definition = {
  id = "item-removed", title = "Turn In Mod Content",
  objectives = {{id = "turn-in-tonic", title = "Turn in the tonic", target = 1,
                 event = "game.item_removed", match = {id = "test:item-removed-event"}}}
}
local prior_item_removed_quest = game.quests.get({id = "item-removed"})
local item_removed_quest = assert(game.quests.register(item_removed_quest_definition))
assert(item_removed_quest.id == "test.content-probe:item-removed")
assert(assert(game.quests.register(item_removed_quest_definition)).idempotent)
if prior_item_removed_quest == nil then
  assert(assert(game.quests.start({id = "item-removed"})).status == "active")
  assert(item_removed_quest_started_event == 1)
else
  assert(prior_item_removed_quest.status == "completed" and prior_item_removed_quest.objectives[1].progress == 1)
end
game.events.subscribe("game.item_removed", function(event)
  assert(event.id == "test:item-removed-event" and event.source_mod == "test.content-probe" and event.frame >= 0)
  assert(event.requested == 1 and event.removed == 1 and event.remaining == 0)
  item_removed_event_count = item_removed_event_count + 1
  assert(item_removed_event_count == 1, "a no-op removal must not emit an event")
  assert(item_removed_quest_advanced_event == 1 and item_removed_quest_completed_event == 1)
  local progress = assert(game.quests.get({id = "item-removed"}))
  assert(progress.status == "completed" and progress.objectives[1].progress == 1)
  moonmod.log("info", "MOONMOD_ITEM_REMOVED_QUEST_OK")
end)
game.events.subscribe("tests.request_item_removed", function()
  assert(item_removed_event_count == 0, "item-removed events must not re-enter the requesting callback")
  local removed = assert(game.items.remove({id = "test:item-removed-event", count = 1}))
  assert(removed.removed == 1 and removed.remaining == 0 and item_removed_event_count == 0)
  local no_op = assert(game.items.remove({id = "test:item-remove-no-op", count = 1}))
  assert(no_op.removed == 0 and item_removed_event_count == 0)
end)

local prior_reward_definition = game.quests.get({id = "reward-definition"})
local all_quests = assert(game.quests.get())
assert(#all_quests.quests == (prior_reward_definition ~= nil and 6 or 5))
local bad_quest, bad_quest_error = game.quests.register({
  id = "bad", title = "Bad", objectives = {{id = "same", title = "One", target = 1},
                                              {id = "same", title = "Two", target = 1}}
})
assert(bad_quest == nil and bad_quest_error.code == "invalid_argument")
local bad_event_quest, bad_event_error = game.quests.register({
  id = "bad-event", title = "Bad Event",
  objectives = {{id = "one", title = "One", target = 1, event = "quests.completed"}}
})
assert(bad_event_quest == nil and bad_event_error.code == "invalid_argument")
local bad_reward, bad_reward_error = game.quests.register({
  id = "bad-reward", title = "Bad Reward", objectives = {{id = "one", title = "One", target = 1}},
  rewards = {{kind = "item", amount = 1}}
})
assert(bad_reward == nil and bad_reward_error.code == "invalid_argument")
local malformed_weapon_reward, malformed_weapon_reward_error = game.quests.register({
  id = "malformed-weapon-reward", title = "Malformed Weapon Reward",
  objectives = {{id = "one", title = "One", target = 1}},
  rewards = {{kind = "weapon", id = "test:blade", count = 2}}
})
assert(malformed_weapon_reward == nil and malformed_weapon_reward_error.code == "invalid_argument")
local oversized_reward, oversized_reward_error = game.quests.register({
  id = "oversized-reward", title = "Oversized Reward", objectives = {{id = "one", title = "One", target = 1}},
  rewards = {{kind = "currency", amount = 32768}}
})
assert(oversized_reward == nil and oversized_reward_error.code == "invalid_argument")
local reward_definition = assert(game.quests.register({
  id = "reward-definition", title = "Reward Definition", objectives = {{id = "one", title = "One", target = 1}},
  rewards = {{kind = "currency", amount = 25}, {kind = "item", id = "test.content-probe:moon-cookie", count = 2},
             {kind = "weapon", id = "test.content-probe:quest-reward-blade"}}
}))
local reward_record = assert(game.quests.get({id = "reward-definition"}))
assert(reward_definition.idempotent == (prior_reward_definition ~= nil) and #reward_record.rewards == 3 and
       reward_record.rewards[1].kind == "currency" and reward_record.rewards[1].amount == 25 and
       reward_record.rewards[2].kind == "item" and reward_record.rewards[2].id == "test.content-probe:moon-cookie" and
       reward_record.rewards[2].count == 2 and reward_record.rewards[3].kind == "weapon" and
       reward_record.rewards[3].id == "test.content-probe:quest-reward-blade")
game.events.subscribe("game.tick", function(frame)
  if frame.frame == 1 then
    local persisted = assert(game.quests.get({id = "story"}))
    assert(persisted.status == "completed" and persisted.objectives[2].progress == 1)
    local scene_persisted = assert(game.quests.get({id = "scene-check"}))
    assert(scene_persisted.status == "completed" and scene_persisted.objectives[1].progress == 1 and
           scene_persisted.objectives[2].progress == 1)
    local item_use_persisted = assert(game.quests.get({id = "item-use"}))
    assert(item_use_persisted.status == "completed" and item_use_persisted.objectives[1].progress == 1 and
           item_use_persisted.objectives[2].progress == 1)
    local item_given_persisted = assert(game.quests.get({id = "item-given"}))
    assert(item_given_persisted.status == "completed" and item_given_persisted.objectives[1].progress == 1 and
           item_given_persisted.objectives[2].progress == 1)
    local item_removed_persisted = assert(game.quests.get({id = "item-removed"}))
    assert(item_removed_persisted.status == "completed" and item_removed_persisted.objectives[1].progress == 1)
    moonmod.log("info", "MOONMOD_QUEST_PERSISTED:" .. info.id)
  end
end)
local core = {
  ["info.get"] = true, ["compat.list"] = true, ["compat.supports"] = true,
  ["api.list"] = true, ["api.supports"] = true, ["api.call"] = true,
  ["events.list"] = true, ["events.subscribe"] = true, ["events.unsubscribe"] = true,
  ["save.get"] = true, ["save.set"] = true, ["save.migrate"] = true,
  ["quests.get"] = true, ["quests.register"] = true, ["quests.start"] = true,
  ["quests.advance"] = true,
  ["localization.register"] = true, ["localization.get"] = true,
  ["resources.load"] = true, ["resources.resolve"] = true, ["resources.release"] = true,
  ["resources.override"] = true, ["resources.unoverride"] = true,
  ["ui.register"] = true, ["ui.remove"] = true,
  ["ui.notify"] = true,
  ["commands.register"] = true, ["commands.execute"] = true, ["commands.unregister"] = true,
  ["scripts.register_function"] = true, ["scripts.list_functions"] = true, ["scripts.call_function"] = true,
  ["scripts.unregister_function"] = true, ["scripts.list_hooks"] = true, ["scripts.call"] = true,
  ["scripts.override"] = true,
  ["skills.list"] = true, ["skills.get"] = true, ["skills.register"] = true,
  ["skills.unlock"] = true, ["skills.set_rank"] = true,
  ["debug.profile"] = true,
  ["ui.navigate"] = true,
  ["ui.open_skills"] = true,
  ["ui.sheet_set"] = true, ["ui.sheet_clear"] = true, ["ui.sheet_open"] = true
}
local dispatched = 0
for _, operation in ipairs(catalog) do
  if operation.supported and not core[operation.path] and
     operation.path ~= "items.register" and operation.path ~= "world.register_dungeon" and
     operation.path ~= "player.get" then
    local response, response_error = moonmod.api.call(operation.path, {probe = "every-domain"})
    assert(response_error == nil and response.path == operation.path, operation.path)
    dispatched = dispatched + 1
  end
end
assert(dispatched >= 35)
local player, unsupported = game.player.get()
assert(player == nil and unsupported.code == "unsupported")
moonmod.on("runtime.loaded", function()
  moonmod.log("info", "MOONMOD_API_PROBE_OK:" .. info.id)
end)
local mod = {}
function mod.on_load()
  local version = assert(game.save.get("__moonmod_schema_version"))
  if version == 0 then
    local migration, migration_error = game.save.migrate({
      from_version = 0,
      to_version = 1,
      set = { ["migration-probe"] = {revision = 1, migrated = true} },
      delete = {"legacy"}
    })
    assert(migration_error == nil and migration.migrated and migration.to_version == 1)
    moonmod.log("info", "MOONMOD_SAVE_MIGRATED:" .. info.id)
  else
    assert(version == 1)
    local migration, migration_error = game.save.migrate({
      from_version = 0,
      to_version = 1,
      set = { ["migration-probe"] = {revision = 1, migrated = true} },
      delete = {"legacy"}
    })
    assert(migration_error == nil and not migration.migrated and migration.version == 1)
    moonmod.log("info", "MOONMOD_SAVE_MIGRATION_IDEMPOTENT:" .. info.id)
  end
end
return mod
)lua");

    std::vector<std::string> calls;
    std::vector<std::string> logs;
    moonmod::RuntimeConfig config;
    config.game_id = game_id;
    config.game_name = game_id == "chronicle" ? "Chronicle" : "Chronicle Two";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    std::string current_language = "fr-FR";
    config.current_language = [&current_language] { return current_language; };
    for (const moonmod::ApiOperationSpec &operation : moonmod::ApiCatalog()) {
        const bool future_native_operation = (operation.path.starts_with("locations.") &&
                                              operation.path != "locations.register") ||
                                             operation.path.starts_with("dungeons.") ||
                                             operation.path.starts_with("georama.") ||
                                             (operation.path.starts_with("models.") && operation.path != "models.resolve" &&
                                              operation.path != "models.replace" && operation.path != "models.remove") ||
                                             operation.path.starts_with("combat.styles_");
        if (!moonmod::IsCoreApiOperation(operation.path) && operation.path != "player.get" &&
            !future_native_operation) {
            config.api_capabilities.push_back({std::string(operation.path), true, ""});
        }
    }
    config.api_handler = [&](std::string_view, const moonmod::ApiOperationSpec &operation,
                             const moonmod::Json &arguments) -> moonmod::ApiCallResult {
        calls.emplace_back(operation.path);
        EXPECT_TRUE(arguments.is_array());
        EXPECT_EQ(arguments.size(), 1u);
        if (!arguments[0].is_object()) {
            return {moonmod::ApiCallStatus::InvalidArgument, nullptr, "test adapter expected an object"};
        }
        if (operation.path == "items.register") {
            EXPECT_EQ(arguments[0].value("id", ""), "test:starlight-tonic");
            EXPECT_TRUE(arguments[0].contains("assets") && arguments[0]["assets"].empty());
            return {moonmod::ApiCallStatus::Ok, {{"id", "test:starlight-tonic"}}, {}};
        }
        if (operation.path == "world.register_dungeon") {
            EXPECT_EQ(arguments[0].value("id", ""), "test:moon-vault");
            EXPECT_EQ(arguments[0]["floors"].size(), 1u);
            return {moonmod::ApiCallStatus::Ok, {{"id", "test:moon-vault"}}, {}};
        }
        if (operation.path == "items.use" && arguments[0].value("id", "") == "test:item-used-event") {
            return {moonmod::ApiCallStatus::Ok,
                    {{"id", "test:item-used-event"}, {"item_no", 777}, {"target", "max"}, {"consumed", 1}}, {}};
        }
        if (operation.path == "items.give") {
            const std::string id = arguments[0].value("id", "");
            if (id == "test:item-given-event") {
                return {moonmod::ApiCallStatus::Ok,
                        {{"id", id}, {"native_item_no", 778}, {"requested", 2}, {"added", 2}}, {}};
            }
            if (id == "test:item-given-refused") {
                return {moonmod::ApiCallStatus::Ok,
                        {{"id", id}, {"native_item_no", 780}, {"requested", 1}, {"added", 0}}, {}};
            }
        }
        if (operation.path == "weapons.give" &&
            arguments[0].value("id", "") == "test.content-probe:quest-reward-blade") {
            return {moonmod::ApiCallStatus::Ok,
                    {{"id", "test.content-probe:quest-reward-blade"}, {"added", true}}, {}};
        }
        if (operation.path == "weapons.give" &&
            arguments[0].value("id", "") == "test.content-probe:weapon-reward-refused") {
            return {moonmod::ApiCallStatus::Ok,
                    {{"id", "test.content-probe:weapon-reward-refused"}, {"added", false}}, {}};
        }
        if (operation.path == "items.remove") {
            const std::string id = arguments[0].value("id", "");
            if (id == "test:item-removed-event") {
                return {moonmod::ApiCallStatus::Ok,
                        {{"id", id}, {"requested", 1}, {"removed", 1}, {"remaining", 0}}, {}};
            }
            if (id == "test:item-remove-no-op") {
                return {moonmod::ApiCallStatus::Ok,
                        {{"id", id}, {"requested", 1}, {"removed", 0}, {"remaining", 0}}, {}};
            }
        }
        if (operation.path == "weapons.give" && arguments[0].value("id", "") == "test:weapon-given-event") {
            return {moonmod::ApiCallStatus::Ok,
                    {{"id", "test:weapon-given-event"}, {"native_item_no", 779}, {"requested", 1},
                     {"added", true}, {"slot", 0}}, {}};
        }
        if (operation.path == "world.story_flag_set" && arguments[0].value("id", 0) == 1911) {
            return {moonmod::ApiCallStatus::Ok,
                    {{"id", 1911}, {"previous", false}, {"value", true}, {"changed", true}}, {}};
        }
        return {moonmod::ApiCallStatus::Ok, {{"path", operation.path}}, {}};
    };
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };

    moonmod::Runtime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Start(config, error)) << error;
    const moonmod::Json combat_bonuses = runtime.CombatDamageBonuses();
    // the probe's log says where its Lua stopped when it did not get as far as the skills
    ASSERT_EQ(combat_bonuses.size(), 1u) << [&logs] {
        std::string joined;
        for (const std::string &line : logs) joined += line + "\n";
        return joined;
    }();
    EXPECT_EQ(combat_bonuses[0].at("owner"), "test.content-probe");
    EXPECT_EQ(combat_bonuses[0].at("skill_id"), "test.content-probe:combat.focus");
    EXPECT_EQ(combat_bonuses[0].at("rank"), 2);
    EXPECT_DOUBLE_EQ(combat_bonuses[0].at("per_rank").get<double>(), 0.1);
    EXPECT_FLOAT_EQ(moonmod::ApplyCombatDamageBonuses(10.0f, combat_bonuses), 12.0f);
    EXPECT_TRUE(moonmod::FindNativeResourceOverride("probe/unload.bin").has_value());
    ASSERT_TRUE(runtime.PublishToMod("test.function-provider", "tests.functions.unregister", moonmod::Json::object()));
    ASSERT_TRUE(runtime.PublishToMod("test.function-caller", "tests.functions.check_cleanup", moonmod::Json::object()));
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_FUNCTION_LIST_CLEANUP_OK"), logs.end());
    runtime.Publish("game.scene.entered", {{"scene_id", "dungeon"}, {"native_mode", 2}, {"frame", runtime.Frame()}});
    runtime.Publish("game.scene.entered", {{"scene_id", "town"}, {"native_mode", 1}, {"frame", runtime.Frame()}});
    runtime.Publish("game.scene.will_enter", {{"scene_id", "dungeon"}, {"native_mode", 2}, {"frame", runtime.Frame()}});
    runtime.Publish("tests.request_story_flag", moonmod::Json::object());
    runtime.Publish("tests.request_item_use", moonmod::Json::object());
    runtime.Publish("tests.request_item_given", moonmod::Json::object());
    runtime.Publish("tests.request_item_removed", moonmod::Json::object());
    EXPECT_EQ(std::find(logs.begin(), logs.end(), "MOONMOD_ITEM_USED_EVENT_OK"), logs.end());
    EXPECT_EQ(std::find(logs.begin(), logs.end(), "MOONMOD_ITEM_GIVEN_QUEST_OK"), logs.end());
    EXPECT_EQ(std::find(logs.begin(), logs.end(), "MOONMOD_STORY_FLAG_EVENT_OK"), logs.end());
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_SCENE_EVENT:dungeon:2"), logs.end());
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_QUEST_EVENT_PROGRESSION_OK"), logs.end());
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_SCENE_WILL_ENTER_EVENT:dungeon:2"), logs.end());
    EXPECT_EQ(moonmod::ManagerState().notice, "Resource probe ready");
    EXPECT_EQ(moonmod::ManagerState().notice_frames, 120);
    moonmod::OpenManager();
    EXPECT_TRUE(moonmod::ManagerOpen());
    EXPECT_EQ(moonmod::ManagerState().notice, "Resource probe ready");
    moonmod::CloseManager();
    ASSERT_EQ(runtime.Mods().size(), 6u);
    ASSERT_EQ(runtime.Mods()[0].state, moonmod::ModState::Loaded) << runtime.Mods()[0].status_message;
    ASSERT_EQ(runtime.Mods()[1].state, moonmod::ModState::Loaded);
    ASSERT_EQ(runtime.Mods()[2].state, moonmod::ModState::Loaded);
    std::size_t expected_calls = 2;
    for (const moonmod::ApiOperationSpec &operation : moonmod::ApiCatalog()) {
        const bool future_native_operation = (operation.path.starts_with("locations.") &&
                                              operation.path != "locations.register") ||
                                             operation.path.starts_with("dungeons.") ||
                                             operation.path.starts_with("georama.") ||
                                             (operation.path.starts_with("models.") && operation.path != "models.resolve" &&
                                              operation.path != "models.replace" && operation.path != "models.remove") ||
                                             operation.path.starts_with("combat.styles_");
        if (!moonmod::IsCoreApiOperation(operation.path) && operation.path != "player.get" &&
            operation.path != "items.register" && operation.path != "world.register_dungeon" &&
            !future_native_operation) {
            ++expected_calls;
        }
    }
    expected_calls += 11; // story flag, item use, item/weapon grants, four quest payouts, and two removal attempts
    EXPECT_EQ(calls.size(), expected_calls) << testing::PrintToString(calls);
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_API_PROBE_OK:" + game_id), logs.end());
    runtime.Pump();
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_ITEM_USED_EVENT_OK"), logs.end())
        << testing::PrintToString(logs);
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_STORY_FLAG_EVENT_OK"), logs.end())
        << testing::PrintToString(logs);
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_ITEM_GIVEN_QUEST_OK"), logs.end())
        << testing::PrintToString(logs);
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_ITEM_REMOVED_QUEST_OK"), logs.end())
        << testing::PrintToString(logs);
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_RESOURCE_OWNER_ISOLATED"), logs.end());
    EXPECT_EQ(std::find(logs.begin(), logs.end(), "MOONMOD_RESOURCE_RELEASE_OK"), logs.end());
    runtime.Pump();
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_RESOURCE_RELEASE_OK"), logs.end());
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_TICK:1"), 1);
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_TICK:2"), 0);
    runtime.Stop();
    EXPECT_FALSE(moonmod::FindNativeResourceOverride("probe/unload.bin").has_value());
    std::ifstream save_file(root.path / "save" / "moonmod" / "data" / "test.content-probe" / "storage.json");
    ASSERT_TRUE(save_file.good());
    const moonmod::Json saved_storage = moonmod::Json::parse(save_file);
    save_file.close();
    ASSERT_EQ(saved_storage.value("api-probe", moonmod::Json::object()).value("count", 0), 4);
    EXPECT_EQ(saved_storage.value("__moonmod_schema_version", -1), 1);
    EXPECT_EQ(saved_storage.value("migration-probe", moonmod::Json::object()).value("revision", 0), 1);
    EXPECT_FALSE(saved_storage.contains("legacy"));
    ASSERT_TRUE(saved_storage.contains("__moonmod_skills_v1"));
    EXPECT_EQ(saved_storage.at("__moonmod_skills_v1").at("combat").at("ranks").at("focus"), 2);
    EXPECT_EQ(saved_storage.at("__moonmod_skills_v1").at("combat").at("ranks").at("power"), 1);
    ASSERT_TRUE(saved_storage.contains("__moonmod_quests_v1"));
    const auto saved_quest = saved_storage.at("__moonmod_quests_v1").at("story");
    EXPECT_EQ(saved_quest.value("status", ""), "completed");
    EXPECT_EQ(saved_quest.at("progress").value("find", 0), 3);
    const auto saved_scene_quest = saved_storage.at("__moonmod_quests_v1").at("scene-check");
    EXPECT_EQ(saved_scene_quest.value("status", ""), "completed");
    EXPECT_EQ(saved_scene_quest.at("progress").value("enter-dungeon", 0), 1);
    EXPECT_EQ(saved_scene_quest.at("progress").value("return-to-town", 0), 1);

    moonmod::Runtime reloaded_runtime;
    ASSERT_TRUE(reloaded_runtime.Start(config, error)) << error;
    const moonmod::Json reloaded_combat_bonuses = reloaded_runtime.CombatDamageBonuses();
    std::string reloaded_mod_status;
    for (const moonmod::ModRecord &record : reloaded_runtime.Mods()) {
        reloaded_mod_status += record.id + ":" + std::to_string(static_cast<int>(record.state)) + ":" +
                               record.status_message + ";";
    }
    ASSERT_EQ(reloaded_combat_bonuses.size(), 1u) << reloaded_mod_status;
    EXPECT_EQ(reloaded_combat_bonuses[0].at("rank"), 2);
    EXPECT_TRUE(moonmod::FindNativeResourceOverride("probe/unload.bin").has_value());
    ASSERT_EQ(reloaded_runtime.Mods().size(), 6u);
    ASSERT_EQ(reloaded_runtime.Mods()[0].state, moonmod::ModState::Loaded)
        << reloaded_runtime.Mods()[0].status_message;
    EXPECT_NE(std::find(logs.begin(), logs.end(), "MOONMOD_SAVE_RELOAD_OK:" + game_id), logs.end());
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_SAVE_MIGRATED:" + game_id), 1);
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_SAVE_MIGRATION_IDEMPOTENT:" + game_id), 1);
    reloaded_runtime.Pump();
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_QUEST_PERSISTED:" + game_id), 2);
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_RESOURCE_OWNER_ISOLATED"), 2);
    reloaded_runtime.Pump();
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_RESOURCE_RELEASE_OK"), 2);
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_TICK:1"), 2);
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_TICK:2"), 0);
    reloaded_runtime.Stop();
    EXPECT_FALSE(moonmod::FindNativeResourceOverride("probe/unload.bin").has_value());
    moonmod::CloseManager();
}

} // namespace

// MOONMOD_NO_MODS=1, which a launcher sets when its Mods switch is off, starts every mod off for that run only: the
// player's choices stay in the profile, nothing reads as waiting for a restart, and the next launch loads them again.
TEST(MoonModRuntime, NoModsEnvironmentStartsEveryModOffAndKeepsTheProfile) {
    TempRoot root;
    Write(root.path / "mods" / "test.no-mods" / "mod.json",
          "{\"schema_version\":1,\"id\":\"test.no-mods\",\"name\":\"No Mods\",\"version\":\"1.0.0\","
          "\"framework_api\":1,\"games\":[\"chronicle\"],\"entry\":\"main.lua\",\"permissions\":[]}");
    Write(root.path / "mods" / "test.no-mods" / "main.lua", "moonmod.log('info', 'NO_MODS_TEST_LOADED')\n");
    const fs::path profile = root.path / "save" / "moonmod" / "profile.json";
    Write(profile, "{\"format_version\":1,\"enabled\":{\"test.no-mods\":true},\"settings\":{}}");

    std::vector<std::string> logs;
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };
    const auto loaded = [&] { return std::ranges::count(logs, std::string("NO_MODS_TEST_LOADED")); };
    std::string error;

#ifdef _WIN32
    _putenv_s("MOONMOD_NO_MODS", "1");
#else
    setenv("MOONMOD_NO_MODS", "1", 1);
#endif
    {
        moonmod::Runtime off;
        ASSERT_TRUE(off.Start(config, error)) << error;
        EXPECT_EQ(loaded(), 0);
        const auto record = std::ranges::find(off.Mods(), "test.no-mods", &moonmod::ModRecord::id);
        ASSERT_NE(record, off.Mods().end());
        EXPECT_FALSE(record->enabled);
        EXPECT_TRUE(record->enabled_next_launch);
        EXPECT_FALSE(record->restart_required);
        EXPECT_EQ(record->state, moonmod::ModState::Disabled);
        off.Stop();
    }
    std::ifstream kept(profile);
    const std::string text((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("\"test.no-mods\":true"), std::string::npos) << text;

#ifdef _WIN32
    _putenv_s("MOONMOD_NO_MODS", "");
#else
    unsetenv("MOONMOD_NO_MODS");
#endif
    moonmod::Runtime on;
    ASSERT_TRUE(on.Start(config, error)) << error;
    EXPECT_EQ(loaded(), 1);
    on.Stop();
}

TEST(MoonModFaultIsolation, LuaMemoryBudgetFailureIsScopedToChronicleMod) {
    ExerciseLuaMemoryBudgetIsolation("chronicle");
}
TEST(MoonModFaultIsolation, LuaMemoryBudgetFailureIsScopedToChronicleTwoMod) {
    ExerciseLuaMemoryBudgetIsolation("chronicle-two");
}
TEST(MoonModApi, ChronicleExposesCapabilityCheckedContentOperations) { ExerciseApi("chronicle"); }
TEST(MoonModApi, ChronicleTwoExposesCapabilityCheckedContentOperations) { ExerciseApi("chronicle-two"); }
TEST(MoonModScriptFunctions, OwnerExportsFollowModToggleChronicle) { ExerciseFunctionExportsAcrossReload("chronicle"); }
TEST(MoonModScriptFunctions, OwnerExportsFollowModToggleChronicleTwo) { ExerciseFunctionExportsAcrossReload("chronicle-two"); }
TEST(MoonModScriptHooks, ChronicleCanWrapReplaceAndRemoveNativeEventHooks) { ExerciseNativeScriptHooks("chronicle"); }
TEST(MoonModScriptHooks, ChronicleTwoCanWrapReplaceAndRemoveNativeEventHooks) { ExerciseNativeScriptHooks("chronicle-two"); }

TEST(MoonModApi, RejectsCapabilitiesWithoutNativeHandlers) {
    moonmod::Runtime runtime;
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.api_capabilities = {{"items.register", true, ""}};
    std::string error;
    EXPECT_FALSE(runtime.Start(std::move(config), error));
    EXPECT_NE(error.find("without an API handler"), std::string::npos);
}

TEST(MoonModApi, ChronicleAdapterReportsEveryNonCoreOperation) {
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    moonmod::ConfigureChronicleApi(config);
    EXPECT_TRUE(config.require_complete_api_capabilities);

    for (const moonmod::ApiOperationSpec &operation : moonmod::ApiCatalog()) {
        if (moonmod::IsCoreApiOperation(operation.path)) continue;
        const auto found = std::ranges::find(config.api_capabilities, operation.path, &moonmod::ApiCapability::path);
        ASSERT_NE(found, config.api_capabilities.end()) << operation.path;
        if (!found->supported) EXPECT_FALSE(found->reason.empty()) << operation.path;
    }

    const auto location = std::ranges::find(config.api_capabilities, "locations.register", &moonmod::ApiCapability::path);
    ASSERT_NE(location, config.api_capabilities.end());
    EXPECT_TRUE(location->supported);
    EXPECT_TRUE(location->reason.empty());

    const auto georama_register = std::ranges::find(config.api_capabilities, "georama.register",
                                                    &moonmod::ApiCapability::path);
    ASSERT_NE(georama_register, config.api_capabilities.end());
    EXPECT_TRUE(georama_register->supported);
    EXPECT_TRUE(georama_register->reason.empty());

    for (const std::string_view path : {"dungeons.list", "dungeons.get"}) {
        const auto dungeon = std::ranges::find(config.api_capabilities, path, &moonmod::ApiCapability::path);
        ASSERT_NE(dungeon, config.api_capabilities.end()) << path;
        EXPECT_TRUE(dungeon->supported) << path;
    }
    const auto dungeon_register = std::ranges::find(config.api_capabilities, "dungeons.register",
                                                   &moonmod::ApiCapability::path);
    ASSERT_NE(dungeon_register, config.api_capabilities.end());
    EXPECT_TRUE(dungeon_register->supported);
    EXPECT_TRUE(dungeon_register->reason.empty());
    const auto dungeon_alias = std::ranges::find(config.api_capabilities, "world.register_dungeon",
                                                 &moonmod::ApiCapability::path);
    ASSERT_NE(dungeon_alias, config.api_capabilities.end());
    EXPECT_TRUE(dungeon_alias->supported);
    EXPECT_TRUE(dungeon_alias->reason.empty());

    const auto despawn = std::ranges::find(config.api_capabilities, "entities.despawn", &moonmod::ApiCapability::path);
    ASSERT_NE(despawn, config.api_capabilities.end());
    EXPECT_TRUE(despawn->supported);

    const auto formula = std::ranges::find(config.api_capabilities, "combat.register_formula", &moonmod::ApiCapability::path);
    ASSERT_NE(formula, config.api_capabilities.end());
    EXPECT_TRUE(formula->supported);
    EXPECT_TRUE(formula->reason.empty());

    const auto style = std::ranges::find(config.api_capabilities, "combat.styles_register",
                                         &moonmod::ApiCapability::path);
    ASSERT_NE(style, config.api_capabilities.end());
    EXPECT_FALSE(style->supported);
    EXPECT_NE(style->reason.find("deferred"), std::string::npos);
    EXPECT_NE(style->reason.find("model rigs"), std::string::npos);
}

TEST(MoonModLocalization, ChronicleUsesTheSelectedNativeLanguage) {
    const int previous_language = LanguageCode;
    struct RestoreLanguage {
        int previous;
        ~RestoreLanguage() { LanguageCode = previous; }
    } restore{previous_language};

    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    moonmod::ConfigureChronicleApi(config);
    ASSERT_TRUE(config.current_language);
    for (const auto &[native_language, expected] : {
             std::pair{LANG_JAPANESE, std::string_view("ja")},
             std::pair{LANG_ENGLISH_US, std::string_view("en-US")},
             std::pair{LANG_ENGLISH_UK, std::string_view("en-GB")},
             std::pair{LANG_FRENCH, std::string_view("fr")},
             std::pair{LANG_GERMAN, std::string_view("de")},
             std::pair{LANG_ITALIAN, std::string_view("it")},
             std::pair{LANG_SPANISH, std::string_view("es")},
         }) {
        LanguageCode = native_language;
        EXPECT_EQ(config.current_language(), expected);
    }
}

TEST(MoonModApi, RequiredCapabilityLedgerRejectsMissingOperation) {
    moonmod::Runtime runtime;
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.require_complete_api_capabilities = true;
    config.api_capabilities = {{"items.register", true, {}}};
    config.api_handler = [](std::string_view, const moonmod::ApiOperationSpec &, const moonmod::Json &) {
        return moonmod::ApiCallResult{moonmod::ApiCallStatus::Ok, nullptr, {}};
    };
    std::string error;
    EXPECT_FALSE(runtime.Start(std::move(config), error));
    EXPECT_NE(error.find("did not record support or unsupported status"), std::string::npos);
}

TEST(MoonModApi, RequiredCapabilityLedgerRejectsUnsupportedOperationWithoutReason) {
    moonmod::Runtime runtime;
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.require_complete_api_capabilities = true;
    config.api_capabilities = {{"locations.register", false, {}}};
    moonmod::CompleteApiCapabilityLedger(config.game_id, config.api_capabilities);
    config.api_handler = [](std::string_view, const moonmod::ApiOperationSpec &, const moonmod::Json &) {
        return moonmod::ApiCallResult{moonmod::ApiCallStatus::Ok, nullptr, {}};
    };
    std::string error;
    EXPECT_FALSE(runtime.Start(std::move(config), error));
    EXPECT_NE(error.find("unsupported without a reason"), std::string::npos);
}

TEST(MoonModApi, PublishesWorldLocationChangesForQuestObjectives) {
    moonmod::CloseManager();
    TempRoot root;
    const fs::path package = root.path / "mods" / "test.world-observer";
    Write(package / "mod.json",
          R"json({"schema_version":1,"id":"test.world-observer","name":"World Observer","version":"1.0.0","framework_api":1,"games":["chronicle-two"],"entry":"main.lua","permissions":["content.register","game.read","game.write"]})json");
    Write(package / "main.lua", R"lua(
assert(game.quests.register({
  id = "reach-map", title = "Reach Map Two",
  objectives = {{id = "arrive", title = "Arrive at map two", target = 1,
                event = "game.world.location_changed", match = {map_id = 2}}}
}))
assert(game.quests.start({id = "reach-map"}))
game.events.subscribe("game.world.location_changed", function(event)
  assert(event.frame and event.previous and event.map_id)
  local quest = assert(game.quests.get({id = "reach-map"}))
  if event.map_id == 2 then
    assert(event.previous.map_id == 1)
    assert(quest.status == "completed" and quest.objectives[1].progress == 1)
    moonmod.log("info", "MOONMOD_WORLD_LOCATION_QUEST_ADVANCED")
  elseif event.map_id == 3 then
    assert(event.previous.map_id == 2)
    assert(quest.status == "completed" and quest.objectives[1].progress == 1)
    moonmod.log("info", "MOONMOD_WORLD_LOCATION_CHANGED_AGAIN")
  else
    error("unexpected world location " .. tostring(event.map_id))
  end
end)
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.world-observer":true},"settings":{}})json");

    moonmod::Json location = {{"context", "map"}, {"area_id", 0}, {"map_id", 1},
                              {"sub_map_id", nullptr}, {"dungeon_id", nullptr}, {"floor_id", nullptr}};
    bool location_available = true;
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle-two";
    config.game_name = "Chronicle Two";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    config.api_capabilities = {{"world.get", true, {}}};
    config.world_location_reader = [&location, &location_available] {
        if (!location_available) {
            return moonmod::ApiCallResult{moonmod::ApiCallStatus::Failed, nullptr, "world is loading"};
        }
        return moonmod::ApiCallResult{moonmod::ApiCallStatus::Ok, location, {}};
    };
    config.api_handler = [&location](std::string_view, const moonmod::ApiOperationSpec &operation,
                                     const moonmod::Json &) {
        if (operation.path != "world.get") {
            return moonmod::ApiCallResult{moonmod::ApiCallStatus::Unsupported, nullptr, "not part of this probe"};
        }
        return moonmod::ApiCallResult{moonmod::ApiCallStatus::Ok, location, {}};
    };
    std::vector<std::string> logs;
    config.log = [&logs](std::string_view, std::string_view, std::string_view message) {
        logs.emplace_back(message);
    };
    moonmod::Runtime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Start(std::move(config), error)) << error;
    ASSERT_EQ(runtime.Mods().size(), 1u);
    ASSERT_EQ(runtime.Mods()[0].state, moonmod::ModState::Loaded) << runtime.Mods()[0].status_message;

    runtime.Publish("game.scene.entered", {{"scene_id", "town"}, {"native_mode", 1}, {"frame", runtime.Frame()}});
    runtime.ObserveWorldLocationNow(); // Establish the initial location without reporting a change.
    EXPECT_TRUE(logs.empty());
    location["map_id"] = 2;
    location_available = false;
    runtime.Pump(); // A transient loader gap must not discard the last valid location.
    location_available = true;
    runtime.Publish("game.scene.entered", {{"scene_id", "town"}, {"native_mode", 1}, {"frame", runtime.Frame()}});
    runtime.ObserveWorldLocationNow();
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_WORLD_LOCATION_QUEST_ADVANCED"), 1);
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_WORLD_LOCATION_CHANGED_AGAIN"), 0);
    runtime.Pump();
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_WORLD_LOCATION_QUEST_ADVANCED"), 1);
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_WORLD_LOCATION_CHANGED_AGAIN"), 0);
    runtime.Pump(); // A stable location does not emit again.
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_WORLD_LOCATION_QUEST_ADVANCED"), 1);
    location["map_id"] = 3;
    runtime.Pump();
    EXPECT_EQ(std::count(logs.begin(), logs.end(), "MOONMOD_WORLD_LOCATION_CHANGED_AGAIN"), 1);
    runtime.Stop();
    moonmod::CloseManager();
}


TEST(MoonModApi, ChroniclePlayerTuningValidatesCombinesAndRestoresOnUnload) {
    using moonmod::ApiCallStatus;
    using moonmod::Json;
    moonmod::ResetChroniclePlayerTuningForTests();
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    moonmod::ConfigureChronicleApi(config);
    const moonmod::ApiOperationSpec *set = moonmod::FindApiOperation("player.tuning_set");
    const moonmod::ApiOperationSpec *clear = moonmod::FindApiOperation("player.tuning_clear");
    const moonmod::ApiOperationSpec *invincible = moonmod::FindApiOperation("player.invincible");
    ASSERT_NE(set, nullptr);
    ASSERT_NE(clear, nullptr);
    ASSERT_NE(invincible, nullptr);
    for (const char *path : {"player.tuning_set", "player.tuning_clear", "player.invincible"}) {
        const auto capability = std::ranges::find(config.api_capabilities, path, &moonmod::ApiCapability::path);
        ASSERT_NE(capability, config.api_capabilities.end()) << path;
        EXPECT_TRUE(capability->supported) << path;
        EXPECT_FALSE(moonmod::IsCoreApiOperation(path)) << path;
    }
    auto call = [&](const moonmod::ApiOperationSpec *operation, std::string_view owner, const Json &args) {
        return config.api_handler(owner, *operation, args);
    };
    const moonmod::ChroniclePlayerTuning &tuning = moonmod::ChroniclePlayerTuningGet();
    EXPECT_FLOAT_EQ(tuning.run_speed, 1.0f);
    EXPECT_FALSE(tuning.map_reveal);

    const Json full = Json::array({Json{{"run_speed", 1.24}, {"attack_anim_speed", 1.18}, {"invincible_bonus_frames", 24},
                                        {"recoil_scale", 0.5}, {"gauge_refill", 1.0}, {"thirst_drain", 0.64},
                                        {"durability_wear", 0.64}, {"map_reveal", true}, {"monster_reveal", true}}});
    moonmod::ApiCallResult result = call(set, "test.rpg", full);
    ASSERT_EQ(result.status, ApiCallStatus::Ok) << result.message;
    EXPECT_EQ(result.value["owner"], "test.rpg");
    EXPECT_EQ(result.value["status"], "set");
    EXPECT_NEAR(result.value["effective"]["run_speed"].get<double>(), 1.24, 1e-5);
    EXPECT_FLOAT_EQ(tuning.run_speed, 1.24f);
    EXPECT_FLOAT_EQ(tuning.attack_anim_speed, 1.18f);
    EXPECT_EQ(tuning.invincible_bonus_frames, 24);
    EXPECT_FLOAT_EQ(tuning.recoil_scale, 0.5f);
    EXPECT_FLOAT_EQ(tuning.gauge_refill, 1.0f);
    EXPECT_FLOAT_EQ(tuning.thirst_drain, 0.64f);
    EXPECT_FLOAT_EQ(tuning.durability_wear, 0.64f);
    EXPECT_TRUE(tuning.map_reveal);
    EXPECT_TRUE(tuning.monster_reveal);
    EXPECT_FLOAT_EQ(moonmod::ChroniclePlayerThirstDrainScale(), 0.64f);
    EXPECT_FLOAT_EQ(moonmod::ChroniclePlayerDurabilityWearScale(), 0.64f);
    EXPECT_FLOAT_EQ(moonmod::ChroniclePlayerGaugeRefillScale(), 1.0f);
    EXPECT_FLOAT_EQ(moonmod::ChroniclePlayerAttackAnimSpeed(), 1.18f);
    EXPECT_TRUE(moonmod::ChroniclePlayerMapReveal());
    EXPECT_TRUE(moonmod::ChroniclePlayerMonsterReveal());

    // Ranges and types: multipliers 0.25..4, frames integer 0..120, flags boolean, no unknown fields.
    for (const Json &bad : {Json{{"run_speed", 4.5}}, Json{{"run_speed", 0.1}}, Json{{"thirst_drain", "slow"}},
                            Json{{"invincible_bonus_frames", 121}}, Json{{"invincible_bonus_frames", -1}},
                            Json{{"invincible_bonus_frames", 2.5}}, Json{{"map_reveal", 1}},
                            Json{{"monster_reveal", "yes"}}, Json{{"colour", "red"}}}) {
        const moonmod::ApiCallResult rejected = call(set, "test.rpg", Json::array({bad}));
        EXPECT_EQ(rejected.status, ApiCallStatus::InvalidArgument) << bad.dump();
        EXPECT_FALSE(rejected.message.empty()) << bad.dump();
    }
    EXPECT_EQ(call(set, "test.rpg", Json::array({1})).status, ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call(set, "test.rpg", Json::array()).status, ApiCallStatus::InvalidArgument);
    EXPECT_FLOAT_EQ(tuning.run_speed, 1.24f); // a rejected call leaves the owner's tuning alone

    // An empty table is valid (the mod's support probe) and means retail values for that owner.
    EXPECT_EQ(call(set, "test.other", Json::array({Json::object()})).status, ApiCallStatus::Ok);
    EXPECT_FLOAT_EQ(tuning.run_speed, 1.24f);

    // Owners combine: multipliers multiply, bonus frames add, flags OR.
    ASSERT_EQ(call(set, "test.other", Json::array({Json{{"run_speed", 2.0}, {"invincible_bonus_frames", 10},
                                                         {"monster_reveal", false}}})).status, ApiCallStatus::Ok);
    EXPECT_NEAR(tuning.run_speed, 2.48f, 1e-5f);
    EXPECT_EQ(tuning.invincible_bonus_frames, 34);
    EXPECT_TRUE(tuning.monster_reveal);
    EXPECT_FLOAT_EQ(tuning.recoil_scale, 0.5f);

    // Unloading an owner restores its share; a later set by the same owner replaces, not stacks.
    ASSERT_TRUE(config.api_owner_unloaded);
    config.api_owner_unloaded("test.rpg");
    EXPECT_FLOAT_EQ(tuning.run_speed, 2.0f);
    EXPECT_EQ(tuning.invincible_bonus_frames, 10);
    EXPECT_FALSE(tuning.map_reveal);
    EXPECT_FALSE(tuning.monster_reveal);
    EXPECT_FLOAT_EQ(tuning.recoil_scale, 1.0f);
    ASSERT_EQ(call(set, "test.other", Json::array({Json{{"run_speed", 1.5}}})).status, ApiCallStatus::Ok);
    EXPECT_FLOAT_EQ(tuning.run_speed, 1.5f);
    EXPECT_EQ(tuning.invincible_bonus_frames, 0);

    result = call(clear, "test.other", Json::array());
    ASSERT_EQ(result.status, ApiCallStatus::Ok) << result.message;
    EXPECT_TRUE(result.value["cleared"].get<bool>());
    EXPECT_FLOAT_EQ(tuning.run_speed, 1.0f);
    EXPECT_FALSE(call(clear, "test.other", Json::array()).value["cleared"].get<bool>());
    EXPECT_EQ(call(clear, "test.other", Json::array({1})).status, ApiCallStatus::InvalidArgument);

    // player.invincible: dungeon only, 1..600 frames, never shortens a longer run.
    const s32 previous_mode = mode;
    const int previous_frames = BtActStatus.invincible_frames;
    mode = -1;
    EXPECT_EQ(call(invincible, "test.rpg", Json::array({Json{{"frames", 90}}})).status, ApiCallStatus::Failed);
    mode = GAME_MODE_DUNGEON;
    BtActStatus.invincible_frames = 0;
    result = call(invincible, "test.rpg", Json::array({Json{{"frames", 90}}}));
    ASSERT_EQ(result.status, ApiCallStatus::Ok) << result.message;
    EXPECT_EQ(BtActStatus.invincible_frames, 90);
    EXPECT_EQ(result.value["frames"].get<int>(), 90);
    EXPECT_EQ(result.value["previous"].get<int>(), 0);
    result = call(invincible, "test.rpg", Json::array({Json{{"frames", 30}}}));
    ASSERT_EQ(result.status, ApiCallStatus::Ok) << result.message;
    EXPECT_EQ(BtActStatus.invincible_frames, 90);
    for (const Json &bad : {Json{{"frames", 0}}, Json{{"frames", 601}}, Json{{"frames", "90"}}, Json{{"frames", 1.5}},
                            Json::object(), Json{{"frames", 10}, {"owner", "x"}}}) {
        EXPECT_EQ(call(invincible, "test.rpg", Json::array({bad})).status, ApiCallStatus::InvalidArgument) << bad.dump();
    }
    EXPECT_EQ(call(invincible, "test.rpg", Json::array()).status, ApiCallStatus::InvalidArgument);
    EXPECT_EQ(BtActStatus.invincible_frames, 90);
    mode = previous_mode;
    BtActStatus.invincible_frames = previous_frames;
    moonmod::ResetChroniclePlayerTuningForTests();
}

TEST(MoonModApi, ChroniclePlayerMovesTakeTheComboTriggers) {
    using moonmod::ApiCallStatus;
    using moonmod::Json;
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    moonmod::ConfigureChronicleApi(config);
    auto call = [&](std::string_view path, std::string_view owner, const Json &args) {
        const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation(path);
        EXPECT_NE(operation, nullptr) << path;
        return config.api_handler(owner, *operation, args);
    };
    auto move = [](const char *id, const char *trigger) { return Json{{"id", id}, {"character", "toan"}, {"trigger", trigger}, {"motion", 0x25}}; };
    Json heavy = move("heavy", "heavy");
    Json launcher = move("launcher", "combo_finisher");
    launcher["chain"] = 2;
    Json spin = move("spin", "combo_finisher");
    spin["chain"] = 3;
    Json lunge = move("lunge", "lock_attack");
    lunge["direction"] = "toward";
    Json back = move("back", "lock_attack");
    back["direction"] = "away";
    const Json follow = move("follow", "evade_attack");
    // A finisher per chain and a lock attack per direction sit side by side.
    EXPECT_EQ(call("player.moves_set", "test.a", Json::array({Json{{"moves", {heavy, launcher, spin, lunge, back, follow}}}})).status,
              ApiCallStatus::Ok);
    // Another mod may add a finisher on a chain test.a doesn't use, not one on the same chain.
    Json four = move("four", "combo_finisher");
    four["chain"] = 4;
    EXPECT_EQ(call("player.moves_set", "test.b", Json::array({Json{{"moves", {four}}}})).status, ApiCallStatus::Ok);
    Json clash = move("clash", "combo_finisher");
    clash["chain"] = 2;
    EXPECT_EQ(call("player.moves_set", "test.b", Json::array({Json{{"moves", {clash}}}})).status, ApiCallStatus::Failed);
    Json no_chain = move("x", "combo_finisher");
    Json bad_chain = move("x", "combo_finisher");
    bad_chain["chain"] = 7;
    Json stray_chain = move("x", "heavy");
    stray_chain["chain"] = 2;
    Json no_direction = move("x", "lock_attack");
    Json bad_direction = move("x", "lock_attack");
    bad_direction["direction"] = "sideways";
    Json stray_direction = move("x", "evade_attack");
    stray_direction["direction"] = "toward";
    for (const Json &bad : {no_chain, bad_chain, stray_chain, no_direction, bad_direction, stray_direction, move("x", "uppercut")}) {
        EXPECT_EQ(call("player.moves_set", "test.c", Json::array({Json{{"moves", {bad}}}})).status, ApiCallStatus::InvalidArgument) << bad.dump();
    }
    // Up to 160 moves a mod.
    Json many = Json::array();
    for (int n = 0; n < 161; ++n) many.push_back(move(("m" + std::to_string(n)).c_str(), "run_attack"));
    EXPECT_EQ(call("player.moves_set", "test.c", Json::array({Json{{"moves", many}}})).status, ApiCallStatus::InvalidArgument);
    many.erase(many.size() - 1);
    EXPECT_EQ(call("player.moves_set", "test.c", Json::array({Json{{"moves", many}}})).status, ApiCallStatus::Ok);
    for (const char *owner : {"test.a", "test.b", "test.c"}) EXPECT_EQ(call("player.moves_clear", owner, Json::array()).status, ApiCallStatus::Ok);
}

TEST(MoonModApi, ChronicleTuningForStatusMapSwapAndTheFatalGuard) {
    using moonmod::ApiCallStatus;
    using moonmod::Json;
    moonmod::ResetChroniclePlayerTuningForTests();
    moonmod::ResetChronicleFatalGuardForTests();
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    moonmod::ConfigureChronicleApi(config);
    auto call = [&](std::string_view path, std::string_view owner, const Json &args) {
        const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation(path);
        EXPECT_NE(operation, nullptr) << path;
        return config.api_handler(owner, *operation, args);
    };
    const moonmod::ChroniclePlayerTuning &tuning = moonmod::ChroniclePlayerTuningGet();

    // Blind Halls, Hardy, Clear Mind and Swift Swap's knobs; owners combine (flags and immunities OR).
    ASSERT_EQ(call("player.tuning_set", "test.a", Json::array({Json{{"map_hidden", true}, {"status_duration", 0.5},
                                                                     {"status_immune", {"freeze"}}, {"swap_delay", 0}}})).status,
              ApiCallStatus::Ok);
    ASSERT_EQ(call("player.tuning_set", "test.b", Json::array({Json{{"status_immune", {"stamina"}}, {"status_duration", 0.5}}})).status,
              ApiCallStatus::Ok);
    EXPECT_TRUE(moonmod::ChroniclePlayerMapHidden());
    EXPECT_FLOAT_EQ(moonmod::ChroniclePlayerStatusDuration(), 0.25f);
    EXPECT_EQ(moonmod::ChroniclePlayerStatusImmune(), AILMENT_FREEZE | AILMENT_STAMINA);
    EXPECT_FLOAT_EQ(moonmod::ChroniclePlayerSwapDelay(), 0.0f);
    EXPECT_EQ(call("player.tuning_set", "test.a", Json::array({Json{{"swap_delay", 0.5}}})).value["tuning"]["swap_delay"], 0.5);
    EXPECT_FALSE(tuning.map_hidden); // test.a replaced its set
    for (const Json &bad : {Json{{"status_immune", {"sleep"}}}, Json{{"status_immune", "freeze"}}, Json{{"swap_delay", -1}},
                            Json{{"swap_delay", 5}}, Json{{"status_duration", 0}}, Json{{"map_hidden", "yes"}}}) {
        EXPECT_EQ(call("player.tuning_set", "test.a", Json::array({bad})).status, ApiCallStatus::InvalidArgument) << bad.dump();
    }
    // Pebble Storm / Orb Split, Genie's Daughter, Spear Reach.
    ASSERT_EQ(call("player.tuning_set", "test.c", Json::array({Json{{"extra_shots", 1}, {"shots_pierce", true}, {"reach_scale", 1.2}}})).status,
              ApiCallStatus::Ok);
    EXPECT_EQ(moonmod::ChroniclePlayerExtraShots(), 1);
    EXPECT_TRUE(moonmod::ChroniclePlayerShotsPierce());
    EXPECT_FLOAT_EQ(moonmod::ChroniclePlayerReachScale(), 1.2f);
    for (const Json &bad : {Json{{"extra_shots", 3}}, Json{{"shots_pierce", 1}}, Json{{"reach_scale", 0.1}}}) {
        EXPECT_EQ(call("player.tuning_set", "test.c", Json::array({bad})).status, ApiCallStatus::InvalidArgument) << bad.dump();
    }
    // Moonwalker's dodge: power 0 (off) or 0.25..4, frames 0..60; owners take the strongest.
    ASSERT_EQ(call("player.tuning_set", "test.a", Json::array({Json{{"dodge_power", 1.0}, {"dodge_invincible_frames", 20}}})).status,
              ApiCallStatus::Ok);
    ASSERT_EQ(call("player.tuning_set", "test.b", Json::array({Json{{"dodge_power", 1.8}, {"dodge_hop", true}}})).status, ApiCallStatus::Ok);
    EXPECT_FLOAT_EQ(moonmod::ChroniclePlayerDodgePower(), 1.8f);
    EXPECT_EQ(moonmod::ChroniclePlayerDodgeInvincibleFrames(), 20);
    EXPECT_TRUE(moonmod::ChroniclePlayerDodgeHop());
    EXPECT_EQ(call("player.tuning_set", "test.b", Json::array({Json{{"dodge_power", 0}}})).status, ApiCallStatus::Ok);
    EXPECT_FLOAT_EQ(moonmod::ChroniclePlayerDodgePower(), 1.0f);
    for (const Json &bad : {Json{{"dodge_power", 0.1}}, Json{{"dodge_power", 5}}, Json{{"dodge_invincible_frames", 61}},
                            Json{{"dodge_hop", 1}}}) {
        EXPECT_EQ(call("player.tuning_set", "test.a", Json::array({bad})).status, ApiCallStatus::InvalidArgument) << bad.dump();
    }
    moonmod::ResetChroniclePlayerTuningForTests();

    // The fatal guard: charges per character, spent one at a time.
    ASSERT_EQ(call("player.fatal_guard", "test.a", Json::array({Json{{"charges", 1}}})).status, ApiCallStatus::Ok);
    const auto set = call("player.fatal_guard", "test.a", Json::array({Json{{"charges", 2}, {"character", "xiao"}}}));
    ASSERT_EQ(set.status, ApiCallStatus::Ok);
    EXPECT_EQ(set.value["charges"]["xiao"], 2);
    EXPECT_EQ(set.value["charges"]["toan"], 1);
    EXPECT_TRUE(moonmod::ConsumeChronicleFatalGuard(1));
    EXPECT_TRUE(moonmod::ConsumeChronicleFatalGuard(1));
    EXPECT_FALSE(moonmod::ConsumeChronicleFatalGuard(1));
    EXPECT_TRUE(moonmod::ConsumeChronicleFatalGuard(0));
    EXPECT_FALSE(moonmod::ConsumeChronicleFatalGuard(0));
    EXPECT_FALSE(moonmod::ConsumeChronicleFatalGuard(7));
    for (const Json &bad : {Json{{"charges", 100}}, Json{{"charges", -1}}, Json{{"character", "max"}, {"charges", 1}}, Json{{"lives", 1}}}) {
        EXPECT_EQ(call("player.fatal_guard", "test.a", Json::array({bad})).status, ApiCallStatus::InvalidArgument) << bad.dump();
    }
    moonmod::ResetChronicleFatalGuardForTests();

    // Barren: chests alone may go below the retail count.
    EXPECT_EQ(call("dungeons.set_modifiers", "test.a", Json::array({Json{{"chests", -100}}})).status, ApiCallStatus::Ok);
    EXPECT_EQ(DngChestBonusPercent, -100);
    EXPECT_EQ(call("dungeons.set_modifiers", "test.a", Json::array({Json{{"chests", -101}}})).status, ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call("dungeons.set_modifiers", "test.a", Json::array({Json{{"monsters", -10}}})).status, ApiCallStatus::InvalidArgument);
    EXPECT_EQ(call("dungeons.set_modifiers", "test.a", Json::array({Json::object()})).status, ApiCallStatus::Ok);
    EXPECT_EQ(DngChestBonusPercent, 0);
}

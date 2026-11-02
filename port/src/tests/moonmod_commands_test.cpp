#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "moonmod/manager_ui.hpp"



namespace {

namespace fs = std::filesystem;

struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-commands-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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

moonmod::RuntimeConfig Config(const fs::path &root) {
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.mod_directories = {root / "mods"};
    config.save_root = root / "save";
    return config;
}

} // namespace

TEST(MoonModCommands, ExecutesOwnerScopedCallbacksFromApiAndManagerAndCleansUp) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    TempRoot root;
    const fs::path package = root.path / "mods" / "test.commands";
    Write(package / "mod.json", R"json({
      "schema_version":1,"id":"test.commands","name":"Commands Probe","version":"1.0.0",
      "framework_api":1,"games":["chronicle","chronicle-two"],"entry":"main.lua",
      "permissions":["content.register","game.read","game.write"]
    })json");
    Write(package / "main.lua", R"lua(
local calls = 0
local command = assert(game.commands.register({id = "sum", label = "Add numbers", description = "Probe command dispatch"}, function(args)
  calls = calls + 1
  local total = (args.left or 0) + (args.right or 0)
  moonmod.log("info", "COMMAND_CALLBACK:" .. calls .. ":" .. total)
  return {calls = calls, total = total}
end))
assert(command.id == "test.commands:sum")
assert(command.action_id == "test.commands:cmd.sum")
local duplicate, duplicate_error = game.commands.register({id = "sum", label = "Duplicate"}, function() end)
assert(duplicate == nil and duplicate_error.code == "invalid_argument")
local malformed, malformed_error = game.commands.register({id = "bad", label = "Bad", extra = true}, function() end)
assert(malformed == nil and malformed_error.code == "invalid_argument")
local result = assert(game.commands.execute({id = "sum", arguments = {left = 3, right = 4}}))
assert(result.calls == 1 and result.total == 7)
local foreign, foreign_error = game.commands.execute({id = "another.mod:sum"})
assert(foreign == nil and foreign_error.code == "invalid_argument")
moonmod.on("tests.unregister", function()
  assert(game.commands.unregister({id = "sum"}).removed)
  local missing, missing_error = game.commands.execute({id = "sum"})
  assert(missing == nil and missing_error.code == "failed")
  local repeated, repeated_error = game.commands.unregister({id = "sum"})
  assert(repeated == nil and repeated_error.code == "failed")
  moonmod.log("info", "COMMAND_UNREGISTERED")
end)
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.commands":true},"settings":{}})json");

    std::vector<std::string> logs;
    moonmod::RuntimeConfig config = Config(root.path);
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(config, error)) << error;
    ASSERT_EQ(moonmod::SharedRuntime().Mods().size(), 1u);
    if (moonmod::SharedRuntime().Mods()[0].state == moonmod::ModState::Disabled) {
        ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.commands", true, error)) << error;
    }
    ASSERT_EQ(moonmod::SharedRuntime().Mods()[0].state, moonmod::ModState::Loaded);
    ASSERT_EQ(moonmod::ManagerActions("test.commands").size(), 1u);
    EXPECT_EQ(moonmod::ManagerActions("test.commands")[0].id, "test.commands:cmd.sum");
    EXPECT_TRUE(moonmod::ActivateManagerAction("test.commands", "cmd.sum"));
    EXPECT_TRUE(std::ranges::any_of(logs, [](const std::string &message) {
        return message.find("COMMAND_CALLBACK:1:7") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(logs, [](const std::string &message) {
        return message.find("COMMAND_CALLBACK:2:0") != std::string::npos;
    }));

    ASSERT_TRUE(moonmod::SharedRuntime().PublishToMod("test.commands", "tests.unregister", moonmod::Json::object()));
    EXPECT_TRUE(std::ranges::any_of(logs, [](const std::string &message) {
        return message.find("COMMAND_UNREGISTERED") != std::string::npos;
    }));
    EXPECT_TRUE(moonmod::ManagerActions("test.commands").empty());
    EXPECT_FALSE(moonmod::ActivateManagerAction("test.commands", "cmd.sum"));

    moonmod::SharedRuntime().Stop();
    EXPECT_TRUE(moonmod::ManagerActions("test.commands").empty());
    ASSERT_TRUE(moonmod::SharedRuntime().Start(config, error)) << error;
    EXPECT_EQ(moonmod::ManagerActions("test.commands").size(), 1u);
    moonmod::SharedRuntime().Stop();
}

TEST(MoonModCommands, RequiresManifestPermissionsForRegistrationAndExecution) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    TempRoot root;
    const fs::path package = root.path / "mods" / "test.commands-permission";
    Write(package / "mod.json", R"json({
      "schema_version":1,"id":"test.commands-permission","name":"Commands Permission Probe","version":"1.0.0",
      "framework_api":1,"games":["chronicle","chronicle-two"],"entry":"main.lua","permissions":[]
    })json");
    Write(package / "main.lua", R"lua(
local command, register_error = game.commands.register({id = "blocked", label = "Blocked"}, function() end)
assert(command == nil and register_error.code == "permission_denied")
local result, execute_error = game.commands.execute({id = "blocked"})
assert(result == nil and execute_error.code == "permission_denied")
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.commands-permission":true},"settings":{}})json");

    moonmod::RuntimeConfig config = Config(root.path);
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(config, error)) << error;
    ASSERT_EQ(moonmod::SharedRuntime().Mods().size(), 1u);
    if (moonmod::SharedRuntime().Mods()[0].state == moonmod::ModState::Disabled) {
        ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.commands-permission", true, error)) << error;
    }
    EXPECT_EQ(moonmod::SharedRuntime().Mods()[0].state, moonmod::ModState::Loaded);
    EXPECT_TRUE(moonmod::ManagerActions("test.commands-permission").empty());
    moonmod::SharedRuntime().Stop();
}

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <ranges>
#include <string>
#include <vector>

#include "moonmod/api.hpp"
#include "moonmod/runtime.hpp"

// mods.*: the loaded mods with their co-op roles, another mod's settings, and session-only setting values (co-op's,
// the host's) that are never saved and end with the session.

namespace {

namespace fs = std::filesystem;

struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-mods-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

void Write(const fs::path &path, std::string_view content) {
    fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << content;
}

std::string Manifest(std::string_view id, std::string_view permissions, std::string_view extra) {
    return "{\"schema_version\":1,\"id\":\"" + std::string(id) + "\",\"name\":\"" + std::string(id) +
           "\",\"version\":\"1.2.3\",\"framework_api\":1,\"games\":[\"chronicle\"],\"entry\":\"main.lua\","
           "\"permissions\":[" + std::string(permissions) + "]" + std::string(extra) + "}";
}

bool Logged(const std::vector<std::string> &logs, std::string_view line) {
    return std::ranges::find(logs, std::string(line)) != logs.end();
}

std::string Joined(const std::vector<std::string> &logs) {
    std::string all;
    for (const auto &line : logs) all += line + "\n";
    return all;
}

} // namespace

TEST(MoonModModsApi, ListsSettingsAndOverlaysThatEndWithTheSession) {
    TempRoot root;
    const fs::path mods = root.path / "mods";
    Write(mods / "test.host" / "mod.json", Manifest("test.host", "\"game.read\",\"mods.settings\"", ""));
    Write(mods / "test.host" / "main.lua", R"lua(
local list = game.mods.list()
local rows = {}
for _, m in ipairs(list.mods) do rows[#rows + 1] = m.id .. "=" .. m.coop .. "@" .. m.version end
table.sort(rows)
moonmod.log("info", "LIST " .. table.concat(rows, ","))
local s = game.mods.settings_get({id = "test.target"})
moonmod.log("info", "GET " .. tostring(s.values.level) .. " " .. tostring(s.values.fast) .. " " .. tostring(s.overlaid))
local r = game.mods.settings_overlay({id = "test.target", values = {level = 7, fast = true, bogus = 1, mode = "nope"}})
moonmod.log("info", "OVERLAY " .. tostring(r.applied) .. " " .. tostring(#r.refused))
s = game.mods.settings_get({id = "test.target"})
moonmod.log("info", "AFTER " .. tostring(s.values.level) .. " " .. tostring(s.values.fast) .. " " .. tostring(s.overlaid))
local own = game.mods.settings_get()
moonmod.log("info", "OWN " .. tostring(own.id))
)lua");
    Write(mods / "test.target" / "mod.json",
          Manifest("test.target", "", ",\"coop\":\"host_only\",\"settings\":["
                   "{\"id\":\"level\",\"name\":\"Level\",\"type\":\"integer\",\"default\":3,\"min\":1,\"max\":10},"
                   "{\"id\":\"fast\",\"name\":\"Fast\",\"type\":\"boolean\",\"default\":false},"
                   "{\"id\":\"mode\",\"name\":\"Mode\",\"type\":\"choice\",\"default\":\"a\",\"choices\":[\"a\",\"b\"]}]"));
    Write(mods / "test.target" / "main.lua", R"lua(
moonmod.on("settings.changed", function(e) moonmod.log("info", "CHANGED " .. e.id .. "=" .. tostring(e.value)) end)
moonmod.on("game.tick", function() moonmod.log("info", "SEES " .. tostring(moonmod.settings.get("level"))) end)
)lua");
    Write(mods / "test.plain" / "mod.json", Manifest("test.plain", "\"game.read\"", ",\"coop\":\"local\""));
    Write(mods / "test.plain" / "main.lua", R"lua(
local v, err = game.mods.settings_get({id = "test.target"})
moonmod.log("info", "PLAIN GET " .. tostring(v) .. " " .. tostring(err and err.code))
v, err = game.mods.settings_overlay({id = "test.plain", values = {}})
moonmod.log("info", "PLAIN OVERLAY " .. tostring(v) .. " " .. tostring(err and err.code))
)lua");
    Write(mods / "test.badrole" / "mod.json", Manifest("test.badrole", "", ",\"coop\":\"sometimes\""));
    Write(mods / "test.badrole" / "main.lua", "");
    const fs::path profile = root.path / "save" / "moonmod" / "profile.json";
    Write(profile, "{\"format_version\":1,\"enabled\":{\"test.host\":true,\"test.target\":true,\"test.plain\":true,"
                   "\"test.badrole\":true},\"settings\":{\"test.target\":{\"level\":4}}}");

    std::vector<std::string> logs;
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.mod_directories = {mods};
    config.save_root = root.path / "save";
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };
    moonmod::Runtime runtime;
    std::string error;
    ASSERT_TRUE(runtime.Start(config, error)) << error;

    const auto bad = std::ranges::find(runtime.Mods(), "test.badrole", &moonmod::ModRecord::id);
    ASSERT_NE(bad, runtime.Mods().end());
    EXPECT_FALSE(bad->valid_manifest) << "an unknown co-op role is refused";

    // The host's view. Load order puts the host first, so the target isn't loaded yet when it lists: run again below.
    EXPECT_TRUE(Logged(logs, "GET 4 false false")) << Joined(logs);
    EXPECT_TRUE(Logged(logs, "OVERLAY 2 2")) << Joined(logs);
    EXPECT_TRUE(Logged(logs, "AFTER 7 true true")) << Joined(logs);
    EXPECT_TRUE(Logged(logs, "OWN test.host")) << Joined(logs);
    EXPECT_TRUE(Logged(logs, "PLAIN GET nil permission_denied")) << Joined(logs);
    EXPECT_TRUE(Logged(logs, "PLAIN OVERLAY nil permission_denied")) << Joined(logs);

    const moonmod::Json listed = runtime.ModList();
    std::vector<std::string> rows;
    for (const auto &m : listed) rows.push_back(m.at("id").get<std::string>() + "=" + m.at("coop").get<std::string>());
    std::ranges::sort(rows);
    EXPECT_EQ(rows, (std::vector<std::string>{"test.host=required", "test.plain=local", "test.target=host_only"}));

    // The target sees the overlay, told once per changed setting.
    logs.clear();
    runtime.Pump();
    EXPECT_TRUE(Logged(logs, "CHANGED level=7")) << Joined(logs);
    EXPECT_TRUE(Logged(logs, "CHANGED fast=true")) << Joined(logs);
    EXPECT_TRUE(Logged(logs, "SEES 7")) << Joined(logs);
    EXPECT_EQ(runtime.Setting("test.target", "level"), 7);

    // Never saved.
    {
        std::ifstream kept(profile);
        const std::string text((std::istreambuf_iterator<char>(kept)), std::istreambuf_iterator<char>());
        EXPECT_EQ(text.find("7"), std::string::npos) << text;
    }

    // The session ends: the overlay goes, and the target is told its own values again.
    runtime.Publish("game.session.ended", {{"kind", "visit"}});
    logs.clear();
    runtime.Pump();
    EXPECT_TRUE(Logged(logs, "CHANGED level=4")) << Joined(logs);
    EXPECT_TRUE(Logged(logs, "CHANGED fast=false")) << Joined(logs);
    EXPECT_TRUE(Logged(logs, "SEES 4")) << Joined(logs);
    EXPECT_FALSE(runtime.ModSettings("test.target").value.at("overlaid").get<bool>());

    // An overlay of one mod is cleared by name; values the setting refuses are refused.
    const auto applied = runtime.OverlayModSettings("test.target", {{"level", 11}, {"mode", "b"}});
    ASSERT_EQ(applied.status, moonmod::ApiCallStatus::Ok) << applied.message;
    EXPECT_EQ(applied.value.at("applied"), 1);
    EXPECT_EQ(runtime.Setting("test.target", "mode"), "b");
    EXPECT_EQ(runtime.Setting("test.target", "level"), 4);
    runtime.ClearModSettingsOverlay("test.target");
    EXPECT_EQ(runtime.Setting("test.target", "mode"), "a");
    EXPECT_EQ(runtime.OverlayModSettings("test.nobody", {{"x", 1}}).status, moonmod::ApiCallStatus::Failed);
    runtime.Stop();
}

TEST(MoonModModsApi, CatalogHasTheOperationsAsCoreApi) {
    for (const char *path : {"mods.list", "mods.settings_get", "mods.settings_overlay", "mods.settings_overlay_clear"}) {
        const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation(path);
        ASSERT_NE(operation, nullptr) << path;
        EXPECT_TRUE(moonmod::IsCoreApiOperation(path)) << path;
    }
    EXPECT_EQ(moonmod::FindApiOperation("mods.settings_overlay")->permission, "mods.settings");
}

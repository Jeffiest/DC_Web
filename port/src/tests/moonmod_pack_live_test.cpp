// Merged packs (manifest "parts": several mods in one script, each still its own owner) and live mod on/off.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>
#include <vector>

#include "moonmod/manager_ui.hpp"
#include "moonmod/runtime.hpp"

namespace {

namespace fs = std::filesystem;

struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-pack-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempRoot() {
        moonmod::SharedRuntime().Stop();
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

std::string Read(const fs::path &path) {
    std::ifstream input(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}

struct Probe {
    std::vector<std::string> logs;
    bool Saw(std::string_view text) const {
        return std::ranges::any_of(logs, [&](const std::string &line) { return line.find(text) != std::string::npos; });
    }
    int Count(std::string_view text) const {
        return static_cast<int>(std::ranges::count_if(logs, [&](const std::string &line) {
            return line.find(text) != std::string::npos;
        }));
    }
};

moonmod::RuntimeConfig Config(const fs::path &root, Probe &probe) {
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.mod_directories = {root / "mods"};
    config.save_root = root / "save";
    config.log = [&probe](std::string_view, std::string_view mod_id, std::string_view message) {
        probe.logs.push_back(std::string(mod_id) + "|" + std::string(message));
    };
    return config;
}

const moonmod::ModRecord *Record(std::string_view id) {
    for (const moonmod::ModRecord &record : moonmod::SharedRuntime().Mods()) {
        if (record.id == id) return &record;
    }
    return nullptr;
}

// A pack of two parts, test.alpha and test.beta, written the way tools/pack/build_pack.py writes one.
void WritePack(const fs::path &mods) {
    Write(mods / "test.pack" / "mod.json", R"json({
      "schema_version":1,"id":"test.pack","name":"Test Pack","version":"1.0.0","coop":"local",
      "framework_api":1,"games":["chronicle"],"entry":"main.lua",
      "permissions":["game.read","game.write","content.register","storage.read","storage.write"],
      "parts":[
        {"id":"test.alpha","name":"Alpha","version":"2.0.0","coop":"host_only",
         "settings":[{"id":"power","name":"Power","type":"integer","default":3,"min":0,"max":9}]},
        {"id":"test.beta","name":"Beta","version":"3.1.0","dependencies":["test.alpha"],
         "settings":[{"id":"power","name":"Power","type":"integer","default":7,"min":0,"max":9}]}
      ]
    })json");
    Write(mods / "test.pack" / "main.lua", R"lua(
-- moonmod:part test.alpha
moonmod.part.run("test.alpha", function()
  moonmod.log("info", "ALPHA_LOADED power=" .. tostring(moonmod.settings.get("power")))
  moonmod.storage.set("who", "alpha")
  moonmod.on("probe.ping", function() moonmod.log("info", "ALPHA_PING") end)
  moonmod.on("settings.changed", function(e) moonmod.log("info", "ALPHA_SETTING " .. e.id .. "=" .. tostring(e.value)) end)
  moonmod.on("runtime.loaded", function(e) moonmod.log("info", "ALPHA_RUNTIME_LOADED live=" .. tostring(e.live)) end)
  moonmod.on("runtime.unloaded", function() moonmod.log("info", "ALPHA_UNLOADED") end)
  assert(game.scripts.register_function({id = "double", description = "x2"}, function(a) return {value = a.value * 2} end))
end)
-- moonmod:end
-- moonmod:part test.beta
moonmod.part.run("test.beta", function()
  moonmod.log("info", "BETA_LOADED power=" .. tostring(moonmod.settings.get("power")))
  moonmod.storage.set("who", "beta")
  moonmod.on("probe.ping", function() moonmod.log("info", "BETA_PING") end)
  moonmod.on("runtime.unloaded", function() moonmod.log("info", "BETA_UNLOADED") end)
  local doubled = assert(game.scripts.call_function({id = "test.alpha:double", arguments = {value = 21}}))
  moonmod.log("info", "BETA_CALLED " .. tostring(doubled.value))
  return {on_tick = function() end}
end)
-- moonmod:end
moonmod.log("info", "PACK_LOADED")
)lua");
}

void WriteMod(const fs::path &mods, std::string_view id, std::string_view script, std::string_view extra = "") {
    Write(mods / std::string(id) / "mod.json",
          std::string(R"json({"schema_version":1,"id":")json") + std::string(id) +
              R"json(","name":"Probe","version":"1.0.0","framework_api":1,"games":["chronicle"],"entry":"main.lua",)json"
              R"json("permissions":["game.read","content.register"])json" +
              std::string(extra) + "}");
    Write(mods / std::string(id) / "main.lua", script);
}

} // namespace

TEST(MoonModPack, PartsRunInOneScriptEachAsItsOwnOwner) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    WritePack(root.path / "mods");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.pack":true},"settings":{"test.alpha":{"power":5}}})json");
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;

    // the parts are records of their own, listed right under the pack
    const auto mods = moonmod::SharedRuntime().Mods();
    ASSERT_EQ(mods.size(), 3u);
    EXPECT_EQ(mods[0].id, "test.pack");
    EXPECT_EQ(mods[1].id, "test.alpha");
    EXPECT_EQ(mods[2].id, "test.beta");
    EXPECT_EQ(mods[1].pack, "test.pack");
    EXPECT_EQ(Record("test.pack")->state, moonmod::ModState::Loaded);
    EXPECT_EQ(Record("test.alpha")->state, moonmod::ModState::Loaded);
    EXPECT_EQ(Record("test.beta")->state, moonmod::ModState::Loaded);

    // each part reads its own settings, logs under its own id, and calls another part's functions
    EXPECT_TRUE(probe.Saw("test.alpha|ALPHA_LOADED power=5"));
    EXPECT_TRUE(probe.Saw("test.beta|BETA_LOADED power=7"));
    EXPECT_TRUE(probe.Saw("test.beta|BETA_CALLED 42"));
    EXPECT_TRUE(probe.Saw("test.pack|PACK_LOADED"));
    EXPECT_TRUE(probe.Saw("ALPHA_RUNTIME_LOADED live=nil"));

    // each part's saved data is in its own folder, as when it was a mod of its own
    moonmod::SharedRuntime().Stop();
    EXPECT_NE(Read(root.path / "save" / "moonmod" / "data" / "test.alpha" / "storage.json").find("alpha"), std::string::npos);
    EXPECT_NE(Read(root.path / "save" / "moonmod" / "data" / "test.beta" / "storage.json").find("beta"), std::string::npos);
    EXPECT_TRUE(probe.Saw("ALPHA_UNLOADED"));
    EXPECT_TRUE(probe.Saw("BETA_UNLOADED"));
}

TEST(MoonModPack, EventsSettingsAndModListReachThePartThemselves) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    WritePack(root.path / "mods");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.pack":true},"settings":{}})json");
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;

    ASSERT_TRUE(moonmod::SharedRuntime().PublishToMod("test.beta", "probe.ping", moonmod::Json::object()));
    EXPECT_TRUE(probe.Saw("BETA_PING"));
    EXPECT_FALSE(probe.Saw("ALPHA_PING"));
    moonmod::SharedRuntime().Publish("probe.ping", moonmod::Json::object());
    EXPECT_TRUE(probe.Saw("ALPHA_PING"));

    ASSERT_TRUE(moonmod::SharedRuntime().StepSetting("test.alpha", "power", 1, error)) << error;
    EXPECT_TRUE(probe.Saw("test.alpha|ALPHA_SETTING power=4"));
    EXPECT_EQ(moonmod::SharedRuntime().Setting("test.beta", "power"), 7);

    // co-op compares the parts, each with its own version and role; the pack itself is local
    const moonmod::Json list = moonmod::SharedRuntime().ModList();
    ASSERT_EQ(list.size(), 3u);
    EXPECT_EQ(list[0]["id"], "test.pack");
    EXPECT_EQ(list[0]["coop"], "local");
    EXPECT_EQ(list[1]["id"], "test.alpha");
    EXPECT_EQ(list[1]["version"], "2.0.0");
    EXPECT_EQ(list[1]["coop"], "host_only");
    EXPECT_EQ(list[2]["id"], "test.beta");
    EXPECT_EQ(list[2]["version"], "3.1.0");
    const moonmod::ApiCallResult settings = moonmod::SharedRuntime().ModSettings("test.alpha");
    ASSERT_EQ(settings.status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(settings.value["values"]["power"], 4);
}

TEST(MoonModPack, PartTurnsOffAndOnWhileThePackRuns) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    WritePack(root.path / "mods");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.pack":true},"parts":{"test.pack":{"test.beta":false}},"settings":{}})json");
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;
    EXPECT_EQ(Record("test.beta")->state, moonmod::ModState::Disabled);
    EXPECT_FALSE(probe.Saw("BETA_LOADED"));

    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.beta", true, error)) << error;
    EXPECT_FALSE(Record("test.beta")->restart_required) << Record("test.beta")->restart_reason;
    EXPECT_EQ(Record("test.beta")->state, moonmod::ModState::Loaded);
    EXPECT_TRUE(probe.Saw("BETA_LOADED power=7"));

    // Alpha can't go while Beta, which needs it, runs
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.alpha", false, error)) << error;
    EXPECT_TRUE(Record("test.alpha")->restart_required);
    EXPECT_NE(Record("test.alpha")->restart_reason.find("Beta"), std::string::npos);
    EXPECT_EQ(Record("test.alpha")->state, moonmod::ModState::Loaded);
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.alpha", true, error)) << error;

    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.beta", false, error)) << error;
    EXPECT_FALSE(Record("test.beta")->restart_required);
    EXPECT_EQ(Record("test.beta")->state, moonmod::ModState::Disabled);
    EXPECT_TRUE(probe.Saw("BETA_UNLOADED"));
    probe.logs.clear();
    moonmod::SharedRuntime().Publish("probe.ping", moonmod::Json::object());
    EXPECT_TRUE(probe.Saw("ALPHA_PING"));
    EXPECT_FALSE(probe.Saw("BETA_PING"));
    EXPECT_FALSE(moonmod::SharedRuntime().PublishToMod("test.beta", "probe.ping", moonmod::Json::object()));
}

TEST(MoonModPack, PackStandsInForPackagesOfTheSameIdAndGoesOnAsAUnit) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    WritePack(root.path / "mods");
    WriteMod(root.path / "mods", "test.alpha", "moonmod.log('info', 'STANDALONE_ALPHA')\n");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.alpha":true},"settings":{}})json");
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;
    EXPECT_TRUE(probe.Saw("STANDALONE_ALPHA"));
    EXPECT_TRUE(Record("test.alpha")->pack.empty());
    ASSERT_NE(Record("test.beta"), nullptr) << "a part with no package of its own shows under its pack, off";
    EXPECT_EQ(Record("test.beta")->state, moonmod::ModState::Disabled);

    // on: the standalone Alpha goes, the pack's Alpha and Beta come, at once
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.pack", true, error)) << error;
    EXPECT_FALSE(Record("test.pack")->restart_required) << Record("test.pack")->restart_reason;
    EXPECT_EQ(Record("test.pack")->state, moonmod::ModState::Loaded);
    EXPECT_EQ(Record("test.alpha")->pack, "test.pack");
    EXPECT_EQ(Record("test.alpha")->state, moonmod::ModState::Loaded);
    EXPECT_EQ(Record("test.beta")->state, moonmod::ModState::Loaded);
    EXPECT_TRUE(probe.Saw("ALPHA_RUNTIME_LOADED live=true"));
    EXPECT_EQ(probe.Count("STANDALONE_ALPHA"), 1);

    // off: the parts go with it, and the separate Alpha, still switched on, is itself again at once
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.pack", false, error)) << error;
    EXPECT_FALSE(Record("test.pack")->restart_required) << Record("test.pack")->restart_reason;
    EXPECT_EQ(Record("test.pack")->state, moonmod::ModState::Disabled);
    EXPECT_TRUE(Record("test.alpha")->pack.empty());
    EXPECT_EQ(Record("test.alpha")->state, moonmod::ModState::Loaded);
    EXPECT_EQ(probe.Count("STANDALONE_ALPHA"), 2);
    EXPECT_TRUE(probe.Saw("BETA_UNLOADED"));

    // the pack's switch never wrote the separate Alpha's choice
    const moonmod::Json profile = moonmod::Json::parse(Read(root.path / "save" / "moonmod" / "profile.json"));
    EXPECT_EQ(profile["enabled"]["test.alpha"], true);
    EXPECT_FALSE(profile["enabled"].contains("test.beta"));
}

TEST(MoonModPack, PartChoicesOutliveThePacksSwitch) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    WritePack(root.path / "mods");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.pack":true},"settings":{}})json");
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;
    ASSERT_EQ(Record("test.beta")->state, moonmod::ModState::Loaded) << "parts start on";

    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.beta", false, error)) << error;
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.pack", false, error)) << error;
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.pack", true, error)) << error;
    EXPECT_EQ(Record("test.alpha")->state, moonmod::ModState::Loaded);
    EXPECT_EQ(Record("test.beta")->state, moonmod::ModState::Disabled) << "Beta stays off as it was left";
    EXPECT_FALSE(Record("test.beta")->restart_required);

    // and after a restart
    moonmod::SharedRuntime().Stop();
    probe.logs.clear();
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;
    EXPECT_EQ(Record("test.alpha")->state, moonmod::ModState::Loaded);
    EXPECT_EQ(Record("test.beta")->state, moonmod::ModState::Disabled);
    EXPECT_FALSE(probe.Saw("BETA_LOADED"));
    const moonmod::Json profile = moonmod::Json::parse(Read(root.path / "save" / "moonmod" / "profile.json"));
    EXPECT_EQ(profile["parts"]["test.pack"]["test.beta"], false);
}

// A part handed over as its script's source is a chunk of its own: its own globals, a main chunk's room for locals,
// and errors that name its own file and line.
TEST(MoonModPack, PartGivenAsSourceIsAChunkOfItsOwn) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    const fs::path mods = root.path / "mods";
    Write(mods / "test.src" / "mod.json", R"json({
      "schema_version":1,"id":"test.src","name":"Source Pack","version":"1.0.0","coop":"local",
      "framework_api":1,"games":["chronicle"],"entry":"main.lua","permissions":["game.read"],
      "parts":[{"id":"test.many","name":"Many","version":"1.0.0"},{"id":"test.broken","name":"Broken","version":"1.0.0"},
               {"id":"test.typo","name":"Typo","version":"1.0.0"}]
    })json");
    std::string many; // 199 locals: as much as a mod's own main chunk may have
    for (int i = 0; i < 199; ++i) many += "local v" + std::to_string(i) + " = " + std::to_string(i) + "\n";
    many += "shared = v198\nmoonmod.log('info', 'MANY ' .. tostring(v198))\n";
    Write(mods / "test.src" / "main.lua",
          "local function env() return setmetatable({}, {__index = _ENV}) end\n"
          "moonmod.part.run('test.many', [[\n" + many + "]], env(), 'test.many/main.lua')\n"
          "moonmod.part.run('test.broken', [[\nlocal x = 1\nerror('BROKEN_ON_PURPOSE')\n]], env(), 'test.broken/main.lua')\n"
          "moonmod.part.run('test.typo', [[\nlocal = 1\n]], env(), 'test.typo/main.lua')\n"
          "moonmod.log('info', 'SHARED=' .. tostring(shared))\n");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.src":true},"settings":{}})json");
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;
    EXPECT_EQ(Record("test.src")->state, moonmod::ModState::Loaded) << Record("test.src")->status_message;
    EXPECT_EQ(Record("test.many")->state, moonmod::ModState::Loaded) << Record("test.many")->status_message;
    EXPECT_TRUE(probe.Saw("test.many|MANY 198"));
    EXPECT_TRUE(probe.Saw("test.src|SHARED=nil")) << "a part's globals stay its own";

    EXPECT_EQ(Record("test.broken")->state, moonmod::ModState::LoadError);
    EXPECT_NE(Record("test.broken")->status_message.find("test.broken/main.lua:2:"), std::string::npos)
        << Record("test.broken")->status_message;
    EXPECT_EQ(Record("test.typo")->state, moonmod::ModState::LoadError);
    EXPECT_NE(Record("test.typo")->status_message.find("test.typo/main.lua:1:"), std::string::npos)
        << Record("test.typo")->status_message;

    // a part that failed is tried again when switched off and on
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.broken", false, error)) << error;
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.broken", true, error)) << error;
    EXPECT_EQ(Record("test.broken")->state, moonmod::ModState::LoadError);
}

TEST(MoonModLive, ModTurnsOnAndOffWithoutARestart) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    WriteMod(root.path / "mods", "test.live", R"lua(
moonmod.log("info", "LIVE_LOADED")
moonmod.on("runtime.loaded", function(e) moonmod.log("info", "LIVE_RUNTIME live=" .. tostring(e.live)) end)
moonmod.on("runtime.unloaded", function() moonmod.log("info", "LIVE_UNLOADED") end)
moonmod.on("probe.ping", function() moonmod.log("info", "LIVE_PING") end)
)lua");
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;
    EXPECT_EQ(Record("test.live")->state, moonmod::ModState::Disabled);

    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.live", true, error)) << error;
    EXPECT_FALSE(Record("test.live")->restart_required);
    EXPECT_TRUE(Record("test.live")->enabled);
    EXPECT_EQ(Record("test.live")->state, moonmod::ModState::Loaded);
    EXPECT_TRUE(probe.Saw("LIVE_RUNTIME live=true"));
    moonmod::SharedRuntime().Publish("probe.ping", moonmod::Json::object());
    EXPECT_TRUE(probe.Saw("LIVE_PING"));

    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.live", false, error)) << error;
    EXPECT_FALSE(Record("test.live")->restart_required);
    EXPECT_FALSE(Record("test.live")->enabled);
    EXPECT_EQ(Record("test.live")->state, moonmod::ModState::Disabled);
    EXPECT_TRUE(probe.Saw("LIVE_UNLOADED"));
    probe.logs.clear();
    moonmod::SharedRuntime().Publish("probe.ping", moonmod::Json::object());
    EXPECT_FALSE(probe.Saw("LIVE_PING"));
}

TEST(MoonModLive, ContentTheGameKeepsWaitsForARestart) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    WriteMod(root.path / "mods", "test.sticky", R"lua(
assert(game.items.register({id = "probe", name = "Probe"}))
moonmod.log("info", "STICKY_LOADED")
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.sticky":true},"settings":{}})json");
    moonmod::RuntimeConfig config = Config(root.path, probe);
    config.api_capabilities.push_back({"items.register", true, {}});
    config.api_handler = [](std::string_view, const moonmod::ApiOperationSpec &, const moonmod::Json &) {
        return moonmod::ApiCallResult{moonmod::ApiCallStatus::Ok, {{"id", "test.sticky:probe"}}, {}};
    };
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(config, error)) << error;
    ASSERT_TRUE(probe.Saw("STICKY_LOADED"));

    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.sticky", false, error)) << error;
    EXPECT_TRUE(Record("test.sticky")->restart_required);
    EXPECT_FALSE(Record("test.sticky")->enabled_next_launch);
    EXPECT_EQ(Record("test.sticky")->state, moonmod::ModState::Loaded);
    EXPECT_NE(Record("test.sticky")->restart_reason.find("added items, which the game keeps"), std::string::npos);

    // turning it back on cancels the pending change
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.sticky", true, error)) << error;
    EXPECT_FALSE(Record("test.sticky")->restart_required);

    // a mod whose script registers such content starts mid-game only from the title screen
    WriteMod(root.path / "mods", "test.later", "game.items.register({id = 'x', name = 'X'})\nmoonmod.log('info', 'LATER_LOADED')\n");
    moonmod::SharedRuntime().Stop();
    ASSERT_TRUE(moonmod::SharedRuntime().Start(config, error)) << error;
    moonmod::SharedRuntime().Publish("game.session.started", moonmod::Json::object());
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.later", true, error)) << error;
    EXPECT_TRUE(Record("test.later")->restart_required);
    EXPECT_NE(Record("test.later")->restart_reason.find("title screen"), std::string::npos);
    EXPECT_FALSE(probe.Saw("LATER_LOADED"));
    moonmod::SharedRuntime().Publish("game.session.ended", moonmod::Json::object());
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.later", true, error)) << error;
    EXPECT_FALSE(Record("test.later")->restart_required);
    EXPECT_TRUE(probe.Saw("LATER_LOADED"));
}

TEST(MoonModLive, ChangedFilesReloadThePackage) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    WriteMod(root.path / "mods", "test.edit", "moonmod.log('info', 'EDIT_V1')\n");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.edit":true},"settings":{}})json");
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;
    ASSERT_TRUE(probe.Saw("EDIT_V1"));

    const fs::path script = root.path / "mods" / "test.edit" / "main.lua";
    Write(script, "moonmod.log('info', 'EDIT_V2')\n");
    fs::last_write_time(script, fs::last_write_time(script) + std::chrono::seconds(5));
    for (int frame = 0; frame < 120; ++frame) moonmod::SharedRuntime().Pump();
    EXPECT_TRUE(probe.Saw("EDIT_V2"));
    EXPECT_TRUE(probe.Saw("reloaded after its files changed"));
    EXPECT_EQ(Record("test.edit")->state, moonmod::ModState::Loaded);

    // saved half-way: the new files don't load, and the mod comes back once they're fixed
    Write(script, "moonmod.log('info', 'EDIT_V3'\n");
    fs::last_write_time(script, fs::last_write_time(script) + std::chrono::seconds(10));
    for (int frame = 0; frame < 120; ++frame) moonmod::SharedRuntime().Pump();
    EXPECT_EQ(Record("test.edit")->state, moonmod::ModState::LoadError);
    EXPECT_NE(Record("test.edit")->status_message.find("fix and save"), std::string::npos)
        << Record("test.edit")->status_message;
    Write(script, "moonmod.log('info', 'EDIT_V4')\n");
    fs::last_write_time(script, fs::last_write_time(script) + std::chrono::seconds(15));
    for (int frame = 0; frame < 120; ++frame) moonmod::SharedRuntime().Pump();
    EXPECT_TRUE(probe.Saw("EDIT_V4"));
    EXPECT_EQ(Record("test.edit")->state, moonmod::ModState::Loaded);
}


// A part's switch holds while its pack is off: it can be set ahead, and the pack starts it (or not) as it was left.
TEST(MoonModPack, PartSwitchHoldsWhileItsPackIsOff) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    WritePack(root.path / "mods");
    // a hand-edited profile with a pack's parts as anything but an object is read as "all on"
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.pack":false},"parts":{"test.pack":true},"settings":{}})json");
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;
    ASSERT_TRUE(Record("test.beta")->part_switch);

    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.beta", false, error)) << error;
    EXPECT_FALSE(Record("test.beta")->part_switch);
    EXPECT_FALSE(Record("test.beta")->restart_required);
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.pack", true, error)) << error;
    EXPECT_EQ(Record("test.alpha")->state, moonmod::ModState::Loaded);
    EXPECT_EQ(Record("test.beta")->state, moonmod::ModState::Disabled);
    EXPECT_FALSE(probe.Saw("BETA_LOADED"));
    const moonmod::Json profile = moonmod::Json::parse(Read(root.path / "save" / "moonmod" / "profile.json"));
    EXPECT_EQ(profile["parts"]["test.pack"]["test.beta"], false);
}

// A part turned off and on again starts with the globals it was handed, not those its last run left.
TEST(MoonModPack, PartStartsAgainWithFreshGlobals) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    const fs::path mods = root.path / "mods";
    Write(mods / "test.src" / "mod.json", R"json({
      "schema_version":1,"id":"test.src","name":"Source Pack","version":"1.0.0","coop":"local",
      "framework_api":1,"games":["chronicle"],"entry":"main.lua","permissions":["game.read"],
      "parts":[{"id":"test.once","name":"Once","version":"1.0.0"}]
    })json");
    Write(mods / "test.src" / "main.lua",
          "moonmod.part.run('test.once', [[\n"
          "if SEEN then moonmod.log('info', 'STALE') end\nSEEN = true\nmoonmod.log('info', 'FRESH ' .. GIVEN)\n"
          "]], setmetatable({GIVEN = 'given'}, {__index = _ENV}), 'test.once/main.lua')\n");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.src":true},"settings":{}})json");
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.once", false, error)) << error;
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.once", true, error)) << error;
    EXPECT_EQ(probe.Count("FRESH given"), 2);
    EXPECT_FALSE(probe.Saw("STALE"));
}

// Part markers are found whatever the script's line ends, so one part's content doesn't hold back another.
TEST(MoonModPack, PartMarkersWorkWithCrlfLineEnds) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    const fs::path mods = root.path / "mods";
    Write(mods / "test.crlf" / "mod.json", R"json({
      "schema_version":1,"id":"test.crlf","name":"CRLF Pack","version":"1.0.0","coop":"local",
      "framework_api":1,"games":["chronicle"],"entry":"main.lua","permissions":["game.read","content.register"],
      "parts":[{"id":"test.plain","name":"Plain","version":"1.0.0"},{"id":"test.adds","name":"Adds","version":"1.0.0"}]
    })json");
    Write(mods / "test.crlf" / "main.lua",
          "-- moonmod:part test.plain\r\nmoonmod.part.run('test.plain', function() moonmod.log('info', 'PLAIN') end)\r\n"
          "-- moonmod:end\r\n-- moonmod:part test.adds\r\n"
          "moonmod.part.run('test.adds', function() game.items.register({id = 'x', name = 'X'}) end)\r\n"
          "-- moonmod:end\r\n");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.crlf":true},
                  "parts":{"test.crlf":{"test.plain":false,"test.adds":false}},"settings":{}})json");
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;
    moonmod::SharedRuntime().Publish("game.scene.entered", {{"scene_id", "town"}});
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.plain", true, error)) << error;
    EXPECT_FALSE(Record("test.plain")->restart_required) << Record("test.plain")->restart_reason;
    EXPECT_TRUE(probe.Saw("PLAIN"));
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.adds", true, error)) << error;
    EXPECT_TRUE(Record("test.adds")->restart_required);
}

// A pack whose script fails takes its parts' rows with it: none says it is ready.
TEST(MoonModPack, PackThatFailsMarksItsParts) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    WritePack(root.path / "mods");
    const fs::path script = root.path / "mods" / "test.pack" / "main.lua";
    Write(script, Read(script) + "error('PACK_BROKEN')\n");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.pack":true},"settings":{}})json");
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;
    EXPECT_EQ(Record("test.pack")->state, moonmod::ModState::LoadError);
    for (const char *part : {"test.alpha", "test.beta"}) {
        EXPECT_EQ(Record(part)->state, moonmod::ModState::LoadError) << part;
        EXPECT_NE(Record(part)->status_message.find("didn't load"), std::string::npos) << Record(part)->status_message;
    }
}

// In a story game (any scene but the title), content the game keeps waits for the title screen.
TEST(MoonModLive, AStoryGameCountsAsMidGame) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    WriteMod(root.path / "mods", "test.later",
             "game.items.register({id = 'x', name = 'X'})\nmoonmod.log('info', 'LATER_LOADED')\n");
    moonmod::RuntimeConfig config = Config(root.path, probe);
    config.api_capabilities.push_back({"items.register", true, {}});
    config.api_handler = [](std::string_view, const moonmod::ApiOperationSpec &, const moonmod::Json &) {
        return moonmod::ApiCallResult{moonmod::ApiCallStatus::Ok, {{"id", "test.later:x"}}, {}};
    };
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(config, error)) << error;
    moonmod::SharedRuntime().Publish("game.scene.entered", {{"scene_id", "dungeon"}});
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.later", true, error)) << error;
    EXPECT_TRUE(Record("test.later")->restart_required);
    EXPECT_FALSE(probe.Saw("LATER_LOADED"));
    moonmod::SharedRuntime().Publish("game.scene.entered", {{"scene_id", "title"}});
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.later", true, error)) << error;
    EXPECT_FALSE(Record("test.later")->restart_required) << Record("test.later")->restart_reason;
    EXPECT_TRUE(probe.Saw("LATER_LOADED"));
}

// A mod switched off while its broken files wait for a fix stays off when the fix is saved.
TEST(MoonModLive, ModSwitchedOffAfterABadSaveStaysOff) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    Probe probe; // declared first: TempRoot stops the runtime, which still logs to it
    TempRoot root;
    WriteMod(root.path / "mods", "test.edit", "moonmod.log('info', 'EDIT_V1')\n");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.edit":true},"settings":{}})json");
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(Config(root.path, probe), error)) << error;
    const fs::path script = root.path / "mods" / "test.edit" / "main.lua";
    Write(script, "moonmod.log('info', 'EDIT_V2'\n");
    fs::last_write_time(script, fs::last_write_time(script) + std::chrono::seconds(5));
    for (int frame = 0; frame < 120; ++frame) moonmod::SharedRuntime().Pump();
    ASSERT_EQ(Record("test.edit")->state, moonmod::ModState::LoadError);
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.edit", false, error)) << error;
    Write(script, "moonmod.log('info', 'EDIT_V3')\n");
    fs::last_write_time(script, fs::last_write_time(script) + std::chrono::seconds(10));
    for (int frame = 0; frame < 120; ++frame) moonmod::SharedRuntime().Pump();
    EXPECT_FALSE(probe.Saw("EDIT_V3"));
    EXPECT_FALSE(Record("test.edit")->enabled);
}

// Loads the MoonMod Pack (MoonMod's mods/moonmod.pack: twelve mods merged into one script) through the real Lua
// runtime and the Chronicle adapter: every part starts as its own owner, the Gauntlet's and Vigil's title modes both
// register, co-op sees the parts, and a part turns off and on while the pack runs.
// Set MOONMOD_PACK_PATH to the pack's folder to run it; skipped otherwise.
#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "gamemode.hpp"
#include "main.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/runtime.hpp"
#include "moonmod/title_modes.hpp"
#include "savedata.hpp"
#include "ui/framework.hpp"

namespace dc::game {}
using namespace dc::game;

namespace {

namespace fs = std::filesystem;

struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-real-pack-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

struct Restore {
    CSaveData *save = SaveData;
    int current_mode = mode;
    ~Restore() {
        moonmod::SharedRuntime().Stop();
        SaveData = save;
        mode = current_mode;
        moonmod::ResetTitleModesForTests();
        moonmod::ResetChronicleDungeonsForTests();
        moonmod::ResetChronicleCharactersForTests();
    }
};

const moonmod::ModRecord *Record(std::string_view id) {
    for (const moonmod::ModRecord &record : moonmod::SharedRuntime().Mods()) {
        if (record.id == id) return &record;
    }
    return nullptr;
}

} // namespace

TEST(MoonModRealPack, EveryPartStartsAsItsOwnOwner) {
    const char *pack_path = std::getenv("MOONMOD_PACK_PATH");
    if (pack_path == nullptr || !fs::exists(fs::path(pack_path) / "mod.json")) {
        GTEST_SKIP() << "Set MOONMOD_PACK_PATH to MoonMod's mods/moonmod.pack to run this.";
    }
    std::vector<std::string> logs; // declared before Restore: stopping the runtime still logs
    Restore restore;
    moonmod::SharedRuntime().Stop();
    moonmod::ResetTitleModesForTests();
    moonmod::ResetChronicleDungeonsForTests();
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    mode = GAME_MODE_TITLE;

    TempRoot root;
    const fs::path package = root.path / "mods" / "moonmod.pack";
    fs::create_directories(package.parent_path());
    fs::copy(pack_path, package, fs::copy_options::recursive);
    fs::create_directories(root.path / "save" / "moonmod");
    {
        std::ofstream profile(root.path / "save" / "moonmod" / "profile.json", std::ios::binary | std::ios::trunc);
        profile << R"json({"format_version":1,"enabled":{"moonmod.pack":true},"settings":{}})json";
    }
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    moonmod::ConfigureChronicleApi(config);
    config.log = [&logs](std::string_view level, std::string_view mod_id, std::string_view message) {
        logs.push_back(std::string(level) + " " + std::string(mod_id) + ": " + std::string(message));
    };
    std::string error;
    const auto started = std::chrono::steady_clock::now();
    ASSERT_TRUE(moonmod::SharedRuntime().Start(std::move(config), error)) << error;
    const double seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    std::printf("pack load: %.3f s\n", seconds);

    const moonmod::ModRecord *pack = Record("moonmod.pack");
    ASSERT_NE(pack, nullptr);
    EXPECT_EQ(pack->state, moonmod::ModState::Loaded) << pack->status_message;
    ASSERT_EQ(pack->manifest.parts.size(), 12u);
    for (const moonmod::ModManifest &part : pack->manifest.parts) {
        const moonmod::ModRecord *record = Record(part.id);
        ASSERT_NE(record, nullptr) << part.id;
        EXPECT_EQ(record->state, moonmod::ModState::Loaded) << part.id << ": " << record->status_message;
    }
    for (const std::string &line : logs) {
        EXPECT_EQ(line.rfind("error", 0), std::string::npos) << line;
        std::printf("  %s\n", line.c_str());
    }

    // the Gauntlet and Vigil each keep their own title-menu row
    std::vector<std::string> modes;
    for (const moonmod::TitleMode &title : moonmod::TitleModes()) modes.push_back(title.id);
    EXPECT_NE(std::ranges::find(modes, "moonmod.roguelike:gauntlet"), modes.end());

    // co-op sees the parts with their own versions
    const moonmod::Json list = moonmod::SharedRuntime().ModList();
    EXPECT_EQ(list.size(), 13u);

    // a few frames at the title screen
    for (int frame = 0; frame < 30; ++frame) moonmod::SharedRuntime().Pump();

    // the Hub (nothing the game keeps) turns off and on again while the pack runs
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("moonmod.hub", false, error)) << error;
    EXPECT_FALSE(Record("moonmod.hub")->restart_required) << Record("moonmod.hub")->restart_reason;
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("moonmod.hub", true, error)) << error;
    EXPECT_EQ(Record("moonmod.hub")->state, moonmod::ModState::Loaded) << Record("moonmod.hub")->status_message;
    // the Gauntlet registered playable characters and floors: turning it off waits for a restart
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("moonmod.roguelike", false, error)) << error;
    EXPECT_TRUE(Record("moonmod.roguelike")->restart_required);
    std::printf("roguelike off: %s\n", Record("moonmod.roguelike")->restart_reason.c_str());
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("moonmod.roguelike", true, error)) << error;
    for (const std::string &line : logs) EXPECT_EQ(line.rfind("error", 0), std::string::npos) << line;
}

// Every part, one at a time: off and on again at the title screen, then in a game session. A part either switches at
// once (and is running again after) or waits for a restart and says why; nothing logs an error either way, and the
// other parts keep running.
TEST(MoonModRealPack, EveryPartSwitchesLiveOrSaysWhyNot) {
    const char *pack_path = std::getenv("MOONMOD_PACK_PATH");
    if (pack_path == nullptr || !fs::exists(fs::path(pack_path) / "mod.json")) {
        GTEST_SKIP() << "Set MOONMOD_PACK_PATH to MoonMod's mods/moonmod.pack to run this.";
    }
    std::vector<std::string> logs; // declared before Restore: stopping the runtime still logs
    Restore restore;
    moonmod::SharedRuntime().Stop();
    moonmod::ResetTitleModesForTests();
    moonmod::ResetChronicleDungeonsForTests();
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    mode = GAME_MODE_TITLE;

    TempRoot root;
    const fs::path package = root.path / "mods" / "moonmod.pack";
    fs::create_directories(package.parent_path());
    fs::copy(pack_path, package, fs::copy_options::recursive);
    fs::create_directories(root.path / "save" / "moonmod");
    {
        std::ofstream profile(root.path / "save" / "moonmod" / "profile.json", std::ios::binary | std::ios::trunc);
        profile << R"json({"format_version":1,"enabled":{"moonmod.pack":true},"settings":{}})json";
    }
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    moonmod::ConfigureChronicleApi(config);
    config.log = [&logs](std::string_view level, std::string_view mod_id, std::string_view message) {
        logs.push_back(std::string(level) + " " + std::string(mod_id) + ": " + std::string(message));
    };
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(std::move(config), error)) << error;
    std::vector<std::string> parts;
    for (const moonmod::ModManifest &part : Record("moonmod.pack")->manifest.parts) parts.push_back(part.id);
    ASSERT_EQ(parts.size(), 12u);

    auto all_others_running = [&](std::string_view except) {
        for (const std::string &id : parts) {
            if (id == except) continue;
            EXPECT_EQ(Record(id)->state, moonmod::ModState::Loaded) << "after switching " << except << ", " << id
                                                                   << ": " << Record(id)->status_message;
        }
    };
    auto sweep = [&](const char *where) {
        std::printf("%s:\n", where);
        for (const std::string &id : parts) {
            const std::size_t logged = logs.size();
            ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled(id, false, error)) << id << ": " << error;
            const moonmod::ModRecord *record = Record(id);
            const bool live = !record->restart_required;
            const std::string reason = record->restart_reason;
            if (live) {
                EXPECT_EQ(record->state, moonmod::ModState::Disabled) << id;
                EXPECT_TRUE(record->restart_reason.empty()) << id;
            } else {
                EXPECT_EQ(record->state, moonmod::ModState::Loaded) << id;
                EXPECT_FALSE(record->restart_reason.empty()) << id << " waits without saying why";
                // The mod's page has two lines for it, 320 wide in small text.
                const std::string shown = "Off after a restart: " + reason;
                EXPECT_LE(dc::ui::WrapText(shown, 320, dc::ui::metrics::kScaleSmall).size(), 2u) << shown;
            }
            for (int frame = 0; frame < 10; ++frame) moonmod::SharedRuntime().Pump();
            all_others_running(id);
            ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled(id, true, error)) << id << ": " << error;
            EXPECT_FALSE(Record(id)->restart_required) << id << ": " << Record(id)->restart_reason;
            EXPECT_EQ(Record(id)->state, moonmod::ModState::Loaded) << id << ": " << Record(id)->status_message;
            for (int frame = 0; frame < 10; ++frame) moonmod::SharedRuntime().Pump();
            std::printf("  %-26s %s\n", id.c_str(), live ? "live" : ("restart: " + reason).c_str());
            for (std::size_t i = logged; i < logs.size(); ++i) {
                EXPECT_EQ(logs[i].rfind("error", 0), std::string::npos) << "switching " << id << ": " << logs[i];
            }
        }
    };
    sweep("title screen");
    // the Gauntlet's title row is still there after all that
    std::vector<std::string> modes;
    for (const moonmod::TitleMode &title : moonmod::TitleModes()) modes.push_back(title.id);
    EXPECT_NE(std::ranges::find(modes, "moonmod.roguelike:gauntlet"), modes.end());

    moonmod::SharedRuntime().Publish("game.session.started", moonmod::Json::object());
    for (int frame = 0; frame < 10; ++frame) moonmod::SharedRuntime().Pump();
    sweep("in a session");
    moonmod::SharedRuntime().Publish("game.session.ended", moonmod::Json::object());
    EXPECT_EQ(moonmod::SharedRuntime().ModList().size(), 13u);
}

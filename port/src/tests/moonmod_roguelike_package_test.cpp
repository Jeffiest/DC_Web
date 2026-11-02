// Loads MoonFactory's Dark Genie's Gauntlet package (mods/moonmod.roguelike) through the real Lua runtime and
// the Chronicle adapter: its title mode, its pages, Start Run's sandbox session and a defeat that brings the
// player back to its title screen. Skipped when the MoonFactory checkout is not beside this game checkout.

#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "dngstatusdata.hpp"
#include "app/gameloop.hpp"
#include "dun/gameloop.hpp"
#include "gamemode.hpp"
#include "main.hpp"
#include "mainselect.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/runtime.hpp"
#include "moonmod/title_modes.hpp"
#include "savedata.hpp"

namespace dc::game {}
using namespace dc::game;


namespace {

namespace fs = std::filesystem;

struct TempRoot {
    fs::path path = fs::temp_directory_path() /
                    ("moonmod-roguelike-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    ~TempRoot() {
        std::error_code ignored;
        fs::remove_all(path, ignored);
    }
};

struct Restore {
    CSaveData *save = SaveData;
    int current_mode = mode;
    int map_no = MapNo;
    int next_map_no = NextMapNo;
    int selected_dungeon = selectMapNo;
    ~Restore() {
        moonmod::SharedRuntime().Stop();
        SaveData = save;
        mode = current_mode;
        MapNo = map_no;
        NextMapNo = next_map_no;
        selectMapNo = selected_dungeon;
        moonmod::ResetTitleModesForTests();
        moonmod::ResetChronicleDungeonsForTests();
        moonmod::ResetChronicleCharactersForTests(); // the Gauntlet's Wardens and its playable Opar
    }
};

std::vector<std::string> EntryIds(const moonmod::TitleMode &mode) {
    std::vector<std::string> ids;
    for (const auto &entry : mode.entries) ids.push_back(entry.id);
    return ids;
}

} // namespace

TEST(MoonModRoguelikePackage, TitleModeStartsASandboxRunAndADefeatReturnsToIt) {
#if !defined(MOONFACTORY_ROGUELIKE_PATH)
    GTEST_SKIP() << "The MoonFactory checkout (mods/moonmod.roguelike) is not beside this game checkout.";
#else
    Restore restore;
    moonmod::SharedRuntime().Stop();
    moonmod::ResetTitleModesForTests();
    moonmod::ResetChronicleDungeonsForTests();
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    mode = GAME_MODE_TITLE;
    MapNo = 800;
    NextMapNo = -1;

    TempRoot root;
    const fs::path package = root.path / "mods" / "moonmod.roguelike";
    fs::create_directories(package.parent_path());
    fs::copy(MOONFACTORY_ROGUELIKE_PATH, package, fs::copy_options::recursive);
    fs::create_directories(root.path / "save" / "moonmod");
    {
        std::ofstream profile(root.path / "save" / "moonmod" / "profile.json", std::ios::binary | std::ios::trunc);
        profile << R"json({"format_version":1,"enabled":{"moonmod.roguelike":true},"settings":{}})json";
    }
    moonmod::RuntimeConfig config;
    config.game_id = MOONMOD_TEST_GAME_ID;
    config.game_name = "Chronicle";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    moonmod::ConfigureChronicleApi(config);
    std::vector<std::string> logs;
    config.log = [&logs](std::string_view, std::string_view level, std::string_view message) {
        logs.push_back(std::string(level) + ": " + std::string(message));
    };
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(std::move(config), error)) << error;
    ASSERT_EQ(moonmod::SharedRuntime().Mods().size(), 1u);
    if (moonmod::SharedRuntime().Mods()[0].state != moonmod::ModState::Loaded) {
        for (const std::string &message : logs) ADD_FAILURE() << message;
    }
    ASSERT_EQ(moonmod::SharedRuntime().Mods()[0].state, moonmod::ModState::Loaded);
    bool ready = false;
    for (const std::string &message : logs) {
        EXPECT_EQ(message.find("warn"), std::string::npos) << message;
        if (message.find("Moon Gauntlet ready: 5 of 5 biomes") != std::string::npos) ready = true;
    }
    EXPECT_TRUE(ready);

    // The title mode and its root page.
    ASSERT_EQ(moonmod::TitleModes().size(), 1u);
    const moonmod::TitleMode &gauntlet = moonmod::TitleModes()[0];
    EXPECT_EQ(gauntlet.id, "moonmod.roguelike:gauntlet");
    EXPECT_EQ(gauntlet.label, "Dark Genie's Gauntlet");
    EXPECT_EQ(EntryIds(gauntlet), (std::vector<std::string>{"start", "character", "legend", "eternal", "records"}));

    ASSERT_TRUE(moonmod::OpenTitleModeScreen(0));
    using moonmod::TitleModeInput;
    using moonmod::TitleModeOutcome;
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Down), TitleModeOutcome::Stay);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Confirm), TitleModeOutcome::Stay);
    EXPECT_EQ(moonmod::TitleModes()[0].entries.at(1).label, "Starting Character: Xiao");
    EXPECT_EQ(moonmod::TitleModes()[0].cursor, 1u);

    // Norune Eternal is a sub-page (past the starting weapon); Circle comes back to the root page.
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Down), TitleModeOutcome::Stay);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Down), TitleModeOutcome::Stay);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Confirm), TitleModeOutcome::Stay);
    EXPECT_FALSE(moonmod::TitleModes()[0].root);
    ASSERT_TRUE(moonmod::TitleModes()[0].sky.has_value()); // the Norune Eternal sky
    EXPECT_GE(moonmod::TitleModes()[0].sky->nodes.size(), 100u);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Right), TitleModeOutcome::Stay);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Back), TitleModeOutcome::Stay);
    EXPECT_TRUE(moonmod::TitleModes()[0].root);

    // Start Run: a sandbox session on Divine Beast Cave's first run floor with all six joined.
    while (moonmod::TitleModes()[0].cursor != 0) moonmod::HandleTitleModeInput(TitleModeInput::Up);
    ASSERT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Confirm), TitleModeOutcome::StartSession);
    ASSERT_TRUE(moonmod::StartPendingChronicleSandbox());
    EXPECT_TRUE(moonmod::SandboxActive());
    CDngStatusData *status = save->GetDngStatus();
    EXPECT_EQ(status->GetPartySize(), 6);
    EXPECT_EQ(status->cur_chara, 1); // Xiao, chosen above
    EXPECT_EQ(NextMapNo, 200);
    moonmod::SharedRuntime().Pump();

    // A game over: the session ends, the run is recorded as a defeat and the title shows it.
    mode = GAME_MODE_DUNGEON;
    moonmod::NoteChronicleSandboxDefeat();
    NextMapNo = 0;
    ASSERT_TRUE(moonmod::RedirectChronicleSandboxMapJump());
    EXPECT_EQ(NextMapNo, 800);
    moonmod::SharedRuntime().Pump();
    const auto reopen = moonmod::TakeTitleModeReturn();
    ASSERT_TRUE(reopen.has_value());
    EXPECT_EQ(*reopen, "moonmod.roguelike:gauntlet");
    bool shown = false;
    for (const std::string &line : moonmod::TitleModes()[0].lines) {
        if (line.find("Last run: Fell in Divine Beast Cave") != std::string::npos) shown = true;
    }
    EXPECT_TRUE(shown);
    for (const std::string &message : logs) EXPECT_EQ(message.find("error"), std::string::npos) << message;
#endif
}

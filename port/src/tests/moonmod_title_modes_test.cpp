#include <gtest/gtest.h>

#include <memory>
#include <ranges>

#include "dngstatusdata.hpp"
#include "app/gameloop.hpp"
#include "dun/gameloop.hpp"
#include "gamemode.hpp"
#include "itemdata.hpp"
#include "main.hpp"
#include "mainselect.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/title_modes.hpp"
#include "savedata.hpp"

namespace dc::game {}
using namespace dc::game;


namespace {

struct SavedGameState {
    CSaveData *save = SaveData;
    int current_mode = mode;
    int map_no = MapNo;
    int next_map_no = NextMapNo;
    int start_event_no = StartEventNo;
    int selected_dungeon = selectMapNo;

    ~SavedGameState() {
        SaveData = save;
        mode = current_mode;
        MapNo = map_no;
        NextMapNo = next_map_no;
        StartEventNo = start_event_no;
        selectMapNo = selected_dungeon;
        moonmod::ResetTitleModesForTests();
        moonmod::ResetChronicleDungeonsForTests();
    }
};

struct Api {
    moonmod::RuntimeConfig config;
    Api() { moonmod::ConfigureChronicleApi(config); }
    moonmod::ApiCallResult operator()(std::string_view mod, std::string_view path, moonmod::Json argument) {
        const moonmod::ApiOperationSpec *operation = moonmod::FindApiOperation(path);
        EXPECT_NE(operation, nullptr) << path;
        if (operation == nullptr) return {};
        moonmod::Json args = argument.is_null() ? moonmod::Json::array() : moonmod::Json::array({argument});
        return config.api_handler(mod, *operation, args);
    }
};

moonmod::Json GauntletPage() {
    return {{"id", "gauntlet"}, {"label", "Dark Genie's Gauntlet"}, {"title", "DARK GENIE'S GAUNTLET"},
            {"subtitle", "A run that is not in any save"}, {"lines", {"Moon Shards 12", "Best depth 7"}},
            {"entries", {{{"id", "start"}, {"label", "Start Run"}},
                         {{"id", "eternal"}, {"label", "Norune Eternal"}},
                         {{"id", "locked"}, {"label", "Ascension (locked)"}, {"enabled", false}}}}};
}

} // namespace

TEST(MoonModTitleModes, CatalogAndCapabilities) {
    Api api;
    for (const char *path : {"title.mode_register", "title.mode_set", "title.mode_remove", "session.get", "session.start", "session.finish"}) {
        ASSERT_NE(moonmod::FindApiOperation(path), nullptr) << path;
        const auto capability = std::ranges::find(api.config.api_capabilities, std::string_view(path), &moonmod::ApiCapability::path);
        ASSERT_NE(capability, api.config.api_capabilities.end()) << path;
        EXPECT_TRUE(capability->supported) << path;
    }
    for (const char *event : {"game.title.opened", "game.title.selected", "game.session.started", "game.session.ended"}) {
        EXPECT_NE(std::ranges::find(moonmod::EventCatalog(), std::string_view(event), &moonmod::EventSpec::name),
                  moonmod::EventCatalog().end()) << event;
    }
}

TEST(MoonModTitleModes, RegistersUpdatesAndRemovesOwnedModes) {
    SavedGameState saved;
    moonmod::ResetTitleModesForTests();
    Api api;

    const auto registered = api("mod.a", "title.mode_register", GauntletPage());
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    EXPECT_EQ(registered.value.at("id"), "mod.a:gauntlet");
    EXPECT_EQ(registered.value.at("entries").size(), 3u);
    ASSERT_EQ(moonmod::TitleModes().size(), 1u);

    // Registering the same id again updates it in place.
    moonmod::Json again = GauntletPage();
    again["label"] = "Gauntlet";
    ASSERT_EQ(api("mod.a", "title.mode_register", again).status, moonmod::ApiCallStatus::Ok);
    ASSERT_EQ(moonmod::TitleModes().size(), 1u);
    EXPECT_EQ(moonmod::TitleModes()[0].label, "Gauntlet");

    // Validation.
    EXPECT_EQ(api("mod.a", "title.mode_register", {{"id", "x"}, {"label", "X"}}).status, moonmod::ApiCallStatus::InvalidArgument);
    moonmod::Json too_many = GauntletPage();
    too_many["id"] = "many";
    too_many["entries"] = moonmod::Json::array();
    for (int i = 0; i < 8; ++i) too_many["entries"].push_back({{"id", "e" + std::to_string(i)}, {"label", "E"}});
    EXPECT_EQ(api("mod.a", "title.mode_register", too_many).status, moonmod::ApiCallStatus::InvalidArgument);
    moonmod::Json back_entry = GauntletPage();
    back_entry["id"] = "back-entry";
    back_entry["entries"] = moonmod::Json::array({{{"id", "back"}, {"label", "Back"}}});
    EXPECT_EQ(api("mod.a", "title.mode_register", back_entry).status, moonmod::ApiCallStatus::InvalidArgument);
    moonmod::Json control = GauntletPage();
    control["id"] = "control";
    control["title"] = "bad\ntitle";
    EXPECT_EQ(api("mod.a", "title.mode_register", control).status, moonmod::ApiCallStatus::InvalidArgument);

    // Two modes at most.
    moonmod::Json second = GauntletPage();
    second["id"] = "second";
    ASSERT_EQ(api("mod.b", "title.mode_register", second).status, moonmod::ApiCallStatus::Ok);
    moonmod::Json third = GauntletPage();
    third["id"] = "third";
    EXPECT_EQ(api("mod.c", "title.mode_register", third).status, moonmod::ApiCallStatus::Failed);

    // Only the owner changes or removes a mode.
    EXPECT_EQ(api("mod.b", "title.mode_set", {{"id", "gauntlet"}, {"title", "MINE"}}).status, moonmod::ApiCallStatus::Failed);
    const auto set = api("mod.a", "title.mode_set", {{"id", "gauntlet"}, {"lines", {"Moon Shards 40"}}, {"cursor", 1}});
    ASSERT_EQ(set.status, moonmod::ApiCallStatus::Ok) << set.message;
    EXPECT_EQ(moonmod::TitleModes()[0].lines.size(), 1u);
    EXPECT_EQ(moonmod::TitleModes()[0].cursor, 1u);
    EXPECT_EQ(api("mod.b", "title.mode_remove", {{"id", "mod.a:gauntlet"}}).status, moonmod::ApiCallStatus::Failed);

    // Unloading the owner removes its mode.
    api.config.api_owner_unloaded("mod.b");
    ASSERT_EQ(moonmod::TitleModes().size(), 1u);
    ASSERT_EQ(api("mod.a", "title.mode_remove", {{"id", "gauntlet"}}).status, moonmod::ApiCallStatus::Ok);
    EXPECT_TRUE(moonmod::TitleModes().empty());
}

TEST(MoonModTitleModes, ScreenNavigationWrapsAndBacksOutFromTheRoot) {
    SavedGameState saved;
    moonmod::ResetTitleModesForTests();
    Api api;
    ASSERT_EQ(api("mod.a", "title.mode_register", GauntletPage()).status, moonmod::ApiCallStatus::Ok);

    EXPECT_FALSE(moonmod::OpenTitleModeScreen(1));
    ASSERT_TRUE(moonmod::OpenTitleModeScreen(0));
    ASSERT_NE(moonmod::OpenTitleModeScreenMode(), nullptr);
    using moonmod::TitleModeInput;
    using moonmod::TitleModeOutcome;
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Up), TitleModeOutcome::Stay);
    EXPECT_EQ(moonmod::OpenTitleModeScreenMode()->cursor, 2u); // wraps to the last entry
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Confirm), TitleModeOutcome::Stay); // disabled entry
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Down), TitleModeOutcome::Stay);
    EXPECT_EQ(moonmod::OpenTitleModeScreenMode()->cursor, 0u);

    // A sub-page (root = false): Circle stays on the screen and the mod gets "back".
    ASSERT_EQ(api("mod.a", "title.mode_set", {{"id", "gauntlet"}, {"root", false}}).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Back), TitleModeOutcome::Stay);
    ASSERT_EQ(api("mod.a", "title.mode_set", {{"id", "gauntlet"}, {"root", true}}).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Back), TitleModeOutcome::ReturnToTitle);
    EXPECT_EQ(moonmod::OpenTitleModeScreenMode(), nullptr);

    // A sub-page whose mod never answers Circle still lets the player out on the third press.
    ASSERT_TRUE(moonmod::OpenTitleModeScreen(0));
    ASSERT_EQ(api("mod.a", "title.mode_set", {{"id", "gauntlet"}, {"root", false}}).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Back), TitleModeOutcome::Stay);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Back), TitleModeOutcome::Stay);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Back), TitleModeOutcome::ReturnToTitle);
    EXPECT_EQ(moonmod::OpenTitleModeScreenMode(), nullptr);
}

TEST(MoonModTitleModes, AQueuedSessionNeverOutlivesItsScreen) {
    SavedGameState saved;
    moonmod::ResetTitleModesForTests();
    Api api;
    ASSERT_EQ(api("mod.a", "title.mode_register", GauntletPage()).status, moonmod::ApiCallStatus::Ok);
    // A request queued outside an input (from game.tick) starts on the next frame, whatever was pressed.
    ASSERT_TRUE(moonmod::OpenTitleModeScreen(0));
    moonmod::QueueSandboxRequest({"mod.a", "mod.a:gauntlet", "mod.a:x:i", 0, 15, 1, 0});
    EXPECT_EQ(moonmod::HandleTitleModeInput(moonmod::TitleModeInput::Back), moonmod::TitleModeOutcome::StartSession);
    // A start that cannot happen closes the screen and drops the request.
    EXPECT_FALSE(moonmod::StartPendingChronicleSandbox());
    EXPECT_EQ(moonmod::OpenTitleModeScreenMode(), nullptr);
    EXPECT_FALSE(moonmod::SandboxRequestPending());
    // Leaving the screen drops a request too.
    ASSERT_TRUE(moonmod::OpenTitleModeScreen(0));
    moonmod::CloseTitleModeScreen();
    moonmod::QueueSandboxRequest({"mod.a", "mod.a:gauntlet", "mod.a:x:i", 0, 15, 1, 0});
    moonmod::CloseTitleModeScreen();
    EXPECT_FALSE(moonmod::SandboxRequestPending());
}

TEST(MoonModTitleModes, SandboxSessionStartsFromTheOpenScreenAndReturnsToIt) {
    SavedGameState saved;
    moonmod::ResetTitleModesForTests();
    moonmod::ResetChronicleDungeonsForTests();
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    Api api;

    const auto registered = api("mod.a", "dungeons.register",
        {{"id", "gauntlet-dbc"}, {"name", "Gauntlet"}, {"dungeon", 0},
         {"floors", {{{"id", "i"}, {"source_floor", 2}}, {{"id", "ii"}, {"source_floor", 5}}}}});
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    const std::string floor_id = registered.value.at("floors").at(0).at("id").get<std::string>();
    ASSERT_EQ(api("mod.a", "title.mode_register", GauntletPage()).status, moonmod::ApiCallStatus::Ok);

    const moonmod::Json start = {{"floor", floor_id}, {"party_size", 3}, {"character", "toan"}};
    mode = GAME_MODE_TITLE;
    MapNo = 800;
    NextMapNo = -1;
    // Not before the screen is open, and not by another mod.
    EXPECT_EQ(api("mod.a", "session.start", start).status, moonmod::ApiCallStatus::Failed);
    ASSERT_TRUE(moonmod::OpenTitleModeScreen(0));
    EXPECT_EQ(api("mod.b", "session.start", start).status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(api("mod.a", "session.start", {{"floor", "mod.a:nowhere:i"}}).status, moonmod::ApiCallStatus::Failed);
    EXPECT_EQ(api("mod.a", "session.start", {{"floor", floor_id}, {"party_size", 1}, {"character", "goro"}}).status,
              moonmod::ApiCallStatus::InvalidArgument);
    EXPECT_EQ(api("mod.a", "session.get", nullptr).value.at("kind"), "title");

    const auto queued = api("mod.a", "session.start", start);
    ASSERT_EQ(queued.status, moonmod::ApiCallStatus::Ok) << queued.message;
    EXPECT_EQ(queued.value.at("native_floor_id"), 15);
    EXPECT_EQ(moonmod::HandleTitleModeInput(moonmod::TitleModeInput::None), moonmod::TitleModeOutcome::StartSession);
    EXPECT_EQ(api("mod.a", "session.start", start).status, moonmod::ApiCallStatus::Failed); // already starting

    // The game loop starts it: a new game's save with three fighters, straight to the floor.
    ASSERT_TRUE(moonmod::StartPendingChronicleSandbox());
    EXPECT_FALSE(moonmod::StartPendingChronicleSandbox());
    CDngStatusData *status = save->GetDngStatus();
    EXPECT_EQ(status->GetPartySize(), 3);
    for (int character = 0; character < 3; ++character) {
        EXPECT_EQ(status->equipped_weapon_slot[character], 0) << character;
        EXPECT_GE(status->chara_weapons[character][0].item_no, ITEM_WEAPON_START) << character;
    }
    EXPECT_EQ(status->chara_weapons[1][0].item_no, ITEM_WEAPON_WOODEN_SLINGSHOT);
    EXPECT_EQ(status->equipped_weapon_slot[3], -1);
    EXPECT_EQ(NextMapNo, 200);
    EXPECT_EQ(status->cur_georama, 0);
    EXPECT_TRUE(moonmod::SandboxActive());
    const auto session = api("mod.a", "session.get", nullptr);
    EXPECT_EQ(session.value.at("kind"), "sandbox");
    EXPECT_EQ(session.value.at("mode"), "mod.a:gauntlet");
    const auto slot = api("mod.a", "save.slot_id", nullptr);
    ASSERT_EQ(slot.status, moonmod::ApiCallStatus::Ok);
    EXPECT_TRUE(slot.value.at("slot_id").is_null());

    // Moving between floors keeps the session.
    EXPECT_FALSE(moonmod::RedirectChronicleSandboxMapJump());
    mode = GAME_MODE_DUNGEON;
    selectMapNo = 0;
    ASSERT_TRUE(moonmod::PreparePendingChronicleDungeonEntry());
    EXPECT_EQ(moonmod::TakePendingChronicleDungeonFloor(0), 15);
    NextMapNo = 201;
    EXPECT_FALSE(moonmod::RedirectChronicleSandboxMapJump());
    EXPECT_EQ(NextMapNo, 201);
    // The next biome: a transition into another dungeon moves the session's area with it.
    const auto wof = api("mod.a", "dungeons.register",
        {{"id", "gauntlet-wof"}, {"name", "Gauntlet"}, {"dungeon", 1}, {"floors", {{{"id", "i"}, {"source_floor", 2}}}}});
    ASSERT_EQ(wof.status, moonmod::ApiCallStatus::Ok) << wof.message;
    NextMapNo = -1;
    ASSERT_EQ(api("mod.a", "world.transition", {{"id", wof.value.at("floors").at(0).at("id")}}).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(NextMapNo, 201);
    selectMapNo = 1;
    ASSERT_TRUE(moonmod::PreparePendingChronicleDungeonEntry());
    EXPECT_EQ(status->cur_georama, 1);
    EXPECT_EQ(api("mod.a", "world.get", nullptr).status, moonmod::ApiCallStatus::Ok);

    // A game over sends the run back to the title screen of its mode.
    NextMapNo = -1;
    moonmod::NoteChronicleSandboxDefeat();
    EXPECT_EQ(api("mod.a", "session.get", nullptr).value.at("ending"), "defeat");
    NextMapNo = 0; // the dungeon's exit to Norune
    EXPECT_TRUE(moonmod::RedirectChronicleSandboxMapJump());
    EXPECT_EQ(NextMapNo, 800);
    EXPECT_FALSE(moonmod::SandboxActive());
    const auto reopen = moonmod::TakeTitleModeReturn();
    ASSERT_TRUE(reopen.has_value());
    EXPECT_EQ(*reopen, "mod.a:gauntlet");
    EXPECT_FALSE(moonmod::TakeTitleModeReturn().has_value());
}

TEST(MoonModTitleModes, SessionEndIsTheOwnersAndLoadingASaveEndsItInPlace) {
    SavedGameState saved;
    moonmod::ResetTitleModesForTests();
    moonmod::ResetChronicleDungeonsForTests();
    auto save = std::make_unique<CSaveData>();
    save->Initialize();
    SaveData = save.get();
    Api api;
    const auto registered = api("mod.a", "dungeons.register",
        {{"id", "gauntlet-wof"}, {"name", "Gauntlet"}, {"dungeon", 1}, {"floors", {{{"id", "i"}, {"source_floor", 2}}}}});
    ASSERT_EQ(registered.status, moonmod::ApiCallStatus::Ok) << registered.message;
    ASSERT_EQ(api("mod.a", "title.mode_register", GauntletPage()).status, moonmod::ApiCallStatus::Ok);
    mode = GAME_MODE_TITLE;
    NextMapNo = -1;
    ASSERT_TRUE(moonmod::OpenTitleModeScreen(0));
    ASSERT_EQ(api("mod.a", "session.start", {{"floor", registered.value.at("floors").at(0).at("id")}}).status,
              moonmod::ApiCallStatus::Ok);
    ASSERT_TRUE(moonmod::StartPendingChronicleSandbox());
    EXPECT_EQ(NextMapNo, 201);

    mode = GAME_MODE_DUNGEON;
    NextMapNo = -1;
    EXPECT_EQ(api("mod.b", "session.finish", nullptr).status, moonmod::ApiCallStatus::Failed);
    const auto ended = api("mod.a", "session.finish", nullptr);
    ASSERT_EQ(ended.status, moonmod::ApiCallStatus::Ok) << ended.message;
    EXPECT_EQ(NextMapNo, 800);
    EXPECT_EQ(api("mod.a", "session.get", nullptr).value.at("ending"), "ended");
    EXPECT_TRUE(moonmod::RedirectChronicleSandboxMapJump());
    EXPECT_FALSE(moonmod::SandboxActive());
    EXPECT_TRUE(moonmod::TakeTitleModeReturn().has_value());

    // Loading a real save from the battle menu ends the session without reopening the mode.
    mode = GAME_MODE_TITLE;
    NextMapNo = -1;
    ASSERT_TRUE(moonmod::OpenTitleModeScreen(0));
    ASSERT_EQ(api("mod.a", "session.start", {{"floor", registered.value.at("floors").at(0).at("id")}}).status,
              moonmod::ApiCallStatus::Ok);
    ASSERT_TRUE(moonmod::StartPendingChronicleSandbox());
    moonmod::EndChronicleSandboxInPlace("loaded_save");
    EXPECT_FALSE(moonmod::SandboxActive());
    EXPECT_FALSE(moonmod::TakeTitleModeReturn().has_value());
}

namespace {

moonmod::Json SkyNode(const char *id, const char *group, double x, double y, const char *state, moonmod::Json links = moonmod::Json::array()) {
    return {{"id", id}, {"group", group}, {"x", x}, {"y", y}, {"label", id}, {"title", id}, {"state", state}, {"links", links}};
}

moonmod::Json SkyPage() {
    return {{"id", "gauntlet"},
            {"root", false},
            {"sky",
             {{"groups", {{{"id", "heart"}, {"name", "Norune Eternal"}, {"x", 0}, {"y", 0}},
                          {{"id", "arms"}, {"name", "Arms"}, {"colour", {248, 206, 92}}, {"x", 0}, {"y", -400}}}},
              {"nodes",
               {SkyNode("norune", "heart", 0, 0, "owned"), SkyNode("left", "heart", -120, 60, "open", {"norune"}),
                SkyNode("right", "heart", 120, 60, "locked", {"norune"}), SkyNode("whetstone", "arms", 0, -300, "open", {"norune"})}},
              {"texts", {{{"text", "EDGE"}, {"x", -100}, {"y", -380}}}},
              {"top", {"Moon Shards 40", "Heat 0"}}}}};
}

} // namespace

TEST(MoonModTitleModes, SkyPageValidatesAndMovesByDirection) {
    SavedGameState saved;
    moonmod::ResetTitleModesForTests();
    Api api;
    ASSERT_EQ(api("mod.a", "title.mode_register", GauntletPage()).status, moonmod::ApiCallStatus::Ok);
    ASSERT_EQ(api("mod.a", "title.mode_set", SkyPage()).status, moonmod::ApiCallStatus::Ok);
    const moonmod::TitleMode *mode = moonmod::FindTitleMode("gauntlet", "mod.a");
    ASSERT_NE(mode, nullptr);
    ASSERT_TRUE(mode->sky.has_value());
    EXPECT_EQ(mode->sky->nodes.size(), 4u);
    EXPECT_EQ(mode->sky->nodes[3].links.size(), 1u);
    EXPECT_EQ(mode->sky->nodes[2].state, 0);
    EXPECT_EQ(mode->sky->top.size(), 2u);

    // Directional steps: the nearest node within the cone, none where nothing lies.
    const moonmod::TitleSky &sky = *mode->sky;
    EXPECT_EQ(moonmod::TitleSkyStep(sky, 0, 1, 0), 2u);
    EXPECT_EQ(moonmod::TitleSkyStep(sky, 0, -1, 0), 1u);
    EXPECT_EQ(moonmod::TitleSkyStep(sky, 0, 0, -1), 3u);
    EXPECT_EQ(moonmod::TitleSkyStep(sky, 3, 0, -1), 3u);

    // Bad skies are refused whole.
    moonmod::Json bad = SkyPage();
    bad["sky"]["nodes"][1]["links"] = {"nowhere"};
    EXPECT_EQ(api("mod.a", "title.mode_set", bad).status, moonmod::ApiCallStatus::InvalidArgument);
    bad = SkyPage();
    bad["sky"]["nodes"][1]["group"] = "nowhere";
    EXPECT_EQ(api("mod.a", "title.mode_set", bad).status, moonmod::ApiCallStatus::InvalidArgument);
    bad = SkyPage();
    bad["sky"]["nodes"][1]["state"] = "sold";
    EXPECT_EQ(api("mod.a", "title.mode_set", bad).status, moonmod::ApiCallStatus::InvalidArgument);
    bad = SkyPage();
    bad["sky"]["top"] = {"a", "b", "c", "d"};
    EXPECT_EQ(api("mod.a", "title.mode_set", bad).status, moonmod::ApiCallStatus::InvalidArgument);
    bad = SkyPage();
    for (int i = 0; i < 200; ++i) bad["sky"]["nodes"].push_back(SkyNode(("n" + std::to_string(i)).c_str(), "arms", i, 0, "open"));
    EXPECT_EQ(api("mod.a", "title.mode_set", bad).status, moonmod::ApiCallStatus::InvalidArgument);
    ASSERT_TRUE(moonmod::FindTitleMode("gauntlet", "mod.a")->sky.has_value());

    // The screen: arrows move the cursor, Triangle shows the whole sky, and a new payload keeps the cursor's node.
    ASSERT_TRUE(moonmod::OpenTitleModeScreen(0));
    using moonmod::TitleModeInput;
    using moonmod::TitleModeOutcome;
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Right), TitleModeOutcome::Stay);
    EXPECT_EQ(moonmod::OpenTitleModeScreenMode()->sky->cursor, 2u);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Map), TitleModeOutcome::Stay);
    EXPECT_TRUE(moonmod::OpenTitleModeScreenMode()->sky->map);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Confirm), TitleModeOutcome::Stay);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Square), TitleModeOutcome::Stay);
    moonmod::Json moved = SkyPage();
    std::swap(moved["sky"]["nodes"][1], moved["sky"]["nodes"][2]);
    ASSERT_EQ(api("mod.a", "title.mode_set", moved).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(moonmod::OpenTitleModeScreenMode()->sky->cursor, 1u); // still on "right"
    EXPECT_TRUE(moonmod::OpenTitleModeScreenMode()->sky->map);

    // A list page in place of the sky clears it; Circle on a sub-page stays for the mod's "back".
    ASSERT_EQ(api("mod.a", "title.mode_set", GauntletPage()).status, moonmod::ApiCallStatus::Ok);
    EXPECT_FALSE(moonmod::OpenTitleModeScreenMode()->sky.has_value());
    ASSERT_EQ(api("mod.a", "title.mode_set", SkyPage()).status, moonmod::ApiCallStatus::Ok);
    EXPECT_EQ(moonmod::HandleTitleModeInput(TitleModeInput::Back), TitleModeOutcome::Stay);
}

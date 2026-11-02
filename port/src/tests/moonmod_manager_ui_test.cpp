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
                    ("moonmod-manager-ui-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
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

} // namespace

TEST(MoonModManagerUi, ChangesTypedSettingsNotifiesModsAndSurvivesReload) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    TempRoot root;
    const fs::path package = root.path / "mods" / "test.settings-ui";
    Write(package / "mod.json", R"json({
      "schema_version":1,"id":"test.settings-ui","name":"Settings UI Probe","version":"1.0.0",
      "framework_api":1,"games":["chronicle"],"entry":"main.lua","permissions":["content.register","game.read","game.write"],
      "settings":[
        {"id":"enabled","name":"Feature Enabled","type":"boolean","default":true},
        {"id":"amount","name":"Amount","type":"integer","default":1,"min":0,"max":5,"step":2},
        {"id":"mode","name":"Mode","type":"choice","default":"easy","choices":["easy","hard"]}
      ]
    })json");
    Write(package / "main.lua", R"lua(
moonmod.on("settings.changed", function(change)
  moonmod.log("info", "SETTING_CHANGED:" .. change.id .. ":" .. tostring(change.value))
end)
local action = assert(game.ui.register({id = "clear_cache", label = "Clear cache", description = "Remove cached data"}))
assert(action.id == "test.settings-ui:clear_cache" and action.status == "registered")
local duplicate, duplicate_error = game.ui.register({id = "clear_cache", label = "Duplicate"})
assert(duplicate == nil and duplicate_error.code == "invalid_argument")
local malformed, malformed_error = game.ui.register({id = "bad-field", label = "Bad", extra = true})
assert(malformed == nil and malformed_error.code == "invalid_argument")
moonmod.on("ui.action", function(action)
  assert(action.id == "clear_cache" and action.qualified_id == "test.settings-ui:clear_cache")
  assert(action.label == "Clear cache" and action.description == "Remove cached data")
  moonmod.log("info", "UI_ACTION:" .. action.id)
  assert(game.ui.remove({id = action.id}))
  local stale, stale_error = game.ui.remove({id = action.id})
  assert(stale == nil and stale_error.code == "failed")
end)
assert(game.quests.register({
  id = "journey", title = "A Small Journey", description = "Find the first clue.",
  rewards = {{kind = "currency", amount = 25}},
  objectives = {{id = "clue", title = "Find a clue", target = 2}}
}))
assert(game.quests.register({
  id = "side", title = "Optional Route", description = "A second quest in the journal.",
  objectives = {{id = "visit", title = "Visit a settlement", target = 1}}
}))
assert(game.skills.register({
  id = "combat", title = "Combat Training", description = "A new skill tree owned by this mod.",
  skills = {
    {id = "focus", title = "Focused Strike", description = "Improve attack precision.", max_rank = 2,
     effects = {enemy_damage_per_rank = 0.1}},
    {id = "power", title = "Heavy Blow", description = "Requires focus.", max_rank = 1,
     prerequisites = {{id = "focus", rank = 1}}}
  }
}))
local focus = assert(game.skills.get({id = "combat.focus"}))
if focus.rank == 0 then assert(game.skills.unlock({id = "combat.focus"})) end
moonmod.on("game.tick", function()
  local quest = assert(game.quests.get({id = "journey"}))
  if quest.status == "registered" then
    assert(game.quests.start({id = "journey"}))
    assert(game.quests.advance({id = "journey", objective = "clue", amount = 1}))
  end
end)
)lua");
    const fs::path journal_package = root.path / "mods" / "test.settings-ui-journal";
    Write(journal_package / "mod.json", R"json({
      "schema_version":1,"id":"test.settings-ui-journal","name":"Journal Helper","version":"1.0.0",
      "framework_api":1,"games":["chronicle"],"entry":"main.lua","permissions":["content.register"]
    })json");
    Write(journal_package / "main.lua", R"lua(
assert(game.quests.register({
  id = "helper", title = "Helper Package Quest", description = "Confirms cross-package journal entries.",
  objectives = {
    {id = "check", title = "Check the journal", target = 1},
    {id = "first", title = "Complete the first step", target = 1},
    {id = "second", title = "Complete the second step", target = 1},
    {id = "third", title = "Complete the third step", target = 1},
    {id = "fourth", title = "Complete the fourth step", target = 1},
    {id = "fifth", title = "Complete the fifth step", target = 1}
  }
}))
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.settings-ui":true,"test.settings-ui-journal":true},"settings":{}})json");

    std::vector<std::string> logs;
    moonmod::RuntimeConfig config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(config, error)) << error;
    ASSERT_EQ(moonmod::SharedRuntime().Mods().size(), 2u);
    ASSERT_EQ(moonmod::SharedRuntime().Mods()[0].state, moonmod::ModState::Loaded);
    ASSERT_TRUE(moonmod::SharedRuntime().Setting("test.settings-ui", "enabled").get<bool>());
    ASSERT_EQ(moonmod::ManagerActions("test.settings-ui").size(), 1u);
    const moonmod::ApiCallResult skills = moonmod::ManagerSkills("test.settings-ui");
    ASSERT_EQ(skills.status, moonmod::ApiCallStatus::Ok) << skills.message;
    ASSERT_EQ(skills.value.at("skills").size(), 2u);
    EXPECT_EQ(skills.value.at("skills").at(0).at("title"), "Focused Strike");
    EXPECT_EQ(skills.value.at("skills").at(0).at("rank"), 1);
    EXPECT_DOUBLE_EQ(skills.value.at("skills").at(0).at("effects").at("enemy_damage_per_rank").get<double>(), 0.1);
    EXPECT_EQ(skills.value.at("skills").at(0).at("max_rank"), 2);
    EXPECT_TRUE(skills.value.at("skills").at(1).at("prerequisites_met").get<bool>());
    moonmod::SharedRuntime().Pump();
    const moonmod::ApiCallResult quests = moonmod::SharedRuntime().Quests("test.settings-ui");
    ASSERT_EQ(quests.status, moonmod::ApiCallStatus::Ok) << quests.message;
    ASSERT_EQ(quests.value.at("quests").size(), 2u);
    const auto journey = std::find_if(quests.value.at("quests").begin(), quests.value.at("quests").end(),
                                      [](const moonmod::Json &quest) { return quest.value("id", std::string{}) == "test.settings-ui:journey"; });
    ASSERT_NE(journey, quests.value.at("quests").end());
    EXPECT_EQ(journey->at("status"), "active");
    EXPECT_EQ(moonmod::ManagerQuestRewardSummary(*journey), "Reward: +25 Gilda");
    EXPECT_EQ(moonmod::ManagerQuestRewardSummary(moonmod::Json{{"rewards", moonmod::Json::array({
                  moonmod::Json{{"kind", "currency"}, {"amount", 25}},
                  moonmod::Json{{"kind", "weapon"}, {"id", "test.settings-ui:moon-sword"}}})}}),
              "Rewards: +25 Gilda + Weapon: moon-sword");
    EXPECT_EQ(moonmod::ManagerQuestRewardSummary(moonmod::Json{{"rewards", moonmod::Json::array({
                  moonmod::Json{{"kind", "item"}, {"id", "test.settings-ui:moon-cookie"}, {"count", 2}}})}}),
              "Reward: 2x moon-cookie");
    EXPECT_EQ(journey->at("objectives").at(0).at("progress"), 1);
    const auto journal = moonmod::ManagerJournalQuests();
    ASSERT_EQ(journal.size(), 3u);
    EXPECT_TRUE(std::any_of(journal.begin(), journal.end(), [](const moonmod::ManagerJournalQuest &entry) {
        return entry.mod_id == "test.settings-ui" && entry.mod_name == "Settings UI Probe";
    }));
    EXPECT_TRUE(std::any_of(journal.begin(), journal.end(), [](const moonmod::ManagerJournalQuest &entry) {
        return entry.mod_id == "test.settings-ui-journal" && entry.quest.value("title", std::string{}) == "Helper Package Quest";
    }));

    moonmod::OpenManager();
    ASSERT_TRUE(moonmod::ManagerOpen());
    const auto mods = moonmod::SharedRuntime().Mods();
    const auto selected_mod = std::find_if(mods.begin(), mods.end(), [](const moonmod::ModRecord &mod) {
        return mod.id == "test.settings-ui";
    });
    ASSERT_NE(selected_mod, mods.end());
    moonmod::ManagerState().selected_mod = static_cast<int>(std::distance(mods.begin(), selected_mod));
    EXPECT_EQ(moonmod::ManagerSelectedMod()->id, "test.settings-ui");
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.next_focus = true}));
    EXPECT_TRUE(moonmod::ManagerState().settings_focus);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.right = true}));
    EXPECT_FALSE(moonmod::SharedRuntime().Setting("test.settings-ui", "enabled").get<bool>());

    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.down = true}));
    EXPECT_EQ(moonmod::ManagerState().selected_setting, 1);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.right = true}));
    EXPECT_EQ(moonmod::SharedRuntime().Setting("test.settings-ui", "amount").get<int>(), 3);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.right = true}));
    EXPECT_EQ(moonmod::SharedRuntime().Setting("test.settings-ui", "amount").get<int>(), 5);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.right = true}));
    EXPECT_EQ(moonmod::SharedRuntime().Setting("test.settings-ui", "amount").get<int>(), 5);

    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.down = true}));
    EXPECT_EQ(moonmod::ManagerState().selected_setting, 2);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.right = true}));
    EXPECT_EQ(moonmod::SharedRuntime().Setting("test.settings-ui", "mode").get<std::string>(), "hard");
    EXPECT_TRUE(std::ranges::any_of(logs, [](const std::string &message) {
        return message.find("SETTING_CHANGED:enabled:false") != std::string::npos;
    }));

    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.next_focus = true}));
    EXPECT_EQ(moonmod::ManagerState().detail_tab, 1);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.confirm = true}));
    EXPECT_TRUE(std::ranges::any_of(logs, [](const std::string &message) {
        return message.find("UI_ACTION:clear_cache") != std::string::npos;
    }));
    EXPECT_TRUE(moonmod::ManagerActions("test.settings-ui").empty());
    ASSERT_EQ(moonmod::ManagerState().detail_tab, 2);
    ASSERT_EQ(moonmod::ManagerQuests("test.settings-ui").status, moonmod::ApiCallStatus::Ok);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.left = true}));
    EXPECT_EQ(moonmod::ManagerState().selected_quest, 0);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.next_focus = true}));
    EXPECT_EQ(moonmod::ManagerState().detail_tab, 3);
    EXPECT_EQ(moonmod::ManagerState().selected_skill, 0);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.mouse_left = true, .mouse_x = 512.0f, .mouse_y = 200.0f}));
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.mouse_left = true, .mouse_x = 270.0f, .mouse_y = 292.0f}));
    EXPECT_EQ(moonmod::ManagerState().selected_skill, 1);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.next_focus = true}));
    EXPECT_EQ(moonmod::ManagerState().detail_tab, 4);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.previous_focus = true}));
    EXPECT_FALSE(moonmod::ManagerState().settings_focus);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.down = true}));
    EXPECT_EQ(moonmod::ManagerState().selected_journal_quest, 1);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.down = true}));
    EXPECT_EQ(moonmod::ManagerState().selected_journal_quest, 2);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.next_focus = true}));
    EXPECT_TRUE(moonmod::ManagerState().settings_focus);
    EXPECT_EQ(moonmod::ManagerState().first_quest_objective, 0);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.down = true}));
    EXPECT_EQ(moonmod::ManagerState().first_quest_objective, 1);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.down = true}));
    EXPECT_EQ(moonmod::ManagerState().first_quest_objective, 2);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.down = true}));
    EXPECT_EQ(moonmod::ManagerState().first_quest_objective, 2);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.up = true}));
    EXPECT_EQ(moonmod::ManagerState().first_quest_objective, 1);

    // cancel steps out of the focused pane first, then closes the manager
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.cancel = true}));
    EXPECT_TRUE(moonmod::ManagerOpen());
    EXPECT_FALSE(moonmod::ManagerState().settings_focus);
    EXPECT_TRUE(moonmod::HandleManagerInput(moonmod::ManagerInput{.cancel = true}));
    EXPECT_FALSE(moonmod::ManagerOpen());
    std::string toggle_error;
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.settings-ui", false, toggle_error)) << toggle_error;
    const moonmod::ApiCallResult disabled_quests = moonmod::SharedRuntime().Quests("test.settings-ui");
    ASSERT_EQ(disabled_quests.status, moonmod::ApiCallStatus::Ok) << disabled_quests.message;
    const auto disabled_journey = std::find_if(disabled_quests.value.at("quests").begin(), disabled_quests.value.at("quests").end(),
                                               [](const moonmod::Json &quest) {
                                                   return quest.value("id", std::string{}) == "test.settings-ui:journey";
                                               });
    ASSERT_NE(disabled_journey, disabled_quests.value.at("quests").end());
    EXPECT_EQ(disabled_journey->at("objectives").at(0).at("progress"), 1);
    EXPECT_EQ(moonmod::ManagerJournalQuests().size(), 3u);
    const moonmod::ApiCallResult disabled_skills = moonmod::ManagerSkills("test.settings-ui");
    ASSERT_EQ(disabled_skills.status, moonmod::ApiCallStatus::Ok) << disabled_skills.message;
    EXPECT_EQ(disabled_skills.value.at("skills").at(0).at("rank"), 1);
    ASSERT_TRUE(moonmod::SharedRuntime().SetEnabled("test.settings-ui", true, toggle_error)) << toggle_error;
    moonmod::SharedRuntime().Stop();
    EXPECT_TRUE(moonmod::ManagerActions("test.settings-ui").empty());
    ASSERT_TRUE(moonmod::SharedRuntime().Start(config, error)) << error;
    EXPECT_EQ(moonmod::ManagerActions("test.settings-ui").size(), 1u);
    const moonmod::ApiCallResult persisted_quests = moonmod::SharedRuntime().Quests("test.settings-ui");
    ASSERT_EQ(persisted_quests.status, moonmod::ApiCallStatus::Ok) << persisted_quests.message;
    EXPECT_EQ(persisted_quests.value.at("quests").at(0).at("objectives").at(0).at("progress"), 1);
    const moonmod::ApiCallResult persisted_skills = moonmod::SharedRuntime().Skills("test.settings-ui");
    ASSERT_EQ(persisted_skills.status, moonmod::ApiCallStatus::Ok) << persisted_skills.message;
    EXPECT_EQ(persisted_skills.value.at("skills").at(0).at("rank"), 1);
    EXPECT_FALSE(moonmod::SharedRuntime().Setting("test.settings-ui", "enabled").get<bool>());
    EXPECT_EQ(moonmod::SharedRuntime().Setting("test.settings-ui", "amount").get<int>(), 5);
    EXPECT_EQ(moonmod::SharedRuntime().Setting("test.settings-ui", "mode").get<std::string>(), "hard");

    std::ifstream profile_file(root.path / "save" / "moonmod" / "profile.json", std::ios::binary);
    ASSERT_TRUE(profile_file.good());
    moonmod::Json profile;
    profile_file >> profile;
    EXPECT_FALSE(profile.at("settings").at("test.settings-ui").at("enabled").get<bool>());
    EXPECT_EQ(profile.at("settings").at("test.settings-ui").at("amount").get<int>(), 5);
    EXPECT_EQ(profile.at("settings").at("test.settings-ui").at("mode").get<std::string>(), "hard");
    moonmod::SharedRuntime().Stop();
    EXPECT_TRUE(moonmod::ManagerActions("test.settings-ui").empty());
}

TEST(MoonModManagerUi, CharacterSheetFromLuaFeedsTheJournalPage) {
    moonmod::SharedRuntime().Stop();
    moonmod::CloseManager();
    TempRoot root;
    const fs::path package = root.path / "mods" / "test.sheet";
    Write(package / "mod.json", R"json({
      "schema_version":1,"id":"test.sheet","name":"Sheet Probe","version":"1.0.0",
      "framework_api":1,"games":["chronicle"],"entry":"main.lua","permissions":["content.register","game.read","game.write"]
    })json");
    Write(package / "main.lua", R"lua(
assert(game.skills.register({
  id = "sword", title = "SWORDSMAN",
  skills = {
    {id = "keen_edge", title = "Keen Edge", description = "+3% damage per rank.", max_rank = 5},
    {id = "quick_blade", title = "Quick Blade", description = "Faster swings.", max_rank = 3,
     prerequisites = {{id = "keen_edge", rank = 1}}}
  }
}))
local set = assert(game.ui.sheet_set({title = "TOAN", lines = {"Level 3", "XP 40 / 128"}, progress = 0.3125,
                                      progress_label = "XP 40 / 128", hint = "2 skill points to spend", trees = {"sword"}}))
assert(set.owner == "test.sheet" and set.status == "set")
local bad, bad_error = game.ui.sheet_set({title = "TOAN", progress = 2})
assert(bad == nil and bad_error.code == "invalid_argument")
local unknown, unknown_error = game.ui.sheet_set({title = "TOAN", colour = "red"})
assert(unknown == nil and unknown_error.code == "invalid_argument")
moonmod.on("skills.train_requested", function(event)
  moonmod.log("info", "TRAIN:" .. tostring(event.id))
  if event.id == "test.sheet:sword.keen_edge" then assert(game.skills.set_rank({id = "sword.keen_edge", rank = 1})) end
end)
)lua");
    Write(root.path / "save" / "moonmod" / "profile.json",
          R"json({"format_version":1,"enabled":{"test.sheet":true},"settings":{}})json");

    std::vector<std::string> logs;
    moonmod::RuntimeConfig   config;
    config.game_id = "chronicle";
    config.game_name = "Chronicle";
    config.mod_directories = {root.path / "mods"};
    config.save_root = root.path / "save";
    config.log = [&](std::string_view, std::string_view, std::string_view message) { logs.emplace_back(message); };
    std::string error;
    ASSERT_TRUE(moonmod::SharedRuntime().Start(config, error)) << error;
    ASSERT_EQ(moonmod::SharedRuntime().Mods().size(), 1u);
    ASSERT_EQ(moonmod::SharedRuntime().Mods()[0].state, moonmod::ModState::Loaded);

    const moonmod::CharacterSheet *sheet = moonmod::ActiveCharacterSheet();
    ASSERT_NE(sheet, nullptr);
    EXPECT_EQ(sheet->owner_id, "test.sheet");
    EXPECT_EQ(sheet->title, "TOAN");
    ASSERT_EQ(sheet->lines.size(), 2u);
    EXPECT_EQ(sheet->lines[1], "XP 40 / 128");
    EXPECT_DOUBLE_EQ(sheet->progress, 0.3125);
    EXPECT_EQ(sheet->hint, "2 skill points to spend");
    ASSERT_EQ(sheet->trees.size(), 1u);
    EXPECT_EQ(sheet->trees[0], "test.sheet:sword");

    // Cross on a node reaches the mod as skills.train_requested, and the mod's rank change shows in the listing.
    ASSERT_TRUE(moonmod::RequestSkillTraining("test.sheet", "test.sheet:sword.keen_edge"));
    for (int i = 0; i < 4; ++i) moonmod::SharedRuntime().Pump();
    EXPECT_TRUE(std::ranges::any_of(logs, [](const std::string &message) {
        return message.find("TRAIN:test.sheet:sword.keen_edge") != std::string::npos;
    }));
    const moonmod::ApiCallResult skills = moonmod::ManagerSkills("test.sheet");
    ASSERT_EQ(skills.status, moonmod::ApiCallStatus::Ok) << skills.message;
    ASSERT_EQ(skills.value["skills"].size(), 2u);
    EXPECT_EQ(skills.value["skills"][0].value("rank", 0), 1);
    EXPECT_TRUE(skills.value["skills"][1].value("prerequisites_met", false));

    // A stopped runtime shows no sheet.
    moonmod::SharedRuntime().Stop();
    EXPECT_EQ(moonmod::ActiveCharacterSheet(), nullptr);
}

TEST(MoonModManagerUi, GameNoticesQueueAndCountDownOnlyWhileShown) {
    // Drain anything an earlier test left.
    std::string text;
    std::string icon;
    for (int frame = 0; frame < 20000 && moonmod::CurrentGameNotice(text, icon); ++frame) {
        moonmod::MarkGameNoticeShown();
        moonmod::AdvanceManagerUi();
    }
    ASSERT_FALSE(moonmod::CurrentGameNotice(text, icon));

    moonmod::ShowManagerNotice("Next floor is cursed.", 300, "curse");
    moonmod::ShowManagerNotice("Miracle cache!", 300);
    ASSERT_TRUE(moonmod::CurrentGameNotice(text, icon));
    EXPECT_EQ(text, "Next floor is cursed.");
    EXPECT_EQ(icon, "curse");
    EXPECT_EQ(moonmod::GameToastLines(), std::vector<std::string>{"Next floor is cursed."});

    // Not drawn (a menu is open, the stairs are still being walked): it waits.
    for (int frame = 0; frame < 400; ++frame) moonmod::AdvanceManagerUi();
    ASSERT_TRUE(moonmod::CurrentGameNotice(text, icon));
    EXPECT_EQ(text, "Next floor is cursed.");

    // Drawn for its 300 frames, it goes, and the next one takes its place.
    for (int frame = 0; frame < 300; ++frame) {
        moonmod::MarkGameNoticeShown();
        moonmod::AdvanceManagerUi();
    }
    ASSERT_TRUE(moonmod::CurrentGameNotice(text, icon));
    EXPECT_EQ(text, "Miracle cache!");
    EXPECT_TRUE(icon.empty());
    for (int frame = 0; frame < 300; ++frame) {
        moonmod::MarkGameNoticeShown();
        moonmod::AdvanceManagerUi();
    }
    EXPECT_FALSE(moonmod::CurrentGameNotice(text, icon));
}

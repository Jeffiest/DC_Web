#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "moonmod/runtime.hpp"

namespace moonmod {

inline constexpr int kManagerVisibleMods = 7;
inline constexpr int kManagerVisibleSettings = 5;
inline constexpr int kManagerVisibleQuestObjectives = 4;
inline constexpr int kManagerVisibleJournalObjectives = 4;
inline constexpr int kManagerVisibleSkills = 4;

struct ManagerUiAction {
    std::string id;
    std::string owner_id;
    std::string label;
    std::string description;
};

struct ManagerJournalQuest {
    std::string mod_id;
    std::string mod_name;
    Json        quest;
};

struct ManagerUiState {
    bool        open = false;
    bool        settings_focus = false;
    bool        native_trust_focus = false;
    bool        native_trust_confirmation = false;
    bool        pending_native_trust = false;
    bool        native_menu_context = false;
    int         detail_tab = 0;
    int         selected_mod = 0;
    int         first_mod = 0;
    int         selected_setting = 0;
    int         first_setting = 0;
    int         selected_action = 0;
    int         first_action = 0;
    int         selected_quest = 0;
    int         first_quest_objective = 0;
    int         selected_skill = 0;
    int         first_skill = 0;
    int         selected_journal_quest = 0;
    int         first_journal_quest = 0;
    std::string notice;
    int         notice_frames = 0;
};

struct ManagerInput {
    bool up = false;
    bool down = false;
    bool left = false;
    bool right = false;
    bool confirm = false;
    bool cancel = false;
    bool next_focus = false;
    bool previous_focus = false;
    bool mouse_left = false;
    bool mouse_right = false;
    float mouse_x = -1.0f;
    float mouse_y = -1.0f;
    float wheel = 0.0f;
};

ManagerUiState &ManagerState();
void           OpenManager();
void           CloseManager();
bool           ManagerOpen();
void           SetManagerNativeMenuContext(bool enabled);
// `icon` (ui.notify's) is a native status badge name or a png game path, drawn beside the notice in a dungeon.
void           ShowManagerNotice(std::string message, int duration_frames, std::string icon = {});
// The notice on screen over gameplay: notices queue and show one at a time, each for its own frames.
std::vector<std::string> GameToastLines();
// The notice on screen and its icon; false when there is none.
bool CurrentGameNotice(std::string &text, std::string &icon);
// Says the notice was on screen this frame: a notice only counts down while something draws it.
void MarkGameNoticeShown();
// True once after a mod opened a manager page outside the menu (the menu must open to show it).
bool TakeManagerMenuOpenRequest();
// The same, without consuming it: the host checks it can open the menu first.
bool PeekManagerMenuOpenRequest();
// Where the native-trust modal drew its Confirm and Cancel buttons (top edges, logical pixels).
void SetManagerTrustButtons(int confirm_y, int cancel_y);
// True once after a page a mod opened during play (hotkey) was closed: the host menu closes too.
bool TakeManagerHostCloseRequest();
void           AdvanceManagerUi();
bool           HandleManagerInput(const ManagerInput &input);
const ModRecord *ManagerSelectedMod();
bool RegisterManagerAction(std::string_view owner_id, std::string_view local_id, std::string label,
                           std::string description, std::string &error);
bool RemoveManagerAction(std::string_view owner_id, std::string_view local_id);
void RemoveManagerActions(std::string_view owner_id);
std::vector<ManagerUiAction> ManagerActions(std::string_view owner_id);
ApiCallResult ManagerQuests(std::string_view owner_id);
std::vector<ManagerJournalQuest> ManagerJournalQuests();
std::string ManagerQuestRewardSummary(const Json &quest);
ApiCallResult ManagerSkills(std::string_view owner_id);
bool ActivateManagerAction(std::string_view owner_id, std::string_view local_id);
bool NavigateManager(std::string_view mod_id, std::string_view tab, std::string &error);

// A mod's character sheet: the header of the Journal's Character page, beside that mod's skill tree.
struct CharacterSheet {
    std::string              owner_id;
    std::string              title;          // the banner, "JOURNAL: <title>"
    std::vector<std::string> lines;          // up to six short lines (level, XP, points...)
    std::string              progress_label; // text on the progress bar, if any
    double                   progress = -1.0; // 0..1 draws a bar; below 0 draws none
    std::string              hint;           // one line under the lines (what to do next)
    std::vector<std::string> trees;          // local tree ids in the order the page shows them (others follow)
};
bool SetCharacterSheet(std::string_view owner_id, CharacterSheet sheet, std::string &error);
bool ClearCharacterSheet(std::string_view owner_id);
// The sheet the Journal shows: the last one set whose mod is still loaded, or null.
const CharacterSheet *ActiveCharacterSheet();
// Asks the owning mod to train one skill node (publishes skills.train_requested).
bool RequestSkillTraining(std::string_view owner_id, std::string_view skill_id);
// A mod asks for the Journal's Character page: opens the menu during play, then that page.
void RequestCharacterPageOpen();
bool TakeCharacterPageOpenRequest();

} // namespace moonmod

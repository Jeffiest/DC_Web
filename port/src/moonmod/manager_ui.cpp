#include "moonmod/manager_ui.hpp"

#include <algorithm>
#include <cstdio>
#include <iterator>
#include <map>
#include <ranges>
#include <string>
#include <string_view>



namespace moonmod {
namespace {

ManagerUiState g_state;
std::map<std::string, ManagerUiAction, std::less<>> g_actions;

bool ValidActionId(std::string_view value) {
    return !value.empty() && value.size() <= 64 &&
           std::ranges::all_of(value, [](unsigned char character) {
               return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                      (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                      character == '-';
           });
}

bool SafeActionText(std::string_view value, std::size_t maximum, bool allow_empty) {
    return (allow_empty || !value.empty()) && value.size() <= maximum &&
           std::ranges::none_of(value, [](unsigned char character) { return character < 0x20 || character == 0x7f; });
}

int FirstDetailTab(bool settings, bool actions, bool quests, bool skills, bool journal) {
    if (settings) return 0;
    if (actions) return 1;
    if (quests) return 2;
    if (skills) return 3;
    if (journal) return 4;
    return 0;
}

int AdjacentDetailTab(int current, int direction, bool settings, bool actions, bool quests, bool skills, bool journal) {
    const auto available = [=](int tab) {
        switch (tab) {
            case 0: return settings;
            case 1: return actions;
            case 2: return quests;
            case 3: return skills;
            case 4: return journal;
            default: return false;
        }
    };
    for (int offset = 1; offset <= 5; ++offset) {
        const int candidate = (current + direction * offset + 10) % 5;
        if (available(candidate)) return candidate;
    }
    return current;
}

void Reconcile() {
    const auto mods = SharedRuntime().Mods();
    if (mods.empty()) {
        g_state.selected_mod = 0;
        g_state.first_mod = 0;
        g_state.selected_setting = 0;
        g_state.first_setting = 0;
        g_state.selected_quest = 0;
        g_state.first_quest_objective = 0;
        g_state.selected_skill = 0;
        g_state.first_skill = 0;
        g_state.selected_journal_quest = 0;
        g_state.first_journal_quest = 0;
        g_state.settings_focus = false;
        g_state.detail_tab = 0;
        return;
    }
    g_state.selected_mod = std::clamp(g_state.selected_mod, 0, static_cast<int>(mods.size()) - 1);
    g_state.first_mod = std::clamp(g_state.first_mod,
                                   std::max(0, g_state.selected_mod - kManagerVisibleMods + 1),
                                   g_state.selected_mod);
    const int setting_count = static_cast<int>(mods[g_state.selected_mod].manifest.settings.size());
    const auto actions = ManagerActions(mods[g_state.selected_mod].id);
    const ApiCallResult quests = ManagerQuests(mods[g_state.selected_mod].id);
    const ApiCallResult skills = ManagerSkills(mods[g_state.selected_mod].id);
    const auto journal_quests = ManagerJournalQuests();
    const int quest_count = quests.status == ApiCallStatus::Ok && quests.value.is_object() &&
                                    quests.value.contains("quests") && quests.value["quests"].is_array()
                                ? static_cast<int>(quests.value["quests"].size())
                                : 0;
    const int skill_count = skills.status == ApiCallStatus::Ok && skills.value.is_object() &&
                                    skills.value.contains("skills") && skills.value["skills"].is_array()
                                ? static_cast<int>(skills.value["skills"].size())
                                : 0;
    if (mods[g_state.selected_mod].manifest.native_library.empty()) {
        g_state.native_trust_focus = false;
        g_state.native_trust_confirmation = false;
    }
    const bool has_settings = setting_count > 0;
    const bool has_actions = !actions.empty();
    const bool has_quests = quest_count > 0;
    const bool has_skills = skill_count > 0;
    const bool has_journal = !journal_quests.empty();
    if (!has_settings && !has_actions && !has_quests && !has_skills && !has_journal) g_state.settings_focus = false;
    if (g_state.detail_tab == 4 && !has_journal) {
        g_state.detail_tab = FirstDetailTab(has_settings, has_actions, has_quests, has_skills, false);
    }
    // an emptied tab moves on to the next one that has something, as the input handler does
    if (g_state.detail_tab == 1 && !has_actions && (has_quests || has_skills)) g_state.detail_tab = has_quests ? 2 : 3;
    if ((g_state.detail_tab == 0 && !has_settings) || (g_state.detail_tab == 1 && !has_actions) ||
        (g_state.detail_tab == 2 && !has_quests) || (g_state.detail_tab == 3 && !has_skills) ||
        (g_state.detail_tab == 4 && !has_journal)) {
        g_state.detail_tab = FirstDetailTab(has_settings, has_actions, has_quests, has_skills, false);
    }
    g_state.selected_setting = setting_count == 0 ? 0 : std::clamp(g_state.selected_setting, 0, setting_count - 1);
    g_state.first_setting = setting_count == 0
                                ? 0
                                : std::clamp(g_state.first_setting,
                                             std::max(0, g_state.selected_setting - kManagerVisibleSettings + 1),
                                             g_state.selected_setting);
    g_state.selected_action = actions.empty() ? 0 : std::clamp(g_state.selected_action, 0, static_cast<int>(actions.size()) - 1);
    g_state.first_action = actions.empty()
                               ? 0
                               : std::clamp(g_state.first_action,
                                            std::max(0, g_state.selected_action - kManagerVisibleSettings + 1),
                                            g_state.selected_action);
    g_state.selected_quest = quest_count == 0 ? 0 : std::clamp(g_state.selected_quest, 0, quest_count - 1);
    g_state.selected_skill = skill_count == 0 ? 0 : std::clamp(g_state.selected_skill, 0, skill_count - 1);
    g_state.first_skill = skill_count == 0 ? 0 : std::clamp(g_state.first_skill,
        std::max(0, g_state.selected_skill - kManagerVisibleSkills + 1), g_state.selected_skill);
    const int journal_count = static_cast<int>(journal_quests.size());
    g_state.selected_journal_quest = journal_count == 0 ? 0 : std::clamp(g_state.selected_journal_quest, 0, journal_count - 1);
    g_state.first_journal_quest = journal_count == 0
                                      ? 0
                                      : std::clamp(g_state.first_journal_quest,
                                                   std::max(0, g_state.selected_journal_quest - kManagerVisibleMods + 1),
                                                   g_state.selected_journal_quest);
    int objective_count = quest_count == 0 ? 0 : static_cast<int>(quests.value["quests"][g_state.selected_quest]
                                                                          .value("objectives", Json::array()).size());
    if (g_state.detail_tab == 4 && journal_count > 0) {
        objective_count = static_cast<int>(journal_quests[static_cast<std::size_t>(g_state.selected_journal_quest)]
                                               .quest.value("objectives", Json::array()).size());
    }
    const int visible_objectives = g_state.detail_tab == 4 ? kManagerVisibleJournalObjectives : kManagerVisibleQuestObjectives;
    g_state.first_quest_objective = objective_count == 0
                                        ? 0
                                        : std::clamp(g_state.first_quest_objective, 0,
                                                     std::max(0, objective_count - visible_objectives));
}

void Notice(std::string message) {
    g_state.notice = std::move(message);
    g_state.notice_frames = 240;
}

void MoveMod(int direction) {
    const auto mods = SharedRuntime().Mods();
    if (mods.empty()) return;
    g_state.selected_mod = std::clamp(g_state.selected_mod + direction, 0, static_cast<int>(mods.size()) - 1);
    g_state.selected_setting = 0;
    g_state.first_setting = 0;
    g_state.selected_action = 0;
    g_state.first_action = 0;
    g_state.selected_quest = 0;
    g_state.first_quest_objective = 0;
    g_state.selected_skill = 0;
    g_state.first_skill = 0;
    g_state.detail_tab = 0;
    g_state.native_trust_focus = false;
    Reconcile();
}

void MoveSetting(int direction) {
    const ModRecord *record = ManagerSelectedMod();
    if (record == nullptr || record->manifest.settings.empty()) return;
    g_state.selected_setting = std::clamp(g_state.selected_setting + direction, 0,
                                          static_cast<int>(record->manifest.settings.size()) - 1);
    Reconcile();
}

void MoveAction(int direction) {
    const ModRecord *record = ManagerSelectedMod();
    if (record == nullptr) return;
    const auto actions = ManagerActions(record->id);
    if (actions.empty()) return;
    g_state.selected_action = std::clamp(g_state.selected_action + direction, 0, static_cast<int>(actions.size()) - 1);
    Reconcile();
}

void ToggleSelectedMod() {
    const ModRecord *record = ManagerSelectedMod();
    if (record == nullptr || !record->valid_manifest) {
        Notice(record == nullptr ? "No package is selected" : record->status_message);
        return;
    }
    std::string error;
    if (!SharedRuntime().SetEnabled(record->id, !record->enabled_next_launch, error)) {
        Notice(error);
        return;
    }
    g_state.settings_focus = false;
    g_state.native_trust_focus = false;
    g_state.detail_tab = 0;
    const ModRecord *updated = ManagerSelectedMod();
    Notice(updated && updated->restart_required
               ? (updated->enabled_next_launch ? "Will enable after game restart" : "Will disable after game restart")
               : "Restart change canceled; current session unchanged");
    Reconcile();
}

void RequestNativeTrustConfirmation() {
    const ModRecord *record = ManagerSelectedMod();
    if (record == nullptr || record->manifest.native_library.empty()) {
        Notice("This package has no native extension");
        return;
    }
    g_state.native_trust_confirmation = true;
    g_state.pending_native_trust = !record->native_trusted;
}

void ConfirmNativeTrustChange() {
    const ModRecord *record = ManagerSelectedMod();
    if (record == nullptr) {
        g_state.native_trust_confirmation = false;
        Notice("The selected package is unavailable");
        return;
    }
    const std::string id = record->id;
    const bool trust = g_state.pending_native_trust;
    std::string error;
    g_state.native_trust_confirmation = false;
    if (!SharedRuntime().SetNativeTrusted(id, trust, error)) {
        Notice(error);
        return;
    }
    Notice(trust ? "Native extension trusted for this library" : "Native extension trust removed");
    Reconcile();
}

void SelectDetailTab(int tab) {
    const ModRecord *record = ManagerSelectedMod();
    if (record == nullptr) return;
    const bool settings_available = !record->manifest.settings.empty();
    const bool actions_available = !ManagerActions(record->id).empty();
    const ApiCallResult quests = ManagerQuests(record->id);
    const bool quests_available = quests.status == ApiCallStatus::Ok && quests.value.is_object() &&
                                  quests.value.contains("quests") && quests.value["quests"].is_array() &&
                                  !quests.value["quests"].empty();
    const ApiCallResult skills = ManagerSkills(record->id);
    const bool skills_available = skills.status == ApiCallStatus::Ok && skills.value.is_object() &&
                                  skills.value.contains("skills") && skills.value["skills"].is_array() &&
                                  !skills.value["skills"].empty();
    if (tab == 4 && !ManagerJournalQuests().empty()) {
        g_state.detail_tab = 4;
        g_state.settings_focus = true;
        g_state.native_trust_focus = false;
        return;
    }
    g_state.native_trust_focus = false;
    if (tab == 0 && settings_available) {
        g_state.detail_tab = 0;
        g_state.settings_focus = true;
    } else if (tab == 1 && actions_available) {
        g_state.detail_tab = 1;
        g_state.settings_focus = true;
    } else if (tab == 2 && quests_available) {
        g_state.detail_tab = 2;
        g_state.settings_focus = true;
    } else if (tab == 3 && skills_available) {
        g_state.detail_tab = 3;
        g_state.settings_focus = true;
    }
}

void MoveJournalQuest(int direction) {
    const int count = static_cast<int>(ManagerJournalQuests().size());
    if (count == 0) return;
    g_state.selected_journal_quest = std::clamp(g_state.selected_journal_quest + direction, 0, count - 1);
    g_state.first_quest_objective = 0;
    Reconcile();
}

void MoveJournalObjective(int direction) {
    const auto quests = ManagerJournalQuests();
    if (g_state.selected_journal_quest < 0 || g_state.selected_journal_quest >= static_cast<int>(quests.size())) return;
    const Json objectives = quests[static_cast<std::size_t>(g_state.selected_journal_quest)].quest.value("objectives", Json::array());
    if (!objectives.is_array() || objectives.empty()) return;
    g_state.first_quest_objective = std::clamp(
        g_state.first_quest_objective + direction, 0,
        std::max(0, static_cast<int>(objectives.size()) - kManagerVisibleJournalObjectives));
}

void MoveQuest(int direction) {
    const ModRecord *record = ManagerSelectedMod();
    if (record == nullptr) return;
    const ApiCallResult quests = ManagerQuests(record->id);
    if (quests.status != ApiCallStatus::Ok || !quests.value.is_object() ||
        !quests.value.contains("quests") || !quests.value["quests"].is_array() || quests.value["quests"].empty()) return;
    g_state.selected_quest = std::clamp(g_state.selected_quest + direction, 0,
                                        static_cast<int>(quests.value["quests"].size()) - 1);
    g_state.first_quest_objective = 0;
    Reconcile();
}

void MoveQuestObjective(int direction) {
    const ModRecord *record = ManagerSelectedMod();
    if (record == nullptr) return;
    const ApiCallResult quests = ManagerQuests(record->id);
    if (quests.status != ApiCallStatus::Ok || !quests.value.is_object() ||
        !quests.value.contains("quests") || !quests.value["quests"].is_array() ||
        g_state.selected_quest < 0 || g_state.selected_quest >= static_cast<int>(quests.value["quests"].size())) return;
    const Json objectives = quests.value["quests"][g_state.selected_quest].value("objectives", Json::array());
    if (!objectives.is_array() || objectives.empty()) return;
    const int next = std::clamp(g_state.first_quest_objective + direction, 0,
                                std::max(0, static_cast<int>(objectives.size()) - kManagerVisibleQuestObjectives));
    g_state.first_quest_objective = next;
}

void MoveSkill(int direction) {
    const ModRecord *record = ManagerSelectedMod();
    if (record == nullptr) return;
    const ApiCallResult result = ManagerSkills(record->id);
    if (result.status != ApiCallStatus::Ok || !result.value.is_object() ||
        !result.value.contains("skills") || !result.value["skills"].is_array() || result.value["skills"].empty()) return;
    const auto &skills=result.value["skills"];
    const auto tree=skills[g_state.selected_skill].value("tree_title",std::string{});
    for(int next=g_state.selected_skill+direction;next>=0 && next<int(skills.size());next+=direction) {
        if(skills[next].value("tree_title",std::string{})==tree){g_state.selected_skill=next;break;}
    }
    Reconcile();
}

void StepSelectedSetting(int direction) {
    const ModRecord *record = ManagerSelectedMod();
    if (record == nullptr || g_state.selected_setting < 0 ||
        g_state.selected_setting >= static_cast<int>(record->manifest.settings.size())) {
        return;
    }
    const SettingDefinition &setting = record->manifest.settings[g_state.selected_setting];
    std::string error;
    if (!SharedRuntime().StepSetting(record->id, setting.id, direction, error)) {
        if (!error.empty()) Notice(error);
    } else {
        Notice(setting.name + " updated");
    }
}

void ActivateSelectedAction() {
    const ModRecord *record = ManagerSelectedMod();
    if (record == nullptr) return;
    const auto actions = ManagerActions(record->id);
    if (g_state.selected_action < 0 || g_state.selected_action >= static_cast<int>(actions.size())) return;
    const ManagerUiAction &action = actions[static_cast<std::size_t>(g_state.selected_action)];
    if (action.id.size() <= record->id.size() + 1 ||
        !ActivateManagerAction(record->id, std::string_view(action.id).substr(record->id.size() + 1))) {
        Notice("This mod action is no longer available");
    }
}

int RowAt(float y, float top, float step, int visible, int first, int total) {
    if (y < top || y >= top + step * visible) return -1;
    const int index = first + static_cast<int>((y - top) / step);
    return index < total ? index : -1;
}

} // namespace

ManagerUiState &ManagerState() { return g_state; }

namespace {
// The open page came from a mod during play: one cancel goes straight back to the game.
bool g_opened_in_play = false;
bool g_host_close_request = false;
} // namespace

void OpenManager() {
    std::string notice = std::move(g_state.notice);
    const int notice_frames = g_state.notice_frames;
    g_state = {};
    g_state.open = true;
    if (notice_frames > 0) {
        g_state.notice = std::move(notice);
        g_state.notice_frames = notice_frames;
    }
    Reconcile();
}

void CloseManager() {
    g_opened_in_play = false;
    g_state.open = false;
    g_state.settings_focus = false;
    g_state.notice.clear();
    g_state.notice_frames = 0;
}

bool ManagerOpen() { return g_state.open; }

void SetManagerNativeMenuContext(bool enabled) { g_state.native_menu_context = enabled; }

namespace {
// Notices also show over gameplay (the manager footer alone is invisible while playing).
struct GameToast {
    std::string text;
    int frames = 0;
    std::string icon;
};
std::vector<GameToast> g_game_toasts; // a queue: only the first is on screen and counts down
bool                   g_game_notice_shown = false;
// Set when a mod opens a manager page during play: the dungeon loop opens its menu, where the manager draws.
bool g_menu_open_request = false;
// Top edges of the native-trust modal's Confirm and Cancel buttons, as last drawn.
int g_trust_confirm_y = -100;
int g_trust_cancel_y = -100;
} // namespace

bool TakeManagerHostCloseRequest() {
    const bool requested = g_host_close_request;
    g_host_close_request = false;
    return requested;
}

void SetManagerTrustButtons(int confirm_y, int cancel_y) {
    g_trust_confirm_y = confirm_y;
    g_trust_cancel_y = cancel_y;
}

bool TakeManagerMenuOpenRequest() {
    const bool requested = g_menu_open_request && g_state.open;
    g_menu_open_request = false;
    return requested;
}

bool PeekManagerMenuOpenRequest() { return g_menu_open_request && g_state.open; }

namespace {
std::vector<CharacterSheet> g_sheets;
bool g_character_page_request = false;
} // namespace

bool SetCharacterSheet(std::string_view owner_id, CharacterSheet sheet, std::string &error) {
    if (owner_id.empty()) { error = "a character sheet needs an owner"; return false; }
    if (sheet.title.empty() || sheet.title.size() > 32) { error = "title must be 1-32 bytes"; return false; }
    if (sheet.lines.size() > 6) { error = "at most six lines"; return false; }
    for (const std::string &line : sheet.lines) {
        if (line.size() > 48) { error = "each line must be at most 48 bytes"; return false; }
    }
    if (sheet.progress_label.size() > 32) { error = "progress_label must be at most 32 bytes"; return false; }
    if (sheet.hint.size() > 80) { error = "hint must be at most 80 bytes"; return false; }
    if (sheet.trees.size() > 32) { error = "at most 32 tree ids"; return false; }
    for (const std::string &tree : sheet.trees) {
        if (tree.empty() || tree.size() > 64) { error = "each tree id must be 1-64 bytes"; return false; }
    }
    if (sheet.progress > 1.0) sheet.progress = 1.0;
    sheet.owner_id = std::string(owner_id);
    std::erase_if(g_sheets, [&](const CharacterSheet &existing) { return existing.owner_id == owner_id; });
    g_sheets.push_back(std::move(sheet));
    return true;
}

bool ClearCharacterSheet(std::string_view owner_id) {
    return std::erase_if(g_sheets, [&](const CharacterSheet &existing) { return existing.owner_id == owner_id; }) != 0;
}

const CharacterSheet *ActiveCharacterSheet() {
    const auto mods = SharedRuntime().Mods();
    for (auto it = g_sheets.rbegin(); it != g_sheets.rend(); ++it) {
        const auto record = std::ranges::find(mods, it->owner_id, &ModRecord::id);
        if (record != mods.end() && record->enabled && record->state == ModState::Loaded) return &*it;
    }
    return nullptr;
}

bool RequestSkillTraining(std::string_view owner_id, std::string_view skill_id) {
    std::string local(skill_id);
    const std::string prefix = std::string(owner_id) + ":";
    if (local.starts_with(prefix)) local.erase(0, prefix.size());
    return ActivateManagerAction(owner_id, "train_" + local) ||
           SharedRuntime().PublishToMod(owner_id, "skills.train_requested", {{"id", std::string(skill_id)}});
}

void RequestCharacterPageOpen() {
    g_character_page_request = true;
    if (!g_state.open) {
        OpenManager();
        g_menu_open_request = true;
        g_opened_in_play = true;
    }
}

bool TakeCharacterPageOpenRequest() {
    const bool requested = g_character_page_request;
    g_character_page_request = false;
    return requested;
}

void ShowManagerNotice(std::string message, int duration_frames, std::string icon) {
    const int frames = std::clamp(duration_frames, 1, 3600);
    std::erase_if(g_game_toasts, [&message](const GameToast &toast) { return toast.text == message; });
    g_game_toasts.push_back({message, std::max(frames, 180), std::move(icon)});
    if (g_game_toasts.size() > 4) g_game_toasts.erase(g_game_toasts.begin());
    g_state.notice = std::move(message);
    g_state.notice_frames = frames;
}

std::vector<std::string> GameToastLines() {
    if (g_game_toasts.empty()) return {};
    return {g_game_toasts.front().text};
}

bool CurrentGameNotice(std::string &text, std::string &icon) {
    if (g_game_toasts.empty()) return false;
    text = g_game_toasts.front().text;
    icon = g_game_toasts.front().icon;
    return true;
}

void MarkGameNoticeShown() { g_game_notice_shown = true; }

void AdvanceManagerUi() {
    if (g_state.notice_frames > 0 && --g_state.notice_frames == 0) g_state.notice.clear();
    if (!g_game_toasts.empty() && g_game_notice_shown && --g_game_toasts.front().frames <= 0) {
        g_game_toasts.erase(g_game_toasts.begin());
    }
    g_game_notice_shown = false;
}

const ModRecord *ManagerSelectedMod() {
    const auto mods = SharedRuntime().Mods();
    if (mods.empty() || g_state.selected_mod < 0 || g_state.selected_mod >= static_cast<int>(mods.size())) return nullptr;
    return &mods[g_state.selected_mod];
}

bool RegisterManagerAction(std::string_view owner_id, std::string_view local_id, std::string label,
                           std::string description, std::string &error) {
    if (owner_id.empty() || owner_id.size() > 128 || !ValidActionId(local_id)) {
        error = "action id must contain 1-64 ASCII letters, digits, '.', '_' or '-'";
        return false;
    }
    if (!SafeActionText(label, 48, false) || !SafeActionText(description, 120, true)) {
        error = "action label must contain 1-48 bytes and description at most 120 bytes without control characters";
        return false;
    }
    const std::size_t owner_count = static_cast<std::size_t>(std::ranges::count_if(
        g_actions, [owner_id](const auto &entry) { return entry.second.owner_id == owner_id; }));
    if (owner_count >= 12 || g_actions.size() >= 256) {
        error = owner_count >= 12 ? "a mod may register at most 12 manager actions"
                                  : "the manager supports at most 256 registered actions";
        return false;
    }
    std::string qualified_id(owner_id);
    qualified_id.push_back(':');
    qualified_id.append(local_id);
    if (g_actions.contains(qualified_id)) {
        error = "action id is already registered by this mod";
        return false;
    }
    g_actions.emplace(qualified_id, ManagerUiAction{qualified_id, std::string(owner_id),
                                                    std::move(label), std::move(description)});
    error.clear();
    Reconcile();
    return true;
}

bool RemoveManagerAction(std::string_view owner_id, std::string_view local_id) {
    if (owner_id.empty() || local_id.empty()) return false;
    std::string qualified_id(owner_id);
    qualified_id.push_back(':');
    qualified_id.append(local_id);
    const bool removed = g_actions.erase(qualified_id) != 0;
    if (removed) Reconcile();
    return removed;
}

void RemoveManagerActions(std::string_view owner_id) {
    if (owner_id.empty()) return;
    std::erase_if(g_actions, [owner_id](const auto &entry) { return entry.second.owner_id == owner_id; });
    Reconcile();
}

std::vector<ManagerUiAction> ManagerActions(std::string_view owner_id) {
    std::vector<ManagerUiAction> actions;
    for (const auto &[id, action] : g_actions) {
        (void)id;
        if (action.owner_id == owner_id) actions.push_back(action);
    }
    return actions;
}

ApiCallResult ManagerQuests(std::string_view owner_id) {
    return SharedRuntime().Quests(owner_id);
}

std::vector<ManagerJournalQuest> ManagerJournalQuests() {
    std::vector<ManagerJournalQuest> entries;
    for (const ModRecord &mod : SharedRuntime().Mods()) {
        const ApiCallResult result = ManagerQuests(mod.id);
        if (result.status != ApiCallStatus::Ok || !result.value.is_object()) continue;
        const Json quests = result.value.value("quests", Json::array());
        if (!quests.is_array()) continue;
        for (const Json &quest : quests) {
            if (quest.is_object()) entries.push_back({mod.id, mod.display_name, quest});
        }
    }
    return entries;
}

std::string ManagerQuestRewardSummary(const Json &quest) {
    const Json rewards = quest.is_object() ? quest.value("rewards", Json::array()) : Json::array();
    if (!rewards.is_array() || rewards.empty()) return "Rewards: none";
    std::vector<std::string> summaries;
    const std::size_t visible = std::min<std::size_t>(rewards.size(), 2);
    for (std::size_t index = 0; index < visible; ++index) {
        const Json &reward = rewards[index];
        if (!reward.is_object()) continue;
        const std::string kind = reward.value("kind", std::string());
        if (kind == "currency" && reward.contains("amount") && reward["amount"].is_number_integer()) {
            summaries.push_back("+" + std::to_string(reward["amount"].get<std::int64_t>()) + " Gilda");
        } else if (kind == "item" && reward.contains("id") && reward["id"].is_string()) {
            std::string id = reward["id"].get<std::string>();
            const std::size_t separator = id.rfind(':');
            if (separator != std::string::npos) id.erase(0, separator + 1);
            if (id.size() > 24) id = id.substr(0, 21) + "...";
            summaries.push_back(std::to_string(reward.value("count", std::int64_t{1})) + "x " + id);
        } else if (kind == "weapon" && reward.contains("id") && reward["id"].is_string()) {
            std::string id = reward["id"].get<std::string>();
            const std::size_t separator = id.rfind(':');
            if (separator != std::string::npos) id.erase(0, separator + 1);
            if (id.size() > 20) id = id.substr(0, 17) + "...";
            summaries.push_back("Weapon: " + id);
        } else {
            summaries.push_back("Reward " + std::to_string(index + 1));
        }
    }
    if (summaries.empty()) return "Rewards: " + std::to_string(rewards.size()) + " entries";
    std::string summary = summaries.size() == 1 ? "Reward: " : "Rewards: ";
    for (std::size_t index = 0; index < summaries.size(); ++index) {
        if (index > 0) summary += " + ";
        summary += summaries[index];
    }
    if (rewards.size() > visible) summary += " + " + std::to_string(rewards.size() - visible) + " more";
    return summary;
}

ApiCallResult ManagerSkills(std::string_view owner_id) {
    return SharedRuntime().Skills(owner_id);
}

bool ActivateManagerAction(std::string_view owner_id, std::string_view local_id) {
    std::string qualified_id(owner_id);
    qualified_id.push_back(':');
    qualified_id.append(local_id);
    const auto found = g_actions.find(qualified_id);
    if (found == g_actions.end() || found->second.owner_id != owner_id) return false;
    const ManagerUiAction action = found->second;
    const bool delivered = SharedRuntime().PublishToMod(
        action.owner_id, "ui.action",
        {{"id", local_id}, {"qualified_id", action.id}, {"label", action.label},
         {"description", action.description}});
    if (delivered) ShowManagerNotice(action.label, 180);
    return delivered;
}

bool NavigateManager(std::string_view mod_id, std::string_view tab, std::string &error) {
    const auto mods = SharedRuntime().Mods();
    const auto found = std::ranges::find(mods, mod_id, &ModRecord::id);
    if (found == mods.end() || !found->valid_manifest || !found->enabled || found->state != ModState::Loaded) {
        error = "destination package is not currently loaded";
        return false;
    }

    int requested_tab = -1;
    if (tab == "settings") requested_tab = 0;
    else if (tab == "actions") requested_tab = 1;
    else if (tab == "quests") requested_tab = 2;
    else if (tab == "skills") requested_tab = 3;
    else if (tab == "journal") requested_tab = 4;
    else {
        error = "tab must be settings, actions, quests, skills, or journal";
        return false;
    }

    const auto actions = ManagerActions(found->id);
    const ApiCallResult quests = ManagerQuests(found->id);
    const ApiCallResult skills = ManagerSkills(found->id);
    const auto journal = ManagerJournalQuests();
    const bool available[] = {
        !found->manifest.settings.empty(), !actions.empty(),
        quests.status == ApiCallStatus::Ok && quests.value.is_object() &&
            quests.value.value("quests", Json::array()).is_array() && !quests.value.value("quests", Json::array()).empty(),
        skills.status == ApiCallStatus::Ok && skills.value.is_object() &&
            skills.value.value("skills", Json::array()).is_array() && !skills.value.value("skills", Json::array()).empty(),
        !journal.empty()};
    if (!available[requested_tab]) {
        error = "destination package does not expose the requested manager tab";
        return false;
    }

    if (!g_state.open) {
        OpenManager();
        g_menu_open_request = true;
        g_opened_in_play = true;
    }
    const auto refreshed = SharedRuntime().Mods();
    const auto destination = std::ranges::find(refreshed, mod_id, &ModRecord::id);
    if (destination == refreshed.end()) {
        error = "destination package disappeared";
        return false;
    }
    g_state.selected_mod = static_cast<int>(std::distance(refreshed.begin(), destination));
    g_state.first_mod = std::max(0, g_state.selected_mod - kManagerVisibleMods + 1);
    g_state.detail_tab = requested_tab;
    g_state.settings_focus = true;
    g_state.native_trust_focus = false;
    g_state.native_trust_confirmation = false;
    g_state.selected_setting = 0;
    g_state.first_setting = 0;
    g_state.selected_action = 0;
    g_state.first_action = 0;
    g_state.selected_quest = 0;
    g_state.first_quest_objective = 0;
    g_state.selected_skill = 0;
    g_state.first_skill = 0;
    g_state.selected_journal_quest = 0;
    g_state.first_journal_quest = 0;
    Reconcile();
    return true;
}

bool HandleManagerInput(const ManagerInput &input) {
    if (!g_state.open) return false;

    if (g_state.native_trust_confirmation) {
        // The modal's two buttons are stacked and centred; the drawing reports where they are.
        const auto on_button = [&](int top) {
            return input.mouse_left && input.mouse_x >= 270 && input.mouse_x < 370 && input.mouse_y >= top &&
                   input.mouse_y < top + 26;
        };
        if (input.cancel || input.mouse_right || on_button(g_trust_cancel_y)) {
            g_state.native_trust_confirmation = false;
        } else if (input.confirm || on_button(g_trust_confirm_y)) {
            ConfirmNativeTrustChange();
        }
        return true;
    }

    const auto mods = SharedRuntime().Mods();
    const ModRecord *record = ManagerSelectedMod();
    const int setting_count = record == nullptr ? 0 : static_cast<int>(record->manifest.settings.size());
    const auto actions = record == nullptr ? std::vector<ManagerUiAction>{} : ManagerActions(record->id);
    const int action_count = static_cast<int>(actions.size());
    const ApiCallResult quest_result = record == nullptr ? ApiCallResult{} : ManagerQuests(record->id);
    const Json quest_list = quest_result.status == ApiCallStatus::Ok && quest_result.value.is_object()
                                ? quest_result.value.value("quests", Json::array())
                                : Json::array();
    const ApiCallResult skill_result = record == nullptr ? ApiCallResult{} : ManagerSkills(record->id);
    const Json skill_list = skill_result.status == ApiCallStatus::Ok && skill_result.value.is_object()
                                ? skill_result.value.value("skills", Json::array())
                                : Json::array();
    const bool has_quests = quest_list.is_array() && !quest_list.empty();
    const bool has_skills = skill_list.is_array() && !skill_list.empty();
    const auto journal_quests = ManagerJournalQuests();
    const bool has_journal = !journal_quests.empty();
    const int skill_count = has_skills ? static_cast<int>(skill_list.size()) : 0;
    if (g_state.detail_tab == 1 && action_count == 0) g_state.detail_tab = has_quests ? 2 : has_skills ? 3 : 0;
    if (g_state.detail_tab == 2 && !has_quests) g_state.detail_tab = has_skills ? 3 : action_count > 0 ? 1 : 0;
    if (g_state.detail_tab == 3 && !has_skills) g_state.detail_tab = has_quests ? 2 : action_count > 0 ? 1 : 0;
    const bool has_settings = setting_count > 0;
    const bool has_actions = action_count > 0;
    if (g_state.detail_tab == 4 && !has_journal) {
        g_state.detail_tab = FirstDetailTab(has_settings, has_actions, has_quests, has_skills, false);
    }
    const bool action_tab = g_state.detail_tab == 1 && has_actions;
    const bool quest_tab = g_state.detail_tab == 2 && has_quests;
    const bool skill_tab = g_state.detail_tab == 3 && has_skills;
    const bool journal_tab = g_state.detail_tab == 4 && has_journal;
    if (input.cancel || input.mouse_right ||
        (input.mouse_left && input.mouse_x >= 44 && input.mouse_x < 140 && input.mouse_y >= 422 && input.mouse_y < 449) ||
        (input.mouse_left && input.mouse_x >= 558 && input.mouse_x < 596 && input.mouse_y >= 76 && input.mouse_y < 111)) {
        if (g_opened_in_play) {
            g_opened_in_play = false;
            g_host_close_request = true;
            CloseManager();
        } else if (g_state.settings_focus || g_state.native_trust_focus) {
            g_state.settings_focus = false;
            g_state.native_trust_focus = false;
            g_state.detail_tab = 0;
            Reconcile();
        } else {
            CloseManager();
        }
        return true;
    }

    if (g_state.settings_focus && skill_tab && input.mouse_left) {
        if(input.mouse_x>=56 && input.mouse_x<592 && input.mouse_y>=128 && input.mouse_y<278) {
            std::vector<std::string> branches;
            for(const auto &skill:skill_list) {
                auto tree=skill.value("tree_title",std::string{});
                if(std::find(branches.begin(),branches.end(),tree)==branches.end()) branches.push_back(tree);
            }
            const int col=std::clamp(int((input.mouse_x-56)/(536/int(branches.size()))),0,int(branches.size())-1);
            int row=0,target=int((input.mouse_y-128)/50);
            for(int i=0;i<int(skill_list.size());++i) if(skill_list[i].value("tree_title",std::string{})==branches[col] && row++==target) {g_state.selected_skill=i;break;}
            Reconcile();
        }
        return true;
    }
    if (input.mouse_left) {
        const int journal_row = journal_tab
                                    ? RowAt(input.mouse_y, 141.0f, 29.0f, kManagerVisibleMods, g_state.first_journal_quest,
                                            static_cast<int>(journal_quests.size()))
                                    : -1;
        if (journal_tab && input.mouse_x >= 48 && input.mouse_x < 246 && journal_row >= 0) {
            g_state.selected_journal_quest = journal_row;
            g_state.first_quest_objective = 0;
            g_state.settings_focus = false;
            Reconcile();
            return true;
        }
        const int mod_row = journal_tab ? -1 : RowAt(input.mouse_y, 141.0f, 29.0f, kManagerVisibleMods, g_state.first_mod,
                                                    static_cast<int>(mods.size()));
        if (!journal_tab && input.mouse_x >= 48 && input.mouse_x < 246 && mod_row >= 0) {
            g_state.selected_mod = mod_row;
            g_state.selected_setting = 0;
            g_state.first_setting = 0;
            g_state.selected_action = 0;
            g_state.first_action = 0;
            g_state.selected_skill = 0;
            g_state.first_skill = 0;
            g_state.detail_tab = 0;
            g_state.settings_focus = false;
            if (input.mouse_x >= 216 && !mods[static_cast<std::size_t>(mod_row)].manifest.native_library.empty()) {
                g_state.native_trust_focus = true;
                RequestNativeTrustConfirmation();
            } else if (input.mouse_x >= 184) {
                g_state.native_trust_focus = false;
                ToggleSelectedMod();
            }
            Reconcile();
            return true;
        }
        if (input.mouse_x >= 258 && input.mouse_x < 592) {
            const float tab_top = journal_tab ? 139.0f : 230.0f;
            if (input.mouse_y >= tab_top && input.mouse_y < tab_top + 24.0f) {
                SelectDetailTab(std::clamp(static_cast<int>((input.mouse_x - 263.0f) / 64.0f), 0, 4));
                return true;
            }
            if (journal_tab) {
                g_state.settings_focus = true;
                return true;
            }
            if (action_tab) {
                const int action_row = RowAt(input.mouse_y, 260.0f, 24.0f, kManagerVisibleSettings,
                                             g_state.first_action, action_count);
                if (action_row >= 0) {
                    g_state.selected_action = action_row;
                    g_state.settings_focus = true;
                    ActivateSelectedAction();
                    Reconcile();
                    return true;
                }
            } else if (skill_tab) {
                const int skill_row = RowAt(input.mouse_y, 278.0f, 24.0f, kManagerVisibleSkills,
                                            g_state.first_skill, skill_count);
                if (skill_row >= 0) {
                    g_state.selected_skill = skill_row;
                    g_state.settings_focus = true;
                    Reconcile();
                    return true;
                }
            } else if (!quest_tab) {
                const int setting_row = RowAt(input.mouse_y, 260.0f, 24.0f, kManagerVisibleSettings,
                                              g_state.first_setting, setting_count);
                if (setting_row >= 0) {
                    g_state.selected_setting = setting_row;
                    g_state.settings_focus = true;
                    if (input.mouse_x >= 506) StepSelectedSetting(input.mouse_x < 544 ? -1 : 1);
                    Reconcile();
                    return true;
                }
            }
        }
    }

    if (input.wheel != 0.0f) {
        if (input.mouse_x < 250) {
            if (journal_tab) MoveJournalQuest(input.wheel > 0.0f ? -1 : 1);
            else MoveMod(input.wheel > 0.0f ? -1 : 1);
        } else if (journal_tab) {
            MoveJournalObjective(input.wheel > 0.0f ? -1 : 1);
        } else if (action_tab && has_actions) {
            MoveAction(input.wheel > 0.0f ? -1 : 1);
        } else if (quest_tab) {
            MoveQuestObjective(input.wheel > 0.0f ? -1 : 1);
        } else if (skill_tab) {
            MoveSkill(input.wheel > 0.0f ? -1 : 1);
        } else if (has_settings) {
            g_state.settings_focus = true;
            MoveSetting(input.wheel > 0.0f ? -1 : 1);
        }
        return true;
    }

    const bool has_native_extension = record != nullptr && !record->manifest.native_library.empty();
    if (g_state.native_trust_focus && (input.next_focus || input.right)) {
        g_state.native_trust_focus = false;
        g_state.settings_focus = has_settings || has_actions || has_quests || has_skills || has_journal;
        if (g_state.settings_focus) g_state.detail_tab = FirstDetailTab(has_settings, has_actions, has_quests, has_skills, has_journal);
        return true;
    }
    if (input.next_focus || (input.right && !g_state.settings_focus && !g_state.native_trust_focus)) {
        if (!g_state.settings_focus && has_native_extension && !journal_tab) {
            g_state.native_trust_focus = true;
        } else if (!g_state.settings_focus && (has_settings || has_actions || has_quests || has_skills || has_journal)) {
            g_state.settings_focus = true;
            if (!journal_tab) g_state.detail_tab = FirstDetailTab(has_settings, has_actions, has_quests, has_skills, has_journal);
        } else if (g_state.settings_focus) {
            g_state.detail_tab = AdjacentDetailTab(g_state.detail_tab, 1, has_settings, has_actions, has_quests, has_skills, has_journal);
        }
        return true;
    }
    if (input.previous_focus || (input.left && g_state.native_trust_focus)) {
        if (g_state.native_trust_focus) {
            g_state.native_trust_focus = false;
        } else if (g_state.settings_focus && journal_tab) {
            // the Journal is two panes: back from its objectives to its quest list
            g_state.settings_focus = false;
        } else if (g_state.settings_focus) {
            g_state.detail_tab = AdjacentDetailTab(g_state.detail_tab, -1, has_settings, has_actions, has_quests, has_skills, has_journal);
        }
        return true;
    }
    if (input.up) {
        if (journal_tab) {
            if (g_state.settings_focus) MoveJournalObjective(-1);
            else MoveJournalQuest(-1);
        } else if (g_state.settings_focus && quest_tab) MoveQuestObjective(-1);
        else if (g_state.settings_focus && action_tab) MoveAction(-1);
        else if (g_state.settings_focus && skill_tab) MoveSkill(-1);
        else if (g_state.settings_focus) MoveSetting(-1);
        else MoveMod(-1);
        return true;
    }
    if (input.down) {
        if (journal_tab) {
            if (g_state.settings_focus) MoveJournalObjective(1);
            else MoveJournalQuest(1);
        } else if (g_state.settings_focus && quest_tab) MoveQuestObjective(1);
        else if (g_state.settings_focus && action_tab) MoveAction(1);
        else if (g_state.settings_focus && skill_tab) MoveSkill(1);
        else if (g_state.settings_focus) MoveSetting(1);
        else MoveMod(1);
        return true;
    }
    if (g_state.settings_focus && skill_tab && (input.left || input.right)) {
        const int step=input.left?-1:1;
        const auto current=skill_list[g_state.selected_skill].value("tree_title",std::string{});
        for(int next=g_state.selected_skill+step;next>=0 && next<int(skill_list.size());next+=step)
            if(skill_list[next].value("tree_title",std::string{})!=current){g_state.selected_skill=next;break;}
        Reconcile();return true;
    }
    if (input.left || input.right) {
        if (journal_tab) MoveJournalQuest(input.left ? -1 : 1);
        else if (g_state.settings_focus && quest_tab) MoveQuest(input.left ? -1 : 1);
        else if (g_state.settings_focus && !action_tab && !skill_tab) StepSelectedSetting(input.left ? -1 : 1);
        return true;
    }
    if (input.confirm) {
        if (g_state.native_trust_focus) RequestNativeTrustConfirmation();
        else if (g_state.settings_focus && action_tab) ActivateSelectedAction();
        else if (g_state.settings_focus && skill_tab) {
            const Json &skill = skill_list[static_cast<std::size_t>(g_state.selected_skill)];
            std::string id = skill.value("id", "");
            const std::string prefix = record->id + ":";
            if (id.starts_with(prefix)) id.erase(0, prefix.size());
            if (!ActivateManagerAction(record->id, "train_" + id) &&
                !SharedRuntime().PublishToMod(record->id, "skills.train_requested", {{"id", skill.value("id", "")}})) {
                Notice("This package cannot train skills here");
            }
        }
        else if (journal_tab) return true;
        else if (g_state.settings_focus && !skill_tab) StepSelectedSetting(1);
        else ToggleSelectedMod();
        return true;
    }
    return true;
}

} // namespace moonmod

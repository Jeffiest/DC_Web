#include "options/mod_manager_ui.hpp"

#include <algorithm>
#include <cmath>
#include <format>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "gameutil.hpp"
#include "menu_draw.hpp"
#include "moonmod/manager_ui.hpp"
#include "moonmod/runtime.hpp"
#include "ui/framework.hpp"

namespace dc::game {}
using namespace dc::game;

// The MoonFactory mod manager, drawn with the UI framework like the other menus: a stone window
// with a title banner, recessed list and detail panes, framework labels, a tab plate, and the
// bracket-and-hand selection. Layout is in the 640x480 logical frame (the window spans 36..604 by
// 66..462), which is also the space manager_ui.cpp hit-tests mouse input in.
namespace dc::options::mod_manager_ui {
namespace {

constexpr float kHeader = ui::metrics::kScaleHeader;
constexpr float kBody = ui::metrics::kScaleBody;
constexpr float kSmall = ui::metrics::kScaleSmall;

ui::Cursor    s_cursor;
ui::Brackets  s_brackets;
ui::ScrollBar s_mod_bar;
ui::ScrollBar s_detail_bar;

// Where the hand and brackets go this frame; the last call wins.
struct Focus {
    bool     set = false;
    ui::Rect box;
};
Focus g_focus;

void FocusOn(const ui::Rect &box) { g_focus = {true, box}; }

int Measure(const std::string &text, float scale) { return ui::Label(text, ui::ColorWhite, ui::Alignment::Left, true, scale).GetWidth(); }

// Shortens text with "..." until it fits width, never splitting a UTF-8 sequence.
std::string Fit(std::string text, int width, float scale) {
    if (Measure(text, scale) <= width) return text;
    while (!text.empty() && Measure(text + "...", scale) > width) {
        text.pop_back();
        while (!text.empty() && (static_cast<unsigned char>(text.back()) & 0xc0) == 0x80) text.pop_back();
        if (!text.empty() && static_cast<unsigned char>(text.back()) >= 0xc0) text.pop_back();
    }
    return text + "...";
}

void Text(const std::string &text, int x, int y, int width, u32 color, int alpha, float scale = kBody,
          ui::Alignment align = ui::Alignment::Left) {
    ui::Label(Fit(text, width, scale), color, align, true, scale).Draw(x, y, alpha);
}

u32 RowColor(bool selected) { return selected ? ui::ColorYellow : ui::ColorWhite; }

// Word-wraps text into at most max_lines lines; the last line ends with "..." if it runs out.
void Wrapped(const std::string &text, int x, int y, int width, int line_step, int max_lines, int alpha,
             float scale = kSmall, u32 color = ui::ColorWhite) {
    std::vector<std::string> lines = ui::WrapText(text, width, scale);
    if (static_cast<int>(lines.size()) > max_lines) {
        lines.resize(max_lines);
        lines.back() = Fit(lines.back() + "...", width, scale);
    }
    for (std::size_t i = 0; i < lines.size(); ++i) {
        ui::Label(lines[i], color, ui::Alignment::Left, true, scale).Draw(x, y + int(i) * line_step, alpha);
    }
}

std::string ModTitle(const moonmod::ModRecord &record) {
    return std::format("{}  {}", record.display_name, record.manifest.version);
}

std::string JournalStatus(const std::string &status) {
    if (status == "completed") return "DONE";
    if (status == "registered") return "NEW";
    if (status == "failed") return "FAILED";
    return status == "active" ? "ACTIVE" : status;
}

std::string SkillState(const moonmod::Json &skill) {
    if (skill.value("rank", 0) >= skill.value("max_rank", 1)) return "MAX";
    return skill.value("prerequisites_met", false) ? "READY" : "LOCKED";
}

std::string NativePermissions(const moonmod::ModRecord *record) {
    if (record == nullptr || record->manifest.permissions.empty()) return "Permissions: none";
    std::vector<std::string> sorted = record->manifest.permissions;
    std::ranges::sort(sorted);
    std::string line = "Permissions:";
    for (std::size_t i = 0; i < sorted.size(); ++i) line += (i == 0 ? " " : ", ") + sorted[i];
    return line;
}

void Frame(const std::string &title, int alpha) {
    ui::Window::Draw(36, 66, 568, 396, alpha, ui::WindowPreset::BlueStone);
    ui::TrackerWindow::DrawStoneBanner(ui::Rect(150, 52, 340, 28), alpha, 0x80, title);
    MenuTextureReload(0x1A);
}

void Footer(const std::string &line, const std::string &hint, bool highlight, int alpha) {
    ui::HelpBox::Draw(ui::Rect(45, 397, 550, 55), alpha);
    MenuTextureReload(0x1A);
    // A notice too long for one line (why a mod waits for a restart) takes the hint's line while it shows.
    if (highlight && ui::WrapText(line, 528, kSmall).size() > 1) {
        Wrapped(line, 56, 403, 528, 24, 2, alpha, kSmall, ui::ColorYellow);
        return;
    }
    Text(line, 56, 403, 528, highlight ? ui::ColorYellow : ui::ColorWhite, alpha, kSmall);
    Text(hint, 56, 427, 528, ui::ColorGold, alpha, kSmall);
}

void DrawSkillTree(const moonmod::ModRecord &record, const moonmod::Json &skills, int alpha) {
    using namespace moonmod;
    const ManagerUiState &state = ManagerState();
    Frame(record.display_name + " / SKILL TREE", alpha);

    std::vector<std::string> branches;
    std::vector<int> rows;
    std::map<std::string, std::pair<int, int>> positions;
    for (const auto &skill : skills) {
        const std::string branch = skill.value("tree_title", std::string("Skills"));
        auto it = std::find(branches.begin(), branches.end(), branch);
        if (it == branches.end()) {
            branches.push_back(branch);
            rows.push_back(0);
            it = branches.end() - 1;
        }
        const int col = int(it - branches.begin());
        positions[skill.value("id", std::string{})] = {col, rows[col]++};
    }
    constexpr int kVisibleRows = 3, kRowPitch = 50, kFirstRowY = 128, kCardHeight = 44;
    const int cols = std::max(1, int(branches.size()));
    const int width = 536 / cols;
    std::vector<int> starts(branches.size(), 0);
    if (state.selected_skill >= 0 && state.selected_skill < int(skills.size())) {
        if (auto found = positions.find(skills[state.selected_skill].value("id", std::string{})); found != positions.end()) {
            const auto [col, row] = found->second;
            starts[col] = std::clamp(row - kVisibleRows + 1, 0, std::max(0, rows[col] - kVisibleRows));
        }
    }
    for (int col = 0; col < cols; ++col) {
        const int x = 56 + col * width;
        Text(branches[col], x, 98, width - 30, ui::ColorGold, alpha, kHeader);
        // Glyph tokens are drawn whole, never shortened.
        if (starts[col] > 0) ui::Label("{up}", ui::ColorWhite, ui::Alignment::Left, true, kSmall).Draw(x + width - 46, 98, alpha);
        if (starts[col] + kVisibleRows < rows[col]) {
            ui::Label("{down}", ui::ColorWhite, ui::Alignment::Left, true, kSmall).Draw(x + width - 28, 98, alpha);
        }
    }
    int index = 0;
    for (const auto &skill : skills) {
        const auto [col, row] = positions.at(skill.value("id", std::string{}));
        const int start = starts[col];
        if (row < start || row >= start + kVisibleRows) {
            ++index;
            continue;
        }
        const int x = 56 + col * width;
        const int y = kFirstRowY + (row - start) * kRowPitch;
        const bool selected = index == state.selected_skill;
        ui::TrackerWindow::DrawIndentation(ui::Rect(x, y, width - 14, kCardHeight), !selected, alpha);
        MenuTextureReload(0x1A);
        Text(skill.value("title", std::string("Skill")), x + 8, y + 4, width - 30, RowColor(selected), alpha);
        Text(std::format("{}/{}  {}", skill.value("rank", 0), skill.value("max_rank", 1), SkillState(skill)), x + 8,
             y + 24, width - 30, selected ? ui::ColorYellow : ui::ColorGold, alpha, kSmall);
        if (selected) FocusOn(ui::Rect(x - 4, y - 2, width - 6, kCardHeight + 4));
        ++index;
    }
    ui::Divider::DrawHorizontal(52, 588, 284, alpha);
    if (state.selected_skill >= 0 && state.selected_skill < int(skills.size())) {
        const Json &skill = skills[state.selected_skill];
        Text(skill.value("title", std::string("Skill")) + "  " +
                 std::format("{}/{}  {}", skill.value("rank", 0), skill.value("max_rank", 1), SkillState(skill)),
             56, 294, 528, ui::ColorYellow, alpha, kBody);
        Wrapped(skill.value("description", std::string{}), 56, 320, 528, 20, 3, alpha);
    }
    Footer(state.notice.empty() ? "{up}{down} skill   {left}{right} branch   {cross} train" : state.notice,
           "{circle} packages   {L1}/{R1} tabs", !state.notice.empty(), alpha);
}

void DrawObjectives(const moonmod::Json &objectives, int first, int count, int y, int step, bool focus, int alpha) {
    if (!objectives.is_array()) return;
    for (int row = 0; row < count; ++row) {
        const int index = first + row;
        if (index >= int(objectives.size())) break;
        const moonmod::Json &objective = objectives[std::size_t(index)];
        const int target = std::max(1, objective.value("target", 1));
        const int progress = std::clamp(objective.value("progress", 0), 0, target);
        const int row_y = y + row * step;
        Text(objective.value("title", std::string("Objective")), 270, row_y, 190, RowColor(focus && row == 0), alpha, kSmall);
        ui::ProgressBarStyle style{};
        style.text_scale = 0.68f;
        ui::ProgressBar::Draw(ui::Rect(466, row_y + 1, 112, 16), float(progress) / float(target),
                              std::format("{}/{}", progress, target), style, alpha);
        MenuTextureReload(0x1A);
    }
}

int QuestsDone(const moonmod::Json &objectives) {
    if (!objectives.is_array()) return 0;
    return int(std::ranges::count_if(objectives, [](const moonmod::Json &o) { return o.value("complete", false); }));
}

} // namespace

// The manager lays out and hit-tests in the 640x480 logical frame, the same space the menus report
// the pointer in, so the point passes through unchanged.
std::pair<float, float> InputPoint(float x, float y) { return {x, y}; }

void Draw(int alpha) {
    using namespace moonmod;
    const ManagerUiState &state = ManagerState();
    const auto mods = SharedRuntime().Mods();
    const auto journal_quests = ManagerJournalQuests();
    const bool journal_tab = state.detail_tab == 4 && !journal_quests.empty();
    g_focus = {};

    if (state.settings_focus && state.detail_tab == 3) {
        if (const ModRecord *record = ManagerSelectedMod()) {
            const ApiCallResult result = ManagerSkills(record->id);
            if (result.status == ApiCallStatus::Ok && result.value.contains("skills") && result.value["skills"].is_array() &&
                !result.value["skills"].empty()) {
                DrawSkillTree(*record, result.value["skills"], alpha);
                if (g_focus.set) {
                    s_brackets.Draw(g_focus.box, alpha);
                    s_cursor.SetTarget(float(g_focus.box.x - 34), float(g_focus.box.y + g_focus.box.h / 2 - 8));
                }
                s_cursor.Draw(alpha);
                return;
            }
        }
    }

    Frame("MOD MANAGER", alpha);
    ui::TrackerWindow::DrawIndentation(ui::Rect(45, 104, 198, 284), true, alpha);
    ui::TrackerWindow::DrawIndentation(ui::Rect(252, 104, 344, 284), true, alpha);
    MenuTextureReload(0x1A);
    Text(journal_tab ? "QUEST JOURNAL" : "PACKAGES", 56, 112, 176, ui::ColorGold, alpha, kHeader);
    // On the Journal tab the tab plate sits where the details heading would be.
    if (!journal_tab) Text("DETAILS", 264, 112, 320, ui::ColorGold, alpha, kHeader);
    ui::Divider::DrawHorizontal(54, 234, 136, alpha);

    if (mods.empty()) {
        Wrapped("No mods found. Put each mod in its own folder in the mods folder of your Chronicle folder, "
                "then restart the game.",
                56, 148, 176, 20, 8, alpha);
    } else if (journal_tab) {
        const int end = std::min(int(journal_quests.size()), state.first_journal_quest + kManagerVisibleMods);
        for (int row = state.first_journal_quest; row < end; ++row) {
            const ManagerJournalQuest &entry = journal_quests[std::size_t(row)];
            const int y = 143 + (row - state.first_journal_quest) * 29;
            const bool selected = row == state.selected_journal_quest;
            Text(entry.quest.value("title", std::string("Untitled quest")), 56, y + 2, 112, RowColor(selected), alpha, kSmall);
            Text(JournalStatus(entry.quest.value("status", std::string("unknown"))), 228, y + 3, 54, ui::ColorGold, alpha,
                 kSmall, ui::Alignment::Right);
            if (selected && !state.settings_focus) FocusOn(ui::Rect(50, y - 3, 188, 26));
        }
        MenuTextureReload(0x1A);
        s_mod_bar.Draw(238, 143, kManagerVisibleMods * 29, int(journal_quests.size()), kManagerVisibleMods,
                       state.first_journal_quest, alpha);
    } else {
        const int end = std::min(int(mods.size()), state.first_mod + kManagerVisibleMods);
        for (int row = state.first_mod; row < end; ++row) {
            const ModRecord &record = mods[std::size_t(row)];
            const int y = 143 + (row - state.first_mod) * 29;
            const bool selected = row == state.selected_mod;
            const bool native = !record.manifest.native_library.empty();
            // a merged pack's parts sit indented under it
            const int indent = record.pack.empty() ? 0 : 10;
            Text(record.pack.empty() ? record.display_name : "- " + record.display_name, 56 + indent, y + 2,
                 (native ? 128 : 150) - indent, RowColor(selected), alpha, kSmall);
            // A part of a pack that is off shows its own switch, dimmed: it applies when the pack is on.
            const bool idle_part = !record.pack.empty() && !record.enabled && !record.restart_required;
            const std::string on = record.restart_required ? (record.enabled_next_launch ? "ON*" : "OFF*")
                                   : idle_part             ? (record.part_switch ? "ON" : "OFF")
                                                           : (record.enabled ? "ON" : "OFF");
            Text(on, native ? 202 : 228, y + 3, 44, record.enabled ? ui::ColorGold : ui::ColorWhite,
                 record.enabled ? alpha : alpha / 2, kSmall, ui::Alignment::Right);
            if (native) {
                Text(record.native_trusted ? "N+" : "N?", 228, y + 3, 24,
                     selected && state.native_trust_focus ? ui::ColorYellow : ui::ColorWhite, alpha, kSmall,
                     ui::Alignment::Right);
            }
            if (selected && !state.settings_focus) FocusOn(ui::Rect(50, y - 3, 188, 26));
        }
        MenuTextureReload(0x1A);
        s_mod_bar.Draw(238, 143, kManagerVisibleMods * 29, int(mods.size()), kManagerVisibleMods, state.first_mod, alpha);
    }

    std::string footer = state.detail_tab == 4   ? "{up}{down} quest   {left}{right} objectives   {L1}/{R1} tabs"
                         : state.detail_tab == 2 ? "{up}{down} objectives   {left}{right} quest   {L1}/{R1} tabs"
                         : state.detail_tab == 3 ? "{up}{down} skill   {cross} open tree   {L1}/{R1} tabs"
                                                 : "{up}{down} select   {right} details   {L1}/{R1} tabs   {circle} back";

    if (const ModRecord *record = ManagerSelectedMod(); record != nullptr && !mods.empty()) {
        if (!journal_tab) {
            Text(ModTitle(*record), 264, 144, 320, ui::ColorYellow, alpha, kHeader);
            const std::string author = record->manifest.author.empty() ? "Unknown author" : record->manifest.author;
            Text(std::format("{}  /  {}{}", author, ModStateText(record->state),
                             record->restart_required ? "  /  RESTART REQUIRED"
                             : !record->pack.empty()      ? "  /  IN " + record->pack
                                                          : std::string{}),
                 264, 166, 320, ui::ColorWhite, alpha, kSmall);
            const std::string description =
                record->restart_required && !record->restart_reason.empty()
                    ? std::string(record->enabled_next_launch ? "On" : "Off") + " after a restart: " + record->restart_reason
                : record->manifest.description.empty() ? record->status_message
                                                        : record->manifest.description;
            Wrapped(description, 264, 184, 320, 17, record->native_status.empty() ? 2 : 1, alpha);
            if (!record->native_status.empty()) Text(record->native_status, 264, 201, 320, ui::ColorWhite, alpha, kSmall);
        }

        const auto actions = ManagerActions(record->id);
        const ApiCallResult quest_result = ManagerQuests(record->id);
        const Json quests = quest_result.status == ApiCallStatus::Ok && quest_result.value.is_object()
                                ? quest_result.value.value("quests", Json::array())
                                : Json::array();
        const ApiCallResult skill_result = ManagerSkills(record->id);
        const Json skills = skill_result.status == ApiCallStatus::Ok && skill_result.value.is_object()
                                ? skill_result.value.value("skills", Json::array())
                                : Json::array();
        const bool has_quests = quests.is_array() && !quests.empty();
        const bool has_skills = skills.is_array() && !skills.empty();
        const bool action_tab = state.detail_tab == 1 && !actions.empty();
        const bool quest_tab = state.detail_tab == 2 && has_quests;
        const bool skill_tab = state.detail_tab == 3 && has_skills;

        // Detail tabs on a tab plate; the shown one is gold, unavailable ones are dimmed.
        const int tab_y = journal_tab ? 139 : 230;
        ui::TabPlate::Draw(258, 590, tab_y, alpha);
        MenuTextureReload(0x1A);
        const struct {
            const char *label;
            bool available;
        } tabs[5] = {{"SETTINGS", !record->manifest.settings.empty()},
                     {"ACTIONS", !actions.empty()},
                     {"QUESTS", has_quests},
                     {"SKILLS", has_skills},
                     {"JOURNAL", !journal_quests.empty()}};
        for (int index = 0; index < 5; ++index) {
            const int x = 263 + index * 64;
            const bool shown = state.detail_tab == index && tabs[index].available;
            const u32 color = shown ? (state.settings_focus ? ui::ColorYellow : ui::ColorGold) : ui::ColorWhite;
            Text(tabs[index].label, x + 30, tab_y + 5, 62, color, tabs[index].available ? (shown ? alpha : alpha * 3 / 4) : alpha / 3,
                 0.62f, ui::Alignment::Center);
        }

        if (journal_tab) {
            if (state.selected_journal_quest >= 0 && state.selected_journal_quest < int(journal_quests.size())) {
                const ManagerJournalQuest &entry = journal_quests[std::size_t(state.selected_journal_quest)];
                const Json &quest = entry.quest;
                const Json objectives = quest.value("objectives", Json::array());
                Text(quest.value("title", std::string("Untitled quest")), 264, 170, 320, ui::ColorYellow, alpha, kHeader);
                Text(std::format("{}  /  {}  /  {}/{} objectives", entry.mod_name,
                                 JournalStatus(quest.value("status", std::string("unknown"))), QuestsDone(objectives),
                                 objectives.size()),
                     264, 194, 320, ui::ColorWhite, alpha, kSmall);
                Wrapped(quest.value("description", std::string{}), 264, 214, 320, 18, 2, alpha);
                Text(ManagerQuestRewardSummary(quest), 264, 252, 320, ui::ColorGold, alpha, kSmall);
                DrawObjectives(objectives, state.first_quest_objective, kManagerVisibleJournalObjectives, 278, 26,
                               state.settings_focus, alpha);
            } else {
                Text("Select a quest to see its progress.", 264, 170, 320, ui::ColorWhite, alpha, kSmall);
            }
        } else if (action_tab) {
            const int end = std::min(int(actions.size()), state.first_action + kManagerVisibleSettings);
            for (int row = state.first_action; row < end; ++row) {
                const int y = 262 + (row - state.first_action) * 24;
                const bool selected = state.settings_focus && row == state.selected_action;
                Text(actions[std::size_t(row)].label, 270, y, 300, RowColor(selected), alpha);
                if (selected) {
                    FocusOn(ui::Rect(264, y - 3, 310, 24));
                    footer = actions[std::size_t(row)].description;
                }
            }
            MenuTextureReload(0x1A);
            s_detail_bar.Draw(584, 262, kManagerVisibleSettings * 24, int(actions.size()), kManagerVisibleSettings,
                              state.first_action, alpha);
        } else if (quest_tab && state.selected_quest >= 0 && state.selected_quest < int(quests.size())) {
            const Json &quest = quests[std::size_t(state.selected_quest)];
            const Json objectives = quest.value("objectives", Json::array());
            Text(quest.value("title", std::string("Untitled quest")), 270, 260, 310, ui::ColorYellow, alpha);
            Text(std::format("Quest {}/{}  /  {}  /  {}/{} done", state.selected_quest + 1, quests.size(),
                             JournalStatus(quest.value("status", std::string("unknown"))), QuestsDone(objectives),
                             objectives.size()),
                 270, 282, 310, ui::ColorWhite, alpha, kSmall);
            Text(ManagerQuestRewardSummary(quest), 270, 300, 310, ui::ColorGold, alpha, kSmall);
            DrawObjectives(objectives, state.first_quest_objective, kManagerVisibleQuestObjectives, 322, 20,
                           state.settings_focus, alpha);
            if (state.settings_focus) FocusOn(ui::Rect(264, 319, 320, 22));
        } else if (skill_tab && state.selected_skill >= 0 && state.selected_skill < int(skills.size())) {
            Text(skills[std::size_t(state.selected_skill)].value("tree_title", std::string("Skill tree")), 270, 260, 310,
                 ui::ColorGold, alpha, kSmall);
            const int end = std::min(int(skills.size()), state.first_skill + kManagerVisibleSkills);
            for (int row = state.first_skill; row < end; ++row) {
                const Json &skill = skills[std::size_t(row)];
                const int y = 280 + (row - state.first_skill) * 24;
                const bool selected = state.settings_focus && row == state.selected_skill;
                Text(skill.value("title", std::string("Skill")), 270, y, 170, RowColor(selected), alpha);
                Text(std::format("{}/{}  {}", skill.value("rank", 0), skill.value("max_rank", 1), SkillState(skill)), 578,
                     y + 2, 130, selected ? ui::ColorYellow : ui::ColorGold, alpha, kSmall, ui::Alignment::Right);
                if (selected) FocusOn(ui::Rect(264, y - 3, 310, 24));
            }
            const std::string description = skills[std::size_t(state.selected_skill)].value("description", std::string{});
            if (!description.empty()) footer = description;
        } else {
            const int end = std::min(int(record->manifest.settings.size()), state.first_setting + kManagerVisibleSettings);
            for (int row = state.first_setting; row < end; ++row) {
                const SettingDefinition &setting = record->manifest.settings[std::size_t(row)];
                const int y = 262 + (row - state.first_setting) * 24;
                const bool selected = state.settings_focus && row == state.selected_setting;
                Text(setting.name, 270, y, 210, RowColor(selected), alpha);
                const std::string value = SettingValueText(setting, SharedRuntime().Setting(record->id, setting.id));
                Text(selected ? "< " + value + " >" : value, 526, y, 96, RowColor(selected), alpha, kBody,
                     ui::Alignment::Center);
                if (selected) FocusOn(ui::Rect(264, y - 3, 310, 24));
            }
            if (record->manifest.settings.empty()) {
                Text("This mod has no settings.", 270, 266, 300, ui::ColorWhite, alpha, kSmall);
            }
            MenuTextureReload(0x1A);
            s_detail_bar.Draw(584, 262, kManagerVisibleSettings * 24, int(record->manifest.settings.size()),
                              kManagerVisibleSettings, state.first_setting, alpha);
        }
    }

    if (state.native_menu_context) footer = "{up}{down} select   {left}{right} change   {L1}/{R1} tabs   {triangle} manuals";
    const std::string hint = state.native_menu_context ? "{triangle} game manuals   {cross} open or apply"
                             : state.detail_tab == 4   ? "Journal progress updates as you play"
                             : state.settings_focus    ? "{circle} packages   {cross} apply"
                                                       : "{cross} enable / disable   {circle} back";
    Footer(state.notice.empty() ? footer : state.notice, hint, state.notice_frames > 0, alpha);

    if (state.native_trust_confirmation) {
        const ModRecord *record = ManagerSelectedMod();
        const std::string description =
            "This library runs inside the game with your user permissions.  " +
            (record != nullptr ? record->id + "  " : std::string{}) + NativePermissions(record);
        const std::string title = state.pending_native_trust ? "TRUST NATIVE CODE?" : "REMOVE NATIVE TRUST?";
        ui::ConfirmModal::Draw(title, description, "Confirm", "Cancel", 0, s_cursor, alpha);
        // Same geometry as ConfirmModal::Draw, so the mouse can press its buttons.
        const int lines = int(ui::WrapText(title, 280).size() + ui::WrapText(description, 280).size());
        const int box_y = 164 - std::max(0, lines - 2) * 12;
        const int box_h = 38 + (lines - 1) * 20;
        const int confirm_y = box_y + box_h + 10;
        SetManagerTrustButtons(confirm_y, confirm_y + ui::metrics::kButtonHeight + ui::metrics::kButtonGap);
        return;
    }

    if (g_focus.set) {
        s_brackets.Draw(g_focus.box, alpha);
        s_cursor.SetTarget(float(g_focus.box.x - 34), float(g_focus.box.y + g_focus.box.h / 2 - 8));
        s_cursor.Draw(alpha);
    }
}

} // namespace dc::options::mod_manager_ui

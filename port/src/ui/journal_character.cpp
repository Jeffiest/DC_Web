#include "ui/journal_character.hpp"

#include <algorithm>
#include <cctype>
#include <string>
#include <utility>
#include <vector>

#include "gamepad.hpp"
#include "localize/localize.hpp"
#include "menu_draw.hpp"
#include "menuetc.hpp"
#include "moonmod/manager_ui.hpp"
#include "snd.hpp"
#include "ui/framework.hpp"

namespace dc::game {}
using namespace dc::game;

#define PAD_GLYPH_UP     "{up}"
#define PAD_GLYPH_DOWN   "{down}"
#define PAD_GLYPH_LEFT   "{left}"
#define PAD_GLYPH_RIGHT  "{right}"
#define PAD_GLYPH_CROSS  "{cross}"
#define PAD_GLYPH_CIRCLE "{circle}"

namespace dc::ui::journal_character {

using namespace dc::localize;

namespace {

constexpr int   kRowStartY   = 156;
constexpr int   kRowSpacing  = 25;
constexpr int   kVisibleRows = 6;
constexpr int   kLeftPaneX   = 92;
constexpr int   kDividerX    = 258;
constexpr int   kRightPaneX  = 274;
constexpr int   kRightPaneW  = 268;
constexpr int   kNodeX       = kRightPaneX + 40; // room for the hand between the divider and the titles
constexpr int   kCursorX     = kRightPaneX - 4;  // the hand sits right of the divider, clear of the status text
constexpr int   kCountX      = 458;
constexpr int   kScrollX     = 554;
constexpr int   kDescLines   = 2;
constexpr float kHeaderScale = 0.92f;
constexpr float kBodyScale   = 0.84f;
constexpr float kSmallScale  = 0.76f;

struct Tree {
    std::string                id;
    std::string                title;
    std::vector<moonmod::Json> nodes;
};

int           s_tree    = 0;
int           s_node    = 0;
int           s_first   = 0;
bool          s_pending = false;
ui::Cursor    s_cursor;
ui::Brackets  s_brackets;
ui::ScrollBar s_scrollbar;

// The sheet's skill nodes grouped by tree, in the order the mod registered them.
std::vector<Tree> Trees(const moonmod::CharacterSheet &sheet) {
    std::vector<Tree> trees;
    const moonmod::ApiCallResult listed = moonmod::ManagerSkills(sheet.owner_id);
    if (listed.status != moonmod::ApiCallStatus::Ok || !listed.value.is_object() ||
        !listed.value.contains("skills") || !listed.value["skills"].is_array()) {
        return trees;
    }
    for (const moonmod::Json &node : listed.value["skills"]) {
        const std::string tree_id = node.value("tree_id", std::string{});
        auto tree = std::ranges::find(trees, tree_id, &Tree::id);
        if (tree == trees.end()) {
            trees.push_back({tree_id, node.value("tree_title", std::string{}), {}});
            tree = trees.end() - 1;
        }
        tree->nodes.push_back(node);
    }
    // The sheet's own order first; trees it does not name keep the runtime's order after them.
    std::vector<Tree> ordered;
    for (const std::string &wanted : sheet.trees) {
        auto tree = std::ranges::find(trees, wanted, &Tree::id);
        if (tree != trees.end()) {
            ordered.push_back(std::move(*tree));
            trees.erase(tree);
        }
    }
    for (Tree &rest : trees) ordered.push_back(std::move(rest));
    return ordered;
}

void Clamp(const std::vector<Tree> &trees) {
    if (trees.empty()) {
        s_tree = s_node = s_first = 0;
        return;
    }
    s_tree = std::clamp(s_tree, 0, static_cast<int>(trees.size()) - 1);
    const int count = static_cast<int>(trees[static_cast<std::size_t>(s_tree)].nodes.size());
    s_node = count > 0 ? std::clamp(s_node, 0, count - 1) : 0;
    if (s_node < s_first) s_first = s_node;
    if (s_node >= s_first + kVisibleRows) s_first = s_node - kVisibleRows + 1;
    s_first = std::max(0, std::min(s_first, std::max(0, count - kVisibleRows)));
}

// "Needs Keen Edge 2" from the first unmet prerequisite, named by that node's own title.
std::string Prerequisites(const moonmod::Json &node, const std::vector<Tree> &trees) {
    if (!node.contains("prerequisites") || !node["prerequisites"].is_array()) return {};
    for (const moonmod::Json &requirement : node["prerequisites"]) {
        if (requirement.value("met", true)) continue;
        const std::string id = requirement.value("id", std::string{});
        std::string       name;
        for (const Tree &tree : trees) {
            for (const moonmod::Json &other : tree.nodes) {
                if (other.value("id", std::string{}) == id) name = other.value("title", std::string{});
            }
        }
        if (name.empty()) {
            name = id;
            const auto dot = name.rfind('.');
            if (dot != std::string::npos) name.erase(0, dot + 1);
            std::ranges::replace(name, '_', ' ');
            if (!name.empty()) name[0] = static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])));
        }
        return LocalizeFormat(LocalizeText("character.needs", "Needs %1 %2"),
                              {name, std::to_string(requirement.value("required_rank", 1))});
    }
    return {};
}

} // namespace

bool Available() { return moonmod::ActiveCharacterSheet() != nullptr; }

void Open() {
    s_tree = s_node = s_first = 0;
    s_cursor.SnapTo(static_cast<float>(kCursorX), static_cast<float>(kRowStartY - 2));
}

void RequestPendingOpen() { s_pending = true; }

bool TakePendingOpen() {
    const bool pending = s_pending;
    s_pending = false;
    return pending;
}

bool HandleKey() {
    if (GamePad.Down(PAD_CIRCLE)) {
        ComMenuSePlay(MENU_SOUND_REFUSE);
        return true;
    }
    const moonmod::CharacterSheet *sheet = moonmod::ActiveCharacterSheet();
    if (sheet == nullptr) return true;
    const std::vector<Tree> trees = Trees(*sheet);
    Clamp(trees);
    if (trees.empty()) return false;

    const int tree_count = static_cast<int>(trees.size());
    const int count = static_cast<int>(trees[static_cast<std::size_t>(s_tree)].nodes.size());
    bool      moved = false;
    if (GamePad.Down(PAD_UP) && count > 0) {
        s_node = (s_node + count - 1) % count;
        moved  = true;
    }
    if (GamePad.Down(PAD_DOWN) && count > 0) {
        s_node = (s_node + 1) % count;
        moved  = true;
    }
    if (GamePad.Down(PAD_LEFT | PAD_L1)) {
        s_tree = (s_tree + tree_count - 1) % tree_count;
        s_node = s_first = 0;
        moved = true;
    }
    if (GamePad.Down(PAD_RIGHT | PAD_R1)) {
        s_tree = (s_tree + 1) % tree_count;
        s_node = s_first = 0;
        moved = true;
    }
    if (moved) ComMenuSePlay(MENU_SOUND_CURSOR);
    Clamp(trees);
    s_scrollbar.Update(moved);

    if (GamePad.Down(PAD_CROSS) && count > 0) {
        const moonmod::Json &node = trees[static_cast<std::size_t>(s_tree)].nodes[static_cast<std::size_t>(s_node)];
        if (moonmod::RequestSkillTraining(sheet->owner_id, node.value("id", std::string{}))) {
            ComMenuSePlay(MENU_SOUND_CONFIRM);
        } else {
            ComMenuSePlay(MENU_SOUND_REFUSE);
        }
    }
    return false;
}

void Draw() {
    const moonmod::CharacterSheet *sheet = moonmod::ActiveCharacterSheet();
    if (sheet == nullptr) return;
    const std::vector<Tree> trees = Trees(*sheet);
    Clamp(trees);

    // 1. Paper window and its banner, as the Chest Tracker has them.
    ui::TrackerWindow::DrawPaper(64, 91, 512, 262, 0x80);
    ui::TrackerWindow::DrawStoneBanner(ui::Rect(150, 74, 340, 28), 0x80, 0x80,
                                       LocalizeFormat(LocalizeText("character.title", "JOURNAL: %1"), {sheet->title}));
    ui::Divider::DrawVertical(kDividerX, 132, 338, 0x80);
    ui::Divider::DrawHorizontal(84, kDividerX - 10, 285, 0x80);
    ui::Divider::DrawHorizontal(kRightPaneX, 542, 306, 0x80);
    MenuTextureReload(0x1A);

    // 2. Left pane: the sheet's lines, its hint, and its progress bar.
    ui::Label(LocalizeText("character.status", "STATUS"), ui::ColorYellow, ui::Alignment::Left, true, kHeaderScale)
        .Draw(kLeftPaneX, 132, 0x80);
    int y = kRowStartY;
    for (const std::string &line : sheet->lines) {
        ui::Label(line, ui::ColorWhite, ui::Alignment::Left, true, kBodyScale).Draw(kLeftPaneX, y, 0x80);
        y += 22;
    }
    if (!sheet->hint.empty()) {
        ui::Label(sheet->hint, ui::ColorGold, ui::Alignment::Left, true, kSmallScale)
            .DrawWrapped(kLeftPaneX, 288, 156, 15, 0x80);
    }
    if (sheet->progress >= 0.0) {
        ui::ProgressBarStyle bar_style{};
        bar_style.text_scale = kSmallScale;
        ui::ProgressBar::Draw(ui::Rect(kLeftPaneX, 325, 156, 20), static_cast<float>(sheet->progress),
                              sheet->progress_label, bar_style, 0x80);
    }

    // 3. Right pane: the selected tree's name with arrows, then its nodes.
    const int         tree_count = static_cast<int>(trees.size());
    const std::string tree_title = tree_count > 0 ? trees[static_cast<std::size_t>(s_tree)].title
                                                  : LocalizeText("character.no_skills", "No skills yet");
    const std::string heading =
        tree_count > 1 ? LocalizeFormat(LocalizeText("character.tree", "< %1 >"), {tree_title}) : tree_title;
    ui::Label(heading, ui::ColorYellow, ui::Alignment::Center, true, kHeaderScale)
        .Draw(kRightPaneX + kRightPaneW / 2, 132, 0x80);

    const int count = tree_count > 0 ? static_cast<int>(trees[static_cast<std::size_t>(s_tree)].nodes.size()) : 0;
    for (int row = 0; row < kVisibleRows && s_first + row < count; ++row) {
        const int            index    = s_first + row;
        const moonmod::Json &node     = trees[static_cast<std::size_t>(s_tree)].nodes[static_cast<std::size_t>(index)];
        const int            row_y    = kRowStartY + row * kRowSpacing;
        const bool           selected = index == s_node;
        const int            rank     = node.value("rank", 0);
        const int            max_rank = node.value("max_rank", 1);
        const bool           open     = node.value("prerequisites_met", true);
        const u32            color    = selected ? ui::ColorYellow : ui::ColorWhite;
        const int            alpha    = selected ? 0x80 : (open ? 0x70 : 0x48);
        ui::Label(node.value("title", std::string{}), color, ui::Alignment::Left, true, kBodyScale)
            .Draw(kNodeX, row_y, alpha);
        ui::DrawCounter(kCountX, row_y, rank, max_rank, kBodyScale, alpha);
    }
    if (count > kVisibleRows) {
        s_scrollbar.Draw(kScrollX, kRowStartY, kVisibleRows * kRowSpacing - 3, count, kVisibleRows, s_first, 0x80);
    }

    // 4. The selected node's description under the list, with the prerequisite it still needs.
    if (count > 0) {
        const moonmod::Json &node = trees[static_cast<std::size_t>(s_tree)].nodes[static_cast<std::size_t>(s_node)];
        std::string          description = node.value("description", std::string{});
        const std::string    needs       = Prerequisites(node, trees);
        if (!needs.empty()) description += (description.empty() ? "" : "  ") + needs;
        // At most two lines fit between the list and the window's edge; a longer text ends in "...".
        std::vector<std::string> wrapped = ui::WrapText(description, kRightPaneW, kSmallScale);
        if (static_cast<int>(wrapped.size()) > kDescLines) {
            wrapped.resize(static_cast<std::size_t>(kDescLines));
            wrapped.back() += "...";
        }
        for (std::size_t line = 0; line < wrapped.size(); ++line) {
            ui::Label(wrapped[line], needs.empty() ? ui::ColorWhite : ui::ColorGold, ui::Alignment::Left, true, kSmallScale)
                .Draw(kRightPaneX, 312 + static_cast<int>(line) * 16, 0x80);
        }
    }

    // 5. The mod's latest notice (training results), the controls bar, and the cursor.
    MenuTextureReload(0x1A);
    const moonmod::ManagerUiState &state = moonmod::ManagerState();
    if (state.notice_frames > 0 && !state.notice.empty()) {
        ui::Label(state.notice, ui::ColorGold, ui::Alignment::Center, true, kSmallScale).Draw(320, 372, 0x80);
    }
    const std::string controls = LocalizeText(
        "character.controls",
        PAD_GLYPH_UP " / " PAD_GLYPH_DOWN " Select   "
        PAD_GLYPH_LEFT " / " PAD_GLYPH_RIGHT " Path   "
        PAD_GLYPH_CROSS " Train   "
        PAD_GLYPH_CIRCLE " Return");
    ui::Label(controls, ui::ColorWhite, ui::Alignment::Center).Draw(320, 418, 0x80);

    if (count > 0) {
        const int sel_y = kRowStartY + (s_node - s_first) * kRowSpacing;
        s_brackets.Draw(ui::Rect(kNodeX - 2, sel_y - 1, kRightPaneW - 26, 22), 0x80);
        s_cursor.SetTarget(static_cast<float>(kCursorX), static_cast<float>(sel_y - 2));
        s_cursor.Draw(0x80);
    }
}

void AppendStrings(std::vector<std::pair<std::string, std::string>> &out) {
    const std::pair<const char *, const char *> kEntries[] = {
        {"character.title",     "JOURNAL: %1"},
        {"character.status",    "STATUS"},
        {"character.tree",      "< %1 >"},
        {"character.no_skills", "No skills yet"},
        {"character.needs",     "Needs %1 %2"},
        {"character.controls",  PAD_GLYPH_UP " / " PAD_GLYPH_DOWN " Select   "
                                PAD_GLYPH_LEFT " / " PAD_GLYPH_RIGHT " Path   "
                                PAD_GLYPH_CROSS " Train   "
                                PAD_GLYPH_CIRCLE " Return"},
    };
    for (const auto &[key, value] : kEntries) out.emplace_back(key, value);
}

} // namespace dc::ui::journal_character

// A mod's run menu (ui.run_menu_set): opened from Start in a dungeon (or by the mod) it pauses as the game's own
// pause does and shows the mod's tabs on one stone plate: a list of rows on the left, the tab's lines and the
// chosen row's detail on the right, the buttons along the foot. The pad and the mouse both drive it.

#include "moonmod/run_menu.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "dataread.hpp"
#include "dun/gameloop.hpp"
#include "gamepad.hpp"
#include "memcard.hpp"
#include "menu_draw.hpp"
#include "mglib.hpp"
#include "snd.hpp"
#include "texture.hpp"
#include "ui/framework.hpp"
#include "ui/menu_pointer.hpp"

namespace dc::game {

namespace {

std::optional<MoonModMenu> g_menu;
int                        g_tab = 0;
std::vector<int>           g_row;    // cursor per tab
std::vector<int>           g_scroll; // first row shown per tab
bool                       g_showing = false, g_requested = false, g_close = false;
int                        g_armed = 0;
ui::MenuPointer            g_pointer;

constexpr int kPlateX = 28, kPlateY = 24, kPlateW = 584, kPlateH = 432;
constexpr int kListX = 48, kListW = 300, kListTop = 108, kRowH = 22, kRowsShown = 13;
constexpr int kPaneX = 364, kPaneW = 228, kPaneTop = 104, kPaneH = 300;
constexpr int kTabY = 70;

struct Box {
    int x, y, w, h;
    bool Has(float px, float py) const { return px >= x && px < x + w && py >= y && py < y + h; }
};
std::vector<Box> g_tab_boxes;

void ToneRgb(int tone, unsigned char &r, unsigned char &g, unsigned char &b) {
    switch (tone) {
        case 1: r = 140, g = 232, b = 140; return;
        case 2: r = 240, g = 120, b = 120; return;
        case 3: r = 150, g = 152, b = 166; return;
        case 4: r = 255, g = 214, b = 120; return;
        default: r = 240, g = 238, b = 232; return;
    }
}

int Text(std::string_view text, float scale, int tone, ui::Alignment align, int x, int y, int alpha = 0x80) {
    unsigned char r = 0, g = 0, b = 0;
    ToneRgb(tone, r, g, b);
    ui::Label label(text, ui::ColorWhite, align, true, scale);
    label.SetCustomRgb(r, g, b);
    label.Draw(x, y, alpha);
    return label.GetWidth();
}

} // namespace

void MoonModMenuText(std::string_view text, const unsigned char rgb[3], int center_x, int y, int alpha) {
    ui::Label label(text, ui::ColorWhite, ui::Alignment::Center, false, 0.9f);
    label.SetCustomRgb(rgb[0], rgb[1], rgb[2]);
    label.SetOutline(1.5f, 24, 16, 8);
    label.Draw(center_x, y, alpha);
}

void MoonModMenuLabel(std::string_view text, const unsigned char rgb[3], int x, int y, int alpha, float scale, bool centred) {
    ui::Label label(text, ui::ColorWhite, centred ? ui::Alignment::Center : ui::Alignment::Left, false, scale);
    label.SetCustomRgb(rgb[0], rgb[1], rgb[2]);
    label.SetOutline(1.5f, 24, 16, 8);
    label.Draw(x, y, alpha);
}

namespace {

void Rect(int x, int y, int w, int h, spRGBA colour) { set2DSpriteC4(Vif1Packet, CRect_i_(x, y, w, h), &colour, &colour, &colour, &colour); }

void Fit() {
    if (!g_menu) return;
    const int tabs = static_cast<int>(g_menu->tabs.size());
    g_row.resize(tabs, 0);
    g_scroll.resize(tabs, 0);
    g_tab = std::clamp(g_tab, 0, std::max(0, tabs - 1));
    for (int t = 0; t < tabs; ++t) {
        const int rows = static_cast<int>(g_menu->tabs[t].rows.size());
        g_row[t] = std::clamp(g_row[t], 0, std::max(0, rows - 1));
        if (g_row[t] < g_scroll[t]) g_scroll[t] = g_row[t];
        if (g_row[t] >= g_scroll[t] + kRowsShown) g_scroll[t] = g_row[t] - kRowsShown + 1;
        g_scroll[t] = std::clamp(g_scroll[t], 0, std::max(0, rows - kRowsShown));
    }
}

void Hold(bool on) {
    driveStepHold = on ? 1 : 0;
    CMonUnitHold = on ? 1 : 0;
    CEffectHold = on ? 1 : 0;
    PlayTimeCountFlag(on ? 0 : 1);
}

void Close() {
    if (!g_showing) return;
    Hold(false);
    g_showing = false;
    g_pointer.Abandon();
    ui::MenuPointerShow(nullptr);
    if (g_menu) moonmod::ChronicleMenuShown(g_menu->owner, false, g_menu->tabs.empty() ? std::string() : g_menu->tabs[g_tab].id);
}

const std::string kNone;
const std::string &TabId() { return g_menu && !g_menu->tabs.empty() ? g_menu->tabs[g_tab].id : kNone; }

} // namespace

bool MoonModMenuSet(MoonModMenu menu) {
    if (g_menu && g_menu->owner != menu.owner) return false;
    // Keep the cursor on the same tab and row, by id, across a refresh.
    std::string tab_id = TabId(), row_id;
    if (g_menu && !g_menu->tabs.empty() && g_row.size() > static_cast<std::size_t>(g_tab)) {
        const auto &rows = g_menu->tabs[g_tab].rows;
        if (g_row[g_tab] < static_cast<int>(rows.size())) row_id = rows[g_row[g_tab]].id;
    }
    g_menu = std::move(menu);
    for (int t = 0; t < static_cast<int>(g_menu->tabs.size()); ++t) {
        if (g_menu->tabs[t].id != tab_id) continue;
        g_tab = t;
        g_row.resize(g_menu->tabs.size(), 0);
        const auto &rows = g_menu->tabs[t].rows;
        for (int r = 0; r < static_cast<int>(rows.size()); ++r) {
            if (rows[r].id == row_id) g_row[t] = r;
        }
    }
    Fit();
    return true;
}

void MoonModMenuClear(std::string_view owner) {
    if (!g_menu || g_menu->owner != owner) return;
    g_close = true;
    g_requested = false;
    if (!g_showing) {
        g_menu.reset();
        g_close = false;
    }
}

bool MoonModMenuAvailable() { return g_menu.has_value() && !g_menu->tabs.empty(); }

bool MoonModMenuRequestOpen(std::string_view owner, std::string_view tab) {
    if (!g_menu || g_menu->owner != owner || g_menu->tabs.empty()) return false;
    for (int t = 0; t < static_cast<int>(g_menu->tabs.size()); ++t) {
        if (!tab.empty() && g_menu->tabs[t].id == tab) g_tab = t;
    }
    if (!g_showing) g_requested = true;
    return true;
}

void MoonModMenuRequestClose(std::string_view owner) {
    if (g_menu && g_menu->owner == owner) {
        g_requested = false;
        if (g_showing) g_close = true;
    }
}

bool MoonModMenuWaiting() { return g_requested && MoonModMenuAvailable() && !g_showing; }

bool MoonModMenuUp() { return g_showing; }

void MoonModMenuBegin() {
    g_requested = false;
    if (!MoonModMenuAvailable() || g_showing) return;
    Hold(true);
    g_showing = true;
    g_armed = 12;
    g_close = false;
    Fit();
    ComMenuSePlay(MENU_SOUND_CONFIRM);
    if (!g_pointer.IsOpen()) g_pointer.Open();
    moonmod::ChronicleMenuShown(g_menu->owner, true, TabId());
}

bool MoonModMenuStep() {
    if (!g_showing) return true;
    if (g_close || !MoonModMenuAvailable()) {
        Close();
        if (g_close) {
            g_close = false;
            if (!g_requested && g_menu && g_menu->tabs.empty()) g_menu.reset();
        }
        return true;
    }
    if (g_armed > 0) --g_armed;
    MoonModMenu &menu = *g_menu;
    const int    tabs = static_cast<int>(menu.tabs.size());
    const int    was_tab = g_tab, was_row = g_row[g_tab];
    bool         use = false;

    if (GamePad.Down(PAD_L1)) g_tab = (g_tab + tabs - 1) % tabs;
    if (GamePad.Down(PAD_R1)) g_tab = (g_tab + 1) % tabs;
    const int rows = static_cast<int>(menu.tabs[g_tab].rows.size());
    if (rows > 0) {
        if (GamePad.Down(PAD_UP)) g_row[g_tab] = (g_row[g_tab] + rows - 1) % rows;
        if (GamePad.Down(PAD_DOWN)) g_row[g_tab] = (g_row[g_tab] + 1) % rows;
        if (GamePad.Down(PAD_LEFT)) g_row[g_tab] = std::max(0, g_row[g_tab] - kRowsShown);
        if (GamePad.Down(PAD_RIGHT)) g_row[g_tab] = std::min(rows - 1, g_row[g_tab] + kRowsShown);
    }
    if (g_armed == 0 && GamePad.Down(PAD_CROSS)) use = true;
    bool close = g_armed == 0 && (GamePad.Down(PAD_CIRCLE) || GamePad.Down(PAD_START));

    const auto take = g_pointer.Take(static_cast<float>(kListX), static_cast<float>(kListTop));
    ui::MenuPointerShow(&g_pointer);
    if (take.pointing && take.moved) {
        for (int t = 0; t < static_cast<int>(g_tab_boxes.size()) && t < tabs; ++t) {
            if (g_tab_boxes[t].Has(g_pointer.x, g_pointer.y) && (take.clicked & 1) != 0) g_tab = t;
        }
        const int shown = static_cast<int>(menu.tabs[g_tab].rows.size());
        for (int i = 0; i < kRowsShown && g_scroll[g_tab] + i < shown; ++i) {
            const Box row = {kListX - 6, kListTop + i * kRowH - 2, kListW + 12, kRowH};
            if (!row.Has(g_pointer.x, g_pointer.y)) continue;
            g_row[g_tab] = g_scroll[g_tab] + i;
            if ((take.clicked & 1) != 0 && g_armed == 0) use = true;
        }
        if (take.wheel != 0.0f && shown > 0) g_row[g_tab] = std::clamp(g_row[g_tab] + (take.wheel > 0.0f ? -1 : 1), 0, shown - 1);
        if ((take.clicked & 2) != 0 && g_armed == 0) close = true;
    }
    Fit();
    if (g_tab != was_tab || g_row[g_tab] != was_row) ComMenuSePlay(MENU_SOUND_CURSOR);
    if (use) {
        const auto &tab = menu.tabs[g_tab];
        if (g_row[g_tab] < static_cast<int>(tab.rows.size()) && tab.rows[g_row[g_tab]].enabled) {
            ComMenuSePlay(MENU_SOUND_CONFIRM);
            moonmod::ChronicleMenuAction(menu.owner, tab.id, tab.rows[g_row[g_tab]].id);
        } else {
            ComMenuSePlay(MENU_SOUND_REFUSE);
        }
    }
    if (close) {
        ComMenuSePlay(MENU_SOUND_CONFIRM);
        Close();
        return true;
    }
    return false;
}

void MoonModMenuDraw() {
    if (!g_showing || !MoonModMenuAvailable()) return;
    const MoonModMenu    &menu = *g_menu;
    const MoonModMenuTab &tab = menu.tabs[g_tab];

    Rect(0, 0, 640, SCREEN_HEIGHT, {0, 0, 8, 0x58});
    ui::TrackerWindow::Draw(kPlateX, kPlateY, kPlateW, kPlateH, 0x80, ui::WindowPreset::BlueStone);
    ui::Label heading(menu.title, ui::ColorGold, ui::Alignment::Center, true, 1.0f, ui::FontWeight::Bold);
    heading.SetOutline(2.0f);
    heading.Draw(320, kPlateY + 12);

    // The tabs, spread across the plate; the open one gold and underlined.
    g_tab_boxes.clear();
    const int tabs = static_cast<int>(menu.tabs.size());
    const int span = (kPlateW - 120) / std::max(1, tabs);
    ui::Label(std::string("{L1}"), ui::ColorWhite, ui::Alignment::Left, true, 1.0f).Draw(kPlateX + 20, kTabY - 2);
    ui::Label(std::string("{R1}"), ui::ColorWhite, ui::Alignment::Right, true, 1.0f).Draw(kPlateX + kPlateW - 20, kTabY - 2);
    for (int t = 0; t < tabs; ++t) {
        const int cx = kPlateX + 60 + span * t + span / 2;
        const int w = Text(menu.tabs[t].title, 0.7f, t == g_tab ? 4 : 3, ui::Alignment::Center, cx, kTabY);
        g_tab_boxes.push_back({cx - span / 2, kTabY - 4, span, 24});
        if (t == g_tab) Rect(cx - w / 2, kTabY + 19, w, 2, {248, 206, 92, 0x80});
    }
    Rect(kPlateX + 16, kTabY + 26, kPlateW - 32, 1, {200, 210, 240, 0x30});

    // The rows.
    const int rows = static_cast<int>(tab.rows.size());
    if (rows == 0) Text("Nothing here yet.", 0.66f, 3, ui::Alignment::Left, kListX, kListTop);
    for (int i = 0; i < kRowsShown && g_scroll[g_tab] + i < rows; ++i) {
        const int             index = g_scroll[g_tab] + i;
        const MoonModMenuRow &row = tab.rows[index];
        const int             y = kListTop + i * kRowH;
        const bool            on = index == g_row[g_tab];
        if (on) Rect(kListX - 6, y - 2, kListW + 12, kRowH - 2, {248, 206, 92, 0x22});
        const int tone = !row.enabled ? 3 : (on && row.tone == 0 ? 4 : row.tone);
        Text(row.label, 0.66f, tone, ui::Alignment::Left, kListX + 4, y);
        if (!row.value.empty()) Text(row.value, 0.62f, row.enabled ? 0 : 3, ui::Alignment::Right, kListX + kListW, y + 1);
    }
    if (g_scroll[g_tab] > 0) Text("^", 0.6f, 3, ui::Alignment::Center, kListX + kListW / 2, kListTop - 14);
    if (g_scroll[g_tab] + kRowsShown < rows) Text("v", 0.6f, 3, ui::Alignment::Center, kListX + kListW / 2, kListTop + kRowsShown * kRowH - 4);

    // The side pane: the tab's lines, then the row's detail.
    // A dark inset with a faint rim.
    Rect(kPaneX, kPaneTop, kPaneW, kPaneH, {8, 12, 30, 0x48});
    const spRGBA rim = {200, 210, 240, 0x28};
    Rect(kPaneX, kPaneTop, kPaneW, 1, rim);
    Rect(kPaneX, kPaneTop + kPaneH - 1, kPaneW, 1, rim);
    Rect(kPaneX, kPaneTop, 1, kPaneH, rim);
    Rect(kPaneX + kPaneW - 1, kPaneTop, 1, kPaneH, rim);
    int y = kPaneTop + 12;
    for (const std::string &line : tab.lines) {
        for (const std::string &part : ui::WrapText(line, kPaneW - 24, 0.6f)) {
            if (y > kPaneTop + kPaneH - 18) break;
            Text(part, 0.6f, 0, ui::Alignment::Left, kPaneX + 12, y);
            y += 15;
        }
    }
    if (rows > 0) {
        const MoonModMenuRow &row = tab.rows[g_row[g_tab]];
        if (!tab.lines.empty()) {
            y += 6;
            Rect(kPaneX + 12, y, kPaneW - 24, 1, {200, 210, 240, 0x30});
            y += 8;
        }
        for (const std::string &part : ui::WrapText(row.label, kPaneW - 24, 0.72f)) {
            Text(part, 0.72f, 4, ui::Alignment::Left, kPaneX + 12, y);
            y += 18;
        }
        for (const std::string &part : ui::WrapText(row.detail, kPaneW - 24, 0.6f)) {
            if (y > kPaneTop + kPaneH - 18) break;
            Text(part, 0.6f, 0, ui::Alignment::Left, kPaneX + 12, y);
            y += 15;
        }
    }

    ui::Label("{up}{down} Choose   {cross} Use   {circle} Close", ui::ColorWhite, ui::Alignment::Center, true, 1.0f)
        .Draw(320, kPlateY + kPlateH - 36);

    float hand_x = 0.0f, hand_y = 0.0f;
    if (ui::MenuPointerHand(hand_x, hand_y)) {
        if (CTexture *hand = TexManager.GetTexture(const_cast<char *>("icon01"), -1)) {
            RECT rect = {0, 0, 32, 32};
            setbilinear(0);
            DrawObjectVibe(static_cast<int>(hand_x), static_cast<int>(hand_y), hand, rect, 128, 0);
            setbilinear(1);
        } else {
            Rect(static_cast<int>(hand_x) + 22, static_cast<int>(hand_y) + 10, 8, 8, {248, 206, 92, 0x80});
        }
    }
}

} // namespace dc::game

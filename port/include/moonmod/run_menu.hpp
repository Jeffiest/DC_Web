#pragma once

#include <string>
#include <string_view>
#include <vector>

// A mod's run menu (ui.run_menu_set), drawn and driven by src/moonmod/run_menu_moonmod.cpp: while a mod has one,
// Start in a dungeon opens it in place of the bare pause. Tabs (L1/R1) of rows (up/down); Cross uses a row and the
// owner hears of it (ui.run_menu_action) and may set the menu again while it stays open.
namespace dc::game {
// A line of the game's menu text in a colour, centred on x (the native weapon menu's names a mod gave).
void MoonModMenuText(std::string_view text, const unsigned char rgb[3], int center_x, int y, int alpha);
// The same, at a size, from its left edge or centred on x.
void MoonModMenuLabel(std::string_view text, const unsigned char rgb[3], int x, int y, int alpha, float scale, bool centred);
// True while the weapon menu shows a mod's card for the weapon under the cursor (the description box gives way).
bool MoonModWeaponCardShown();
// True while the item page names a gear piece (the one in hand or under the cursor) in the help window, which then
// shows that in place of the game's description; MoonModGearInfoDraw draws it over the window.
bool MoonModGearInfoShown();
void MoonModGearInfoDraw(int alpha);
// A gear piece's card in the right-hand help window (a shop's: price the good's cost, or -1 for what it sells for).
struct GearPiece;
void MoonModGearCardDraw(const GearPiece &piece, int alpha, int price);
// The hero page (the item page's character while the gear page is open): whether it is shown, and the place under the
// menu point x, y as the mode and cursor the pad would select it with (false for none). drag is whether a press there
// picks something up to carry, as a board cell does.
bool MoonModHeroPageShown();
// How far right the hero page moves ACTIVE ITEM's boxes, to the middle of its tablet (0 off the hero page).
int  MoonModActiveItemShift();
bool MoonModHeroSlotAt(float x, float y, int &mode, int &cursor, bool &drag);
// A weapon's own name in the game's language (empty before the game's text is loaded).
std::string MoonModWeaponNativeName(int item_no);
struct MoonModMenuRow {
    std::string id;
    std::string label;
    std::string value;  // right-aligned beside the label
    std::string detail; // the side pane's text for this row
    int         tone = 0; // 0 plain, 1 good, 2 bad, 3 dim, 4 gold
    bool        enabled = true;
};
struct MoonModMenuTab {
    std::string                 id;
    std::string                 title;
    std::vector<std::string>    lines; // the side pane's heading lines for the tab
    std::vector<MoonModMenuRow> rows;
};
struct MoonModMenu {
    std::string                 owner;
    std::string                 title;
    std::vector<MoonModMenuTab> tabs;
};
// Sets (or replaces, keeping the cursor on the same tab and row ids) the menu; false when another mod owns it.
bool MoonModMenuSet(MoonModMenu menu);
void MoonModMenuClear(std::string_view owner);
bool MoonModMenuAvailable();
// Asks for the menu to open (on a tab id, or where it was) when play next allows.
bool MoonModMenuRequestOpen(std::string_view owner, std::string_view tab);
void MoonModMenuRequestClose(std::string_view owner);
// The dungeon: a request waiting for play to pause for it.
bool MoonModMenuWaiting();
// Opening from Start and the paused task's step: true once the menu has closed.
void MoonModMenuBegin();
bool MoonModMenuStep();
void MoonModMenuDraw();
bool MoonModMenuUp();
} // namespace dc::game

namespace moonmod {
void ChronicleMenuAction(const std::string &owner, const std::string &tab, const std::string &row);
void ChronicleMenuShown(const std::string &owner, bool open, const std::string &tab);
} // namespace moonmod

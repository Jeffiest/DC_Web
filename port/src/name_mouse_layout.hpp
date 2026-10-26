#pragma once

// Where the controls of the Register Name screen (ps2/src/battle_globals.cpp) lie on the game's 640x480
// 2D screen, and which one a point is over. Pure geometry with no game types, so tests can drive it:
// the numbers are the ones DrawNameTemplete, DrawCharaName and NameEnterDraw draw with.

namespace namemouse {

// NameSelect::area, as the game numbers them.
constexpr int kAreaKeyboard = 4;
constexpr int kAreaTabs = 5;
constexpr int kAreaHelp = 6;
constexpr int kAreaConfirm = 7;
constexpr int kAreaEmptyNotice = 8;

// NameSelect::input_mode.
constexpr int kInputKatakana = 0;
constexpr int kInputHiragana = 1;
constexpr int kInputAlphabet = 2;
constexpr int kInputSymbol = 3;

// NameSelect::cursor while the cursor is on the tabs.
constexpr int kTabDefaultName = 4;
constexpr int kTabOk = 5;
constexpr int kTabLeft = 6;
constexpr int kTabRight = 7;
constexpr int kTabDelete = 8;
constexpr int kTabInsert = 9;
constexpr int kTabHelp = 10;
constexpr int kTabCount = 11;

constexpr int kNameSlots = 10;

// What a screen shows, as far as the controls' places depend on it.
struct Screen {
    // Language: 0 Japanese, 1 American English, 2 British English, then French, German, Italian, Spanish.
    int language = 1;
    int input_mode = kInputAlphabet;
    // menu_euro_codetbl[language - 2][0]: the 26 character codes of the accented keys, or null.
    const short *euro_codes = nullptr;
    // euro_code_linelimmit[language - 2]: how many rows of accented keys the language has.
    int euro_rows = 0;
    // menu_kigoutbl: the 40 symbol keys' cells; 16 is a blank key.
    const signed char *symbol_cells = nullptr;
};

enum class Kind {
    None,
    Key,
    Tab,
    Slot,
};

struct Hit {
    Kind kind = Kind::None;
    // Key: NameSelect::cursor. Tab: the NameSelectTab. Slot: the position in the name.
    int index = -1;

    bool operator==(const Hit &) const = default;
};

// A rectangle in the 640x480 space; left and top are inside, right and bottom outside.
struct Rect {
    int left = 0;
    int top = 0;
    int right = 0;
    int bottom = 0;

    bool Contains(float x, float y) const { return x >= left && x < right && y >= top && y < bottom; }
};

// The place of a tab, empty when the language has no such tab (Japanese tabs on a Latin screen).
Rect TabRect(int language, int tab);

// The place of a key of the keyboard being shown, empty when there is no such key.
Rect KeyRect(const Screen &screen, int key);

// The place of a slot of the name.
Rect SlotRect(int slot);

// The window the message of a dialog area (help, confirm, empty-name notice) is in.
Rect DialogRect(int area);

// The confirm dialog ("Accept?") has a Yes line and a No line, the pad's Cross and Circle: 1 for a
// point on Yes, -1 on No, 0 elsewhere.
int ConfirmChoice(float x, float y);

// What lies under the point while the cursor is in the keyboard or tabs. Tabs beat keys where they meet.
Hit HitTest(const Screen &screen, float x, float y);

// How many 640x480 units one count of pointer motion (a window unit) moves along an axis, given how
// many main-target pixels the screen gives a unit (gfx::GetUiMapping's scale), how wide the target is
// in pixels and how wide the window is in the pointer's units. 0 when any is not positive.
float PointerUnitsPerCount(float target_pixels_per_unit, float target_width, float window_width);

} // namespace namemouse

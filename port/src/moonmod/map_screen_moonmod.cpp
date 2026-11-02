// A mod's map screen (ui.map_open): the whole map of a floor on a dimmed screen. Rooms are drawn as blocks and roads as
// lines between the cells they join, only where the player has seen; a mod's marks sit on it (shrines, a waypoint),
// with the player and a cursor. The pad (or the left stick) moves the cursor a cell at a time, L1/R1 step between the
// marks Cross can take, Triangle puts the waypoint, and Circle, Select or Start closes it.

#include "moonmod/map_screen.hpp"

#include <algorithm>
#include <cmath>
#include <optional>
#include <utility>

#include "dataread.hpp"
#include "dun/gameloop.hpp"
#include "dungeonmap.hpp"
#include "gamepad.hpp"
#include "menu_draw.hpp"
#include "mglib.hpp"
#include "snd.hpp"
#include "ui/framework.hpp"

namespace dc::game {

namespace {

std::optional<MoonModMapView> g_map;
bool                          g_showing = false; // the dungeon has paused for it
bool                          g_town_locked = false;
int                           g_armed = 0;       // frames before a press counts (the press that opened it)
int                           g_repeat = 0;      // frames until a held direction moves again
int                           g_held = 0;        // the direction held (bits 1 up, 2 right, 4 left, 8 down)
int                           g_blink = 0;

constexpr int kAreaX = 36, kAreaY = 78, kAreaW = 568;
constexpr int kAreaH = SCREEN_HEIGHT - 78 - 96;

void Rect(int x, int y, int w, int h, spRGBA colour) { set2DSpriteC4(Vif1Packet, CRect_i_(x, y, w, h), &colour, &colour, &colour, &colour); }

// The map as drawn: shapes and seen cells of the live floor, or the kept ones.
const unsigned char *g_shapes = nullptr;
const unsigned char *g_seen = nullptr;
std::vector<unsigned char> g_live_shapes, g_live_seen;
int                        g_live_seed = -1;

void Refresh() {
    if (!g_map) return;
    if (g_map->live) {
        if (g_live_shapes.size() != DNG_CELLS || g_live_seed != MainDungeonMap.map_seed) {
            g_live_shapes.assign(DNG_CELLS, 0);
            DngMapCellShapes(MainDungeonMap, g_live_shapes.data());
            g_live_seed = MainDungeonMap.map_seed;
        }
        g_live_seen.assign(DNG_CELLS, 0);
        const bool all = BtEquipMap != 0;
        for (int cell = 0; cell < DNG_CELLS; ++cell) g_live_seen[cell] = (all || MainDungeonMap.mask[cell] != 0) ? 1 : 0;
        g_shapes = g_live_shapes.data();
        g_seen = g_live_seen.data();
    } else {
        g_shapes = g_map->shapes.data();
        g_seen = g_map->seen.data();
    }
}

bool Shown(int cell) { return g_shapes != nullptr && g_shapes[cell] != 0 && g_seen[cell] != 0; }

// A shrine or gate the player has not found yet stays off the map.
bool Hidden(const MoonModMapMark &mark) {
    return (mark.kind == MOONMOD_MAP_SHRINE || mark.kind == MOONMOD_MAP_GATE) && !Shown(mark.x + mark.y * DNG_GRID);
}

// The player's cell and facing on a live map, or false.
bool PlayerCell(float &col, float &row, float &angle) {
    if (!g_map || !g_map->live || CharaMain.frame == nullptr) return false;
    float at[4];
    CharaMain.GetPosition(at);
    col = at[0] / 160.0f;
    row = at[2] / 160.0f;
    float rotation[4] = {};
    CharaMain.frame->GetRotation(rotation);
    angle = rotation[1];
    return true;
}

// The box of seen cells the map is fitted to (at least a few cells round the cursor).
struct Fit {
    int min_x, min_y, max_x, max_y, tile, origin_x, origin_y;
};
Fit Layout() {
    Fit fit{DNG_GRID, DNG_GRID, -1, -1, 8, 0, 0};
    for (int cell = 0; cell < DNG_CELLS; ++cell) {
        if (!Shown(cell)) continue;
        const int x = cell % DNG_GRID, y = cell / DNG_GRID;
        fit.min_x = std::min(fit.min_x, x), fit.max_x = std::max(fit.max_x, x);
        fit.min_y = std::min(fit.min_y, y), fit.max_y = std::max(fit.max_y, y);
    }
    for (const MoonModMapMark &mark : g_map->marks) {
        if (Hidden(mark)) continue;
        fit.min_x = std::min(fit.min_x, mark.x), fit.max_x = std::max(fit.max_x, mark.x);
        fit.min_y = std::min(fit.min_y, mark.y), fit.max_y = std::max(fit.max_y, mark.y);
    }
    if (fit.max_x < fit.min_x) {
        const int cx = std::clamp(g_map->cursor_x, 0, DNG_GRID - 1), cy = std::clamp(g_map->cursor_y, 0, DNG_GRID - 1);
        fit = {cx, cy, cx, cy, 8, 0, 0};
    }
    fit.min_x = std::max(0, fit.min_x - 2), fit.min_y = std::max(0, fit.min_y - 2);
    fit.max_x = std::min(DNG_GRID - 1, fit.max_x + 2), fit.max_y = std::min(DNG_GRID - 1, fit.max_y + 2);
    const int cols = fit.max_x - fit.min_x + 1, rows = fit.max_y - fit.min_y + 1;
    fit.tile = std::clamp(std::min(kAreaW / cols, kAreaH / rows), 4, 28);
    fit.origin_x = kAreaX + (kAreaW - cols * fit.tile) / 2 - fit.min_x * fit.tile;
    fit.origin_y = kAreaY + (kAreaH - rows * fit.tile) / 2 - fit.min_y * fit.tile;
    return fit;
}

int MarkAt(int x, int y) {
    for (std::size_t i = 0; i < g_map->marks.size(); ++i) {
        const MoonModMapMark &mark = g_map->marks[i];
        if (mark.x == x && mark.y == y && mark.kind != MOONMOD_MAP_WAYPOINT && !Hidden(mark)) return static_cast<int>(i);
    }
    for (std::size_t i = 0; i < g_map->marks.size(); ++i) {
        if (g_map->marks[i].x == x && g_map->marks[i].y == y && !Hidden(g_map->marks[i])) return static_cast<int>(i);
    }
    return -1;
}

// Moves the cursor to the next mark Cross can take, forward or back in the marks' order.
void StepMark(int way) {
    const int count = static_cast<int>(g_map->marks.size());
    if (count == 0) return;
    int at = MarkAt(g_map->cursor_x, g_map->cursor_y);
    for (int n = 0; n < count; ++n) {
        at = at < 0 ? (way > 0 ? 0 : count - 1) : (at + way + count) % count;
        if (g_map->marks[at].pick && !Hidden(g_map->marks[at])) {
            g_map->cursor_x = g_map->marks[at].x;
            g_map->cursor_y = g_map->marks[at].y;
            ComMenuSePlay(MENU_SOUND_CURSOR);
            return;
        }
    }
}

void Close() {
    if (!g_map) return;
    const std::string owner = g_map->owner, id = g_map->id;
    g_map.reset();
    moonmod::ChronicleMapClosed(owner, id);
}

void Resume() {
    if (!g_showing) return;
    driveStepHold = 0;
    CMonUnitHold = 0;
    CEffectHold = 0;
    PlayTimeCountFlag(1);
    g_showing = false;
}

// One frame of the pad: true once the map has closed.
bool Input() {
    if (!g_map) return true;
    if (g_armed > 0) --g_armed;
    ++g_blink;
    // A direction moves the cursor at once, then again every few frames while held.
    int held = 0;
    if (GamePad.On(PAD_UP) || GamePad.GetLYf() < -0.5f) held |= 1;
    if (GamePad.On(PAD_RIGHT) || GamePad.GetLXf() > 0.5f) held |= 2;
    if (GamePad.On(PAD_LEFT) || GamePad.GetLXf() < -0.5f) held |= 4;
    if (GamePad.On(PAD_DOWN) || GamePad.GetLYf() > 0.5f) held |= 8;
    bool move = false;
    if (held != g_held) {
        g_held = held;
        g_repeat = 14;
        move = held != 0;
    } else if (held != 0 && --g_repeat <= 0) {
        g_repeat = 3;
        move = true;
    }
    if (move) {
        const int x = std::clamp(g_map->cursor_x + ((held & 2) ? 1 : 0) - ((held & 4) ? 1 : 0), 0, DNG_GRID - 1);
        const int y = std::clamp(g_map->cursor_y + ((held & 8) ? 1 : 0) - ((held & 1) ? 1 : 0), 0, DNG_GRID - 1);
        if (x != g_map->cursor_x || y != g_map->cursor_y) {
            g_map->cursor_x = x, g_map->cursor_y = y;
            ComMenuSePlay(MENU_SOUND_CURSOR);
        }
    }
    if (GamePad.Down(PAD_R1)) StepMark(1);
    if (GamePad.Down(PAD_L1)) StepMark(-1);
    if (g_armed > 0) return false;
    if (g_map->pages && (GamePad.Down(PAD_L2) || GamePad.Down(PAD_R2))) {
        ComMenuSePlay(MENU_SOUND_CURSOR);
        moonmod::ChronicleMapPage(g_map->owner, g_map->id, GamePad.Down(PAD_R2) ? 1 : -1);
    }
    if (GamePad.Down(PAD_CIRCLE) || GamePad.Down(PAD_SELECT) || GamePad.Down(PAD_START)) {
        ComMenuSePlay(MENU_SOUND_REFUSE);
        Close();
        return true;
    }
    if (GamePad.Down(PAD_TRIANGLE) && g_map->waypoints) {
        const int x = g_map->cursor_x, y = g_map->cursor_y;
        auto &marks = g_map->marks;
        const auto old = std::ranges::find_if(marks, [](const MoonModMapMark &m) { return m.kind == MOONMOD_MAP_WAYPOINT; });
        const bool here = old != marks.end() && old->x == x && old->y == y;
        if (old != marks.end()) marks.erase(old);
        if (!here) marks.push_back({x, y, MOONMOD_MAP_WAYPOINT, "Waypoint", false});
        ComMenuSePlay(here ? MENU_SOUND_REFUSE : MENU_SOUND_CONFIRM);
        if (g_map->live) {
            // the mini map's flag too, at once
            DngWaypointX = here ? -1 : x;
            DngWaypointY = here ? -1 : y;
            DngWaypointSeed = MainDungeonMap.map_seed;
        }
        moonmod::ChronicleMapWaypoint(g_map->owner, g_map->id, x, y, !here);
    }
    if (GamePad.Down(PAD_CROSS)) {
        const int mark = MarkAt(g_map->cursor_x, g_map->cursor_y);
        if (mark >= 0 && g_map->marks[mark].pick) {
            ComMenuSePlay(MENU_SOUND_CONFIRM);
            const std::string owner = g_map->owner, id = g_map->id;
            const int x = g_map->cursor_x, y = g_map->cursor_y;
            g_map.reset();
            moonmod::ChronicleMapChoice(owner, id, x, y, mark + 1);
            moonmod::ChronicleMapClosed(owner, id);
            return true;
        }
        ComMenuSePlay(MENU_SOUND_REFUSE);
    }
    return false;
}

} // namespace

bool MoonModMapOpen(MoonModMapView view) {
    if (g_map && g_map->owner != view.owner) return false;
    const bool page = g_map.has_value();
    g_map = std::move(view);
    g_armed = page ? 0 : 12;
    g_held = 0;
    g_repeat = 0;
    g_live_seed = -1;
    Refresh();
    if (g_map->cursor_x < 0 || g_map->cursor_y < 0) {
        float col = 0.0f, row = 0.0f, angle = 0.0f;
        if (PlayerCell(col, row, angle)) {
            g_map->cursor_x = static_cast<int>(std::lround(col)), g_map->cursor_y = static_cast<int>(std::lround(row));
        } else {
            const Fit fit = Layout();
            g_map->cursor_x = (fit.min_x + fit.max_x) / 2, g_map->cursor_y = (fit.min_y + fit.max_y) / 2;
        }
    }
    g_map->cursor_x = std::clamp(g_map->cursor_x, 0, DNG_GRID - 1);
    g_map->cursor_y = std::clamp(g_map->cursor_y, 0, DNG_GRID - 1);
    return true;
}

void MoonModMapCloseOwner(std::string_view owner) {
    if (!g_map || g_map->owner != owner) return;
    g_map.reset();
}

bool MoonModMapWaiting() { return g_map.has_value() && !g_showing; }

bool MoonModMapUp() { return g_map.has_value(); }

bool MoonModMapStep() {
    if (!g_showing) {
        if (!g_map) return true;
        driveStepHold = 1;
        CMonUnitHold = 1;
        CEffectHold = 1;
        PlayTimeCountFlag(0);
        g_showing = true;
        ComMenuSePlay(MENU_SOUND_CONFIRM);
    }
    if (!g_map || Input()) {
        Resume();
        return true;
    }
    return false;
}

void MoonModMapStepTown(bool walking) {
    if (g_town_locked) {
        GamePad.KeyLock(0);
        g_town_locked = false;
    }
    if (!g_map || !walking) return;
    Input();
    // The town's own controls wait while the map is up, and on the frame it closes (Start closes it, not the pause).
    GamePad.KeyLock(1);
    g_town_locked = true;
}

bool MoonModMapPaused() { return g_showing; }

void MoonModMapDraw() {
    if (!g_map || (!g_showing && !g_town_locked)) return;
    Refresh();
    const Fit fit = Layout();
    const int t = fit.tile;

    Rect(-400, 0, 1440, SCREEN_HEIGHT, {0, 2, 10, 0x68});
    ui::TrackerWindow::Draw(16, 20, 608, SCREEN_HEIGHT - 40, 0x70, ui::WindowPreset::BlueStone);
    ui::Label heading(g_map->title, ui::ColorGold, ui::Alignment::Center, true, 0.95f, ui::FontWeight::Bold);
    heading.SetOutline(2.0f);
    heading.Draw(320, 30);
    if (!g_map->subtitle.empty()) {
        ui::Label sub(g_map->subtitle, ui::ColorWhite, ui::Alignment::Center, true, 0.6f);
        sub.SetCustomRgb(170, 172, 186);
        sub.Draw(320, 56);
    }

    // The cells: a room's floor fills its cell (reaching into the cells it opens to), a road is a line through its
    // middle out to the sides it opens to.
    const spRGBA room = {58, 92, 128, 0x78}, road = {120, 156, 196, 0x78}, rim = {150, 190, 230, 0x50};
    const int    line = std::max(2, t / 3);
    for (int cell = 0; cell < DNG_CELLS; ++cell) {
        if (!Shown(cell)) continue;
        const int shape = g_shapes[cell];
        const int x = (cell % DNG_GRID) * t + fit.origin_x, y = (cell / DNG_GRID) * t + fit.origin_y;
        if ((shape & 16) != 0) {
            const int inset = std::max(1, t / 8);
            const int l = (shape & 4) ? 0 : inset, r = (shape & 2) ? 0 : inset, u = (shape & 1) ? 0 : inset, d = (shape & 8) ? 0 : inset;
            Rect(x + l - 1, y + u - 1, t - l - r + 2, t - u - d + 2, rim);
            Rect(x + l, y + u, t - l - r, t - u - d, room);
        } else {
            const int mid = (t - line) / 2;
            Rect(x + mid, y + mid, line, line, road);
            if (shape & 1) Rect(x + mid, y, line, mid, road);
            if (shape & 8) Rect(x + mid, y + mid + line, line, t - mid - line, road);
            if (shape & 4) Rect(x, y + mid, mid, line, road);
            if (shape & 2) Rect(x + mid + line, y + mid, t - mid - line, line, road);
        }
    }

    // The marks.
    for (const MoonModMapMark &mark : g_map->marks) {
        if (Hidden(mark)) continue; // not found yet
        const int x = mark.x * t + fit.origin_x, y = mark.y * t + fit.origin_y;
        const int size = std::max(6, t * 2 / 3), off = (t - size) / 2;
        switch (mark.kind) {
            case MOONMOD_MAP_SHRINE:
                Rect(x + off, y + off, size, size, {90, 96, 110, 0x80});
                Rect(x + off + 2, y + off + 2, size - 4, size - 4, {40, 44, 54, 0x80});
                break;
            case MOONMOD_MAP_SHRINE_LIT:
                Rect(x + off - 1, y + off - 1, size + 2, size + 2, {255, 236, 160, 0x80});
                Rect(x + off + 1, y + off + 1, size - 2, size - 2, {90, 200, 255, 0x80});
                break;
            case MOONMOD_MAP_WAYPOINT:
                Rect(x + t / 2 - 1, y - t / 2, 2, t, {250, 240, 230, 0x80});
                Rect(x + t / 2 + 1, y - t / 2, std::max(5, t / 2), std::max(4, t / 3), {240, 70, 60, 0x80});
                break;
            case MOONMOD_MAP_GATE: Rect(x + off, y + off, size, size, {220, 160, 70, 0x80}); break;
            case MOONMOD_MAP_STAIR: Rect(x + off, y + off, size, size, {150, 230, 150, 0x80}); break;
            default: Rect(x + off, y + off, size, size, {230, 230, 230, 0x80}); break;
        }
    }

    // The player, a white diamond with a point the way they face.
    float col = 0.0f, row = 0.0f, angle = 0.0f;
    if (PlayerCell(col, row, angle)) {
        const int px = fit.origin_x + static_cast<int>(std::lround(col * t)) + t / 2;
        const int py = fit.origin_y + static_cast<int>(std::lround(row * t)) + t / 2;
        const int size = std::max(6, t * 3 / 4);
        Rect(px - size / 2, py - size / 2, size, size, {255, 120, 200, 0x80});
        Rect(px - size / 2 + 2, py - size / 2 + 2, size - 4, size - 4, {255, 255, 255, 0x80});
        const int tip_x = px + static_cast<int>(std::lround(std::sin(angle) * size)), tip_y = py + static_cast<int>(std::lround(std::cos(angle) * size));
        Rect(tip_x - 2, tip_y - 2, 4, 4, {255, 120, 200, 0x80});
    }

    // The cursor, a gold frame that pulses.
    {
        const int x = g_map->cursor_x * t + fit.origin_x, y = g_map->cursor_y * t + fit.origin_y;
        const unsigned char a = static_cast<unsigned char>(0x50 + (g_blink / 4 % 8) * 6);
        const spRGBA gold = {248, 206, 92, a};
        Rect(x - 2, y - 2, t + 4, 2, gold);
        Rect(x - 2, y + t, t + 4, 2, gold);
        Rect(x - 2, y, 2, t, gold);
        Rect(x + t, y, 2, t, gold);
    }

    // What the cursor is on, and the buttons.
    const int under = MarkAt(g_map->cursor_x, g_map->cursor_y);
    if (under >= 0 && !g_map->marks[under].label.empty()) {
        ui::Label label(g_map->marks[under].label, ui::ColorGold, ui::Alignment::Center, true, 0.75f, ui::FontWeight::Bold);
        label.SetOutline(2.0f);
        label.Draw(320, SCREEN_HEIGHT - 88);
    }
    // The buttons, in two rows.
    std::string moves = "{up}{down}{left}{right} Move    {l1}{r1} Marks";
    if (g_map->pages) moves += "    {l2}{r2} Maps";
    std::string acts;
    if (!g_map->hint.empty()) acts += "{cross} " + g_map->hint + "    ";
    if (g_map->waypoints) acts += "{triangle} Waypoint    ";
    acts += "{circle} Close";
    ui::Label(moves, ui::ColorWhite, ui::Alignment::Center, true, 0.7f).Draw(320, SCREEN_HEIGHT - 62);
    ui::Label(acts, ui::ColorWhite, ui::Alignment::Center, true, 0.7f).Draw(320, SCREEN_HEIGHT - 40);
}

void MoonModMapReset() {
    Resume();
    g_map.reset();
    if (g_town_locked) {
        GamePad.KeyLock(0);
        g_town_locked = false;
    }
}

} // namespace dc::game

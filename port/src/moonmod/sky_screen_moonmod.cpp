// A mod's sky page in play (ui.sky_open): the title mode's node sky over a paused dungeon (moonmod/sky_screen.hpp).

#include "moonmod/sky_screen.hpp"

#include <optional>
#include <utility>

#include "dataread.hpp"
#include "dun/gameloop.hpp"
#include "gamepad.hpp"
#include "menu_draw.hpp"
#include "moonmod/runtime.hpp"
#include "snd.hpp"

namespace dc::game {

namespace {

std::optional<moonmod::TitleMode> g_page;
bool                              g_showing = false; // the dungeon has paused for it
int                               g_armed = 0;       // frames before a press counts (the press that opened it)
int                               g_repeat = 0;      // frames until a held direction moves again
int                               g_held = 0;        // the direction held (bits 1 up, 2 right, 4 left, 8 down)

void Publish(const moonmod::TitleMode &page, std::string_view event, moonmod::Json payload) {
    payload["id"] = page.local_id;
    payload["frame"] = moonmod::SharedRuntime().Frame();
    const std::string owner = page.owner;
    moonmod::SharedRuntime().PublishToMod(owner, std::string(event), payload);
}

// The constellation `step` away from the cursor's that has a node to land on.
std::size_t StepGroup(const moonmod::TitleSky &sky, std::size_t from, int step) {
    if (sky.groups.empty() || from >= sky.nodes.size()) return from;
    const std::size_t count = sky.groups.size();
    std::size_t       group = sky.nodes[from].group;
    for (std::size_t tries = 0; tries < count; ++tries) {
        group = step > 0 ? (group + 1) % count : (group + count - 1) % count;
        const std::size_t entry = moonmod::TitleSkyGroupEntry(sky, group);
        if (entry < sky.nodes.size()) return entry;
    }
    return from;
}

void Resume() {
    if (!g_showing) return;
    driveStepHold = 0;
    CMonUnitHold = 0;
    CEffectHold = 0;
    PlayTimeCountFlag(1);
    g_showing = false;
}

// One frame of the pad: true once the page has closed.
bool PadStep() {
    if (!g_page) return true;
    if (g_armed > 0) --g_armed;
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
        g_repeat = 4;
        move = true;
    }
    using moonmod::TitleModeInput;
    if (move) {
        // one way at a time, up and down first
        const TitleModeInput way = (held & 1)   ? TitleModeInput::Up
                                   : (held & 8) ? TitleModeInput::Down
                                   : (held & 4) ? TitleModeInput::Left
                                                : TitleModeInput::Right;
        if (MoonModSkyInput(way)) return true;
    }
    if (g_armed > 0) return false;
    const std::pair<int, TitleModeInput> buttons[] = {
        {PAD_CIRCLE, TitleModeInput::Back}, {PAD_SELECT, TitleModeInput::Back}, {PAD_START, TitleModeInput::Back},
        {PAD_CROSS, TitleModeInput::Confirm}, {PAD_SQUARE, TitleModeInput::Square}, {PAD_TRIANGLE, TitleModeInput::Map},
        {PAD_L1, TitleModeInput::L1}, {PAD_R1, TitleModeInput::R1}, {PAD_L2, TitleModeInput::L2}, {PAD_R2, TitleModeInput::R2}};
    for (const auto &[pad, input] : buttons) {
        if (GamePad.Down(pad) != 0) return MoonModSkyInput(input);
    }
    return false;
}

} // namespace

bool MoonModSkyInput(moonmod::TitleModeInput input) {
    if (!g_page || !g_page->sky) return true;
    using moonmod::TitleModeInput;
    moonmod::TitleSky &sky = *g_page->sky;
    const std::size_t  was = sky.cursor;
    const auto send = [&](std::string_view action) {
        Publish(*g_page, "ui.sky_selected", {{"entry", sky.nodes.empty() ? std::string() : sky.nodes[sky.cursor].id}, {"action", action}});
    };
    switch (input) {
        case TitleModeInput::Up: sky.cursor = moonmod::TitleSkyStep(sky, sky.cursor, 0, -1); break;
        case TitleModeInput::Down: sky.cursor = moonmod::TitleSkyStep(sky, sky.cursor, 0, 1); break;
        case TitleModeInput::Left: sky.cursor = moonmod::TitleSkyStep(sky, sky.cursor, -1, 0); break;
        case TitleModeInput::Right: sky.cursor = moonmod::TitleSkyStep(sky, sky.cursor, 1, 0); break;
        case TitleModeInput::L2: sky.cursor = StepGroup(sky, sky.cursor, -1); break;
        case TitleModeInput::R2: sky.cursor = StepGroup(sky, sky.cursor, 1); break;
        case TitleModeInput::Map:
            sky.map = !sky.map;
            ComMenuSePlay(MENU_SOUND_CURSOR);
            return false;
        case TitleModeInput::Confirm: send("buy"); return false;
        case TitleModeInput::Square: send("toggle"); return false;
        case TitleModeInput::L1: send("rank_down"); return false;
        case TitleModeInput::R1: send("rank_up"); return false;
        case TitleModeInput::Back: {
            ComMenuSePlay(MENU_SOUND_REFUSE);
            const moonmod::TitleMode page = std::move(*g_page);
            g_page.reset();
            Publish(page, "ui.sky_closed", moonmod::Json::object());
            return true;
        }
        default: return false;
    }
    if (sky.cursor != was) ComMenuSePlay(MENU_SOUND_CURSOR);
    return false;
}

const moonmod::TitleMode *MoonModSkyPage() { return g_page ? &*g_page : nullptr; }

bool MoonModSkyOpen(moonmod::TitleMode mode) {
    if (!mode.sky || mode.sky->nodes.empty()) return false;
    if (g_page && g_page->owner != mode.owner) return false;
    const bool again = g_page.has_value() && g_page->local_id == mode.local_id;
    if (again) {
        // The mod answering a press: the cursor and the view stay where the player left them.
        const moonmod::TitleSky &old = *g_page->sky;
        const std::string       &at = old.nodes[old.cursor].id;
        for (std::size_t i = 0; i < mode.sky->nodes.size(); ++i) {
            if (mode.sky->nodes[i].id == at) mode.sky->cursor = i;
        }
        mode.sky->map = old.map;
    }
    g_page = std::move(mode);
    if (!again) {
        g_armed = 12;
        g_held = 0;
        g_repeat = 0;
    }
    return true;
}

void MoonModSkyCloseOwner(std::string_view owner) {
    if (g_page && g_page->owner == owner) g_page.reset();
}

bool MoonModSkyWaiting() { return g_page.has_value() && !g_showing; }

bool MoonModSkyUp() { return g_page.has_value(); }

bool MoonModSkyStep() {
    if (!g_showing) {
        if (!g_page) return true;
        driveStepHold = 1;
        CMonUnitHold = 1;
        CEffectHold = 1;
        PlayTimeCountFlag(0);
        g_showing = true;
        ComMenuSePlay(MENU_SOUND_CONFIRM);
    }
    if (!g_page || PadStep()) {
        Resume();
        return true;
    }
    return false;
}

bool MoonModSkyPaused() { return g_showing; }

void MoonModSkyDraw() {
    if (!g_page || !g_showing || !g_page->sky) return;
    MoonModDrawSkyPage(*g_page, 0x80);
}

void MoonModSkyReset() {
    g_page.reset();
    g_showing = false;
    g_armed = g_held = g_repeat = 0;
}

} // namespace dc::game

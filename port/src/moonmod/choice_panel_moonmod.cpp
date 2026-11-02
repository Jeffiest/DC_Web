// A mod's choice panel (ui.choice_open): the dungeon pauses as the game's own pause does (the player, monsters and
// effects held, the play clock stopped) and the options show side by side as cards on a dimmed screen. The pad's
// left and right (or L1/R1) move between them and Cross takes one; the mouse picks the card under it.

#include "moonmod/choice_panel.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "dun/gameloop.hpp"
#include "gamepad.hpp"
#include "memcard.hpp"
#include "mglib.hpp"
#include "snd.hpp"
#include "menu_draw.hpp"
#include "dataread.hpp"
#include "texture.hpp"
#include "ui/framework.hpp"
#include "ui/menu_pointer.hpp"

namespace dc::game {

namespace {

std::optional<MoonModChoice> g_choice;
bool                         g_showing = false; // the dungeon has paused for it
int                          g_armed = 0;       // frames before a press can take an option (a held button)
int                          g_resume = -1;     // the task the last panel resumes
bool                         g_town_locked = false; // a town's controls held while its panel is up
ui::MenuPointer              g_pointer;

struct CardBox {
    int x, y, w, h;
};
std::vector<CardBox> g_cards; // where the last frame drew each card, for the mouse

constexpr int kCardTop = 124;
constexpr int kCardHeight = 228;

void ToneRgb(int tone, unsigned char &r, unsigned char &g, unsigned char &b) {
    switch (tone) {
        case 1: r = 140, g = 232, b = 140; return; // good
        case 2: r = 240, g = 120, b = 120; return; // bad
        case 3: r = 170, g = 172, b = 186; return; // dim
        case 4: r = 255, g = 214, b = 120; return; // gold
        default: r = 240, g = 238, b = 232; return;
    }
}

int Text(std::string_view text, float scale, int tone, ui::Alignment align, int x, int y, int alpha) {
    unsigned char r = 0, g = 0, b = 0;
    ToneRgb(tone, r, g, b);
    ui::Label label(text, ui::ColorWhite, align, true, scale);
    label.SetCustomRgb(r, g, b);
    label.Draw(x, y, alpha);
    return label.GetWidth();
}

void Rect(int x, int y, int w, int h, spRGBA colour) { set2DSpriteC4(Vif1Packet, CRect_i_(x, y, w, h), &colour, &colour, &colour, &colour); }

void Layout(int count) {
    g_cards.clear();
    const int gap = 14;
    const int w = std::min(250, (600 - (count - 1) * gap) / std::max(1, count));
    const int total = count * w + (count - 1) * gap;
    for (int i = 0; i < count; ++i) g_cards.push_back({320 - total / 2 + i * (w + gap), kCardTop, w, kCardHeight});
}

void Resume() {
    if (!g_showing) return;
    driveStepHold = 0;
    CMonUnitHold = 0;
    CEffectHold = 0;
    PlayTimeCountFlag(1);
    g_showing = false;
    g_pointer.Abandon();
    ui::MenuPointerShow(nullptr);
}

void Finish(int index) {
    if (!g_choice) return;
    const std::string owner = g_choice->owner, id = g_choice->id;
    auto              done = std::move(g_choice->done);
    g_resume = g_choice->resume_task;
    g_choice.reset();
    if (done) done(index);
    else moonmod::ChronicleChoiceMade(owner, id, index);
}

} // namespace

bool MoonModChoiceOpen(MoonModChoice choice) {
    if (g_choice || choice.options.empty()) return false;
    choice.cursor = std::clamp(choice.cursor, 0, static_cast<int>(choice.options.size()) - 1);
    g_choice = std::move(choice);
    g_armed = 20;
    return true;
}

void MoonModChoiceCloseOwner(std::string_view owner) {
    if (!g_choice || g_choice->owner != owner) return;
    g_choice.reset();
    // The paused task sees the panel gone on its next step and resumes play there.
}

bool MoonModChoiceWaiting() { return g_choice.has_value() && !g_showing; }

bool MoonModChoiceUp() { return g_choice.has_value(); }

int MoonModChoiceResumeTask(int play_task) {
    const int task = g_resume >= 0 ? g_resume : play_task;
    g_resume = -1;
    return task;
}

bool ChoicePick();

bool MoonModChoiceStep() {
    if (!g_showing) {
        if (!g_choice) return true;
        // Paused as the game's own pause pauses.
        driveStepHold = 1;
        CMonUnitHold = 1;
        CEffectHold = 1;
        PlayTimeCountFlag(0);
        g_showing = true;
        ComMenuSePlay(MENU_SOUND_CONFIRM);
        if (!g_pointer.IsOpen()) g_pointer.Open();
    }
    if (!g_choice) {
        Resume();
        return true;
    }
    return ChoicePick();
}

// Reads the pad and mouse for the open panel; true once an option is taken (or the panel backed out of).
bool ChoicePick() {
    MoonModChoice &choice = *g_choice;
    const int      count = static_cast<int>(choice.options.size());
    if (g_armed > 0) --g_armed;

    int pick = -2; // -2 nothing yet, -1 backed out, else the option taken
    const int was = choice.cursor;
    if (GamePad.Down(PAD_LEFT) || GamePad.Down(PAD_L1) || GamePad.Down(PAD_UP)) choice.cursor = (choice.cursor + count - 1) % count;
    if (GamePad.Down(PAD_RIGHT) || GamePad.Down(PAD_R1) || GamePad.Down(PAD_DOWN)) choice.cursor = (choice.cursor + 1) % count;
    if (g_armed == 0 && GamePad.Down(PAD_CROSS)) pick = choice.cursor;
    if (g_armed == 0 && choice.cancel && GamePad.Down(PAD_CIRCLE)) pick = -1;

    const auto take = g_pointer.Take(static_cast<float>(g_cards.empty() ? 320 : g_cards[choice.cursor].x + 10),
                                     static_cast<float>(kCardTop + 10));
    ui::MenuPointerShow(&g_pointer);
    if (take.pointing && take.moved) {
        for (int i = 0; i < static_cast<int>(g_cards.size()) && i < count; ++i) {
            const CardBox &card = g_cards[i];
            if (g_pointer.x >= card.x && g_pointer.x < card.x + card.w && g_pointer.y >= card.y && g_pointer.y < card.y + card.h) {
                choice.cursor = i;
                if ((take.clicked & 1) != 0 && g_armed == 0) pick = i;
            }
        }
        if ((take.clicked & 2) != 0 && choice.cancel && g_armed == 0) pick = -1;
    }
    if (choice.cursor != was) ComMenuSePlay(MENU_SOUND_CURSOR);
    if (pick == -2) return false;
    ComMenuSePlay(pick >= 0 ? MENU_SOUND_CONFIRM : MENU_SOUND_REFUSE);
    Finish(pick);
    Resume();
    return true;
}

void MoonModChoiceDraw() {
    if (!g_choice || !g_showing) return;
    const MoonModChoice &choice = *g_choice;
    const int            count = static_cast<int>(choice.options.size());
    Layout(count);

    // The dimmed scene; a cover of the whole frame reaches a wide window's sides too.
    Rect(0, 0, 640, SCREEN_HEIGHT, {0, 0, 8, 0x58});

    // The heading.
    ui::TrackerWindow::Draw(110, 52, 420, choice.subtitle.empty() ? 46 : 62, 0x80, ui::WindowPreset::BlueStone);
    ui::Label heading(choice.title, ui::ColorGold, ui::Alignment::Center, true, 1.0f, ui::FontWeight::Bold);
    heading.SetOutline(2.0f);
    heading.Draw(320, 60);
    if (!choice.subtitle.empty()) Text(choice.subtitle, 0.62f, 0, ui::Alignment::Center, 320, 90, 0x80);

    // The cards: the one under the cursor lit and framed in gold.
    for (int i = 0; i < count; ++i) {
        const CardBox             &card = g_cards[i];
        const MoonModChoiceOption &option = choice.options[i];
        const bool                 on = i == choice.cursor;
        ui::TrackerWindow::Draw(card.x, card.y, card.w, card.h, on ? 0x80 : 0x58, ui::WindowPreset::BlueStone);
        if (on) {
            const spRGBA gold = {248, 206, 92, 0x80};
            Rect(card.x - 3, card.y - 3, card.w + 6, 3, gold);
            Rect(card.x - 3, card.y + card.h, card.w + 6, 3, gold);
            Rect(card.x - 3, card.y, 3, card.h, gold);
            Rect(card.x + card.w, card.y, 3, card.h, gold);
        }
        const int alpha = on ? 0x80 : 0x60;
        const int inner = card.w - 28;
        int       y = card.y + 14;
        for (const std::string &line : ui::WrapText(option.title, inner, 0.85f)) {
            ui::Label title(line, on ? ui::ColorGold : ui::ColorWhite, ui::Alignment::Left, true, 0.85f, ui::FontWeight::Bold);
            title.Draw(card.x + 14, y, alpha);
            y += 20;
        }
        if (!option.subtitle.empty()) {
            Text(option.subtitle, 0.6f, 3, ui::Alignment::Left, card.x + 14, y, alpha);
            y += 18;
        }
        y += 6;
        for (const MoonModChoiceLine &line : option.lines) {
            for (const std::string &part : ui::WrapText(line.text, inner, 0.62f)) {
                if (y > card.y + card.h - 22) break;
                Text(part, 0.62f, line.tone, ui::Alignment::Left, card.x + 14, y, alpha);
                y += 16;
            }
        }
    }

    // The buttons.
    const std::string hint = choice.cancel ? "{left}{right} Choose    {cross} Take    {circle} Back" : "{left}{right} Choose    {cross} Take";
    ui::Label(hint, ui::ColorWhite, ui::Alignment::Center, true, 1.0f).Draw(320, kCardTop + kCardHeight + 16);

    // The mouse's hand.
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

namespace {
std::string g_card_kicker, g_card_name, g_card_epithet;
int         g_card_frames = 0, g_card_total = 0;
bool        g_card_holding = false;
} // namespace

void MoonModBossCard(const std::string &kicker, const std::string &name, const std::string &epithet, int frames) {
    g_card_kicker = kicker;
    g_card_name = name;
    g_card_epithet = epithet;
    g_card_frames = g_card_total = std::clamp(frames, 30, 600);
}

void MoonModBossCardDraw() {
    if (g_card_frames <= 0) {
        if (g_card_holding) {
            g_card_holding = false;
            if (!g_showing) {
                driveStepHold = 0;
                CMonUnitHold = 0;
                CEffectHold = 0;
            }
        }
        return;
    }
    // Held still while the card is up, unless a panel or the pause already holds the scene.
    if (!g_card_holding && !g_showing) {
        driveStepHold = 1;
        CMonUnitHold = 1;
        CEffectHold = 1;
        g_card_holding = true;
    }
    const int shown = g_card_total - g_card_frames;
    --g_card_frames;
    // In over 20 frames, out over the last 20.
    const int fade = std::min({shown * 128 / 20, g_card_frames * 128 / 20, 128});
    const int bar = std::min(80, shown * 6);
    Rect(-400, 0, 1440, bar, {0, 0, 0, 0x80});
    Rect(-400, SCREEN_HEIGHT - bar, 1440, bar, {0, 0, 0, 0x80});
    Rect(-400, 180, 1440, 120, {0, 0, 0, static_cast<unsigned char>(fade * 0x40 / 128)});
    const spRGBA line = {248, 206, 92, static_cast<unsigned char>(fade * 0x60 / 128)};
    Rect(60, 186, 520, 1, line);
    Rect(60, 293, 520, 1, line);
    Text(g_card_kicker, 0.75f, 2, ui::Alignment::Center, 320, 196, fade);
    ui::Label name(g_card_name, ui::ColorGold, ui::Alignment::Center, true, 1.6f, ui::FontWeight::Bold);
    name.SetOutline(3.0f, 60, 20, 10);
    name.Draw(320, 216, fade);
    if (!g_card_epithet.empty()) Text(g_card_epithet, 0.7f, 0, ui::Alignment::Center, 320, 268, fade);
}

void MoonModChoiceStepTown(bool walking) {
    if (g_town_locked) {
        GamePad.KeyLock(0);
        g_town_locked = false;
    }
    if (!g_choice) {
        if (g_showing) {
            g_showing = false;
            g_pointer.Abandon();
            ui::MenuPointerShow(nullptr);
        }
        return;
    }
    if (!walking) return;
    if (!g_showing) {
        g_showing = true;
        ComMenuSePlay(MENU_SOUND_CONFIRM);
        if (!g_pointer.IsOpen()) g_pointer.Open();
    }
    if (ChoicePick()) {
        g_resume = -1;
        g_showing = false;
    }
    // The town's own controls wait while the panel is up, and on the frame it closes (Cross takes an option, not a talk).
    GamePad.KeyLock(1);
    g_town_locked = true;
}

void MoonModChoiceReset() {
    Resume();
    g_choice.reset();
    g_pointer.Close();
    if (g_town_locked) {
        GamePad.KeyLock(0);
        g_town_locked = false;
    }
}

} // namespace dc::game

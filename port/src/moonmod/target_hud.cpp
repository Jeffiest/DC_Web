// The locked target's bar (hud.target_bar_set, hud.target_meter_set). In place of the game's 64x4 gauge under the
// target's name, a bar half again as wide: a dark frame with a lit top edge, the life in red, and behind it a pale
// trail that holds what the last blows took for half a second, then drains to the life, so a hit reads at a glance.
// Under it, when a mod fills one for this monster, a thin meter (Vigil's poise: how near a stagger it is), which
// flashes white while full.

#include "moonmod/target_hud.hpp"

#include <algorithm>
#include <array>
#include <chrono>

#include "dun/gameloop.hpp"
#include "monstorunit.hpp"
#include "mglib.hpp"
#include "rect.hpp"
#include "snd.hpp"

namespace dc::game {

namespace {

constexpr int   kWidth = 96;       // the bar, in screen pixels (the game's is 64)
constexpr int   kLifeHeight = 5;   // the life row
constexpr int   kMeterHeight = 3;  // the meter row, under it with a pixel between
constexpr float kTrailHold = 0.5f; // seconds the trail holds before it drains
constexpr float kTrailDrain = 1.6f; // the share of the bar the trail drains in a second

struct Meter {
    float fill = 0.0f;
    u32   rgb = 0xF0C040;
    float seconds = 0.0f;
    float set_at = 0.0f;
    bool  on = false;
};

struct Trail {
    int   slot = -1;
    int   life_max = 0;
    float life = 1.0f;  // the share of life the trail shows
    float last = 1.0f;  // the share of life last frame
    float hit_at = 0.0f;
    float drawn_at = 0.0f;
};

bool                                 g_on = false;
std::array<Meter, MONSTOR_SLOT_MAX> g_meters{};
Trail                                g_trail;

float Now() {
    using namespace std::chrono;
    static const steady_clock::time_point start = steady_clock::now();
    return duration<float>(steady_clock::now() - start).count();
}

spRGBA Rgb(u32 rgb, int alpha = 0x80) {
    return {.r = static_cast<u8>(rgb >> 16), .g = static_cast<u8>(rgb >> 8), .b = static_cast<u8>(rgb), .a = static_cast<u8>(alpha)};
}

void Fill(int x, int y, int width, int height, spRGBA top, spRGBA bottom) {
    if (width > 0 && height > 0) {
        set2DSpriteC4(Vif1Packet, CRect_i_(x, y, width, height), &top, &top, &bottom, &bottom);
    }
}

// Shades a colour by percent (100 is itself).
u32 Shade(u32 rgb, int percent) {
    const auto channel = [&](int shift) { return static_cast<u32>(std::min(255, static_cast<int>((rgb >> shift & 0xFF) * percent / 100))) << shift; };
    return channel(16) | channel(8) | channel(0);
}

} // namespace

void MoonModTargetBarSet(bool on) {
    g_on = on;
    g_trail = Trail{};
}

bool MoonModTargetBarOn() {
    return g_on;
}

void MoonModTargetMeterSet(int slot, float fill, u32 rgb, float seconds) {
    if (slot < 0 || slot >= MONSTOR_SLOT_MAX) {
        return;
    }

    g_meters[slot] = {std::clamp(fill, 0.0f, 1.0f), rgb, std::max(seconds, 0.0f), Now(), true};
}

void MoonModTargetMeterClear(int slot) {
    if (slot >= 0 && slot < MONSTOR_SLOT_MAX) {
        g_meters[slot] = Meter{};
    }
}

void MoonModTargetMetersClear() {
    g_meters.fill(Meter{});
    g_trail = Trail{};
}

bool MoonModTargetBarDraw(int slot) {
    if (!g_on || slot < 0 || slot >= MONSTOR_SLOT_MAX || EnemyLifeGage.life_max <= 0) {
        return false;
    }

    const int x = EnemyLifeGage.x - kWidth / 2;
    const int y = (EnemyLifeGage.y >> 1) << 1;

    if (x < 0 || x + kWidth > 640 || y < 0 || y > 470) {
        return true;
    }

    const float now = Now();
    const float life = std::clamp(static_cast<float>(EnemyLifeGage.life) / static_cast<float>(EnemyLifeGage.life_max), 0.0f, 1.0f);

    // A new target, or a target out of sight a while, starts its trail at its life.
    if (g_trail.slot != slot || g_trail.life_max != EnemyLifeGage.life_max || now - g_trail.drawn_at > 1.0f) {
        g_trail = {slot, EnemyLifeGage.life_max, life, life, now, now};
    }

    if (life < g_trail.last) {
        g_trail.hit_at = now;
    }

    if (life > g_trail.life) {
        g_trail.life = life;
    } else if (now - g_trail.hit_at > kTrailHold) {
        g_trail.life = std::max(life, g_trail.life - kTrailDrain * (now - g_trail.drawn_at));
    }

    g_trail.last = life;
    g_trail.drawn_at = now;

    Meter     &meter = g_meters[slot];
    const bool shown = meter.on && (meter.seconds <= 0.0f || now - meter.set_at <= meter.seconds);
    const int  height = kLifeHeight + (shown ? kMeterHeight + 1 : 0);

    // The frame: dark, a pixel round, its top edge lit.
    Fill(x - 2, y - 2, kWidth + 4, height + 4, Rgb(0x0A0A12), Rgb(0x0A0A12));
    Fill(x - 1, y - 1, kWidth + 2, 1, Rgb(0x8C96B4), Rgb(0x8C96B4));
    Fill(x, y, kWidth, kLifeHeight, Rgb(0x1E1A26), Rgb(0x12101A));

    // The trail, then the life over it.
    Fill(x, y, static_cast<int>(kWidth * g_trail.life + 0.5f), kLifeHeight, Rgb(0xF4E2B0), Rgb(0xC8A878));
    Fill(x, y, static_cast<int>(kWidth * life + 0.5f), kLifeHeight, Rgb(0xF0503C), Rgb(0xA81C1C));

    if (shown) {
        const int  top = y + kLifeHeight + 1;
        const bool full = meter.fill >= 1.0f;
        const u32  rgb = full && static_cast<int>(now * 8.0f) % 2 == 0 ? 0xFFFFFF : meter.rgb;
        Fill(x, top, kWidth, kMeterHeight, Rgb(0x16141E), Rgb(0x16141E));
        Fill(x, top, static_cast<int>(kWidth * meter.fill + 0.5f), kMeterHeight, Rgb(Shade(rgb, 115)), Rgb(Shade(rgb, 80)));
    }

    return true;
}

} // namespace dc::game

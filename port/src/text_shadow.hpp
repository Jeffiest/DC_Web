#pragma once

#include <span>

// The drop shadow the message text casts, as taps at half-pixel steps along the light's direction (down
// and right) that the texture filter blends. Message text (clsmes.cpp) and the button symbols drawn in
// it (glyph_draw.cpp) share it, so a symbol sits on the picture the way the letters beside it do.
// Offsets are in logical units, in the shadow's own colour `grey`; `alpha` is capped by the caller.
struct TextShadowTap {
    float dx, dy;
    int   grey, alpha;
};

// Calls `draw(tap)` for each tap of the shadow at `percent` strength: 0 casts none, 50 is the soft
// shadow, 100 the deep one, and in between the soft one fades out or gives way to the deep one.
template <class Draw>
void TextShadowEach(int percent, Draw &&draw) {
    static constexpr TextShadowTap soft[] = {
        {0.6f, 1.4f, 0, 0x0C},
        {0.6f, 2.2f, 0, 0x0E},
        {0.6f, 3.0f, 0, 0x0C},
        {1.4f, 0.6f, 0, 0x0C},
        {1.4f, 1.4f, 0, 0x16},
        {1.4f, 2.2f, 0, 0x1C},
        {1.4f, 3.0f, 0, 0x16},
        {1.4f, 3.8f, 0, 0x0C},
        {2.2f, 0.6f, 0, 0x0E},
        {2.2f, 1.4f, 0, 0x1C},
        {2.2f, 2.2f, 0, 0x20},
        {2.2f, 3.0f, 0, 0x1C},
        {2.2f, 3.8f, 0, 0x0E},
        {3.0f, 0.6f, 0, 0x0C},
        {3.0f, 1.4f, 0, 0x16},
        {3.0f, 2.2f, 0, 0x1C},
        {3.0f, 3.0f, 0, 0x16},
        {3.0f, 3.8f, 0, 0x0C},
        {3.8f, 1.4f, 0, 0x0C},
        {3.8f, 2.2f, 0, 0x0E},
        {3.8f, 3.0f, 0, 0x0C},
    };
    static constexpr TextShadowTap deep[] = {
        {0.8f, 0.8f, 0x40, 0x78},
        {1.4f, 1.4f, 0x10, 0x78},
        {2.0f, 2.0f, 0,    0x78},
        {2.6f, 2.6f, 0,    0x78},
        {3.2f, 3.2f, 0,    0x60},
        {3.8f, 3.8f, 0,    0x40},
        {1.6f, 0.8f, 0,    0x50},
        {0.8f, 1.6f, 0,    0x50},
        {2.4f, 1.6f, 0,    0x48},
        {1.6f, 2.4f, 0,    0x48},
        {3.2f, 2.4f, 0,    0x30},
        {2.4f, 3.2f, 0,    0x30},
    };
    percent = percent < 0 ? 0 : (percent > 100 ? 100 : percent);
    const float to_deep = percent > 50 ? static_cast<float>(percent - 50) / 50.0f : 0.0f;
    const float soft_scale = percent >= 50 ? 1.0f - to_deep : static_cast<float>(percent) / 50.0f;
    auto        cast = [&](std::span<const TextShadowTap> taps, float scale) {
        for (TextShadowTap tap : taps) {
            tap.alpha = static_cast<int>(static_cast<float>(tap.alpha) * scale + 0.5f);
            if (tap.alpha > 0) {
                draw(tap);
            }
        }
    };
    cast(soft, soft_scale);
    cast(deep, to_deep);
}

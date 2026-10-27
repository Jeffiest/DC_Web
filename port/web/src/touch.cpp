// The on-screen gamepad on a touch screen (docs/WEB.md), as Android's port has it (platform/touch_overlay.cpp)
// with the page in Java's place: the page (port/web/site/shell.js) draws the buttons and sticks and hands their
// state to dc_web_touch_state; the game says when to show them, as it hides them while a gamepad is connected
// or a key held, and how opaque.

#include <emscripten/em_asm.h>
#include <emscripten/emscripten.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <mutex>

#include "platform/input.hpp"
#include "platform/touch_overlay.hpp"

namespace {

std::mutex             g_state_mutex;
InputTouchOverlayState g_state{};
std::atomic<bool>      g_touch_screen{false};
bool                   g_told = false;
bool                   g_visible = false;

// The page's button bits, in Android's order.
constexpr std::array<std::uint16_t, 14> kButtons = {
    kInputUp,       kInputDown, kInputLeft, kInputRight, kInputCross, kInputCircle, kInputSquare,
    kInputTriangle, kInputL1,   kInputR1,   kInputL2,    kInputR2,    kInputSelect, kInputStart};

} // namespace

// Read by Kazarov's input (platform/input.cpp): a browser always reports a keyboard and a mouse, so the page says
// whether the screen is a touch screen instead.
bool WebTouchScreen() {
    return g_touch_screen.load();
}

extern "C" EMSCRIPTEN_KEEPALIVE void dc_web_touch_screen(int touch) {
    g_touch_screen.store(touch != 0);
}

extern "C" EMSCRIPTEN_KEEPALIVE void dc_web_touch_state(int mask, float lx, float ly, float rx, float ry) {
    InputTouchOverlayState next{};
    for (std::size_t i = 0; i < kButtons.size(); ++i) {
        if (mask & (1 << i)) {
            next.buttons |= kButtons[i];
        }
    }
    next.left_x = lx;
    next.left_y = ly;
    next.right_x = rx;
    next.right_y = ry;
    std::lock_guard lock(g_state_mutex);
    g_state = next;
}

namespace dc::platform {

void TouchOverlayPoll() {
    const bool visible = InputTouchOverlayVisible();
    if (!g_told || visible != g_visible) {
        g_told = true;
        g_visible = visible;
        MAIN_THREAD_ASYNC_EM_ASM({
            if (Module.dcTouchVisible) {
                Module.dcTouchVisible($0 != 0);
            }
        }, visible);
    }
    InputTouchOverlayState state{};
    if (visible) {
        std::lock_guard lock(g_state_mutex);
        state = g_state;
    }
    InputSetTouchOverlay(state);
}

void TouchOverlaySetOpacity(float opacity) {
    MAIN_THREAD_ASYNC_EM_ASM({
        if (Module.dcTouchOpacity) {
            Module.dcTouchOpacity($0);
        }
    }, opacity);
}

} // namespace dc::platform

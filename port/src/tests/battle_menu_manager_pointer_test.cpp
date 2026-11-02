#include <gtest/gtest.h>

#include "battlemenu.hpp"
#include "battlemenu/main_ring.hpp"
#include "moonmod/manager_ui.hpp"
#include "platform/input.hpp"
#include "ui/menu_mouse.hpp"
#include "ui/menu_pointer.hpp"

namespace dc::test {

using namespace dc::game;
using namespace dc::ui;

namespace {
// Clicks Mouse 1 with the pause menu's ring up and the pointer over its first row; returns the pad press it made.
int ClickOverRing() {
    float x = 0.0f;
    float y = 0.0f;
    battlemenu::BattleMenuIconPosition(0, x, y);
    SysCur[0] = x;
    SysCur[1] = y;
    BattleMenuFlag = BTLMENU_STATE_MAIN;
    InputSetScriptedDevices({});
    MenuMouseNoteBattleMenu();
    MenuMouseUpdate();
    InputKeyboardMouse mouse;
    mouse.mouse_buttons = 1;
    InputSetScriptedDevices(mouse);
    MenuMouseNoteBattleMenu();
    MenuMouseUpdate();
    const int press = MenuPointerSyntheticDown();
    InputSetScriptedDevices({});
    MenuMouseUpdate();
    return press;
}
} // namespace

TEST(BattleMenuManagerPointer, RingClickIsCross) {
    moonmod::CloseManager();
    EXPECT_EQ(ClickOverRing(), kInputCross);
}

// The Mods page lies over the ring: a click there is the manager's, never a Cross on the row the pad left selected
// (which would switch that package on or off).
TEST(BattleMenuManagerPointer, ModsPageClickIsNotACross) {
    moonmod::OpenManager();
    ASSERT_TRUE(moonmod::ManagerOpen());
    EXPECT_EQ(ClickOverRing(), 0);
    moonmod::CloseManager();
}

} // namespace dc::test

// Kazarov's renderer tests in the browser (web workflow, docs/WEB.md): its gfx_*_test.cpp cases against Vulkan on
// WebGPU, offscreen, as headless Chromium on SwiftShader loses the device once a canvas presents. The page
// (gfx_tests.html) passes GoogleTest's arguments, e.g. ?args=--gtest_filter=GfxMisc.*

#include <gtest/gtest.h>

#include <cstdlib>

#include "platform/settings.hpp"

int main(int argc, char **argv) {
    SettingsSetEnvPrefix("DC");
    setenv("DC_GFX_OFFSCREEN", "1", 0);
    testing::InitGoogleTest(&argc, argv);
    return RUN_ALL_TESTS();
}

#include "moonmod/native_plugin.h"

static MoonModNativeStatusV1 FixtureLoad(const MoonModNativeHostV1 *host,
                                         void **plugin_state,
                                         MoonModNativeBufferV1 *error_message) {
    (void)error_message;
    (void)host;
    *plugin_state = 0;
    return MOONMOD_NATIVE_OK_V1;
}

static void FixtureUnload(const MoonModNativeHostV1 *host, void *plugin_state) {
    (void)host;
    (void)plugin_state;
}

static const char *const fixture_permissions[] = {"game.read", "content.register", "native.extension"};

static const MoonModNativePluginV1 fixture_plugin = {
    (uint32_t)sizeof(MoonModNativePluginV1),
    MOONMOD_NATIVE_ABI_MAJOR_V1,
    MOONMOD_NATIVE_ABI_MINOR_V1,
    MOONMOD_NATIVE_ABI_MAJOR_V1,
    MOONMOD_NATIVE_ABI_MINOR_V1,
    "example.native-probe",
    3,
    fixture_permissions,
    FixtureLoad,
    FixtureUnload,
};

const MoonModNativePluginV1 *MoonModTestNativePluginV1(void) {
    return &fixture_plugin;
}

#include "moonmod/native_plugin.h"

#include <string.h>

typedef struct FixtureState {
    const MoonModNativeHostV1 *host;
    uint64_t tick_subscription;
} FixtureState;

static FixtureState fixture_state;
static const char *const fixture_permissions[] = {"game.read", "game.write", "content.register", "native.extension"};

static void OnTick(const char *event_name, const char *json_payload, uint64_t frame, void *user_data) {
    FixtureState *state = (FixtureState *)user_data;
    if (state == 0 || state->host == 0 || event_name == 0 || json_payload == 0 ||
        strcmp(event_name, "game.tick") != 0 || frame == 0) return;
    state->host->log(state->host->context, "info", "native fixture tick callback");
}

static MoonModNativeStatusV1 FixtureLoad(const MoonModNativeHostV1 *host, void **plugin_state,
                                         MoonModNativeBufferV1 *error_message) {
    int32_t supported = 0;
    char reason[256] = {0};
    char result[4096] = {0};
    char error[256] = {0};
    MoonModNativeBufferV1 reason_buffer = {reason, (uint32_t)sizeof(reason), 0};
    MoonModNativeBufferV1 result_buffer = {result, (uint32_t)sizeof(result), 0};
    MoonModNativeBufferV1 error_buffer = {error, (uint32_t)sizeof(error), 0};
    MoonModNativeStatusV1 status;
    (void)error_message;
    if (host == 0 || plugin_state == 0 || host->struct_size < sizeof(MoonModNativeHostV1) ||
        host->game_id == 0 || (strcmp(host->game_id, "chronicle") != 0 &&
                               strcmp(host->game_id, "chronicle-two") != 0)) return MOONMOD_NATIVE_FAILED_V1;
    status = host->api_supports(host->context, "economy.get", &supported, &reason_buffer);
    if (status != MOONMOD_NATIVE_OK_V1 || supported != 1 || reason_buffer.length != 0) {
        return MOONMOD_NATIVE_FAILED_V1;
    }
    status = host->api_call_json(host->context, "economy.get", "[]", &result_buffer, &error_buffer);
    if (status != MOONMOD_NATIVE_OK_V1 || strstr(result, "\"gilda\":42") == 0) {
        return MOONMOD_NATIVE_FAILED_V1;
    }
    status = host->api_supports(host->context, "scripts.list_functions", &supported, &reason_buffer);
    if (status != MOONMOD_NATIVE_OK_V1 || supported != 1) return MOONMOD_NATIVE_FAILED_V1;
    status = host->api_supports(host->context, "api.call", &supported, &reason_buffer);
    if (status != MOONMOD_NATIVE_OK_V1 || supported != 0) return MOONMOD_NATIVE_FAILED_V1;
    memset(result, 0, sizeof(result));
    result_buffer.length = 0;
    error_buffer.length = 0;
    status = host->api_call_json(host->context, "scripts.list_functions", "[]", &result_buffer, &error_buffer);
    if (status != MOONMOD_NATIVE_OK_V1 || strstr(result, "test.native-probe:native-visible") == 0) {
        return MOONMOD_NATIVE_FAILED_V1;
    }
    memset(result, 0, sizeof(result));
    result_buffer.length = 0;
    error_buffer.length = 0;
    status = host->api_call_json(host->context, "scripts.call_function",
                                 "[{\"id\":\"native-visible\",\"arguments\":{\"value\":21}}]",
                                 &result_buffer, &error_buffer);
    if (status != MOONMOD_NATIVE_OK_V1 || strstr(result, "42") == 0) return MOONMOD_NATIVE_FAILED_V1;
    if (host->log(host->context, "info", "native fixture core Lua API calls passed") != MOONMOD_NATIVE_OK_V1) {
        return MOONMOD_NATIVE_FAILED_V1;
    }
    fixture_state.host = host;
    fixture_state.tick_subscription = 0;
    status = host->subscribe(host->context, "game.tick", OnTick, &fixture_state,
                             &fixture_state.tick_subscription);
    if (status != MOONMOD_NATIVE_OK_V1 || fixture_state.tick_subscription == 0) {
        fixture_state.host = 0;
        return MOONMOD_NATIVE_FAILED_V1;
    }
    if (host->log(host->context, "info", "native fixture loaded") != MOONMOD_NATIVE_OK_V1) {
        host->unsubscribe(host->context, fixture_state.tick_subscription);
        fixture_state.host = 0;
        return MOONMOD_NATIVE_FAILED_V1;
    }
    *plugin_state = &fixture_state;
    return MOONMOD_NATIVE_OK_V1;
}

static void FixtureUnload(const MoonModNativeHostV1 *host, void *plugin_state) {
    FixtureState *state = (FixtureState *)plugin_state;
    if (host == 0 || state == 0) return;
    if (state->tick_subscription != 0) host->unsubscribe(host->context, state->tick_subscription);
    state->tick_subscription = 0;
    state->host = 0;
    host->log(host->context, "info", "native fixture unloaded");
}

static const MoonModNativePluginV1 fixture_plugin = {
    (uint32_t)sizeof(MoonModNativePluginV1),
    MOONMOD_NATIVE_ABI_MAJOR_V1,
    MOONMOD_NATIVE_ABI_MINOR_V1,
    MOONMOD_NATIVE_ABI_MAJOR_V1,
    MOONMOD_NATIVE_ABI_MINOR_V1,
    "test.native-probe",
    4,
    fixture_permissions,
    FixtureLoad,
    FixtureUnload,
};

MOONMOD_NATIVE_PLUGIN_EXPORT const MoonModNativePluginV1 *MoonMod_GetNativePluginV1(void) {
    return &fixture_plugin;
}

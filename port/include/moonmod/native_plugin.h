#ifndef MOONMOD_NATIVE_PLUGIN_H
#define MOONMOD_NATIVE_PLUGIN_H

/* Stable C-only ABI for explicitly trusted MoonMod native extensions. */
#include <stdint.h>

#if defined(_WIN32)
#define MOONMOD_NATIVE_EXPORT __declspec(dllexport)
#else
#define MOONMOD_NATIVE_EXPORT __attribute__((visibility("default")))
#endif

#ifdef __cplusplus
#define MOONMOD_NATIVE_PLUGIN_EXPORT extern "C" MOONMOD_NATIVE_EXPORT
#else
#define MOONMOD_NATIVE_PLUGIN_EXPORT MOONMOD_NATIVE_EXPORT
#endif

#ifdef __cplusplus
extern "C" {
#endif

enum {
    MOONMOD_NATIVE_ABI_MAJOR_V1 = 1,
    MOONMOD_NATIVE_ABI_MINOR_V1 = 0
};

typedef int32_t MoonModNativeStatusV1;
enum {
    MOONMOD_NATIVE_OK_V1 = 0,
    MOONMOD_NATIVE_INVALID_ARGUMENT_V1 = 1,
    MOONMOD_NATIVE_UNSUPPORTED_V1 = 2,
    MOONMOD_NATIVE_PERMISSION_DENIED_V1 = 3,
    MOONMOD_NATIVE_BUFFER_TOO_SMALL_V1 = 4,
    MOONMOD_NATIVE_FAILED_V1 = 5
};

/* The caller owns data. Host calls set length to the required byte count, excluding NUL. */
typedef struct MoonModNativeBufferV1 {
    char *data;
    uint32_t capacity;
    uint32_t length;
} MoonModNativeBufferV1;

typedef void (*MoonModNativeEventCallbackV1)(const char *event_name,
                                              const char *json_payload,
                                              uint64_t frame,
                                              void *user_data);

typedef struct MoonModNativeHostV1 {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    void *context;
    const char *game_id;
    const char *build_id;
    const char *platform;

    MoonModNativeStatusV1 (*log)(void *context, const char *level, const char *message);
    MoonModNativeStatusV1 (*api_supports)(void *context, const char *path,
                                          int32_t *supported, MoonModNativeBufferV1 *reason);
    MoonModNativeStatusV1 (*api_call_json)(void *context, const char *path,
                                           const char *arguments_json,
                                           MoonModNativeBufferV1 *result_json,
                                           MoonModNativeBufferV1 *error_json);
    MoonModNativeStatusV1 (*subscribe)(void *context, const char *event_name,
                                       MoonModNativeEventCallbackV1 callback,
                                       void *user_data, uint64_t *subscription_id);
    MoonModNativeStatusV1 (*unsubscribe)(void *context, uint64_t subscription_id);
} MoonModNativeHostV1;

typedef MoonModNativeStatusV1 (*MoonModNativeLoadV1)(const MoonModNativeHostV1 *host,
                                                      void **plugin_state,
                                                      MoonModNativeBufferV1 *error_message);
typedef void (*MoonModNativeUnloadV1)(const MoonModNativeHostV1 *host, void *plugin_state);

typedef struct MoonModNativePluginV1 {
    uint32_t struct_size;
    uint32_t abi_major;
    uint32_t abi_minor;
    uint32_t minimum_host_abi_major;
    uint32_t minimum_host_abi_minor;
    const char *plugin_id;
    uint32_t required_permission_count;
    const char *const *required_permissions;
    MoonModNativeLoadV1 load;
    MoonModNativeUnloadV1 unload;
} MoonModNativePluginV1;

typedef const MoonModNativePluginV1 *(*MoonModGetNativePluginV1)(void);

/* Export this exact symbol from a native extension library. */
#define MOONMOD_NATIVE_PLUGIN_ENTRY_NAME_V1 "MoonMod_GetNativePluginV1"

#ifdef __cplusplus
} /* extern "C" */
#endif

#endif /* MOONMOD_NATIVE_PLUGIN_H */

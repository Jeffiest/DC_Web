#pragma once

#include <filesystem>
#include <cstdint>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "moonmod/manifest.hpp"
#include "moonmod/api.hpp"

namespace moonmod {

enum class ModState {
    Ready,
    Disabled,
    Loaded,
    Invalid,
    Incompatible,
    MissingDependency,
    DependencyCycle,
    LoadError,
};

std::string ModStateText(ModState state);

struct ModRecord {
    ModManifest            manifest;
    std::filesystem::path  package_root;
    std::string            id;
    std::string            display_name;
    std::string            status_message;
    ModState               state = ModState::Invalid;
    bool                   enabled = false; // Active for this process.
    bool                   enabled_next_launch = false;
    bool                   restart_required = false;
    bool                   native_trusted = false;
    bool                   valid_manifest = false;
    std::string            native_status;
    std::string            pack;           // a merged pack's part: the pack's id (it runs inside the pack's Lua)
    bool                   part_switch = true; // a part's own switch, kept apart from its pack's (profile "parts")
    std::string            restart_reason; // why the last on/off waits for a restart (empty: it took effect at once)
};

using PackageApiCallHandler = std::function<ApiCallResult(std::string_view mod_id,
                                                           const std::filesystem::path &package_root,
                                                           const ApiOperationSpec &operation,
                                                           const Json &arguments)>;
using ScriptFunctionCallHandler = std::function<ApiCallResult(std::string_view caller_mod_id,
                                                               std::string_view owner_mod_id,
                                                               std::string_view function_id,
                                                               const Json &arguments)>;
using ScriptFunctionListHandler = std::function<ApiCallResult(std::string_view caller_mod_id)>;
using NativeScriptOriginalCall = std::function<ApiCallResult()>;
using ScriptHookOriginalCallHandler = std::function<ApiCallResult(std::string_view caller_mod_id,
                                                                  std::string_view hook_id)>;

struct RuntimeConfig {
    std::string game_id;
    std::string game_name;
    int         framework_api = 1;
    std::string build_id = "development";
    std::vector<std::filesystem::path> mod_directories;
    std::filesystem::path save_root;
    std::size_t lua_memory_limit = 16 * 1024 * 1024;
    int         lua_instructions_per_callback = 150000;
    std::vector<ApiCapability> api_capabilities;
    bool require_complete_api_capabilities = false;
    ApiCallHandler             api_handler;
    std::function<ApiCallResult()> world_location_reader;
    std::function<std::string()> current_language;
    PackageApiCallHandler package_api_handler;
    ScriptFunctionCallHandler script_function_handler;
    ScriptFunctionListHandler script_function_list_handler;
    ScriptHookOriginalCallHandler script_hook_original_call_handler;
    // mods.* (the runtime sets it): path, the target mod's id (empty: every mod, for an overlay clear), its values.
    std::function<ApiCallResult(std::string_view path, std::string_view mod_id, const Json &values)> mods_handler;
    std::function<std::string(std::string_view mod_id, const Json &arguments)> api_resource_override_removal_guard;
    std::function<void(std::string_view mod_id)> api_owner_unloaded;
    std::function<void(std::string_view level, std::string_view mod_id, std::string_view message)> log;
};

class Runtime {
public:
    Runtime();
    ~Runtime();
    Runtime(const Runtime &) = delete;
    Runtime &operator=(const Runtime &) = delete;

    bool Start(RuntimeConfig config, std::string &error);
    void Stop();
    void Pump();
    void Publish(std::string_view event_name, const Json &payload);
    void ObserveWorldLocationNow();
    void QueueEvent(std::string_view event_name, const Json &payload);
    bool PublishToMod(std::string_view mod_id, std::string_view event_name, const Json &payload);
    ApiCallResult DispatchNativeScriptHook(std::string_view hook_id, const Json &payload,
                                           NativeScriptOriginalCall original);
    ApiCallResult CallNativeScriptOriginal(std::string_view mod_id, std::string_view hook_id);

    std::span<const ModRecord> Mods() const;
    // Enabled Lua packages in their resolved dependency/load_after/load_before order.
    std::vector<std::string> LoadedModOrder() const;
    bool                      Running() const;
    std::uint64_t             Frame() const;
    // Saves the choice, and applies it at once when the package's hooks can be undone (see ModRecord::restart_reason).
    bool                      SetEnabled(std::string_view id, bool enabled, std::string &error);
    bool                      SetNativeTrusted(std::string_view id, bool trusted, std::string &error);
    Json                      Setting(std::string_view mod_id, std::string_view setting_id) const;
    // mods.*: the loaded mods (id, name, version, author, coop role), a mod's effective settings, and session-only
    // setting values (co-op's, the host's) that are never saved and end with the session.
    Json                      ModList() const;
    ApiCallResult             ModSettings(std::string_view mod_id) const;
    ApiCallResult             OverlayModSettings(std::string_view mod_id, const Json &values);
    void                      ClearModSettingsOverlay(std::string_view mod_id);
    ApiCallResult              Quests(std::string_view mod_id) const;
    ApiCallResult              Skills(std::string_view mod_id) const;
    Json                        CombatDamageBonuses() const;
    bool                      StepSetting(std::string_view mod_id, std::string_view setting_id, int direction,
                                           std::string &error);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

Runtime &SharedRuntime();

// Changes whenever a mod installs or removes a native.monster.run override, so the game can hand the override monsters
// already in their idle loop (which never ends on its own).
std::uint64_t MonsterScriptOverrideGeneration();
void          BumpMonsterScriptOverrideGeneration();

} // namespace moonmod

#include <chrono>
#include "moonmod/runtime.hpp"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <deque>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#include "lua_runtime.hpp"
#include "moonmod/manager_ui.hpp"
#include "moonmod/native_plugin_hash.hpp"
#include "moonmod/native_plugin_loader.hpp"

namespace dc::game {}
using namespace dc::game;

namespace moonmod {
namespace {

constexpr std::size_t kMaxQueuedEvents = 256;
constexpr std::size_t kMaxEventsPerPump = 64;

std::string StateName(ModState state) {
    switch (state) {
        case ModState::Ready: return "Ready";
        case ModState::Disabled: return "Disabled";
        case ModState::Loaded: return "Loaded";
        case ModState::Invalid: return "Invalid package";
        case ModState::Incompatible: return "Incompatible";
        case ModState::MissingDependency: return "Dependency unavailable";
        case ModState::DependencyCycle: return "Dependency cycle";
        case ModState::LoadError: return "Load error";
    }
    return "Unknown";
}

bool Contains(std::span<const std::string> values, std::string_view wanted) {
    return std::ranges::find(values, wanted) != values.end();
}

bool NativeJsonApiCallable(std::string_view path) {
    return path != "api.call" && path != "events.subscribe" && path != "events.unsubscribe" &&
           path != "commands.register" && path != "scripts.register_function" && path != "scripts.override";
}

Json DefaultProfile() {
    return {{"format_version", 1}, {"enabled", Json::object()}, {"settings", Json::object()},
            {"native_trusted", Json::object()}};
}

bool ContainsSymlink(const std::filesystem::path &path) {
    std::filesystem::path current = path.root_path();
    for (const auto &part : path.relative_path()) {
        if (part.empty() || part == ".") continue;
        current /= part;
        std::error_code ec;
        const auto status = std::filesystem::symlink_status(current, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) return true;
        if (!ec && std::filesystem::is_symlink(status)) return true;
    }
    return false;
}


} // namespace

struct Runtime::Impl {
    struct NativeScriptHookFrame {
        std::string hook_id;
        std::string mod_id;
        NativeScriptOriginalCall original;
        bool original_called = false;
        ApiCallResult original_result;
    };

    RuntimeConfig config;
    Json profile = DefaultProfile();
    Json session_enabled = Json::object();
    // MOONMOD_NO_MODS=1 (a launcher's Mods switch off): every mod is off for this run; the profile keeps the choices.
    bool mods_off_this_run = false;
    std::filesystem::path profile_file;
    std::vector<ModRecord> records;
    std::vector<std::pair<std::string, std::unique_ptr<LuaMod>>> modules;
    std::vector<std::pair<std::string, std::unique_ptr<NativePluginInstance>>> native_plugins;
    std::vector<NativeScriptHookFrame *> native_script_hook_frames;
    std::deque<std::pair<std::string, Json>> queued_events;
    Json observed_world_location = nullptr;
    std::uint64_t frame = 0;
    int script_function_call_depth = 0;
    bool running = false;

    void Queue(std::string_view event, const Json &payload) {
        if (event.empty() || queued_events.size() >= kMaxQueuedEvents) {
            if (queued_events.size() >= kMaxQueuedEvents) {
                Log("warning", "", "MoonFactory deferred event queue is full; dropping the newest event");
            }
            return;
        }
        queued_events.emplace_back(event, payload);
    }

    void QueueItemUsed(std::string_view mod_id, const Json &result) {
        if (!result.is_object()) return;
        const auto id = result.find("id");
        if (id == result.end() || !id->is_string() || id->get_ref<const std::string &>().empty()) return;
        Json payload = {{"id", *id}, {"source_mod", mod_id}, {"frame", frame}};
        for (const char *field : {"item_no", "target", "character", "effect", "cured_status", "consumed",
                                  "remaining", "healed", "healing", "water_restored", "effects_applied"}) {
            const auto value = result.find(field);
            if (value != result.end() && (value->is_string() || value->is_number() || value->is_boolean())) {
                payload[field] = *value;
            }
        }
        Queue("game.item_used", payload);
    }

    void QueueItemGiven(std::string_view mod_id, std::string_view kind, const Json &result) {
        if (!result.is_object()) return;
        const auto id = result.find("id");
        const auto added = result.find("added");
        if (id == result.end() || !id->is_string() || id->get_ref<const std::string &>().empty() ||
            added == result.end()) {
            return;
        }
        const bool has_added = (added->is_boolean() && added->get<bool>()) ||
                               (added->is_number() && added->get<double>() > 0.0);
        if (!has_added) return;

        Json payload = {{"id", *id}, {"kind", kind}, {"source_mod", mod_id}, {"frame", frame}};
        for (const char *field : {"native_item_no", "requested", "added", "slot"}) {
            const auto value = result.find(field);
            if (value != result.end() && (value->is_string() || value->is_number() || value->is_boolean())) {
                payload[field] = *value;
            }
        }
        Queue("game.item_given", payload);
    }

    void QueueItemRemoved(std::string_view mod_id, const Json &result) {
        if (!result.is_object()) return;
        const auto id = result.find("id");
        const auto removed = result.find("removed");
        if (id == result.end() || !id->is_string() || id->get_ref<const std::string &>().empty() ||
            removed == result.end() || !removed->is_number() || removed->get<double>() <= 0.0) {
            return;
        }

        Json payload = {{"id", *id}, {"source_mod", mod_id}, {"frame", frame}};
        for (const char *field : {"requested", "removed", "remaining"}) {
            const auto value = result.find(field);
            if (value != result.end() && (value->is_string() || value->is_number() || value->is_boolean())) {
                payload[field] = *value;
            }
        }
        Queue("game.item_removed", payload);
    }

    void QueueChangedState(std::string_view event, std::string_view mod_id, const Json &result) {
        if (!result.is_object()) return;
        const auto changed = result.find("changed");
        if (changed == result.end() || !changed->is_boolean() || !changed->get<bool>()) return;
        Json payload = result;
        payload["source_mod"] = mod_id;
        payload["frame"] = frame;
        Queue(event, payload);
    }

    void DrainQueuedEvents() {
        const std::size_t count = std::min(queued_events.size(), kMaxEventsPerPump);
        for (std::size_t index = 0; index < count && running; ++index) {
            auto [event, payload] = std::move(queued_events.front());
            queued_events.pop_front();
            Emit(event, payload);
        }
    }

    void ObserveWorldLocation(bool publish_immediately = false) {
        if (!config.world_location_reader) {
            observed_world_location = nullptr;
            return;
        }
        ApiCallResult result;
        try {
            result = config.world_location_reader();
        } catch (...) {
            // Scene loaders can temporarily make the native world snapshot unavailable.
            // Keep the last valid location so the next readable snapshot still produces
            // a change event instead of silently becoming a new baseline.
            return;
        }
        if (result.status != ApiCallStatus::Ok || !result.value.is_object() ||
            !result.value.contains("context") || !result.value["context"].is_string() ||
            !result.value.contains("map_id")) {
            return;
        }
        Json location = Json::object();
        for (const char *field : {"context", "area_id", "map_id", "sub_map_id", "dungeon_id", "floor_id"}) {
            const auto value = result.value.find(field);
            if (value != result.value.end() && (value->is_string() || value->is_number_integer() || value->is_null())) {
                location[field] = *value;
            }
        }
        if (!observed_world_location.is_object()) {
            observed_world_location = std::move(location);
            return;
        }
        if (location != observed_world_location) {
            Json payload = location;
            payload["previous"] = observed_world_location;
            payload["frame"] = frame;
            if (publish_immediately) {
                Emit("game.world.location_changed", payload);
            } else {
                Queue("game.world.location_changed", payload);
            }
            observed_world_location = std::move(location);
        }
    }

    void Log(std::string_view level, std::string_view mod_id, std::string_view message) const {
        if (config.log) {
            config.log(level, mod_id, message);
        } else {
            std::fprintf(stderr, "moonmod[%.*s][%.*s]: %.*s\n", static_cast<int>(level.size()), level.data(),
                         static_cast<int>(mod_id.size()), mod_id.data(), static_cast<int>(message.size()), message.data());
        }
    }

    bool LoadProfile(std::string &error) {
        error.clear();
        profile_file = config.save_root / "moonmod" / "profile.json";
        profile = DefaultProfile();
        if (ContainsSymlink(profile_file)) {
            error = "MoonFactory profile path contains a symbolic link; defaults are active";
            return false;
        }
        std::error_code ec;
        if (!std::filesystem::is_regular_file(profile_file, ec)) return true;
        const auto size = std::filesystem::file_size(profile_file, ec);
        if (ec || size == 0 || size > 1024 * 1024) {
            error = "MoonFactory profile is unreadable or larger than 1 MiB; defaults are active";
            return false;
        }
        try {
            std::ifstream input(profile_file, std::ios::binary);
            Json loaded;
            input >> loaded;
            if (!loaded.is_object() || !loaded.contains("format_version") ||
                !loaded["format_version"].is_number_integer() || loaded["format_version"] != 1) {
                error = "MoonFactory profile has an unsupported format; defaults are active";
                return false;
            }
            if (loaded.contains("enabled") && loaded["enabled"].is_object()) profile["enabled"] = loaded["enabled"];
            if (loaded.contains("settings") && loaded["settings"].is_object()) profile["settings"] = loaded["settings"];
            if (loaded.contains("native_trusted") && loaded["native_trusted"].is_object()) {
                profile["native_trusted"] = loaded["native_trusted"];
            }
            return true;
        } catch (const std::exception &exception) {
            error = std::string("MoonFactory profile could not be parsed: ") + exception.what();
            profile = DefaultProfile();
            return false;
        }
    }

    bool SaveProfile(std::string &error) const {
        error.clear();
        if (ContainsSymlink(profile_file)) {
            error = "MoonFactory profile path contains a symbolic link";
            return false;
        }
        std::error_code ec;
        std::filesystem::create_directories(profile_file.parent_path(), ec);
        if (ec) {
            error = "could not create the MoonFactory save directory: " + ec.message();
            return false;
        }
        auto temp = profile_file;
        temp += ".tmp";
        if (ContainsSymlink(profile_file) || ContainsSymlink(temp)) {
            error = "MoonFactory profile path contains a symbolic link";
            return false;
        }
        {
            std::ofstream output(temp, std::ios::binary | std::ios::trunc);
            if (!output) {
                error = "could not write the MoonFactory profile";
                return false;
            }
            output << profile.dump(2);
            output.flush();
            if (!output) {
                error = "could not finish writing the MoonFactory profile";
                return false;
            }
        }
        std::filesystem::rename(temp, profile_file, ec);
        if (ec) {
            std::error_code remove_error;
            std::filesystem::remove(profile_file, remove_error);
            ec.clear();
            std::filesystem::rename(temp, profile_file, ec);
        }
        if (ec) {
            error = "could not replace the MoonFactory profile: " + ec.message();
            return false;
        }
        return true;
    }

    bool EnabledFromProfile(std::string_view id) const {
        const Json &enabled = session_enabled;
        const auto  found = enabled.find(std::string(id));
        return found != enabled.end() && found->is_boolean() && found->get<bool>();
    }

    std::string NativeFingerprint(const ModRecord &record) const {
        if (record.manifest.native_library.empty()) return {};
        std::error_code ec;
        const auto size = std::filesystem::file_size(record.manifest.native_library, ec);
        if (ec) return {};
        const auto modified = std::filesystem::last_write_time(record.manifest.native_library, ec);
        if (ec) return {};
        const std::string content_hash = NativePluginFileSha256(record.manifest.native_library);
        if (content_hash.empty()) return {};
        const auto relative = record.manifest.native_library.lexically_relative(record.package_root).generic_string();
        if (relative.empty() || relative.starts_with("..")) return {};
        return record.manifest.version + "|" + relative + "|" + std::to_string(size) + "|" +
               std::to_string(static_cast<long long>(modified.time_since_epoch().count())) + "|" + content_hash;
    }

    bool NativeTrustedFromProfile(const ModRecord &record) const {
        const auto trusted = profile["native_trusted"].find(record.id);
        if (trusted == profile["native_trusted"].end() || !trusted->is_string()) return false;
        const std::string current = NativeFingerprint(record);
        return !current.empty() && trusted->get_ref<const std::string &>() == current;
    }

    void StopModules() {
        for (auto it = native_plugins.rbegin(); it != native_plugins.rend(); ++it) it->second->Unload();
        native_plugins.clear();
        for (auto it = modules.rbegin(); it != modules.rend(); ++it) {
            it->second->Unload();
        }
        modules.clear();
    }

    void Discover() {
        records.clear();
        std::map<std::string, std::size_t, std::less<>> first_by_id;
        for (const auto &root : config.mod_directories) {
            std::error_code ec;
            std::filesystem::create_directories(root, ec);
            ec.clear();
            if (!std::filesystem::is_directory(root, ec)) continue;
            std::vector<std::filesystem::directory_entry> packages;
            for (std::filesystem::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
                if (it->is_directory(ec)) packages.push_back(*it);
            }
            std::ranges::sort(packages, [](const auto &left, const auto &right) {
                return left.path().filename().generic_string() < right.path().filename().generic_string();
            });
            for (const auto &package : packages) {
                const std::string directory_name = package.path().filename().generic_string();
                if (directory_name.empty() || directory_name.front() == '.') continue;
                std::string error;
                auto manifest = ReadManifest(package.path(), error);
                ModRecord record;
                record.package_root = package.path();
                record.id = manifest ? manifest->id : directory_name;
                record.display_name = manifest ? manifest->name : directory_name;
                record.valid_manifest = manifest.has_value();
                if (manifest) {
                    record.manifest = std::move(*manifest);
                    record.enabled = EnabledFromProfile(record.id);
                    record.enabled_next_launch = profile["enabled"].value(record.id, false);
                    record.restart_required = !mods_off_this_run && record.enabled != record.enabled_next_launch;
                    record.native_trusted = NativeTrustedFromProfile(record);
                    record.state = record.enabled ? ModState::Ready : ModState::Disabled;
                    if (record.manifest.framework_api > config.framework_api ||
                        !Contains(record.manifest.games, config.game_id)) {
                        record.state = ModState::Incompatible;
                        record.status_message = record.manifest.framework_api > config.framework_api
                                                    ? "Requires a newer MoonFactory API"
                                                    : "Does not target this game";
                    }
                } else {
                    record.state = ModState::Invalid;
                    record.status_message = std::move(error);
                }
                if (const auto prior = first_by_id.find(record.id); prior != first_by_id.end()) {
                    record.state = ModState::Invalid;
                    record.enabled = false;
                    record.valid_manifest = false;
                    record.status_message = "Duplicate package id; the earlier package directory takes priority";
                } else {
                    first_by_id.emplace(record.id, records.size());
                }
                records.push_back(std::move(record));
            }
        }
        std::stable_sort(records.begin(), records.end(), [](const ModRecord &left, const ModRecord &right) {
            return left.id < right.id;
        });
    }

    ModRecord *Find(std::string_view id) {
        const auto found = std::ranges::find(records, id, &ModRecord::id);
        return found == records.end() ? nullptr : &*found;
    }

    const ModRecord *Find(std::string_view id) const {
        const auto found = std::ranges::find(records, id, &ModRecord::id);
        return found == records.end() ? nullptr : &*found;
    }

    ApiCallResult CallScriptFunction(std::string_view caller_id, std::string_view owner_id,
                                     std::string_view function_id, const Json &arguments) {
        constexpr int kCallDepthLimit = 16;
        if (script_function_call_depth >= kCallDepthLimit) {
            return {ApiCallStatus::Failed, nullptr, "cross-mod script function call depth limit reached"};
        }
        const ModRecord *caller = Find(caller_id);
        const ModRecord *owner = Find(owner_id);
        if (caller == nullptr || owner == nullptr || owner->state != ModState::Loaded) {
            return {ApiCallStatus::Failed, nullptr, "script function owner is not loaded"};
        }
        const bool declared_dependency =
            std::ranges::find(caller->manifest.dependencies, owner_id) != caller->manifest.dependencies.end() ||
            std::ranges::find(caller->manifest.optional_dependencies, owner_id) !=
                caller->manifest.optional_dependencies.end();
        if (caller_id != owner_id && !declared_dependency) {
            return {ApiCallStatus::PermissionDenied, nullptr,
                    "calling another mod's functions requires a declared dependency"};
        }
        const auto module = std::ranges::find(modules, owner_id, [](const auto &entry) -> std::string_view {
            return entry.first;
        });
        if (module == modules.end()) {
            return {ApiCallStatus::Failed, nullptr, "script function owner is not running"};
        }
        ++script_function_call_depth;
        struct DepthGuard {
            int &depth;
            ~DepthGuard() { --depth; }
        } guard{script_function_call_depth};
        return module->second->InvokeFunction(function_id, arguments);
    }

    ApiCallResult ListScriptFunctions(std::string_view caller_id) const {
        const ModRecord *caller = Find(caller_id);
        if (caller == nullptr) {
            return {ApiCallStatus::Failed, nullptr, "script function caller is not loaded"};
        }
        std::vector<std::string> owners;
        for (const std::string &dependency_id : caller->manifest.dependencies) {
            const ModRecord *dependency = Find(dependency_id);
            if (dependency != nullptr && dependency->state == ModState::Loaded) owners.push_back(dependency_id);
        }
        for (const std::string &dependency_id : caller->manifest.optional_dependencies) {
            const ModRecord *dependency = Find(dependency_id);
            if (dependency != nullptr && dependency->state == ModState::Loaded) owners.push_back(dependency_id);
        }
        std::ranges::sort(owners);
        owners.erase(std::unique(owners.begin(), owners.end()), owners.end());

        Json functions = Json::array();
        for (const std::string &owner_id : owners) {
            const auto module = std::ranges::find(modules, owner_id, [](const auto &entry) -> std::string_view {
                return entry.first;
            });
            if (module == modules.end()) continue;
            ApiCallResult listed = module->second->ListFunctions();
            if (listed.status != ApiCallStatus::Ok) return listed;
            const auto entries = listed.value.find("functions");
            if (entries == listed.value.end() || !entries->is_array()) {
                return {ApiCallStatus::Failed, nullptr, "script function owner returned an invalid listing"};
            }
            for (const Json &entry : *entries) functions.push_back(entry);
        }
        return {ApiCallStatus::Ok, {{"functions", std::move(functions)}}, {}};
    }

    ApiCallResult CallLuaApi(std::string_view mod_id, std::string_view path, const Json &arguments) {
        const auto module = std::ranges::find(modules, mod_id, [](const auto &entry) -> std::string_view {
            return entry.first;
        });
        if (module == modules.end()) {
            return {ApiCallStatus::Failed, nullptr, "native API caller's Lua package is not running"};
        }
        return module->second->CallApi(path, arguments);
    }

    ApiCallResult CallNativeScriptOriginal(std::string_view mod_id, std::string_view hook_id) {
        if (native_script_hook_frames.empty()) {
            return {ApiCallStatus::PhaseViolation, nullptr,
                    "scripts.call is available only while handling a native script override"};
        }
        NativeScriptHookFrame &frame = *native_script_hook_frames.back();
        if (frame.mod_id != mod_id || frame.hook_id != hook_id) {
            return {ApiCallStatus::PermissionDenied, nullptr,
                    "scripts.call may call only the native implementation for the active owned hook"};
        }
        if (frame.original_called) {
            return {ApiCallStatus::PhaseViolation, nullptr,
                    "the native implementation for this hook can be called only once"};
        }
        frame.original_called = true;
        try {
            frame.original_result = frame.original ? frame.original()
                                                   : ApiCallResult{ApiCallStatus::Unsupported, nullptr,
                                                                   "native implementation is unavailable"};
        } catch (const std::exception &exception) {
            frame.original_result = {ApiCallStatus::Failed, nullptr,
                                     std::string("native script implementation failed: ") + exception.what()};
        } catch (...) {
            frame.original_result = {ApiCallStatus::Failed, nullptr,
                                     "native script implementation failed with an unknown exception"};
        }
        return frame.original_result;
    }

    ApiCallResult DispatchNativeScriptHook(std::string_view hook_id, const Json &payload,
                                           NativeScriptOriginalCall original) {
        if (!original) return {ApiCallStatus::InvalidArgument, nullptr, "native script hook has no implementation"};
        if (!running || (hook_id != "native.event.run" && hook_id != "native.dialogue.show" &&
                         hook_id != "native.combat.before_hit" && hook_id != "native.loot.roll" &&
                         hook_id != "native.transition.before_enter" && hook_id != "native.monster.run")) {
            return original();
        }

        std::vector<std::pair<std::string_view, LuaMod *>> ordered;
        ordered.reserve(modules.size());
        for (auto &[mod_id, module] : modules) ordered.emplace_back(mod_id, module.get());
        std::ranges::sort(ordered, [](const auto &left, const auto &right) { return left.first < right.first; });
        Json hook_payload = payload;
        if (!hook_payload.is_object()) hook_payload = Json::object();
        hook_payload["hook_id"] = hook_id;

        for (const auto &[mod_id, module] : ordered) {
            std::optional<ApiCallResult> override_result;
            NativeScriptHookFrame frame{std::string(hook_id), std::string(mod_id), original};
            native_script_hook_frames.push_back(&frame);
            try {
                override_result = module->InvokeScriptOverride(hook_id, hook_payload);
            } catch (const std::exception &exception) {
                override_result = ApiCallResult{ApiCallStatus::Failed, nullptr,
                                                std::string("native script override failed: ") + exception.what()};
            } catch (...) {
                override_result = ApiCallResult{ApiCallStatus::Failed, nullptr,
                                                "native script override failed with an unknown exception"};
            }
            native_script_hook_frames.pop_back();
            if (!override_result.has_value()) continue;

            if (frame.original_called) return frame.original_result;
            if (override_result->status != ApiCallStatus::Ok) {
                Log("error", mod_id, "native script override '" + std::string(hook_id) +
                                         "' failed: " + override_result->message);
                continue;
            }
            if (override_result->value.is_null()) continue;
            if (!override_result->value.is_object() ||
                !override_result->value.contains("handled") ||
                !override_result->value["handled"].is_boolean()) {
                Log("warning", mod_id, "native script override '" + std::string(hook_id) +
                                           "' must return nil or {handled = boolean}; running native script");
                continue;
            }
            if (override_result->value["handled"].get<bool>()) {
                return {ApiCallStatus::Ok, {{"run_status", 0}, {"overridden", true}}, {}};
            }
        }
        return original();
    }

    const SettingDefinition *FindSetting(const ModRecord &record, std::string_view setting_id) const {
        const auto found = std::ranges::find(record.manifest.settings, setting_id, &SettingDefinition::id);
        return found == record.manifest.settings.end() ? nullptr : &*found;
    }

    Json SettingValue(const ModRecord &record, const SettingDefinition &setting) const {
        const auto mod_settings = profile["settings"].find(record.id);
        if (mod_settings == profile["settings"].end() || !mod_settings->is_object()) return setting.default_value;
        const auto value = mod_settings->find(setting.id);
        if (value == mod_settings->end()) return setting.default_value;
        const Json &candidate = *value;
        switch (setting.type) {
            case SettingType::Boolean:
                return candidate.is_boolean() ? candidate : setting.default_value;
            case SettingType::Integer:
                if (candidate.is_number_integer()) {
                    const double number = candidate.get<double>();
                    if (number >= setting.minimum && number <= setting.maximum) return candidate;
                }
                return setting.default_value;
            case SettingType::Number:
                if (candidate.is_number() && std::isfinite(candidate.get<double>()) &&
                    candidate.get<double>() >= setting.minimum && candidate.get<double>() <= setting.maximum) {
                    return candidate;
                }
                return setting.default_value;
            case SettingType::String:
                return candidate.is_string() && candidate.get_ref<const std::string &>().size() <= 256
                           ? candidate
                           : setting.default_value;
            case SettingType::Choice:
                return candidate.is_string() &&
                               std::ranges::find(setting.choices, candidate.get<std::string>()) != setting.choices.end()
                           ? candidate
                           : setting.default_value;
        }
        return setting.default_value;
    }

    bool PersistSetting(const ModRecord &record, const SettingDefinition &setting, Json value, std::string &error) {
        profile["settings"][record.id][setting.id] = std::move(value);
        if (!SaveProfile(error)) return false;
        const auto loaded = std::ranges::find_if(modules, [&](const auto &entry) { return entry.first == record.id; });
        if (loaded != modules.end()) {
            loaded->second->Emit("settings.changed", {{"id", setting.id}, {"value", SettingValue(record, setting)}});
        }
        return true;
    }

    void LoadEnabled() {
        StopModules();
        std::map<std::string, std::size_t, std::less<>> index;
        for (std::size_t i = 0; i < records.size(); ++i) {
            ModRecord &record = records[i];
            if (record.valid_manifest) index.emplace(record.id, i);
        }
        std::vector<bool> candidate(records.size(), false);
        for (std::size_t i = 0; i < records.size(); ++i) {
            ModRecord &record = records[i];
            if (record.state != ModState::Ready || !record.enabled) continue;
            bool blocked = false;
            for (const std::string &dependency : record.manifest.dependencies) {
                const auto found = index.find(dependency);
                if (found == index.end() || !records[found->second].enabled ||
                    records[found->second].state == ModState::Incompatible ||
                    records[found->second].state == ModState::Invalid) {
                    record.state = ModState::MissingDependency;
                    record.status_message = "Requires enabled mod '" + dependency + "'";
                    blocked = true;
                    break;
                }
            }
            candidate[i] = !blocked;
        }

        std::vector<std::set<std::size_t>> edges(records.size());
        std::vector<int> indegree(records.size(), 0);
        auto add_edge = [&](std::size_t from, std::size_t to) {
            if (from == to || !candidate[from] || !candidate[to]) return;
            if (edges[from].insert(to).second) ++indegree[to];
        };
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (!candidate[i]) continue;
            const ModManifest &manifest = records[i].manifest;
            for (const std::string &dependency : manifest.dependencies) {
                if (const auto found = index.find(dependency); found != index.end()) add_edge(found->second, i);
            }
            for (const std::string &dependency : manifest.optional_dependencies) {
                if (const auto found = index.find(dependency); found != index.end()) add_edge(found->second, i);
            }
            for (const std::string &after : manifest.load_after) {
                if (const auto found = index.find(after); found != index.end()) add_edge(found->second, i);
            }
            for (const std::string &before : manifest.load_before) {
                if (const auto found = index.find(before); found != index.end()) add_edge(i, found->second);
            }
        }
        auto compare = [&](std::size_t left, std::size_t right) { return records[left].id < records[right].id; };
        std::set<std::size_t, decltype(compare)> ready(compare);
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (candidate[i] && indegree[i] == 0) ready.insert(i);
        }
        std::vector<std::size_t> order;
        while (!ready.empty()) {
            const std::size_t item = *ready.begin();
            ready.erase(ready.begin());
            order.push_back(item);
            for (const std::size_t next : edges[item]) {
                if (--indegree[next] == 0) ready.insert(next);
            }
        }
        for (std::size_t i = 0; i < records.size(); ++i) {
            if (candidate[i] && indegree[i] > 0) {
                records[i].state = ModState::DependencyCycle;
                records[i].status_message = "Load ordering contains a dependency cycle";
            }
        }
        for (const std::size_t i : order) {
            ModRecord &record = records[i];
            bool dependency_failed = false;
            for (const std::string &dependency : record.manifest.dependencies) {
                const ModRecord *dep = Find(dependency);
                if (dep == nullptr || dep->state != ModState::Loaded) {
                    record.state = ModState::MissingDependency;
                    record.status_message = "Dependency '" + dependency + "' did not load";
                    dependency_failed = true;
                    break;
                }
            }
            if (dependency_failed) continue;
            auto module = std::make_unique<LuaMod>(
                config, record.manifest,
                [this, id = record.id](std::string_view setting_id) -> Json {
                    const ModRecord *current = Find(id);
                    if (current == nullptr) return nullptr;
                    const SettingDefinition *setting = FindSetting(*current, setting_id);
                    return setting == nullptr ? Json(nullptr) : SettingValue(*current, *setting);
                });
            std::string error;
            if (!module->Load(error)) {
                record.state = ModState::LoadError;
                record.status_message = error;
                Log("error", record.id, error);
                continue;
            }
            modules.emplace_back(record.id, std::move(module));
            record.state = ModState::Loaded;
            record.status_message = "Lua package is running";
            record.native_status.clear();
            if (!record.manifest.native_library.empty()) {
                if (!record.native_trusted) {
                    record.native_status = "Native code is disabled until this package is trusted locally.";
                    record.status_message = "Lua is running; native extension is not trusted";
                    continue;
                }
                auto native = std::make_unique<NativePluginInstance>();
                NativePluginLoadConfig native_config;
                native_config.library = record.manifest.native_library;
                native_config.plugin_id = record.id;
                native_config.game_id = config.game_id;
                native_config.build_id = config.build_id;
                native_config.manifest_permissions = record.manifest.permissions;
                native_config.api_capabilities = config.api_capabilities;
                for (const ApiOperationSpec &operation : ApiCatalog()) {
                    if (!IsCoreApiOperation(operation.path)) continue;
                    const bool callable = NativeJsonApiCallable(operation.path);
                    const auto existing = std::ranges::find(native_config.api_capabilities, operation.path,
                                                            &ApiCapability::path);
                    const ApiCapability capability{
                        std::string(operation.path), callable,
                        callable ? std::string{} :
                                   "native api_call_json cannot provide callbacks or safely delegate api.call"};
                    if (existing == native_config.api_capabilities.end()) {
                        native_config.api_capabilities.push_back(capability);
                    } else {
                        *existing = capability;
                    }
                }
                const std::filesystem::path package_root = record.package_root;
                const PackageApiCallHandler package_api_handler = config.package_api_handler;
                const ApiCallHandler api_handler = config.api_handler;
                native_config.api_handler = [runtime = this, package_root, package_api_handler, api_handler](
                                                std::string_view mod_id, const ApiOperationSpec &operation,
                                                const Json &arguments) {
                    if (operation.domain == "audio" && package_api_handler) {
                        return package_api_handler(mod_id, package_root, operation, arguments);
                    }
                    if (IsCoreApiOperation(operation.path)) return runtime->CallLuaApi(mod_id, operation.path, arguments);
                    return api_handler ? api_handler(mod_id, operation, arguments)
                                       : ApiCallResult{ApiCallStatus::Unsupported, nullptr,
                                                       "this game adapter has not bound the operation"};
                };
                native_config.log = config.log;
                std::string native_error;
                if (!native->Load(std::move(native_config), native_error)) {
                    record.native_status = "Native extension failed to load: " + native_error;
                    record.status_message = "Lua is running; native extension failed to load";
                    Log("error", record.id, native_error);
                    continue;
                }
                record.native_status = "Trusted native extension is running.";
                record.status_message = "Lua and trusted native extension are running";
                native_plugins.emplace_back(record.id, std::move(native));
            }
        }
        for (auto &[id, module] : modules) {
            module->Emit("runtime.loaded", {{"game", config.game_id}, {"mod", id}, {"frame", frame}});
        }
        for (auto &[id, plugin] : native_plugins) {
            plugin->Publish("runtime.loaded", {{"game", config.game_id}, {"mod", id}, {"frame", frame}}, frame);
        }
    }

    // Per-mod time spent in game.tick, printed every 600 ticks when any mod averages over 0.5 ms.
    std::map<std::string, double> tick_seconds;
    int tick_samples = 0;

    void Emit(std::string_view event, const Json &payload) {
        const bool tick = event == "game.tick";
        for (auto &[id, module] : modules) {
            const auto started = std::chrono::steady_clock::now();
            module->Emit(event, payload);
            if (tick) tick_seconds[id] += std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
        }
        if (tick && ++tick_samples >= 600) {
            bool slow = false;
            for (const auto &[id, seconds] : tick_seconds) slow = slow || seconds / tick_samples > 0.0005;
            if (slow) {
                std::printf("moonmod tick cost (ms per frame):");
                for (const auto &[id, seconds] : tick_seconds) std::printf(" %s=%.2f", id.c_str(), 1000.0 * seconds / tick_samples);
                std::printf("\n");
            }
            tick_seconds.clear();
            tick_samples = 0;
        }
        for (auto &[id, plugin] : native_plugins) {
            (void)id;
            plugin->Publish(event, payload, frame);
        }
    }

    bool EmitTo(std::string_view mod_id, std::string_view event, const Json &payload) {
        const auto found = std::ranges::find(modules, mod_id, [](const auto &entry) -> std::string_view {
            return entry.first;
        });
        if (found == modules.end()) return false;
        found->second->Emit(event, payload);
        const auto native = std::ranges::find(native_plugins, mod_id, [](const auto &entry) -> std::string_view {
            return entry.first;
        });
        if (native != native_plugins.end()) native->second->Publish(event, payload, frame);
        return true;
    }
};

std::string ModStateText(ModState state) { return StateName(state); }

Runtime::Runtime() : impl_(std::make_unique<Impl>()) {}
Runtime::~Runtime() { Stop(); }

bool Runtime::Start(RuntimeConfig config, std::string &error) {
    Stop();
    error.clear();
    if (config.game_id != "chronicle" && config.game_id != "chronicle-two") {
        error = "game id must be 'chronicle' or 'chronicle-two'";
        return false;
    }
    if (config.framework_api < 1) {
        error = "framework API major must be positive";
        return false;
    }
    std::set<std::string, std::less<>> configured_api_paths;
    for (const ApiCapability &capability : config.api_capabilities) {
        if (FindApiOperation(capability.path) == nullptr) {
            error = "game adapter advertised unknown API operation '" + capability.path + "'";
            return false;
        }
        if (!configured_api_paths.insert(capability.path).second) {
            error = "game adapter advertised API operation more than once: '" + capability.path + "'";
            return false;
        }
        const ApiOperationSpec *operation = FindApiOperation(capability.path);
        const bool package_operation = operation != nullptr && operation->domain == "audio";
        if (capability.supported && !config.api_handler &&
            !(package_operation && config.package_api_handler) && !IsCoreApiOperation(capability.path)) {
            error = "game adapter advertised '" + capability.path + "' without an API handler";
            return false;
        }
    }
    if (config.require_complete_api_capabilities) {
        for (const ApiOperationSpec &operation : ApiCatalog()) {
            if (IsCoreApiOperation(operation.path)) continue;
            const auto capability = std::ranges::find(config.api_capabilities, operation.path,
                                                       &ApiCapability::path);
            if (capability == config.api_capabilities.end()) {
                error = "game adapter did not record support or unsupported status for API operation '" +
                        std::string(operation.path) + "'";
                return false;
            }
            if (!capability->supported && capability->reason.empty()) {
                error = "game adapter marked API operation unsupported without a reason: '" +
                        std::string(operation.path) + "'";
                return false;
            }
        }
    }
    config.lua_memory_limit = std::clamp<std::size_t>(config.lua_memory_limit, 1024 * 1024, 128 * 1024 * 1024);
    config.lua_instructions_per_callback = std::clamp(config.lua_instructions_per_callback, 1000, 5000000);
    if (config.mod_directories.empty()) config.mod_directories.push_back(config.save_root / "mods");
    if (config.api_handler) {
        auto adapter_call = std::move(config.api_handler);
        config.api_handler = [runtime = impl_.get(), adapter_call = std::move(adapter_call)](
                                 std::string_view mod_id, const ApiOperationSpec &operation,
                                 const Json &arguments) -> ApiCallResult {
            ApiCallResult result = adapter_call(mod_id, operation, arguments);
            if (result.status == ApiCallStatus::Ok && operation.path == "items.use") {
                runtime->QueueItemUsed(mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "items.give") {
                runtime->QueueItemGiven(mod_id, "item", result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "items.remove") {
                runtime->QueueItemRemoved(mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "weapons.give") {
                runtime->QueueItemGiven(mod_id, "weapon", result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "weapons.upgrade") {
                runtime->QueueChangedState("game.weapon.upgraded", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "weapons.abs_add" &&
                       result.value.value("added", 0) > 0) {
                runtime->QueueChangedState("game.weapon.abs_gained", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok &&
                       (operation.path == "weapons.synthesize" || operation.path == "weapons.spectrumize")) {
                runtime->QueueChangedState("game.weapon.built_up", mod_id, result.value);
                runtime->QueueChangedState("game.weapon.broken", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "weapons.equip" &&
                       result.value.value("changed", false)) {
                runtime->QueueChangedState("game.weapon.equipped", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok &&
                       (operation.path == "shops.register_item" || operation.path == "shops.remove_item" ||
                        operation.path == "shops.set_price")) {
                runtime->QueueChangedState("game.shop.purchased", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "dungeons.floor_rules_set" &&
                       result.value.value("changed", false)) {
                runtime->QueueChangedState("game.floor.cleared", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "dungeons.loot_override" &&
                       result.value.value("changed", false)) {
                runtime->QueueChangedState("game.chest.opened", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "party.set_active" &&
                       result.value.value("changed", false)) {
                runtime->QueueChangedState("game.party.active_changed", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "party.recruit" &&
                       result.value.value("recruited", false)) {
                runtime->QueueChangedState("game.party.recruited", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "world.story_flag_set") {
                runtime->QueueChangedState("game.story_flag_changed", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "world.story_value_set") {
                runtime->QueueChangedState("game.story_value_changed", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "world.map_flag_set") {
                runtime->QueueChangedState("game.map_flag_changed", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "georama.request_set") {
                runtime->QueueChangedState("game.georama.request_changed", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "georama.element_set" &&
                       result.value.value("changed", false)) {
                runtime->QueueChangedState("game.georama.element_changed", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "georama.condition_set") {
                runtime->QueueChangedState("game.georama.condition_changed", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "georama.request_known_set") {
                runtime->QueueChangedState("game.georama.request_known_changed", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "georama.river_set") {
                runtime->QueueChangedState("game.georama.river_changed", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "georama.place") {
                runtime->QueueChangedState("game.georama.piece_placed", mod_id, result.value);
            } else if (result.status == ApiCallStatus::Ok && operation.path == "georama.remove") {
                runtime->QueueChangedState("game.georama.piece_removed", mod_id, result.value);
            }
            return result;
        };
    }
    impl_->config = std::move(config);
    impl_->config.script_function_handler = [runtime = impl_.get()](std::string_view caller_id,
                                                                   std::string_view owner_id,
                                                                   std::string_view function_id,
                                                                   const Json &arguments) {
        return runtime->CallScriptFunction(caller_id, owner_id, function_id, arguments);
    };
    impl_->config.script_function_list_handler = [runtime = impl_.get()](std::string_view caller_id) {
        return runtime->ListScriptFunctions(caller_id);
    };
    impl_->config.script_hook_original_call_handler = [runtime = impl_.get()](std::string_view caller_id,
                                                                              std::string_view hook_id) {
        return runtime->CallNativeScriptOriginal(caller_id, hook_id);
    };
    impl_->frame = 0;
    impl_->observed_world_location = nullptr;
    std::string profile_error;
    if (!impl_->LoadProfile(profile_error)) impl_->Log("warning", "", profile_error);
    impl_->session_enabled = impl_->profile["enabled"];
    const char *no_mods = std::getenv("MOONMOD_NO_MODS");
    impl_->mods_off_this_run = no_mods != nullptr && std::string_view(no_mods) == "1";
    if (impl_->mods_off_this_run) {
        impl_->session_enabled = Json::object();
        impl_->Log("info", "", "MOONMOD_NO_MODS=1: every mod is off for this run (the profile keeps its choices)");
    }
    impl_->Discover();
    impl_->LoadEnabled();
    impl_->running = true;
    impl_->Emit("runtime.started", {{"game", impl_->config.game_id}, {"api_major", impl_->config.framework_api}});
    return true;
}

void Runtime::Stop() {
    if (!impl_) return;
    impl_->Emit("runtime.stopping", {{"game", impl_->config.game_id}, {"frame", impl_->frame}});
    impl_->StopModules();
    impl_->records.clear();
    impl_->queued_events.clear();
    impl_->observed_world_location = nullptr;
    impl_->running = false;
}

void Runtime::Pump() {
    if (!impl_->running) return;
    AdvanceManagerUi();
    ++impl_->frame;
    impl_->ObserveWorldLocation();
    impl_->DrainQueuedEvents();
    impl_->Emit("game.tick", {{"frame", impl_->frame}});
}

void Runtime::Publish(std::string_view event_name, const Json &payload) {
    if (!impl_ || !impl_->running || event_name.empty()) return;
    impl_->Emit(event_name, payload);
}

void Runtime::ObserveWorldLocationNow() {
    if (!impl_ || !impl_->running) return;
    impl_->ObserveWorldLocation(true);
}

void Runtime::QueueEvent(std::string_view event_name, const Json &payload) {
    if (!impl_ || event_name.empty()) return;
    impl_->Queue(event_name, payload);
}

bool Runtime::PublishToMod(std::string_view mod_id, std::string_view event_name, const Json &payload) {
    if (!impl_ || !impl_->running || mod_id.empty() || event_name.empty()) return false;
    return impl_->EmitTo(mod_id, event_name, payload);
}

ApiCallResult Runtime::DispatchNativeScriptHook(std::string_view hook_id, const Json &payload,
                                                NativeScriptOriginalCall original) {
    if (!impl_) return original ? original() : ApiCallResult{ApiCallStatus::Failed, nullptr, "runtime is unavailable"};
    return impl_->DispatchNativeScriptHook(hook_id, payload, std::move(original));
}

ApiCallResult Runtime::CallNativeScriptOriginal(std::string_view mod_id, std::string_view hook_id) {
    if (!impl_) return {ApiCallStatus::PhaseViolation, nullptr, "runtime is unavailable"};
    return impl_->CallNativeScriptOriginal(mod_id, hook_id);
}

std::span<const ModRecord> Runtime::Mods() const { return impl_->records; }
std::vector<std::string> Runtime::LoadedModOrder() const {
    std::vector<std::string> order;
    order.reserve(impl_->modules.size());
    for (const auto &[id, module] : impl_->modules) {
        (void)module;
        order.push_back(id);
    }
    return order;
}
bool                      Runtime::Running() const { return impl_->running; }
std::uint64_t             Runtime::Frame() const { return impl_->frame; }

bool Runtime::SetEnabled(std::string_view id, bool enabled, std::string &error) {
    ModRecord *record = impl_->Find(id);
    if (record == nullptr || !record->valid_manifest) {
        error = "this package does not have a valid manifest";
        return false;
    }
    if (enabled && record->state == ModState::Incompatible) {
        error = record->status_message;
        return false;
    }
    const Json previous=impl_->profile["enabled"];
    impl_->profile["enabled"][record->id] = enabled;
    if (!impl_->SaveProfile(error)) {
        impl_->profile["enabled"]=previous;
        return false;
    }
    record->enabled_next_launch = enabled;
    record->restart_required = record->enabled != enabled;
    return true;
}

bool Runtime::SetNativeTrusted(std::string_view id, bool trusted, std::string &error) {
    error.clear();
    ModRecord *record = impl_->Find(id);
    if (record == nullptr || !record->valid_manifest || record->manifest.native_library.empty()) {
        error = "this package does not declare a valid native extension";
        return false;
    }
    const auto previous = impl_->profile["native_trusted"].find(record->id);
    const bool had_previous = previous != impl_->profile["native_trusted"].end();
    const Json previous_value = had_previous ? *previous : Json();
    if (trusted) {
        const std::string fingerprint = impl_->NativeFingerprint(*record);
        if (fingerprint.empty()) {
            error = "native extension library cannot be fingerprinted";
            return false;
        }
        impl_->profile["native_trusted"][record->id] = fingerprint;
    } else {
        impl_->profile["native_trusted"].erase(record->id);
    }
    if (!impl_->SaveProfile(error)) {
        if (had_previous) impl_->profile["native_trusted"][record->id] = previous_value;
        else impl_->profile["native_trusted"].erase(record->id);
        return false;
    }
    impl_->Discover();
    impl_->LoadEnabled();
    return true;
}

Json Runtime::Setting(std::string_view mod_id, std::string_view setting_id) const {
    const ModRecord *record = impl_->Find(mod_id);
    if (record == nullptr || !record->valid_manifest) return nullptr;
    const SettingDefinition *setting = impl_->FindSetting(*record, setting_id);
    return setting == nullptr ? Json(nullptr) : impl_->SettingValue(*record, *setting);
}

ApiCallResult Runtime::Quests(std::string_view mod_id) const {
    if (!impl_ || !impl_->running || mod_id.empty()) {
        return {ApiCallStatus::Failed, nullptr, "mod runtime is not available"};
    }
    const ModRecord *record = impl_->Find(mod_id);
    if (record == nullptr || !record->valid_manifest) {
        return {ApiCallStatus::Failed, nullptr, "mod package is unavailable"};
    }
    const auto module = std::ranges::find(impl_->modules, mod_id, [](const auto &entry) -> std::string_view {
        return entry.first;
    });
    if (module != impl_->modules.end()) return module->second->GetQuests();

    const std::filesystem::path storage_file = impl_->config.save_root / "moonmod" / "data" / record->id / "storage.json";
    if (ContainsSymlink(storage_file)) {
        return {ApiCallStatus::Failed, nullptr, "saved quest data path contains a symbolic link"};
    }
    std::error_code ec;
    const auto file_status = std::filesystem::status(storage_file, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && !std::filesystem::exists(file_status))) {
        return {ApiCallStatus::Ok, {{"quests", Json::array()}}, {}};
    }
    if (ec || !std::filesystem::is_regular_file(file_status)) {
        return {ApiCallStatus::Failed, nullptr, "saved quest data file is unavailable"};
    }
    const std::uintmax_t size = std::filesystem::file_size(storage_file, ec);
    if (ec || size > 128 * 1024) {
        return {ApiCallStatus::Failed, nullptr, "saved quest data exceeds the 128 KiB storage limit"};
    }
    std::ifstream input(storage_file, std::ios::binary);
    if (!input) return {ApiCallStatus::Failed, nullptr, "saved quest data could not be opened"};
    const std::string bytes((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    Json storage;
    try {
        storage = Json::parse(bytes);
    } catch (const std::exception &) {
        return {ApiCallStatus::Failed, nullptr, "saved quest data is invalid JSON"};
    }
    if (!storage.is_object()) return {ApiCallStatus::Failed, nullptr, "saved quest data is not an object"};
    const auto stored_quests = storage.find("__moonmod_quests_v1");
    if (stored_quests == storage.end()) return {ApiCallStatus::Ok, {{"quests", Json::array()}}, {}};
    if (!stored_quests->is_object() || stored_quests->size() > 128) {
        return {ApiCallStatus::Failed, nullptr, "saved quest collection is invalid"};
    }

    Json quests = Json::array();
    for (auto stored = stored_quests->begin(); stored != stored_quests->end(); ++stored) {
        try {
            if (!stored->is_object() || !stored->contains("definition") || !stored->at("definition").is_object() ||
                !stored->contains("status") || !stored->at("status").is_string() ||
                !stored->contains("progress") || !stored->at("progress").is_object()) {
                return {ApiCallStatus::Failed, nullptr, "saved quest record is invalid"};
            }
            const Json &definition = stored->at("definition");
            if (!definition.contains("title") || !definition.at("title").is_string() ||
                !definition.contains("objectives") || !definition.at("objectives").is_array() ||
                definition.at("objectives").empty() || definition.at("objectives").size() > 64) {
                return {ApiCallStatus::Failed, nullptr, "saved quest definition is invalid"};
            }
            Json objectives = Json::array();
            for (const Json &objective : definition.at("objectives")) {
                if (!objective.is_object() || !objective.contains("id") || !objective.at("id").is_string() ||
                    !objective.contains("title") || !objective.at("title").is_string() ||
                    !objective.contains("target") || !objective.at("target").is_number_integer()) {
                    return {ApiCallStatus::Failed, nullptr, "saved quest objective is invalid"};
                }
                const std::string objective_id = objective.at("id").get<std::string>();
                const Json &saved_progress = stored->at("progress");
                const auto progress_it = saved_progress.find(objective_id);
                const std::int64_t current = progress_it != saved_progress.end() && progress_it->is_number_integer()
                                                 ? progress_it->get<std::int64_t>()
                                                 : 0;
                const std::int64_t target = objective.at("target").get<std::int64_t>();
                Json view = {{"id", objective_id}, {"title", objective.at("title")},
                             {"progress", current}, {"target", target}, {"complete", current >= target}};
                if (objective.contains("event")) view["event"] = objective.at("event");
                if (objective.contains("match")) view["match"] = objective.at("match");
                objectives.push_back(std::move(view));
            }
            quests.push_back({{"id", record->id + ":" + stored.key()}, {"title", definition.at("title")},
                              {"description", definition.value("description", std::string{})},
                              {"status", stored->at("status")}, {"objectives", std::move(objectives)}});
        } catch (const std::exception &) {
            return {ApiCallStatus::Failed, nullptr, "saved quest record is invalid"};
        }
    }
    return {ApiCallStatus::Ok, {{"quests", std::move(quests)}}, {}};
}

ApiCallResult Runtime::Skills(std::string_view mod_id) const {
    if (!impl_ || !impl_->running || mod_id.empty()) {
        return {ApiCallStatus::Failed, nullptr, "mod runtime is not available"};
    }
    const ModRecord *record = impl_->Find(mod_id);
    if (record == nullptr || !record->valid_manifest) {
        return {ApiCallStatus::Failed, nullptr, "mod package is unavailable"};
    }
    const auto module = std::ranges::find(impl_->modules, mod_id, [](const auto &entry) -> std::string_view {
        return entry.first;
    });
    if (module != impl_->modules.end()) return module->second->GetSkills();

    const std::filesystem::path storage_file = impl_->config.save_root / "moonmod" / "data" / record->id / "storage.json";
    if (ContainsSymlink(storage_file)) {
        return {ApiCallStatus::Failed, nullptr, "saved skill data path contains a symbolic link"};
    }
    std::error_code ec;
    const auto file_status = std::filesystem::status(storage_file, ec);
    if (ec == std::errc::no_such_file_or_directory ||
        (!ec && !std::filesystem::exists(file_status))) {
        return {ApiCallStatus::Ok, {{"skills", Json::array()}}, {}};
    }
    if (ec || !std::filesystem::is_regular_file(file_status)) {
        return {ApiCallStatus::Failed, nullptr, "saved skill data file is unavailable"};
    }
    const std::uintmax_t size = std::filesystem::file_size(storage_file, ec);
    if (ec || size > 128 * 1024) {
        return {ApiCallStatus::Failed, nullptr, "saved skill data exceeds the 128 KiB storage limit"};
    }
    std::ifstream input(storage_file, std::ios::binary);
    if (!input) return {ApiCallStatus::Failed, nullptr, "saved skill data could not be opened"};
    Json storage;
    try {
        input >> storage;
    } catch (const std::exception &) {
        return {ApiCallStatus::Failed, nullptr, "saved skill data is invalid JSON"};
    }
    if (!storage.is_object()) return {ApiCallStatus::Failed, nullptr, "saved skill data is not an object"};
    const auto stored_skills = storage.find("__moonmod_skills_v1");
    if (stored_skills == storage.end()) return {ApiCallStatus::Ok, {{"skills", Json::array()}}, {}};
    if (!stored_skills->is_object() || stored_skills->size() > 128) {
        return {ApiCallStatus::Failed, nullptr, "saved skill collection is invalid"};
    }
    Json skills = Json::array();
    for (auto tree = stored_skills->begin(); tree != stored_skills->end(); ++tree) {
        try {
            if (!tree->is_object() || !tree->contains("definition") || !tree->at("definition").is_object() ||
                !tree->contains("ranks") || !tree->at("ranks").is_object()) {
                return {ApiCallStatus::Failed, nullptr, "saved skill tree is invalid"};
            }
            const Json &definition = tree->at("definition");
            const Json &ranks = tree->at("ranks");
            if (!definition.contains("title") || !definition.at("title").is_string() ||
                !definition.contains("skills") || !definition.at("skills").is_array() ||
                definition.at("skills").size() > 128) {
                return {ApiCallStatus::Failed, nullptr, "saved skill definition is invalid"};
            }
            for (const Json &node : definition.at("skills")) {
                if (!node.is_object() || !node.contains("id") || !node.at("id").is_string() ||
                    !node.contains("title") || !node.at("title").is_string() ||
                    !node.contains("max_rank") || !node.at("max_rank").is_number_integer()) {
                    return {ApiCallStatus::Failed, nullptr, "saved skill node is invalid"};
                }
                const std::string node_id = node.at("id").get<std::string>();
                const auto rank_entry = ranks.find(node_id);
                if (rank_entry == ranks.end() || !rank_entry->is_number_integer()) {
                    return {ApiCallStatus::Failed, nullptr, "saved skill rank is invalid"};
                }
                const std::int64_t rank = rank_entry->get<std::int64_t>();
                const std::int64_t max_rank = node.at("max_rank").get<std::int64_t>();
                if (rank < 0 || rank > max_rank || max_rank < 1) {
                    return {ApiCallStatus::Failed, nullptr, "saved skill rank is out of range"};
                }
                bool prerequisites_met = true;
                Json prerequisites = Json::array();
                const Json required = node.value("prerequisites", Json::array());
                if (!required.is_array()) return {ApiCallStatus::Failed, nullptr, "saved skill prerequisites are invalid"};
                for (const Json &requirement : required) {
                    if (!requirement.is_object() || !requirement.contains("id") || !requirement.at("id").is_string() ||
                        !requirement.contains("rank") || !requirement.at("rank").is_number_integer()) {
                        return {ApiCallStatus::Failed, nullptr, "saved skill prerequisite is invalid"};
                    }
                    const std::string required_id = requirement.at("id").get<std::string>();
                    const std::int64_t required_rank = requirement.at("rank").get<std::int64_t>();
                    const auto actual = ranks.find(required_id);
                    const std::int64_t actual_rank = actual != ranks.end() && actual->is_number_integer()
                                                         ? actual->get<std::int64_t>()
                                                         : 0;
                    const bool met = actual_rank >= required_rank;
                    prerequisites_met = prerequisites_met && met;
                    prerequisites.push_back({{"id", record->id + ":" + tree.key() + "." + required_id},
                                              {"required_rank", required_rank}, {"rank", actual_rank}, {"met", met}});
                }
                const std::string qualified_id = record->id + ":" + tree.key() + "." + node_id;
                skills.push_back({{"id", qualified_id}, {"tree_id", record->id + ":" + tree.key()},
                                  {"tree_title", definition.at("title")}, {"id_local", node_id},
                                  {"title", node.at("title")}, {"description", node.value("description", std::string{})},
                                  {"effects", node.value("effects", Json::object())},
                                  {"rank", rank}, {"max_rank", max_rank}, {"unlocked", rank > 0},
                                  {"prerequisites_met", prerequisites_met},
                                  {"available", rank < max_rank && prerequisites_met},
                                  {"prerequisites", std::move(prerequisites)}});
            }
        } catch (const std::exception &) {
            return {ApiCallStatus::Failed, nullptr, "saved skill record is invalid"};
        }
    }
    return {ApiCallStatus::Ok, {{"skills", std::move(skills)}}, {}};
}

Json Runtime::CombatDamageBonuses() const {
    Json bonuses = Json::array();
    if (!impl_ || !impl_->running) return bonuses;

    std::vector<const ModRecord *> active_mods;
    for (const ModRecord &record : impl_->records) {
        if (record.enabled && record.state == ModState::Loaded) active_mods.push_back(&record);
    }
    std::ranges::sort(active_mods, {}, [](const ModRecord *record) -> const std::string & { return record->id; });
    for (const ModRecord *record : active_mods) {
        const ApiCallResult listed = Skills(record->id);
        if (listed.status != ApiCallStatus::Ok || !listed.value.is_object() ||
            !listed.value.contains("skills") || !listed.value["skills"].is_array()) {
            continue;
        }
        for (const Json &skill : listed.value["skills"]) {
            if (!skill.is_object() || !skill.contains("id") || !skill["id"].is_string() ||
                !skill.contains("rank") || !skill["rank"].is_number_integer() ||
                !skill.contains("effects") || !skill["effects"].is_object() ||
                !skill["effects"].contains("enemy_damage_per_rank") ||
                !skill["effects"]["enemy_damage_per_rank"].is_number()) {
                continue;
            }
            const std::int64_t rank = skill["rank"].get<std::int64_t>();
            const double per_rank = skill["effects"]["enemy_damage_per_rank"].get<double>();
            if (rank <= 0 || rank > 99 || !std::isfinite(per_rank) || per_rank <= 0.0 || per_rank > 1.0) {
                continue;
            }
            bonuses.push_back({{"owner", record->id}, {"skill_id", skill["id"]},
                               {"rank", rank}, {"per_rank", per_rank}});
        }
    }
    return bonuses;
}

bool Runtime::StepSetting(std::string_view mod_id, std::string_view setting_id, int direction, std::string &error) {
    error.clear();
    if (direction == 0) return true;
    const ModRecord *record = impl_->Find(mod_id);
    if (record == nullptr || !record->valid_manifest) {
        error = "package is unavailable";
        return false;
    }
    const SettingDefinition *setting = impl_->FindSetting(*record, setting_id);
    if (setting == nullptr) {
        error = "setting is unavailable";
        return false;
    }
    Json value = impl_->SettingValue(*record, *setting);
    switch (setting->type) {
        case SettingType::Boolean:
            value = !value.get<bool>();
            break;
        case SettingType::Integer: {
            const long double next = static_cast<long double>(value.get<std::int64_t>()) +
                                     (direction > 0 ? setting->step : -setting->step);
            const long double rounded = std::round(next);
            value = static_cast<std::int64_t>(std::clamp(rounded, static_cast<long double>(setting->minimum),
                                                         static_cast<long double>(setting->maximum)));
            break;
        }
        case SettingType::Number: {
            const long double next = static_cast<long double>(value.get<double>()) +
                                     (direction > 0 ? setting->step : -setting->step);
            const long double clamped = std::clamp(next, static_cast<long double>(setting->minimum),
                                                   static_cast<long double>(setting->maximum));
            const long double rounded = std::round(clamped / setting->step) * setting->step;
            const long double bounded = std::clamp(rounded, static_cast<long double>(setting->minimum),
                                                   static_cast<long double>(setting->maximum));
            value = static_cast<double>(bounded);
            break;
        }
        case SettingType::Choice: {
            const auto found = std::ranges::find(setting->choices, value.get<std::string>());
            const auto index = found == setting->choices.end() ? 0 : static_cast<int>(found - setting->choices.begin());
            const int count = static_cast<int>(setting->choices.size());
            value = setting->choices[static_cast<std::size_t>((index + (direction > 0 ? 1 : count - 1)) % count)];
            break;
        }
        case SettingType::String:
            error = "text settings are read-only in this manager; edit the mod profile to change them";
            return false;
    }
    return impl_->PersistSetting(*record, *setting, std::move(value), error);
}

Runtime &SharedRuntime() {
    static Runtime runtime;
    return runtime;
}

namespace {
std::uint64_t g_monster_script_override_generation = 1;
}

std::uint64_t MonsterScriptOverrideGeneration() { return g_monster_script_override_generation; }

void BumpMonsterScriptOverrideGeneration() { ++g_monster_script_override_generation; }

} // namespace moonmod

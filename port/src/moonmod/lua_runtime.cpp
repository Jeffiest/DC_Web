#include "lua_runtime.hpp"
#include "moonmod/symlinks.hpp"
#include "moonmod/manager_ui.hpp"
#include "moonmod/runtime.hpp"
#include "moonmod/native_resource_overrides.hpp"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <map>
#include <ranges>
#include <set>
#include <stdexcept>
#include <string>
#include <system_error>
#include <utility>
#include <vector>
#include <cstdlib>
#include <iterator>

extern "C" {
#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

namespace dc::game {}
using namespace dc::game;
}

namespace moonmod {
namespace {

constexpr int         kHookInterval = 1000;
constexpr std::size_t kScriptSizeLimit = 4 * 1024 * 1024;
constexpr std::size_t kStorageSizeLimit = 128 * 1024;
constexpr std::size_t kStorageValueLimit = 4096;
constexpr int         kEventCallbackLimit = 128;
constexpr std::size_t kScriptFunctionLimit = 256;
constexpr int         kScriptFunctionCallDepthLimit = 16;
constexpr int         kJsonDepthLimit = 16;
constexpr std::size_t kApiValueNodeLimit = 4096;
constexpr std::size_t kApiValueBytesLimit = 64 * 1024;
constexpr std::size_t kApiResourceResultBytesLimit = 384 * 1024;
constexpr std::size_t kResourceFileSizeLimit = 256 * 1024;
constexpr std::size_t kResourceTotalSizeLimit = 1024 * 1024;
constexpr std::size_t kResourceHandleLimit = 64;
constexpr std::size_t kLocalizationEntryLimit = 512;
constexpr std::size_t kLocalizationLanguageLimit = 32;
constexpr std::size_t kLocalizationValueLimit = 4096;
constexpr std::size_t kLocalizationBytesLimit = 64 * 1024;
constexpr std::string_view kSaveMigrationVersionKey = "__moonmod_schema_version";
constexpr std::string_view kQuestStorageKey = "__moonmod_quests_v1";
constexpr std::string_view kSkillStorageKey = "__moonmod_skills_v1";

unsigned LuaSeed(std::string_view mod_id) {
    std::uint32_t seed = 2166136261u;
    for (const char character : mod_id) {
        seed = (seed ^ static_cast<unsigned char>(character)) * 16777619u;
    }
    return seed;
}

bool HasPermission(const ModManifest &manifest, std::string_view permission) {
    return std::ranges::find(manifest.permissions, permission) != manifest.permissions.end();
}

bool ValidKey(std::string_view key) {
    return !key.empty() && key.size() <= 128;
}

bool AsciiAlpha(char value) {
    return (value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z');
}

bool AsciiAlnum(char value) {
    return AsciiAlpha(value) || (value >= '0' && value <= '9');
}

bool NormalizeLanguageTag(std::string_view input, std::string &normalized) {
    if (input.empty() || input.size() > 35 || input.front() == '-' || input.back() == '-') return false;
    normalized.assign(input);
    for (char &value : normalized) {
        if (value == '-') continue;
        if (!AsciiAlnum(value)) return false;
        if (value >= 'A' && value <= 'Z') value = static_cast<char>(value - 'A' + 'a');
    }
    std::size_t begin = 0;
    std::size_t part = 0;
    while (begin < normalized.size()) {
        const std::size_t end = normalized.find('-', begin);
        const std::size_t length = (end == std::string::npos ? normalized.size() : end) - begin;
        if (length == 0 || length > 8 || (part == 0 && length < 2)) return false;
        if (part == 0 && !std::ranges::all_of(std::string_view(normalized).substr(begin, length), AsciiAlpha)) {
            return false;
        }
        ++part;
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    return true;
}

bool ValidLocalizationKey(std::string_view key) {
    return !key.empty() && key.size() <= 128 &&
           std::ranges::all_of(key, [](char value) {
               return AsciiAlnum(value) || value == '.' || value == '_' || value == '-';
           });
}

// Storage tables: a 1..n sequence is saved as a JSON array; any other table as an
// object whose integer keys are written "#<n>" (string keys starting with '#' get one
// more '#'), so StoragePush gives the mod back exactly the keys it saved.
Json StorageValue(lua_State *state, int index, bool &valid, int depth = 0) {
    valid = true;
    index = lua_absindex(state, index);
    if (lua_type(state, index) == LUA_TTABLE) {
        if (depth > kJsonDepthLimit) {
            valid = false;
            return {};
        }
        const lua_Integer length = static_cast<lua_Integer>(lua_rawlen(state, index));
        lua_Integer count = 0;
        bool sequence = true;
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            count++;
            if (!lua_isinteger(state, -2) || lua_tointeger(state, -2) < 1 || lua_tointeger(state, -2) > length) sequence = false;
            lua_pop(state, 1);
        }
        if (sequence && count == length && length > 0) {
            Json array = Json::array();
            for (lua_Integer i = 1; i <= length; i++) {
                lua_rawgeti(state, index, i);
                array.push_back(StorageValue(state, -1, valid, depth + 1));
                lua_pop(state, 1);
                if (!valid) return {};
            }
            return array;
        }
        Json object = Json::object();
        lua_pushnil(state);
        while (lua_next(state, index) != 0) {
            std::string key;
            if (lua_type(state, -2) == LUA_TSTRING) {
                std::size_t key_length = 0;
                const char *text = lua_tolstring(state, -2, &key_length);
                key.assign(text, key_length);
                if (!key.empty() && key[0] == '#') key.insert(key.begin(), '#');
            } else if (lua_isinteger(state, -2)) {
                key = "#" + std::to_string(lua_tointeger(state, -2));
            } else {
                lua_pop(state, 2);
                valid = false;
                return {};
            }
            Json item = StorageValue(state, -1, valid, depth + 1);
            lua_pop(state, 1);
            if (!valid) {
                lua_pop(state, 1);
                return {};
            }
            object[key] = std::move(item);
        }
        return object;
    }
    switch (lua_type(state, index)) {
        case LUA_TBOOLEAN:
            return lua_toboolean(state, index) != 0;
        case LUA_TNUMBER:
            if (lua_isinteger(state, index)) return static_cast<std::int64_t>(lua_tointeger(state, index));
            if (const double value = lua_tonumber(state, index); std::isfinite(value)) return value;
            valid = false;
            return {};
        case LUA_TSTRING: {
            std::size_t length = 0;
            const char *value = lua_tolstring(state, index, &length);
            if (length <= kStorageValueLimit) return std::string(value, length);
            valid = false;
            return {};
        }
        default:
            valid = false;
            return {};
    }
}

void PushJson(lua_State *state, const Json &value, int depth = 0) {
    if (depth > kJsonDepthLimit || value.is_null()) {
        lua_pushnil(state);
    } else if (value.is_boolean()) {
        lua_pushboolean(state, value.get<bool>());
    } else if (value.is_number_integer()) {
        lua_pushinteger(state, static_cast<lua_Integer>(value.get<std::int64_t>()));
    } else if (value.is_number()) {
        lua_pushnumber(state, value.get<double>());
    } else if (value.is_string()) {
        const auto &text = value.get_ref<const std::string &>();
        lua_pushlstring(state, text.data(), text.size());
    } else if (value.is_array()) {
        lua_createtable(state, static_cast<int>(value.size()), 0);
        lua_Integer index = 1;
        for (const Json &item : value) {
            PushJson(state, item, depth + 1);
            lua_seti(state, -2, index++);
        }
    } else if (value.is_object()) {
        lua_createtable(state, 0, static_cast<int>(value.size()));
        for (auto it = value.begin(); it != value.end(); ++it) {
            lua_pushlstring(state, it.key().data(), it.key().size());
            PushJson(state, it.value(), depth + 1);
            lua_settable(state, -3);
        }
    } else {
        lua_pushnil(state);
    }
}

// Inverse of StorageValue for tables (see the key rules there).
void StoragePush(lua_State *state, const Json &value, int depth = 0) {
    if (depth > kJsonDepthLimit) {
        lua_pushnil(state);
    } else if (value.is_array()) {
        lua_createtable(state, static_cast<int>(value.size()), 0);
        lua_Integer index = 1;
        for (const Json &item : value) {
            StoragePush(state, item, depth + 1);
            lua_seti(state, -2, index++);
        }
    } else if (value.is_object()) {
        lua_createtable(state, 0, static_cast<int>(value.size()));
        for (auto it = value.begin(); it != value.end(); ++it) {
            const std::string &key = it.key();
            if (key.size() > 1 && key[0] == '#' && key[1] == '#') {
                lua_pushlstring(state, key.data() + 1, key.size() - 1);
            } else if (key.size() > 1 && key[0] == '#') {
                lua_pushinteger(state, static_cast<lua_Integer>(std::strtoll(key.c_str() + 1, nullptr, 10)));
            } else {
                lua_pushlstring(state, key.data(), key.size());
            }
            StoragePush(state, it.value(), depth + 1);
            lua_settable(state, -3);
        }
    } else {
        PushJson(state, value, depth);
    }
}

bool ReadApiJson(lua_State *state, int index, Json &result, std::string &error, int depth,
                 std::size_t &nodes, std::set<const void *> &active) {
    if (++nodes > kApiValueNodeLimit) {
        error = "API value exceeds the node limit";
        return false;
    }
    if (depth > kJsonDepthLimit) {
        error = "API value exceeds the nesting limit";
        return false;
    }
    switch (lua_type(state, index)) {
        case LUA_TNIL:
            result = nullptr;
            return true;
        case LUA_TBOOLEAN:
            result = lua_toboolean(state, index) != 0;
            return true;
        case LUA_TNUMBER:
            if (lua_isinteger(state, index)) {
                result = static_cast<std::int64_t>(lua_tointeger(state, index));
                return true;
            }
            if (const double value = lua_tonumber(state, index); std::isfinite(value)) {
                result = value;
                return true;
            }
            error = "API numbers must be finite";
            return false;
        case LUA_TSTRING: {
            std::size_t length = 0;
            const char *value = lua_tolstring(state, index, &length);
            if (length > kApiValueBytesLimit) {
                error = "API string exceeds the byte limit";
                return false;
            }
            result = std::string(value, length);
            return true;
        }
        case LUA_TTABLE: {
            const int table = lua_absindex(state, index);
            const void *identity = lua_topointer(state, table);
            if (active.contains(identity)) {
                error = "cyclic Lua tables cannot be passed through the game API";
                return false;
            }
            active.insert(identity);
            bool                         saw_string = false;
            bool                         saw_integer = false;
            Json                         object = Json::object();
            std::map<lua_Integer, Json>  array_values;
            lua_pushnil(state);
            while (lua_next(state, table) != 0) {
                Json value;
                if (!ReadApiJson(state, -1, value, error, depth + 1, nodes, active)) {
                    lua_pop(state, 2);
                    active.erase(identity);
                    return false;
                }
                if (lua_type(state, -2) == LUA_TSTRING) {
                    if (saw_integer) {
                        error = "API tables cannot mix string and integer keys";
                        lua_pop(state, 2);
                        active.erase(identity);
                        return false;
                    }
                    saw_string = true;
                    std::size_t key_length = 0;
                    const char *key = lua_tolstring(state, -2, &key_length);
                    if (key_length > 256) {
                        error = "API object keys are limited to 256 bytes";
                        lua_pop(state, 2);
                        active.erase(identity);
                        return false;
                    }
                    object[std::string(key, key_length)] = std::move(value);
                } else if (lua_isinteger(state, -2) && lua_tointeger(state, -2) > 0) {
                    if (saw_string) {
                        error = "API tables cannot mix string and integer keys";
                        lua_pop(state, 2);
                        active.erase(identity);
                        return false;
                    }
                    saw_integer = true;
                    array_values.emplace(lua_tointeger(state, -2), std::move(value));
                } else {
                    error = "API tables require string keys or contiguous positive integer keys";
                    lua_pop(state, 2);
                    active.erase(identity);
                    return false;
                }
                lua_pop(state, 1);
            }
            active.erase(identity);
            if (saw_integer) {
                result = Json::array();
                lua_Integer expected = 1;
                for (auto &[key, value] : array_values) {
                    if (key != expected++) {
                        error = "API arrays must use contiguous integer keys starting at 1";
                        return false;
                    }
                    result.push_back(std::move(value));
                }
            } else {
                result = std::move(object);
            }
            return true;
        }
        default:
            error = "API values support only nil, booleans, finite numbers, strings, and tables";
            return false;
    }
}

void SetFunction(lua_State *state, const char *name, lua_CFunction function, void *context) {
    lua_pushlightuserdata(state, context);
    lua_pushcclosure(state, function, 1);
    lua_setfield(state, -2, name);
}

} // namespace

struct LuaMod::Impl {
    struct EventCallback {
        int token = 0;
        int reference = LUA_NOREF;
        std::string owner; // the mod, or the pack's part, that registered it
    };

    struct CommandCallback {
        int reference = LUA_NOREF;
    };

    struct ScriptFunctionCallback {
        int reference = LUA_NOREF;
        std::string description;
    };

    struct ScriptOverrideCallback {
        int reference = LUA_NOREF;
        std::string owner;
    };

    struct Memory {
        std::size_t used = 0;
        std::size_t limit = 0;
    } memory;

    struct LocalizationEntry {
        std::map<std::string, std::string, std::less<>> translations;
        std::string fallback_language;
        std::size_t bytes = 0;
    };

    struct ResourceEntry {
        std::string path;
        std::string media_type;
        std::string bytes;
    };

    const RuntimeConfig &config;
    // The current owner's manifest: the mod's own, or (in a merged pack, while one of its parts runs) the part's.
    ModManifest manifest;
    std::function<Json(std::string_view owner, std::string_view setting)> setting_reader;
    std::filesystem::path storage_file;
    Json storage = Json::object();
    lua_State *state = nullptr;
    int        budget_remaining = 0;
    std::map<std::string, std::vector<EventCallback>, std::less<>> callbacks;
    std::map<std::string, CommandCallback, std::less<>> commands;
    std::map<std::string, ScriptFunctionCallback, std::less<>> script_functions;
    std::map<std::string, ScriptOverrideCallback, std::less<>> script_overrides;
    std::map<std::string, LocalizationEntry, std::less<>> localizations;
    std::map<std::string, ResourceEntry, std::less<>> resources;
    std::size_t localization_bytes = 0;
    std::size_t resource_bytes = 0;
    int        next_event_token = 1;
    int        script_function_call_depth = 0;
    std::uint64_t next_resource_handle = 1;
    bool       storage_dirty = false;
    bool       save_migration_window = false;
    bool       budget_exceeded = false;

    // A merged pack runs its parts in this one Lua state, each as its own owner: what it registers, its settings and
    // its saved data (moonmod/data/<part id>/, as when it was a mod of its own) are the part's. The current owner's
    // state is in the fields above (manifest, storage, commands, ...); every other owner's waits in its slot here.
    struct OwnerState {
        ModManifest manifest;
        std::filesystem::path storage_file;
        Json storage = Json::object();
        bool storage_dirty = false;
        bool storage_loaded = false;
        std::map<std::string, CommandCallback, std::less<>> commands;
        std::map<std::string, ScriptFunctionCallback, std::less<>> script_functions;
        std::map<std::string, LocalizationEntry, std::less<>> localizations;
        std::size_t localization_bytes = 0;
    };
    std::map<std::string, OwnerState, std::less<>> owners;
    std::string home_id;
    std::set<std::string, std::less<>> enabled_parts;
    std::map<std::string, int, std::less<>> part_functions;
    // A part given as source with a globals table: that table, and a copy of it as it was handed over, so the part
    // starts with fresh globals each time it is turned on again.
    std::map<std::string, std::pair<int, int>, std::less<>> part_globals;
    std::set<std::string, std::less<>> running_parts;
    std::map<std::string, std::string, std::less<>> part_errors;
    std::size_t callback_limit = kEventCallbackLimit;

    Impl(const RuntimeConfig &runtime_config, const ModManifest &mod_manifest,
         std::function<Json(std::string_view, std::string_view)> reader)
        : memory{0, runtime_config.lua_memory_limit}, config(runtime_config), manifest(mod_manifest),
          setting_reader(std::move(reader)),
          storage_file(runtime_config.save_root / "moonmod" / "data" / mod_manifest.id / "storage.json"),
          home_id(mod_manifest.id) {
        // A pack's parts share its state, so they share its budgets too: each part brings its own share.
        const std::size_t shares = 1 + mod_manifest.parts.size();
        memory.limit = std::min<std::size_t>(runtime_config.lua_memory_limit * shares, 512u * 1024u * 1024u);
        callback_limit = kEventCallbackLimit * shares;
    }

    void ResetOwners() {
        owners.clear();
        running_parts.clear();
        part_functions.clear();
        part_globals.clear();
        part_errors.clear();
        if (manifest.parts.empty()) return;
        owners[home_id].storage_loaded = true; // the home owner is checked out: its slot holds nothing until it parks
        for (const ModManifest &part : manifest.parts) {
            OwnerState slot;
            slot.manifest = part;
            slot.storage_file = config.save_root / "moonmod" / "data" / part.id / "storage.json";
            owners.emplace(part.id, std::move(slot));
        }
    }

    void SwapCurrent(OwnerState &slot) {
        std::swap(manifest, slot.manifest);
        std::swap(storage_file, slot.storage_file);
        std::swap(storage, slot.storage);
        std::swap(storage_dirty, slot.storage_dirty);
        std::swap(commands, slot.commands);
        std::swap(script_functions, slot.script_functions);
        std::swap(localizations, slot.localizations);
        std::swap(localization_bytes, slot.localization_bytes);
    }

    bool SwitchOwner(std::string_view id) {
        if (id.empty() || id == manifest.id) return true;
        const auto target = owners.find(id);
        const auto current = owners.find(manifest.id);
        if (target == owners.end() || current == owners.end()) return false;
        SwapCurrent(current->second);
        SwapCurrent(target->second);
        if (!target->second.storage_loaded) {
            target->second.storage_loaded = true;
            std::string error;
            if (!LoadStorage(error)) Log("warning", error);
        }
        return true;
    }

    // While it lives, the given owner (a part of this pack) is current; the previous one comes back after.
    struct OwnerScope {
        Impl &self;
        std::string previous;
        bool switched = false;
        OwnerScope(Impl &impl, std::string_view id) : self(impl), previous(impl.manifest.id) {
            switched = !id.empty() && id != previous && impl.SwitchOwner(id);
        }
        ~OwnerScope() {
            if (switched) self.SwitchOwner(previous);
        }
        OwnerScope(const OwnerScope &) = delete;
        OwnerScope &operator=(const OwnerScope &) = delete;
    };

    std::vector<std::string> OwnerIds() const {
        std::vector<std::string> ids{home_id};
        for (const std::string &part : running_parts) ids.push_back(part);
        return ids;
    }

    // What the game holds for an owner (input binds, HUD, title modes, menus, ...), let go.
    void ReleaseOwner(const std::string &owner) {
        if (config.api_owner_unloaded) {
            try {
                config.api_owner_unloaded(owner);
            } catch (const std::exception &exception) {
                Log("error", std::string("API owner cleanup failed: ") + exception.what());
            } catch (...) {
                Log("error", "API owner cleanup failed with an unknown exception");
            }
        }
        RemoveNativeResourceOverridesByOwner(owner);
        RemoveManagerActions(owner);
        ClearCharacterSheet(owner);
    }

    // Puts a part's globals table back as it was handed over: what an earlier run set is gone.
    void ResetPartGlobals(const std::string &part) {
        const auto globals = part_globals.find(part);
        if (state == nullptr || globals == part_globals.end()) return;
        lua_rawgeti(state, LUA_REGISTRYINDEX, globals->second.first);  // env
        lua_rawgeti(state, LUA_REGISTRYINDEX, globals->second.second); // as handed over
        // Every key now in env gets its handed-over value (nil drops it; assigning an existing field while
        // traversing is allowed).
        lua_pushnil(state);
        while (lua_next(state, -3) != 0) {
            lua_pop(state, 1);
            lua_pushvalue(state, -1);
            lua_pushvalue(state, -1);
            lua_rawget(state, -4);
            lua_rawset(state, -5);
        }
        // Then every handed-over key is put back.
        lua_pushnil(state);
        while (lua_next(state, -2) != 0) {
            lua_pushvalue(state, -2);
            lua_insert(state, -2);
            lua_rawset(state, -5);
        }
        lua_pop(state, 2);
    }

    bool StartPart(const std::string &part, std::string &error) {
        error.clear();
        const auto function = part_functions.find(part);
        if (state == nullptr || function == part_functions.end()) {
            error = "the pack's script did not hand this part over (moonmod.part.run)";
            part_errors[part] = error;
            return false;
        }
        if (running_parts.contains(part)) return true;
        bool started = false;
        {
            OwnerScope scope(*this, part);
            if (!scope.switched) {
                error = "'" + part + "' is not a part of this pack";
                return false;
            }
            ResetPartGlobals(part);
            lua_rawgeti(state, LUA_REGISTRYINDEX, function->second);
            if (ProtectedCall(0, 1, error)) {
                if (lua_istable(state, -1)) {
                    AddModuleCallback("on_load", "runtime.loaded");
                    AddModuleCallback("on_unload", "runtime.unloaded");
                    AddModuleCallback("on_tick", "game.tick");
                }
                lua_pop(state, 1);
                started = true;
            }
        }
        running_parts.insert(part);
        if (!started) {
            StopPart(part, false);
            part_errors[part] = error;
            Log("error", "part '" + part + "' failed to load: " + error);
            return false;
        }
        part_errors.erase(part);
        return true;
    }

    void StopPart(const std::string &part, bool announce) {
        if (!running_parts.contains(part)) return;
        if (announce) Emit("runtime.unloaded", Json::object(), part);
        {
            OwnerScope scope(*this, part);
            if (scope.switched) {
                for (auto &[id, command] : commands) {
                    (void)id;
                    luaL_unref(state, LUA_REGISTRYINDEX, command.reference);
                }
                commands.clear();
                for (auto &[id, function] : script_functions) {
                    (void)id;
                    luaL_unref(state, LUA_REGISTRYINDEX, function.reference);
                }
                script_functions.clear();
                localizations.clear();
                localization_bytes = 0;
                std::string error;
                if (!FlushStorage(error)) Log("error", error);
            }
        }
        for (auto it = callbacks.begin(); it != callbacks.end();) {
            std::erase_if(it->second, [&](const EventCallback &callback) {
                if (callback.owner != part) return false;
                luaL_unref(state, LUA_REGISTRYINDEX, callback.reference);
                return true;
            });
            it = it->second.empty() ? callbacks.erase(it) : std::next(it);
        }
        bool monster_hook = false;
        std::erase_if(script_overrides, [&](const auto &entry) {
            if (entry.second.owner != part) return false;
            monster_hook = monster_hook || entry.first == "native.monster.run";
            luaL_unref(state, LUA_REGISTRYINDEX, entry.second.reference);
            return true;
        });
        if (monster_hook) BumpMonsterScriptOverrideGeneration();
        ReleaseOwner(part);
        running_parts.erase(part);
        // Its saved data is read again from disk when it next runs.
        if (const auto slot = owners.find(part); slot != owners.end()) {
            slot->second.storage = Json::object();
            slot->second.storage_dirty = false;
            slot->second.storage_loaded = false;
        }
    }

    static int ApiPartRun(lua_State *lua) {
        Impl *self = Upvalue(lua);
        std::size_t length = 0;
        const char *raw = luaL_checklstring(lua, 1, &length);
        const std::string part(raw, length);
        // The part's code: a function, or its script's source (compiled here as a chunk of its own, with the
        // globals table given third and the name given fourth, so its line numbers are its own file's and it has a
        // main chunk's room for locals, as when it was a mod of its own).
        const bool source = lua_type(lua, 2) == LUA_TSTRING;
        if (!source) luaL_checktype(lua, 2, LUA_TFUNCTION);
        if (source && !lua_isnoneornil(lua, 3)) luaL_checktype(lua, 3, LUA_TTABLE);
        if (self == nullptr || part == self->home_id || !self->owners.contains(part)) {
            return luaL_error(lua, "'%s' is not a part of this pack", part.c_str());
        }
        if (self->manifest.id != self->home_id) return luaL_error(lua, "a part cannot start another part");
        if (!self->part_functions.contains(part)) {
            if (source) {
                std::size_t code_length = 0;
                const char *code = lua_tolstring(lua, 2, &code_length);
                const std::string name = "=" + (lua_type(lua, 4) == LUA_TSTRING ? std::string(lua_tostring(lua, 4)) : part);
                if (luaL_loadbufferx(lua, code, code_length, name.c_str(), "t") != LUA_OK) {
                    const char *message = lua_tostring(lua, -1);
                    std::string error = message != nullptr ? message : "the part's code does not compile";
                    lua_pop(lua, 1);
                    self->part_errors[part] = error;
                    lua_pushboolean(lua, 0);
                    lua_pushlstring(lua, error.data(), error.size());
                    return 2;
                }
                if (lua_istable(lua, 3)) {
                    lua_pushvalue(lua, 3);
                    lua_setupvalue(lua, -2, 1); // a main chunk's one upvalue is its _ENV
                    lua_newtable(lua);          // a copy of the globals as handed over
                    lua_pushnil(lua);
                    while (lua_next(lua, 3) != 0) {
                        lua_pushvalue(lua, -2);
                        lua_insert(lua, -2);
                        lua_rawset(lua, -4);
                    }
                    const int copy = luaL_ref(lua, LUA_REGISTRYINDEX);
                    lua_pushvalue(lua, 3);
                    self->part_globals[part] = {luaL_ref(lua, LUA_REGISTRYINDEX), copy};
                }
            } else {
                lua_pushvalue(lua, 2);
            }
            self->part_functions[part] = luaL_ref(lua, LUA_REGISTRYINDEX);
        }
        if (!self->enabled_parts.contains(part)) {
            lua_pushboolean(lua, 0);
            lua_pushstring(lua, "off");
            return 2;
        }
        std::string error;
        const bool started = self->StartPart(part, error);
        lua_pushboolean(lua, started ? 1 : 0);
        if (started) {
            lua_pushnil(lua);
        } else {
            lua_pushlstring(lua, error.data(), error.size());
        }
        return 2;
    }

    static int ApiPartEnabled(lua_State *lua) {
        Impl *self = Upvalue(lua);
        std::size_t length = 0;
        const char *raw = luaL_checklstring(lua, 1, &length);
        lua_pushboolean(lua, self != nullptr && self->enabled_parts.contains(std::string_view(raw, length)) ? 1 : 0);
        return 1;
    }

    static int ApiPartList(lua_State *lua) {
        Impl *self = Upvalue(lua);
        Json parts = Json::array();
        if (self != nullptr) {
            for (const auto &[id, slot] : self->owners) {
                if (id == self->home_id) continue;
                parts.push_back({{"id", id},
                                 {"enabled", self->enabled_parts.contains(id)},
                                 {"running", self->running_parts.contains(id)}});
            }
        }
        PushJson(lua, parts);
        return 1;
    }

    static void *Allocate(void *userdata, void *pointer, std::size_t old_size, std::size_t new_size) {
        auto &memory = *static_cast<Memory *>(userdata);
        if (new_size == 0) {
            std::free(pointer);
            if (pointer != nullptr) {
                memory.used = old_size > memory.used ? 0 : memory.used - old_size;
            }
            return nullptr;
        }
        const std::size_t accounted_old = pointer == nullptr ? 0 : old_size;
        if (new_size > accounted_old && new_size - accounted_old > memory.limit - std::min(memory.used, memory.limit)) {
            return nullptr;
        }
        void *replacement = std::realloc(pointer, new_size);
        if (replacement != nullptr) {
            if (new_size >= accounted_old) {
                memory.used += new_size - accounted_old;
            } else {
                memory.used -= std::min(memory.used, accounted_old - new_size);
            }
        }
        return replacement;
    }

    static Impl *Self(lua_State *lua) {
        return *static_cast<Impl **>(lua_getextraspace(lua));
    }

    static void InstructionHook(lua_State *lua, lua_Debug *) {
        Impl *self = Self(lua);
        if (self == nullptr) return;
        if (self->budget_exceeded || self->budget_remaining <= kHookInterval) {
            self->budget_exceeded = true;
            self->budget_remaining = 0;
            lua_sethook(lua, InstructionHook, LUA_MASKCOUNT | LUA_MASKCALL | LUA_MASKRET, 1);
            luaL_error(lua, "Lua instruction budget exceeded");
            return;
        }
        self->budget_remaining -= kHookInterval;
    }

    static void BeginBudget(lua_State *lua, void *userdata) {
        auto *self = static_cast<Impl *>(userdata);
        self->budget_exceeded = false;
        self->budget_remaining = self->config.lua_instructions_per_callback;
        lua_sethook(lua, InstructionHook, LUA_MASKCOUNT, kHookInterval);
    }

    static int Print(lua_State *lua) {
        Impl *self = Self(lua);
        if (self == nullptr) return 0;
        std::string message;
        const int count = lua_gettop(lua);
        for (int index = 1; index <= count; ++index) {
            if (index > 1) message.push_back('\t');
            std::size_t length = 0;
            const char *text = luaL_tolstring(lua, index, &length);
            if (text != nullptr && message.size() + length <= 8192) message.append(text, length);
            lua_pop(lua, 1);
        }
        self->Log("info", message);
        return 0;
    }

    void Log(std::string_view level, std::string_view message) const {
        if (config.log) {
            config.log(level, manifest.id, message);
        } else {
            std::fprintf(stderr, "moonmod[%.*s][%.*s]: %.*s\n", static_cast<int>(level.size()), level.data(),
                         static_cast<int>(manifest.id.size()), manifest.id.data(), static_cast<int>(message.size()),
                         message.data());
        }
    }

    static Impl *Upvalue(lua_State *lua) {
        return static_cast<Impl *>(lua_touserdata(lua, lua_upvalueindex(1)));
    }

    static bool ReservedStorageKey(std::string_view key) {
        return key == kSaveMigrationVersionKey || key == kQuestStorageKey || key == kSkillStorageKey;
    }

    static bool IsBuiltInApi(std::string_view path) {
        return IsCoreApiOperation(path);
    }

    bool Capability(std::string_view path, std::string &reason) const {
        if (IsBuiltInApi(path)) {
            reason.clear();
            return true;
        }
        const auto found = std::ranges::find(config.api_capabilities, path, &ApiCapability::path);
        if (found == config.api_capabilities.end()) {
            reason = "this game adapter does not advertise the operation";
            return false;
        }
        reason = found->reason;
        return found->supported;
    }

    Json CapabilityList() const {
        Json result = Json::array();
        for (const ApiOperationSpec &operation : ApiCatalog()) {
            std::string reason;
            const bool supported = Capability(operation.path, reason);
            result.push_back({{"path", operation.path}, {"domain", operation.domain},
                              {"operation", operation.operation}, {"framework_api_major", config.framework_api},
                              {"domain_api_major", operation.domain_api_major},
                              {"phase", operation.phase}, {"permission", operation.permission},
                              {"summary", operation.summary}, {"supported", supported},
                              {"status", supported ? "supported" : "unsupported"}, {"reason", reason}});
        }
        return result;
    }

    bool ParseLocalizationId(std::string_view raw, std::string &local_id, std::string &error) const {
        const std::string prefix = manifest.id + ":";
        if (raw.starts_with(prefix)) raw.remove_prefix(prefix.size());
        else if (raw.find(':') != std::string_view::npos) {
            error = "qualified localization ids must belong to this mod";
            return false;
        }
        if (!ValidLocalizationKey(raw)) {
            error = "localization id must contain 1-128 ASCII letters, digits, '.', '_' or '-'";
            return false;
        }
        local_id.assign(raw);
        return true;
    }

    bool ParseManagerActionId(std::string_view raw, std::string &local_id, std::string &error) const {
        const std::string prefix = manifest.id + ":";
        if (raw.starts_with(prefix)) raw.remove_prefix(prefix.size());
        else if (raw.find(':') != std::string_view::npos) {
            error = "qualified action ids must belong to this mod";
            return false;
        }
        if (raw.empty() || raw.size() > 64 ||
            !std::ranges::all_of(raw, [](unsigned char character) {
                return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                       character == '-';
            })) {
            error = "action id must contain 1-64 ASCII letters, digits, '.', '_' or '-'";
            return false;
        }
        local_id.assign(raw);
        return true;
    }

    bool ParseCommandId(std::string_view raw, std::string &local_id, std::string &error) const {
        if (!ParseManagerActionId(raw, local_id, error)) return false;
        if (local_id.size() > 60) {
            error = "command id must contain 1-60 ASCII letters, digits, '.', '_' or '-'";
            return false;
        }
        return true;
    }

    ApiCallResult InvokeCommand(std::string_view local_id, const Json &arguments) {
        const auto found = commands.find(local_id);
        if (found == commands.end()) return {ApiCallStatus::Failed, nullptr, "command is not registered"};
        lua_rawgeti(state, LUA_REGISTRYINDEX, found->second.reference);
        PushJson(state, arguments);
        std::string error;
        if (!ProtectedCall(1, 1, error)) {
            return {ApiCallStatus::Failed, nullptr, "command callback failed: " + error};
        }
        Json result;
        std::set<const void *> active;
        std::size_t nodes = 0;
        if (!ReadApiJson(state, -1, result, error, 0, nodes, active)) {
            lua_pop(state, 1);
            return {ApiCallStatus::Failed, nullptr, "command callback returned an invalid value: " + error};
        }
        lua_pop(state, 1);
        try {
            if (result.dump().size() > kApiValueBytesLimit) {
                return {ApiCallStatus::Failed, nullptr, "command result exceeds its 64 KiB limit"};
            }
        } catch (const std::exception &) {
            return {ApiCallStatus::Failed, nullptr, "command result is not valid JSON"};
        }
        return {ApiCallStatus::Ok, std::move(result), {}};
    }

    ApiCallResult InvokeScriptFunction(std::string_view local_id, const Json &arguments) {
        const auto found = script_functions.find(local_id);
        if (found == script_functions.end()) {
            return {ApiCallStatus::Failed, nullptr, "script function is not registered"};
        }
        if (script_function_call_depth >= kScriptFunctionCallDepthLimit) {
            return {ApiCallStatus::Failed, nullptr, "script function call depth limit reached"};
        }
        try {
            if (arguments.dump().size() > kApiValueBytesLimit) {
                return {ApiCallStatus::InvalidArgument, nullptr, "script function arguments exceed 64 KiB"};
            }
        } catch (const std::exception &) {
            return {ApiCallStatus::InvalidArgument, nullptr, "script function arguments are not valid JSON"};
        }

        ++script_function_call_depth;
        struct DepthGuard {
            int &depth;
            ~DepthGuard() { --depth; }
        } guard{script_function_call_depth};

        lua_rawgeti(state, LUA_REGISTRYINDEX, found->second.reference);
        PushJson(state, arguments);
        std::string error;
        if (!ProtectedCall(1, 1, error)) {
            return {ApiCallStatus::Failed, nullptr, "script function callback failed: " + error};
        }
        Json result;
        std::set<const void *> active;
        std::size_t nodes = 0;
        if (!ReadApiJson(state, -1, result, error, 0, nodes, active)) {
            lua_pop(state, 1);
            return {ApiCallStatus::Failed, nullptr, "script function returned an invalid value: " + error};
        }
        lua_pop(state, 1);
        try {
            if (result.dump().size() > kApiValueBytesLimit) {
                return {ApiCallStatus::Failed, nullptr, "script function result exceeds 64 KiB"};
            }
        } catch (const std::exception &) {
            return {ApiCallStatus::Failed, nullptr, "script function result is not valid JSON"};
        }
        return {ApiCallStatus::Ok, std::move(result), {}};
    }

    ApiCallResult ListOwnScriptFunctions() const {
        Json functions = Json::array();
        for (const auto &[local_id, callback] : script_functions) {
            functions.push_back({{"id", manifest.id + ":" + local_id},
                                 {"owner_mod_id", manifest.id},
                                 {"description", callback.description}});
        }
        return {ApiCallStatus::Ok, {{"functions", std::move(functions)}}, {}};
    }

    ApiCallResult ListScriptFunctions(const Json &arguments) const {
        if (!arguments.is_array() || !arguments.empty()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "scripts.list_functions does not accept arguments"};
        }
        ApiCallResult dependencies = {ApiCallStatus::Ok, {{"functions", Json::array()}}, {}};
        try {
            if (config.script_function_list_handler) {
                dependencies = config.script_function_list_handler(manifest.id);
            }
        } catch (const std::exception &exception) {
            return {ApiCallStatus::Failed, nullptr,
                    std::string("script function list dispatcher failed: ") + exception.what()};
        } catch (...) {
            return {ApiCallStatus::Failed, nullptr, "script function list dispatcher failed with an unknown exception"};
        }
        if (dependencies.status != ApiCallStatus::Ok) return dependencies;
        if (!dependencies.value.is_object()) {
            return {ApiCallStatus::Failed, nullptr, "script function list dispatcher returned an invalid listing"};
        }
        const auto dependency_functions = dependencies.value.find("functions");
        if (dependency_functions == dependencies.value.end() || !dependency_functions->is_array()) {
            return {ApiCallStatus::Failed, nullptr, "script function list dispatcher returned an invalid listing"};
        }
        Json functions = *dependency_functions;
        const ApiCallResult own = ListOwnScriptFunctions();
        for (const Json &entry : *own.value.find("functions")) functions.push_back(entry);
        std::sort(functions.begin(), functions.end(), [](const Json &left, const Json &right) {
            return left.value("id", std::string{}) < right.value("id", std::string{});
        });
        return {ApiCallStatus::Ok, {{"functions", std::move(functions)}}, {}};
    }

    ApiCallResult CallScriptFunction(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object() ||
            !HasOnlyFields(arguments[0], {"id", "arguments"})) {
            return {ApiCallStatus::InvalidArgument, nullptr,
                    "scripts.call_function requires an object with id and optional arguments"};
        }
        const Json &request = arguments[0];
        const auto id_field = request.find("id");
        if (id_field == request.end() || !id_field->is_string()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "scripts.call_function requires a string id"};
        }
        std::string_view requested_id = id_field->get_ref<const std::string &>();
        std::string owner_id = manifest.id;
        std::string local_id;
        const std::size_t separator = requested_id.find(':');
        if (separator == std::string_view::npos) {
            local_id.assign(requested_id);
        } else {
            owner_id.assign(requested_id.substr(0, separator));
            requested_id.remove_prefix(separator + 1);
            if (!IsValidModId(owner_id) || requested_id.empty() || requested_id.find(':') != std::string_view::npos) {
                return {ApiCallStatus::InvalidArgument, nullptr,
                        "function id must be local or use the form mod.id:function_id"};
            }
            local_id.assign(requested_id);
        }
        std::string error;
        std::string parsed_id;
        if (!ParseCommandId(local_id, parsed_id, error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        const auto value = request.find("arguments");
        const Json function_arguments = value == request.end() ? Json::object() : *value;
        if (owner_id == manifest.id) return InvokeScriptFunction(parsed_id, function_arguments);
        if (!config.script_function_handler) {
            return {ApiCallStatus::Unsupported, nullptr, "cross-mod script function calls are unavailable"};
        }
        try {
            return config.script_function_handler(manifest.id, owner_id, parsed_id, function_arguments);
        } catch (const std::exception &exception) {
            return {ApiCallStatus::Failed, nullptr, std::string("script function dispatcher failed: ") + exception.what()};
        } catch (...) {
            return {ApiCallStatus::Failed, nullptr, "script function dispatcher failed with an unknown exception"};
        }
    }

    ApiCallResult UnregisterScriptFunction(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object() ||
            !HasOnlyFields(arguments[0], {"id"})) {
            return {ApiCallStatus::InvalidArgument, nullptr,
                    "scripts.unregister_function requires an object with an id"};
        }
        const auto id_field = arguments[0].find("id");
        if (id_field == arguments[0].end() || !id_field->is_string()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "scripts.unregister_function requires a string id"};
        }
        std::string local_id;
        std::string error;
        if (!ParseCommandId(id_field->get_ref<const std::string &>(), local_id, error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        const auto function = script_functions.find(local_id);
        if (function == script_functions.end()) {
            return {ApiCallStatus::Failed, nullptr, "script function is not registered by this mod"};
        }
        luaL_unref(state, LUA_REGISTRYINDEX, function->second.reference);
        script_functions.erase(function);
        return {ApiCallStatus::Ok, {{"removed", true}, {"id", manifest.id + ":" + local_id}}, {}};
    }

    ApiCallResult ExecuteCommand(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "commands.execute requires one object argument"};
        }
        const Json &request = arguments[0];
        if (!HasOnlyFields(request, {"id", "arguments"})) {
            return {ApiCallStatus::InvalidArgument, nullptr, "commands.execute accepts only 'id' and 'arguments'"};
        }
        const auto id_field = request.find("id");
        if (id_field == request.end() || !id_field->is_string()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "commands.execute requires a string command id"};
        }
        std::string local_id;
        std::string error;
        if (!ParseCommandId(id_field->get_ref<const std::string &>(), local_id, error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        const auto args_field = request.find("arguments");
        return InvokeCommand(local_id, args_field == request.end() ? Json::object() : *args_field);
    }

    ApiCallResult UnregisterCommand(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object() ||
            !HasOnlyFields(arguments[0], {"id"})) {
            return {ApiCallStatus::InvalidArgument, nullptr, "commands.unregister requires an object with an id"};
        }
        const auto id_field = arguments[0].find("id");
        if (id_field == arguments[0].end() || !id_field->is_string()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "commands.unregister requires a string command id"};
        }
        std::string local_id;
        std::string error;
        if (!ParseCommandId(id_field->get_ref<const std::string &>(), local_id, error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        const auto command = commands.find(local_id);
        if (command == commands.end()) return {ApiCallStatus::Failed, nullptr, "command is not registered"};
        RemoveManagerAction(manifest.id, "cmd." + local_id);
        luaL_unref(state, LUA_REGISTRYINDEX, command->second.reference);
        commands.erase(command);
        return {ApiCallStatus::Ok, {{"removed", true}, {"id", manifest.id + ":" + local_id}}, {}};
    }

    ApiCallResult RegisterLocalization(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a localization definition object"};
        }
        const Json &definition = arguments[0];
        const auto id_field = definition.find("id");
        const auto translations_field = definition.find("translations");
        if (id_field == definition.end() || !id_field->is_string() ||
            translations_field == definition.end() || !translations_field->is_object() ||
            translations_field->empty() || translations_field->size() > kLocalizationLanguageLimit) {
            return {ApiCallStatus::InvalidArgument, nullptr,
                    "expected id and 1-32 translations keyed by language tag"};
        }
        std::string local_id;
        std::string error;
        if (!ParseLocalizationId(id_field->get_ref<const std::string &>(), local_id, error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        if (localizations.contains(local_id)) {
            return {ApiCallStatus::InvalidArgument, nullptr, "localization id is already registered by this mod"};
        }
        if (localizations.size() >= kLocalizationEntryLimit) {
            return {ApiCallStatus::InvalidArgument, nullptr, "a mod may register at most 512 localization entries"};
        }

        LocalizationEntry entry;
        entry.bytes = local_id.size();
        for (auto it = translations_field->begin(); it != translations_field->end(); ++it) {
            std::string language;
            if (!NormalizeLanguageTag(it.key(), language) || !it.value().is_string()) {
                return {ApiCallStatus::InvalidArgument, nullptr,
                        "translations must map valid language tags to strings"};
            }
            const std::string &text = it.value().get_ref<const std::string &>();
            if (text.size() > kLocalizationValueLimit) {
                return {ApiCallStatus::InvalidArgument, nullptr, "each localized string is limited to 4096 bytes"};
            }
            if (!entry.translations.emplace(language, text).second) {
                return {ApiCallStatus::InvalidArgument, nullptr, "translation language tags must be unique ignoring case"};
            }
            entry.bytes += language.size() + text.size();
        }
        const auto fallback_field = definition.find("fallback_language");
        if (fallback_field != definition.end()) {
            if (!fallback_field->is_string() || !NormalizeLanguageTag(fallback_field->get_ref<const std::string &>(),
                                                                        entry.fallback_language)) {
                return {ApiCallStatus::InvalidArgument, nullptr, "fallback_language must be a valid language tag"};
            }
            if (!entry.translations.contains(entry.fallback_language)) {
                return {ApiCallStatus::InvalidArgument, nullptr, "fallback_language must have a translation"};
            }
        } else if (entry.translations.contains("en")) {
            entry.fallback_language = "en";
        } else {
            entry.fallback_language = entry.translations.begin()->first;
        }
        entry.bytes += entry.fallback_language.size();
        if (entry.bytes > kLocalizationBytesLimit - std::min(localization_bytes, kLocalizationBytesLimit)) {
            return {ApiCallStatus::InvalidArgument, nullptr, "mod localization data is limited to 64 KiB"};
        }
        localization_bytes += entry.bytes;
        localizations.emplace(local_id, std::move(entry));
        return {ApiCallStatus::Ok, {{"id", manifest.id + ":" + local_id}}, {}};
    }

    ApiCallResult GetLocalization(const Json &arguments) const {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a localization lookup object"};
        }
        const Json &query = arguments[0];
        const auto id_field = query.find("id");
        if (id_field == query.end() || !id_field->is_string()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a localization id"};
        }
        std::string local_id;
        std::string error;
        if (!ParseLocalizationId(id_field->get_ref<const std::string &>(), local_id, error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        const auto found = localizations.find(local_id);
        if (found == localizations.end()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "localization id is not registered by this mod"};
        }
        std::string language = "en";
        const auto language_field = query.find("language");
        if (language_field == query.end() && config.current_language) {
            try {
                const std::string native_language = config.current_language();
                if (!NormalizeLanguageTag(native_language, language)) language = "en";
            } catch (...) {
                language = "en";
            }
        } else if (language_field != query.end() &&
                   (!language_field->is_string() ||
                    !NormalizeLanguageTag(language_field->get_ref<const std::string &>(), language))) {
            return {ApiCallStatus::InvalidArgument, nullptr, "language must be a valid language tag"};
        }
        const LocalizationEntry &entry = found->second;
        std::string candidate = std::move(language);
        while (true) {
            if (const auto translation = entry.translations.find(candidate); translation != entry.translations.end()) {
                return {ApiCallStatus::Ok, translation->second, {}};
            }
            const std::size_t separator = candidate.rfind('-');
            if (separator == std::string::npos) break;
            candidate.resize(separator);
        }
        return {ApiCallStatus::Ok, entry.translations.at(entry.fallback_language), {}};
    }


    static std::string Base64Encode(std::string_view bytes) {
        constexpr std::string_view alphabet =
            "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string encoded;
        encoded.reserve(((bytes.size() + 2) / 3) * 4);
        for (std::size_t index = 0; index < bytes.size(); index += 3) {
            const auto first = static_cast<unsigned char>(bytes[index]);
            const auto second = index + 1 < bytes.size() ? static_cast<unsigned char>(bytes[index + 1]) : 0;
            const auto third = index + 2 < bytes.size() ? static_cast<unsigned char>(bytes[index + 2]) : 0;
            const unsigned value = (static_cast<unsigned>(first) << 16) |
                                   (static_cast<unsigned>(second) << 8) | static_cast<unsigned>(third);
            encoded.push_back(alphabet[(value >> 18) & 0x3f]);
            encoded.push_back(alphabet[(value >> 12) & 0x3f]);
            encoded.push_back(index + 1 < bytes.size() ? alphabet[(value >> 6) & 0x3f] : '=');
            encoded.push_back(index + 2 < bytes.size() ? alphabet[value & 0x3f] : '=');
        }
        return encoded;
    }

    static std::string ResourceMediaType(const std::filesystem::path &path) {
        std::string extension = path.extension().string();
        for (char &character : extension) {
            if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
        }
        if (extension == ".json") return "application/json";
        if (extension == ".txt" || extension == ".lua" || extension == ".md") return "text/plain";
        if (extension == ".png") return "image/png";
        if (extension == ".jpg" || extension == ".jpeg") return "image/jpeg";
        if (extension == ".dds") return "image/vnd-ms.dds";
        if (extension == ".wav") return "audio/wav";
        if (extension == ".ogg") return "audio/ogg";
        return "application/octet-stream";
    }

    ApiCallResult LoadResource(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a package resource object with a path"};
        }
        const Json &query = arguments[0];
        const auto path_field = query.find("path");
        if (path_field == query.end() || !path_field->is_string()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a package-relative resource path"};
        }
        const std::string &raw_path = path_field->get_ref<const std::string &>();
        if (raw_path.size() < 11 || raw_path.size() > 240 || !raw_path.starts_with("resources/") ||
            std::ranges::any_of(raw_path, [](unsigned char character) {
                return character < 0x20 || character == '\\' || character == ':' || character == '*' ||
                       character == '?' || character == '"' || character == '<' || character == '>' ||
                       character == '|';
            })) {
            return {ApiCallStatus::InvalidArgument, nullptr,
                    "resource path must be a safe relative path below the package resources/ directory"};
        }
        if (resources.size() >= kResourceHandleLimit) {
            return {ApiCallStatus::InvalidArgument, nullptr, "a mod may hold at most 64 loaded resources"};
        }

        std::filesystem::path relative_path;
        try {
            std::u8string utf8_path;
            utf8_path.reserve(raw_path.size());
            for (unsigned char character : raw_path) utf8_path.push_back(static_cast<char8_t>(character));
            relative_path = std::filesystem::path(utf8_path);
        } catch (const std::exception &) {
            return {ApiCallStatus::InvalidArgument, nullptr, "resource path is not valid UTF-8"};
        }
        if (relative_path.is_absolute() || relative_path.has_root_name() || relative_path.has_root_directory() ||
            std::ranges::any_of(relative_path, [](const std::filesystem::path &part) {
                return part == "." || part == "..";
            })) {
            return {ApiCallStatus::InvalidArgument, nullptr, "resource path must not contain root or traversal segments"};
        }

        std::error_code error;
        const std::filesystem::path package_root = std::filesystem::canonical(manifest.package_root, error);
        if (error || moonmod::ContainsSymlinkBelow(manifest.package_root, manifest.package_root.parent_path())) {
            return {ApiCallStatus::Failed, nullptr, "mod package path is missing or contains a symbolic link"};
        }
        const std::filesystem::path candidate = manifest.package_root / relative_path;
        if (moonmod::ContainsSymlinkBelow(candidate, manifest.package_root)) {
            return {ApiCallStatus::Failed, nullptr, "resource path contains a symbolic link"};
        }
        const std::filesystem::path resolved = std::filesystem::canonical(candidate, error);
        if (error) return {ApiCallStatus::Failed, nullptr, "resource file could not be resolved"};
        const std::filesystem::path within_package = resolved.lexically_relative(package_root);
        if (within_package.empty() || within_package.is_absolute() ||
            (!within_package.empty() && *within_package.begin() == "..")) {
            return {ApiCallStatus::Failed, nullptr, "resource resolves outside this mod package"};
        }
        if (!std::filesystem::is_regular_file(resolved, error) || error) {
            return {ApiCallStatus::Failed, nullptr, "resource path is not a regular file"};
        }
        const std::uintmax_t file_size = std::filesystem::file_size(resolved, error);
        if (error) return {ApiCallStatus::Failed, nullptr, "resource size could not be read"};
        if (file_size > kResourceFileSizeLimit ||
            file_size > kResourceTotalSizeLimit - std::min(resource_bytes, kResourceTotalSizeLimit)) {
            return {ApiCallStatus::InvalidArgument, nullptr,
                    "resource size exceeds the 256 KiB per-file or 1 MiB per-mod limit"};
        }

        std::ifstream input(resolved, std::ios::binary);
        if (!input) return {ApiCallStatus::Failed, nullptr, "resource file could not be opened"};
        std::string bytes(static_cast<std::size_t>(file_size), '\0');
        if (!bytes.empty()) {
            input.read(bytes.data(), static_cast<std::streamsize>(bytes.size()));
            if (input.gcount() != static_cast<std::streamsize>(bytes.size())) {
                return {ApiCallStatus::Failed, nullptr, "resource file changed or could not be read completely"};
            }
        }
        const std::string handle = manifest.id + ":resource:" + std::to_string(next_resource_handle++);
        const std::string normalized_path = within_package.generic_string();
        const std::string media_type = ResourceMediaType(resolved);
        resource_bytes += bytes.size();
        resources.emplace(handle, ResourceEntry{normalized_path, media_type, std::move(bytes)});
        return {ApiCallStatus::Ok,
                {{"handle", handle}, {"path", normalized_path}, {"size", file_size},
                 {"media_type", media_type}, {"status", "loaded"}},
                {}};
    }

    ApiCallResult ResolveResource(const Json &arguments) const {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a resource handle object"};
        }
        const auto handle_field = arguments[0].find("handle");
        if (handle_field == arguments[0].end() || !handle_field->is_string() ||
            handle_field->get_ref<const std::string &>().empty()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a resource handle"};
        }
        const auto found = resources.find(handle_field->get_ref<const std::string &>());
        if (found == resources.end()) {
            return {ApiCallStatus::Failed, nullptr, "resource handle is stale or is not owned by this mod"};
        }
        const ResourceEntry &resource = found->second;
        return {ApiCallStatus::Ok,
                {{"handle", handle_field->get_ref<const std::string &>()}, {"path", resource.path},
                 {"size", resource.bytes.size()}, {"media_type", resource.media_type},
                 {"encoding", "base64"}, {"data", Base64Encode(resource.bytes)}},
                {}};
    }

    ApiCallResult ReleaseResource(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a resource handle object"};
        }
        const auto handle_field = arguments[0].find("handle");
        if (handle_field == arguments[0].end() || !handle_field->is_string() ||
            handle_field->get_ref<const std::string &>().empty()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a resource handle"};
        }
        const auto found = resources.find(handle_field->get_ref<const std::string &>());
        if (found == resources.end()) {
            return {ApiCallStatus::Failed, nullptr, "resource handle is stale or is not owned by this mod"};
        }
        resource_bytes -= found->second.bytes.size();
        resources.erase(found);
        return {ApiCallStatus::Ok, {{"handle", handle_field->get_ref<const std::string &>()}, {"removed", true}}, {}};
    }


    ApiCallResult NotifyUi(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a UI notification object"};
        }
        const Json &query = arguments[0];
        const auto message_field = query.find("message");
        if (message_field == query.end() || !message_field->is_string()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a non-empty notification message"};
        }
        const std::string &message = message_field->get_ref<const std::string &>();
        if (message.empty() || message.size() > 200 ||
            std::ranges::any_of(message, [](unsigned char character) { return character < 0x20; })) {
            return {ApiCallStatus::InvalidArgument, nullptr,
                    "notification messages must contain 1-200 bytes without control characters"};
        }
        int duration = 240;
        const auto duration_field = query.find("duration_frames");
        if (duration_field != query.end()) {
            if (!duration_field->is_number_integer()) {
                return {ApiCallStatus::InvalidArgument, nullptr, "duration_frames must be an integer from 1 to 3600"};
            }
            const std::int64_t requested_duration = duration_field->get<std::int64_t>();
            if (requested_duration < 1 || requested_duration > 3600) {
                return {ApiCallStatus::InvalidArgument, nullptr, "duration_frames must be an integer from 1 to 3600"};
            }
            duration = static_cast<int>(requested_duration);
        }
        // An icon for the dungeon's message window: a native status badge, or a png this mod registered.
        std::string icon;
        if (const auto icon_field = query.find("icon"); icon_field != query.end()) {
            static constexpr std::array<std::string_view, 5> kBadges = {"freeze", "stamina", "poison", "curse", "goo"};
            if (!icon_field->is_string()) {
                return {ApiCallStatus::InvalidArgument, nullptr, "icon must be a status badge name or a png game path"};
            }
            icon = icon_field->get<std::string>();
            if (std::ranges::find(kBadges, icon) == kBadges.end() &&
                !(icon.ends_with(".png") && NativeResourceOverrideOwnedBy(manifest.id, icon))) {
                return {ApiCallStatus::InvalidArgument, nullptr,
                        "icon must be freeze, stamina, poison, curse, goo, or a png game path this mod registered"};
            }
        }
        ShowManagerNotice(message, duration, std::move(icon));
        return {ApiCallStatus::Ok,
                {{"queued", true}, {"manager_open", ManagerOpen()}, {"duration_frames", duration}},
                {}};
    }

    ApiCallResult SetCharacterSheetUi(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object() ||
            !HasOnlyFields(arguments[0], {"title", "lines", "progress", "progress_label", "hint", "trees"})) {
            return {ApiCallStatus::InvalidArgument, nullptr,
                    "expected a character sheet object with title and optional lines, progress, progress_label, hint, trees"};
        }
        const Json &sheet = arguments[0];
        CharacterSheet record;
        const auto title = sheet.find("title");
        if (title == sheet.end() || !title->is_string()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "title must be a string"};
        }
        record.title = title->get<std::string>();
        if (const auto lines = sheet.find("lines"); lines != sheet.end()) {
            if (!lines->is_array()) return {ApiCallStatus::InvalidArgument, nullptr, "lines must be an array of strings"};
            for (const Json &line : *lines) {
                if (!line.is_string()) return {ApiCallStatus::InvalidArgument, nullptr, "lines must be an array of strings"};
                record.lines.push_back(line.get<std::string>());
            }
        }
        if (const auto progress = sheet.find("progress"); progress != sheet.end()) {
            if (!progress->is_number()) return {ApiCallStatus::InvalidArgument, nullptr, "progress must be a number from 0 to 1"};
            const double value = progress->get<double>();
            if (!(value >= 0.0 && value <= 1.0)) return {ApiCallStatus::InvalidArgument, nullptr, "progress must be a number from 0 to 1"};
            record.progress = value;
        }
        if (const auto label = sheet.find("progress_label"); label != sheet.end()) {
            if (!label->is_string()) return {ApiCallStatus::InvalidArgument, nullptr, "progress_label must be a string"};
            record.progress_label = label->get<std::string>();
        }
        if (const auto hint = sheet.find("hint"); hint != sheet.end()) {
            if (!hint->is_string()) return {ApiCallStatus::InvalidArgument, nullptr, "hint must be a string"};
            record.hint = hint->get<std::string>();
        }
        if (const auto trees = sheet.find("trees"); trees != sheet.end()) {
            if (!trees->is_array()) return {ApiCallStatus::InvalidArgument, nullptr, "trees must be an array of tree ids"};
            for (const Json &tree : *trees) {
                if (!tree.is_string()) return {ApiCallStatus::InvalidArgument, nullptr, "trees must be an array of tree ids"};
                record.trees.push_back(manifest.id + ":" + tree.get<std::string>());
            }
        }
        for (const std::string *text : {&record.title, &record.progress_label, &record.hint}) {
            if (std::ranges::any_of(*text, [](unsigned char c) { return c < 0x20; })) {
                return {ApiCallStatus::InvalidArgument, nullptr, "sheet text cannot contain control characters"};
            }
        }
        for (const std::string &line : record.lines) {
            if (std::ranges::any_of(line, [](unsigned char c) { return c < 0x20; })) {
                return {ApiCallStatus::InvalidArgument, nullptr, "sheet text cannot contain control characters"};
            }
        }
        std::string error;
        if (!SetCharacterSheet(manifest.id, std::move(record), error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        return {ApiCallStatus::Ok, {{"owner", manifest.id}, {"status", "set"}}, {}};
    }

    ApiCallResult NavigateUi(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a manager destination object"};
        }
        const Json &destination = arguments[0];
        const auto mod_field = destination.find("mod_id");
        const auto tab_field = destination.find("tab");
        if (mod_field == destination.end() || !mod_field->is_string() || tab_field == destination.end() ||
            !tab_field->is_string() || destination.size() != 2) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected only mod_id and tab strings"};
        }
        const std::string mod_id = mod_field->get<std::string>();
        const std::string tab = tab_field->get<std::string>();
        std::string error;
        if (!NavigateManager(mod_id, tab, error)) {
            return {ApiCallStatus::Failed, nullptr, std::move(error)};
        }
        return {ApiCallStatus::Ok, {{"opened", true}, {"mod_id", mod_id}, {"tab", tab}}, {}};
    }

    ApiCallResult RegisterUiAction(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a manager action definition object"};
        }
        const Json &definition = arguments[0];
        const auto id_field = definition.find("id");
        const auto label_field = definition.find("label");
        if (id_field == definition.end() || !id_field->is_string() ||
            label_field == definition.end() || !label_field->is_string()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected an action id and label"};
        }
        std::string local_id;
        std::string error;
        if (!ParseManagerActionId(id_field->get_ref<const std::string &>(), local_id, error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        std::string description;
        const auto description_field = definition.find("description");
        if (description_field != definition.end()) {
            if (!description_field->is_string()) {
                return {ApiCallStatus::InvalidArgument, nullptr, "description must be a string"};
            }
            description = description_field->get<std::string>();
        }
        for (auto field = definition.begin(); field != definition.end(); ++field) {
            if (field.key() != "id" && field.key() != "label" && field.key() != "description") {
                return {ApiCallStatus::InvalidArgument, nullptr, "unknown manager action definition field"};
            }
        }
        if (!RegisterManagerAction(manifest.id, local_id, label_field->get<std::string>(),
                                   std::move(description), error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        return {ApiCallStatus::Ok,
                {{"id", manifest.id + ":" + local_id}, {"status", "registered"}}, {}};
    }

    ApiCallResult RemoveUiAction(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a manager action id object"};
        }
        const auto id_field = arguments[0].find("id");
        if (id_field == arguments[0].end() || !id_field->is_string() || arguments[0].size() != 1) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected only an action id"};
        }
        std::string local_id;
        std::string error;
        if (!ParseManagerActionId(id_field->get_ref<const std::string &>(), local_id, error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        if (!RemoveManagerAction(manifest.id, local_id)) {
            return {ApiCallStatus::Failed, nullptr, "action is not registered by this mod"};
        }
        return {ApiCallStatus::Ok,
                {{"id", manifest.id + ":" + local_id}, {"removed", true}}, {}};
    }

    bool ParseQuestId(std::string_view raw, std::string &local_id, std::string &error) const {
        const std::string prefix = manifest.id + ":";
        if (raw.starts_with(prefix)) raw.remove_prefix(prefix.size());
        else if (raw.find(':') != std::string_view::npos) {
            error = "qualified quest ids must belong to this mod";
            return false;
        }
        if (raw.empty() || raw.size() > 64 ||
            !std::ranges::all_of(raw, [](unsigned char character) {
                return (character >= 'a' && character <= 'z') || (character >= 'A' && character <= 'Z') ||
                       (character >= '0' && character <= '9') || character == '.' || character == '_' ||
                       character == '-';
            })) {
            error = "quest id must contain 1-64 ASCII letters, digits, '.', '_' or '-'";
            return false;
        }
        local_id.assign(raw);
        return true;
    }

    static bool HasOnlyFields(const Json &object, std::initializer_list<std::string_view> fields) {
        for (auto field = object.begin(); field != object.end(); ++field) {
            if (std::ranges::find(fields, field.key()) == fields.end()) return false;
        }
        return true;
    }

    Json QuestView(std::string_view local_id, const Json &record) const {
        Json objectives = Json::array();
        const Json &progress = record.at("progress");
        for (const Json &definition : record.at("definition").at("objectives")) {
            const std::string objective_id = definition.at("id").get<std::string>();
            const std::int64_t current = progress.value(objective_id, std::int64_t{0});
            const std::int64_t target = definition.at("target").get<std::int64_t>();
            Json view = {{"id", objective_id}, {"title", definition.at("title")},
                         {"progress", current}, {"target", target}, {"complete", current >= target}};
            if (definition.contains("event")) view["event"] = definition.at("event");
            if (definition.contains("match")) view["match"] = definition.at("match");
            objectives.push_back(std::move(view));
        }
        const Json &definition = record.at("definition");
        Json view = {{"id", manifest.id + ":" + std::string(local_id)},
                     {"title", definition.at("title")}, {"description", definition.value("description", "")},
                     {"status", record.at("status")}, {"objectives", std::move(objectives)}};
        if (definition.contains("rewards")) view["rewards"] = definition.at("rewards");
        return view;
    }

    ApiCallResult CommitQuestStorage(Json replacement) {
        try {
            if (replacement.dump().size() > kStorageSizeLimit) {
                return {ApiCallStatus::InvalidArgument, nullptr, "mod quest data exceeds the 128 KiB save limit"};
            }
        } catch (const std::exception &) {
            return {ApiCallStatus::InvalidArgument, nullptr, "quest data is not valid JSON"};
        }
        Json previous = std::move(storage);
        const bool was_dirty = storage_dirty;
        storage = std::move(replacement);
        storage_dirty = true;
        std::string error;
        if (!FlushStorage(error)) {
            storage = std::move(previous);
            storage_dirty = was_dirty;
            return {ApiCallStatus::Failed, nullptr, std::move(error)};
        }
        return {ApiCallStatus::Ok, nullptr, {}};
    }

    ApiCallResult GetQuests(const Json &arguments) const {
        if (!arguments.is_array() || arguments.size() > 1 ||
            (!arguments.empty() && (!arguments[0].is_object() || arguments[0].size() != 1 ||
                                    !arguments[0].contains("id") || !arguments[0]["id"].is_string()))) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected no arguments or {id = quest_id}"};
        }
        const auto found = storage.find(std::string(kQuestStorageKey));
        if (found == storage.end()) {
            if (!arguments.empty()) return {ApiCallStatus::Failed, nullptr, "quest is not registered"};
            return {ApiCallStatus::Ok, {{"quests", Json::array()}}, {}};
        }
        if (!found->is_object()) return {ApiCallStatus::Failed, nullptr, "stored quest data is invalid"};
        if (!arguments.empty()) {
            std::string local_id;
            std::string error;
            if (!ParseQuestId(arguments[0]["id"].get_ref<const std::string &>(), local_id, error)) {
                return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
            }
            const auto quest = found->find(local_id);
            if (quest == found->end()) return {ApiCallStatus::Failed, nullptr, "quest is not registered"};
            try {
                return {ApiCallStatus::Ok, QuestView(local_id, *quest), {}};
            } catch (const std::exception &) {
                return {ApiCallStatus::Failed, nullptr, "stored quest record is invalid"};
            }
        }
        Json quests = Json::array();
        for (auto quest = found->begin(); quest != found->end(); ++quest) {
            try {
                quests.push_back(QuestView(quest.key(), quest.value()));
            } catch (const std::exception &) {
                return {ApiCallStatus::Failed, nullptr, "stored quest record is invalid"};
            }
        }
        return {ApiCallStatus::Ok, {{"quests", std::move(quests)}}, {}};
    }

    ApiCallResult RegisterQuest(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a quest definition object"};
        }
        const Json &request = arguments[0];
        if (!HasOnlyFields(request, {"id", "title", "description", "objectives", "rewards"}) ||
            !request.contains("id") || !request["id"].is_string() ||
            !request.contains("title") || !request["title"].is_string() ||
            !request.contains("objectives") || !request["objectives"].is_array() ||
            request["objectives"].empty() || request["objectives"].size() > 64) {
            return {ApiCallStatus::InvalidArgument, nullptr,
                    "expected {id, title, description = optional string, objectives = 1-64 entries}"};
        }
        const std::string title = request["title"].get<std::string>();
        std::string description;
        if (request.contains("description")) {
            if (!request["description"].is_string()) {
                return {ApiCallStatus::InvalidArgument, nullptr, "description must be a string"};
            }
            description = request["description"].get<std::string>();
        }
        if (title.empty() || title.size() > 128 || description.size() > 2048) {
            return {ApiCallStatus::InvalidArgument, nullptr, "quest title or description exceeds its text limit"};
        }
        std::string local_id;
        std::string error;
        if (!ParseQuestId(request["id"].get_ref<const std::string &>(), local_id, error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        Json objectives = Json::array();
        Json rewards = Json::array();
        if (request.contains("rewards")) {
            if (!request["rewards"].is_array() || request["rewards"].size() > 8) {
                return {ApiCallStatus::InvalidArgument, nullptr, "rewards must be an array with at most 8 entries"};
            }
            if (!request["rewards"].empty() && !HasPermission(manifest, "game.write")) {
                return {ApiCallStatus::PermissionDenied, nullptr,
                        "manifest permission is required for quest rewards: game.write"};
            }
            for (const Json &reward : request["rewards"]) {
                if (!reward.is_object() || !reward.contains("kind") || !reward["kind"].is_string()) {
                    return {ApiCallStatus::InvalidArgument, nullptr,
                            "each reward must have a supported kind"};
                }
                if (reward["kind"] == "currency") {
                    if (!HasOnlyFields(reward, {"kind", "amount"}) || !reward.contains("amount") ||
                        !reward["amount"].is_number_integer()) {
                        return {ApiCallStatus::InvalidArgument, nullptr,
                                "currency rewards must be {kind = 'currency', amount = integer from 1 to 32767}"};
                    }
                    std::int64_t amount = 0;
                    try {
                        amount = reward["amount"].get<std::int64_t>();
                    } catch (const std::exception &) {
                        return {ApiCallStatus::InvalidArgument, nullptr, "quest reward amount is outside the integer range"};
                    }
                    if (amount < 1 || amount > 32767) {
                        return {ApiCallStatus::InvalidArgument, nullptr,
                                "quest currency reward amount must be in the range [1, 32767]"};
                    }
                    rewards.push_back({{"kind", "currency"}, {"amount", amount}});
                } else if (reward["kind"] == "item") {
                    if (!HasOnlyFields(reward, {"kind", "id", "count"}) || !reward.contains("id") ||
                        !reward["id"].is_string() || reward["id"].get_ref<const std::string &>().empty() ||
                        reward["id"].get_ref<const std::string &>().size() > 128 ||
                        reward["id"].get_ref<const std::string &>().find('\0') != std::string::npos) {
                        return {ApiCallStatus::InvalidArgument, nullptr,
                                "item rewards must have a stable item id of 1-128 bytes"};
                    }
                    std::int64_t count = 1;
                    if (reward.contains("count")) {
                        if (!reward["count"].is_number_integer()) {
                            return {ApiCallStatus::InvalidArgument, nullptr, "item reward count must be an integer from 1 to 999"};
                        }
                        try {
                            count = reward["count"].get<std::int64_t>();
                        } catch (const std::exception &) {
                            return {ApiCallStatus::InvalidArgument, nullptr, "item reward count is outside the integer range"};
                        }
                    }
                    if (count < 1 || count > 999) {
                        return {ApiCallStatus::InvalidArgument, nullptr,
                                "item reward count must be in the range [1, 999]"};
                    }
                    rewards.push_back({{"kind", "item"}, {"id", reward["id"]}, {"count", count}});
                } else if (reward["kind"] == "weapon") {
                    if (!HasOnlyFields(reward, {"kind", "id"}) || !reward.contains("id") ||
                        !reward["id"].is_string() || reward["id"].get_ref<const std::string &>().empty() ||
                        reward["id"].get_ref<const std::string &>().size() > 128 ||
                        reward["id"].get_ref<const std::string &>().find('\0') != std::string::npos) {
                        return {ApiCallStatus::InvalidArgument, nullptr,
                                "weapon rewards must have a stable weapon id of 1-128 bytes"};
                    }
                    rewards.push_back({{"kind", "weapon"}, {"id", reward["id"]}});
                } else {
                    return {ApiCallStatus::InvalidArgument, nullptr,
                            "quest reward kind must be 'currency', 'item', or 'weapon'"};
                }
            }
        }
        Json progress = Json::object();
        std::set<std::string, std::less<>> objective_ids;
        for (const Json &objective : request["objectives"]) {
            if (!objective.is_object() || !HasOnlyFields(objective, {"id", "title", "target", "event", "match"}) ||
                !objective.contains("id") || !objective["id"].is_string() ||
                !objective.contains("title") || !objective["title"].is_string() ||
                !objective.contains("target") || !objective["target"].is_number_integer()) {
                return {ApiCallStatus::InvalidArgument, nullptr,
                        "each objective requires id, title, and positive integer target, with optional event and match fields"};
            }
            const std::string objective_id = objective["id"].get<std::string>();
            const std::string objective_title = objective["title"].get<std::string>();
            if (objective_id.empty() || objective_id.size() > 64 || objective_title.empty() ||
                objective_title.size() > 128 || !objective_ids.insert(objective_id).second) {
                return {ApiCallStatus::InvalidArgument, nullptr, "objective ids and titles must be unique and bounded"};
            }
            std::int64_t target = 0;
            try {
                target = objective["target"].get<std::int64_t>();
            } catch (const std::exception &) {
                return {ApiCallStatus::InvalidArgument, nullptr, "objective target is outside the integer range"};
            }
            if (target <= 0 || target > 1000000000) {
                return {ApiCallStatus::InvalidArgument, nullptr, "objective target must be in the range [1, 1000000000]"};
            }
            Json stored_objective = {{"id", objective_id}, {"title", objective_title}, {"target", target}};
            if (objective.contains("event")) {
                if (!objective["event"].is_string()) {
                    return {ApiCallStatus::InvalidArgument, nullptr, "objective event must be a game event name"};
                }
                const std::string event_name = objective["event"].get<std::string>();
                if (!ValidEventName(event_name) || !std::string_view(event_name).starts_with("game.")) {
                    return {ApiCallStatus::InvalidArgument, nullptr,
                            "objective event must use the game.* namespace and a valid event name"};
                }
                stored_objective["event"] = event_name;
                if (objective.contains("match")) {
                    const Json &match = objective["match"];
                    if (!match.is_object() || match.size() > 16) {
                        return {ApiCallStatus::InvalidArgument, nullptr,
                                "objective match must be an object with at most 16 scalar fields"};
                    }
                    for (auto field = match.begin(); field != match.end(); ++field) {
                        if (field.key().empty() || field.key().size() > 64 ||
                            !(field->is_string() || field->is_boolean() || field->is_number_integer())) {
                            return {ApiCallStatus::InvalidArgument, nullptr,
                                    "objective match fields must have bounded names and string, boolean, or integer values"};
                        }
                        if (field->is_string() && field->get_ref<const std::string &>().size() > 256) {
                            return {ApiCallStatus::InvalidArgument, nullptr,
                                    "objective match strings are limited to 256 bytes"};
                        }
                    }
                    stored_objective["match"] = match;
                }
            } else if (objective.contains("match")) {
                return {ApiCallStatus::InvalidArgument, nullptr, "objective match requires an event field"};
            }
            objectives.push_back(std::move(stored_objective));
            progress[objective_id] = 0;
        }
        Json definition = {{"title", title}, {"description", description}, {"objectives", objectives}};
        if (request.contains("rewards")) definition["rewards"] = rewards;
        Json replacement = storage;
        if (!replacement.contains(std::string(kQuestStorageKey))) {
            replacement[std::string(kQuestStorageKey)] = Json::object();
        }
        Json &quests = replacement[std::string(kQuestStorageKey)];
        if (!quests.is_object()) {
            return {ApiCallStatus::Failed, nullptr, "stored quest data is invalid"};
        }
        const auto prior = quests.find(local_id);
        if (prior != quests.end()) {
            if (prior->is_object() && prior->value("definition", Json()) == definition) {
                return {ApiCallStatus::Ok,
                        {{"id", manifest.id + ":" + local_id}, {"status", "registered"}, {"idempotent", true}}, {}};
            }
            return {ApiCallStatus::Failed, nullptr, "quest id is already registered with a different definition"};
        }
        if (quests.size() >= 128) return {ApiCallStatus::InvalidArgument, nullptr, "mod quest limit reached"};
        quests[local_id] = {{"definition", definition}, {"status", "registered"}, {"progress", progress}};
        const ApiCallResult committed = CommitQuestStorage(std::move(replacement));
        if (committed.status != ApiCallStatus::Ok) return committed;
        return {ApiCallStatus::Ok,
                {{"id", manifest.id + ":" + local_id}, {"status", "registered"}, {"idempotent", false}}, {}};
    }

    ApiCallResult StartQuest(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object() ||
            arguments[0].size() != 1 || !arguments[0].contains("id") || !arguments[0]["id"].is_string()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected {id = quest_id}"};
        }
        std::string local_id;
        std::string error;
        if (!ParseQuestId(arguments[0]["id"].get_ref<const std::string &>(), local_id, error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        Json replacement = storage;
        auto quests = replacement.find(std::string(kQuestStorageKey));
        if (quests == replacement.end() || !quests->is_object()) {
            return {ApiCallStatus::Failed, nullptr, "quest is not registered"};
        }
        auto quest = quests->find(local_id);
        if (quest == quests->end() || !quest->is_object()) return {ApiCallStatus::Failed, nullptr, "quest is not registered"};
        const std::string status = quest->value("status", "");
        if (status == "completed") return {ApiCallStatus::Failed, nullptr, "completed quest cannot be started again"};
        if (status == "active") {
            return {ApiCallStatus::Ok, {{"id", manifest.id + ":" + local_id}, {"status", "active"}, {"idempotent", true}}, {}};
        }
        quest->at("status") = "active";
        const ApiCallResult committed = CommitQuestStorage(std::move(replacement));
        if (committed.status != ApiCallStatus::Ok) return committed;
        const Json event = {{"id", manifest.id + ":" + local_id}, {"status", "active"}};
        Emit("quests.started", event);
        return {ApiCallStatus::Ok,
                {{"id", manifest.id + ":" + local_id}, {"status", "active"}, {"idempotent", false}}, {}};
    }

    ApiCallResult AdvanceQuest(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object() ||
            !HasOnlyFields(arguments[0], {"id", "objective", "amount"}) ||
            !arguments[0].contains("id") || !arguments[0]["id"].is_string() ||
            !arguments[0].contains("objective") || !arguments[0]["objective"].is_string() ||
            (arguments[0].contains("amount") && !arguments[0]["amount"].is_number_integer())) {
            return {ApiCallStatus::InvalidArgument, nullptr,
                    "expected {id = quest_id, objective = objective_id, amount = optional positive integer}"};
        }
        std::string local_id;
        std::string error;
        if (!ParseQuestId(arguments[0]["id"].get_ref<const std::string &>(), local_id, error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        const std::string objective_id = arguments[0]["objective"].get<std::string>();
        if (objective_id.empty() || objective_id.size() > 64) {
            return {ApiCallStatus::InvalidArgument, nullptr, "objective id must contain 1-64 bytes"};
        }
        std::int64_t amount = 1;
        if (arguments[0].contains("amount")) {
            try {
                amount = arguments[0]["amount"].get<std::int64_t>();
            } catch (const std::exception &) {
                return {ApiCallStatus::InvalidArgument, nullptr, "amount is outside the integer range"};
            }
        }
        if (amount <= 0 || amount > 1000000000) {
            return {ApiCallStatus::InvalidArgument, nullptr, "amount must be in the range [1, 1000000000]"};
        }
        return AdvanceQuestProgress(local_id, objective_id, amount);
    }

    ApiCallResult AdvanceQuestProgress(const std::string &local_id, const std::string &objective_id,
                                       std::int64_t amount) {
        Json replacement = storage;
        auto quests = replacement.find(std::string(kQuestStorageKey));
        if (quests == replacement.end() || !quests->is_object()) return {ApiCallStatus::Failed, nullptr, "quest is not registered"};
        auto quest = quests->find(local_id);
        if (quest == quests->end() || !quest->is_object()) return {ApiCallStatus::Failed, nullptr, "quest is not registered"};
        if (quest->value("status", "") != "active") return {ApiCallStatus::Failed, nullptr, "quest is not active"};
        const Json &definitions = quest->at("definition").at("objectives");
        const auto definition = std::ranges::find_if(definitions, [&objective_id](const Json &entry) {
            return entry.value("id", "") == objective_id;
        });
        if (definition == definitions.end()) return {ApiCallStatus::Failed, nullptr, "objective is not registered"};
        Json &progress = quest->at("progress");
        const std::int64_t target = definition->at("target").get<std::int64_t>();
        const std::int64_t previous = progress.value(objective_id, std::int64_t{0});
        const std::int64_t current = previous >= target ? target : previous + std::min(amount, target - previous);
        if (current == previous) {
            return {ApiCallStatus::Ok,
                    {{"id", manifest.id + ":" + local_id}, {"objective", objective_id},
                     {"progress", current}, {"target", target}, {"advanced", 0}, {"status", "active"}}, {}};
        }
        progress[objective_id] = current;
        bool complete = true;
        for (const Json &entry : definitions) {
            if (progress.value(entry.at("id").get<std::string>(), std::int64_t{0}) < entry.at("target").get<std::int64_t>()) {
                complete = false;
                break;
            }
        }
        if (complete) quest->at("status") = "completed";
        Json reward_definitions = Json::array();
        if (complete) {
            const auto configured_rewards = quest->at("definition").find("rewards");
            if (configured_rewards != quest->at("definition").end() && configured_rewards->is_array()) {
                reward_definitions = *configured_rewards;
            }
        }
        const ApiCallResult committed = CommitQuestStorage(std::move(replacement));
        if (committed.status != ApiCallStatus::Ok) return committed;
        const std::string status = complete ? "completed" : "active";
        const Json event = {{"id", manifest.id + ":" + local_id}, {"objective", objective_id},
                            {"progress", current}, {"target", target}, {"status", status}};
        Emit("quests.objective_advanced", event);
        if (complete) {
            Json completed_event = {{"id", manifest.id + ":" + local_id}};
            if (!reward_definitions.empty()) {
                Json reward_results = Json::array();
                for (const Json &reward : reward_definitions) {
                    const std::string kind = reward.value("kind", "");
                    const std::int64_t requested = kind == "currency"
                                                       ? reward.value("amount", std::int64_t{0})
                                                       : kind == "item" ? reward.value("count", std::int64_t{1}) : 1;
                    const std::string operation = kind == "currency" ? "economy.add"
                                                  : kind == "item" ? "items.give" : "weapons.give";
                    const Json arguments = kind == "currency"
                                               ? Json::array({Json{{"amount", requested}}})
                                               : kind == "item"
                                                     ? Json::array({Json{{"id", reward.value("id", std::string())},
                                                                         {"count", requested}}})
                                                     : Json::array({Json{{"id", reward.value("id", std::string())}}});
                    const ApiCallResult granted = InvokeApi(operation, arguments);
                    if (granted.status == ApiCallStatus::Ok) {
                        std::int64_t added = requested;
                        if (granted.value.is_object()) {
                            const auto added_value = granted.value.find("added");
                            if (added_value != granted.value.end()) {
                                if (added_value->is_boolean()) added = added_value->get<bool>() ? 1 : 0;
                                else if (added_value->is_number_integer()) {
                                    try {
                                        added = added_value->get<std::int64_t>();
                                    } catch (const std::exception &) {
                                        added = 0;
                                    }
                                } else {
                                    added = 0;
                                }
                            }
                        }
                        const std::string status = added >= requested ? "granted" : added > 0 ? "partial" : "failed";
                        Json result = {{"kind", kind}, {"status", status}, {"requested", requested},
                                       {"result", granted.value}};
                        if (kind == "item" || kind == "weapon") result["id"] = reward.value("id", std::string());
                        reward_results.push_back(std::move(result));
                        if (status != "granted") {
                            Log("error", "quest " + kind + " reward was " + status + " for '" +
                                             manifest.id + ":" + local_id + "'");
                        }
                    } else {
                        Json result = {{"kind", kind}, {"status", "failed"},
                                       {"requested", requested}, {"message", granted.message}};
                        if (kind == "item" || kind == "weapon") result["id"] = reward.value("id", std::string());
                        reward_results.push_back(std::move(result));
                        Log("error", "quest " + kind + " reward failed for '" + manifest.id + ":" + local_id +
                                         "': " + granted.message);
                    }
                }
                completed_event["rewards"] = std::move(reward_results);
            }
            Emit("quests.completed", completed_event);
        }
        return {ApiCallStatus::Ok,
                {{"id", manifest.id + ":" + local_id}, {"objective", objective_id},
                 {"progress", current}, {"target", target}, {"advanced", current - previous},
                 {"status", status}}, {}};
    }

    bool QuestObjectiveMatchesEvent(const Json &objective, std::string_view event_name, const Json &payload) const {
        if (!objective.is_object()) return false;
        const auto event = objective.find("event");
        if (event == objective.end() || !event->is_string() || event->get_ref<const std::string &>() != event_name) {
            return false;
        }
        const auto match = objective.find("match");
        if (match == objective.end()) return true;
        if (!match->is_object() || !payload.is_object()) return false;
        for (auto field = match->begin(); field != match->end(); ++field) {
            const auto value = payload.find(field.key());
            if (value == payload.end() || *value != *field) return false;
        }
        return true;
    }

    void AdvanceQuestsFromEvent(std::string_view event_name, const Json &payload) {
        const auto quests = storage.find(std::string(kQuestStorageKey));
        if (quests == storage.end() || !quests->is_object()) return;
        std::vector<std::pair<std::string, std::string>> matches;
        for (auto quest = quests->begin(); quest != quests->end(); ++quest) {
            if (!quest->is_object() || quest->value("status", "") != "active") continue;
            const auto definition = quest->find("definition");
            if (definition == quest->end() || !definition->is_object()) continue;
            const auto objectives = definition->find("objectives");
            if (objectives == definition->end() || !objectives->is_array()) continue;
            for (const Json &objective : *objectives) {
                if (!QuestObjectiveMatchesEvent(objective, event_name, payload)) continue;
                const auto id = objective.find("id");
                if (id != objective.end() && id->is_string()) {
                    matches.emplace_back(quest.key(), id->get<std::string>());
                }
            }
        }
        for (const auto &[quest_id, objective_id] : matches) {
            const ApiCallResult advanced = AdvanceQuestProgress(quest_id, objective_id, 1);
            if (advanced.status != ApiCallStatus::Ok && advanced.message != "quest is not active") {
                Log("error", "event-driven quest progress failed for '" + quest_id + "." + objective_id + "': " +
                                 advanced.message);
            }
        }
    }

    bool ParseSkillTreeId(std::string_view raw, std::string &local_id, std::string &error) const {
        if (!ParseQuestId(raw, local_id, error)) return false;
        if (local_id.size() > 32) {
            error = "skill tree id is limited to 32 ASCII letters, digits, '.', '_' or '-'";
            return false;
        }
        return true;
    }

    bool ParseSkillId(std::string_view raw, std::string &local_id, std::string &error) const {
        if (!ParseQuestId(raw, local_id, error)) return false;
        if (local_id.size() > 64 || local_id.find('.') == std::string::npos) {
            error = "skill id must use the form tree_id.skill_id and fit within 64 bytes";
            return false;
        }
        return true;
    }

    ApiCallResult CommitSkillStorage(Json replacement) {
        try {
            if (replacement.dump().size() > kStorageSizeLimit) {
                return {ApiCallStatus::InvalidArgument, nullptr, "mod skill data exceeds the 128 KiB save limit"};
            }
        } catch (const std::exception &) {
            return {ApiCallStatus::InvalidArgument, nullptr, "skill data is not valid JSON"};
        }
        Json previous = std::move(storage);
        const bool was_dirty = storage_dirty;
        storage = std::move(replacement);
        storage_dirty = true;
        std::string error;
        if (!FlushStorage(error)) {
            storage = std::move(previous);
            storage_dirty = was_dirty;
            return {ApiCallStatus::Failed, nullptr, std::move(error)};
        }
        return {ApiCallStatus::Ok, nullptr, {}};
    }

    ApiCallResult RegisterSkillTree(const Json &arguments) {
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected a skill tree definition object"};
        }
        const Json &request = arguments[0];
        if (!HasOnlyFields(request, {"id", "title", "description", "skills"}) ||
            !request.contains("id") || !request["id"].is_string() ||
            !request.contains("title") || !request["title"].is_string() ||
            !request.contains("skills") || !request["skills"].is_array() ||
            request["skills"].empty() || request["skills"].size() > 128) {
            return {ApiCallStatus::InvalidArgument, nullptr,
                    "expected {id, title, description = optional string, skills = 1-128 nodes}"};
        }
        std::string tree_id;
        std::string error;
        if (!ParseSkillTreeId(request["id"].get_ref<const std::string &>(), tree_id, error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        const std::string title = request["title"].get<std::string>();
        std::string description;
        if (request.contains("description")) {
            if (!request["description"].is_string()) {
                return {ApiCallStatus::InvalidArgument, nullptr, "skill tree description must be a string"};
            }
            description = request["description"].get<std::string>();
        }
        if (title.empty() || title.size() > 128 || description.size() > 2048) {
            return {ApiCallStatus::InvalidArgument, nullptr, "skill tree title or description exceeds its text limit"};
        }

        Json skills = Json::array();
        std::set<std::string, std::less<>> skill_ids;
        std::map<std::string, Json, std::less<>> skill_definitions;
        for (const Json &skill : request["skills"]) {
            if (!skill.is_object() || !HasOnlyFields(skill, {"id", "title", "description", "max_rank", "prerequisites", "effects"}) ||
                !skill.contains("id") || !skill["id"].is_string() ||
                !skill.contains("title") || !skill["title"].is_string() ||
                !skill.contains("max_rank") || !skill["max_rank"].is_number_integer() ||
                (skill.contains("description") && !skill["description"].is_string()) ||
                (skill.contains("prerequisites") && !skill["prerequisites"].is_array()) ||
                (skill.contains("effects") && (!skill["effects"].is_object() ||
                 !HasOnlyFields(skill["effects"], {"enemy_damage_per_rank"})))) {
                return {ApiCallStatus::InvalidArgument, nullptr,
                        "each skill requires id, title, max_rank, and optional description, prerequisites, or supported effects"};
            }
            std::string node_id;
            if (!ParseQuestId(skill["id"].get_ref<const std::string &>(), node_id, error) || node_id.size() > 24) {
                return {ApiCallStatus::InvalidArgument, nullptr,
                        error.empty() ? "skill node ids are limited to 24 bytes" : std::move(error)};
            }
            const std::string node_title = skill["title"].get<std::string>();
            const std::string node_description = skill.value("description", std::string{});
            if (node_title.empty() || node_title.size() > 128 || node_description.size() > 1024 ||
                !skill_ids.insert(node_id).second) {
                return {ApiCallStatus::InvalidArgument, nullptr,
                        "skill ids must be unique; titles and descriptions must fit their text limits"};
            }
            std::int64_t max_rank = 0;
            try {
                max_rank = skill["max_rank"].get<std::int64_t>();
            } catch (const std::exception &) {
                return {ApiCallStatus::InvalidArgument, nullptr, "skill max_rank is outside the integer range"};
            }
            if (max_rank < 1 || max_rank > 99) {
                return {ApiCallStatus::InvalidArgument, nullptr, "skill max_rank must be in the range [1, 99]"};
            }
            Json prerequisites = Json::array();
            const Json requested_prerequisites = skill.value("prerequisites", Json::array());
            if (requested_prerequisites.size() > 32) {
                return {ApiCallStatus::InvalidArgument, nullptr, "a skill may have at most 32 prerequisites"};
            }
            std::set<std::string, std::less<>> prerequisite_ids;
            for (const Json &prerequisite : requested_prerequisites) {
                if (!prerequisite.is_object() || !HasOnlyFields(prerequisite, {"id", "rank"}) ||
                    !prerequisite.contains("id") || !prerequisite["id"].is_string() ||
                    !prerequisite.contains("rank") || !prerequisite["rank"].is_number_integer()) {
                    return {ApiCallStatus::InvalidArgument, nullptr,
                            "each prerequisite requires a local skill id and integer rank"};
                }
                std::string prerequisite_id;
                if (!ParseQuestId(prerequisite["id"].get_ref<const std::string &>(), prerequisite_id, error) ||
                    prerequisite_id.size() > 24 || prerequisite_id == node_id ||
                    !prerequisite_ids.insert(prerequisite_id).second) {
                    return {ApiCallStatus::InvalidArgument, nullptr,
                            error.empty() ? "prerequisite ids must be unique local skill ids" : std::move(error)};
                }
                std::int64_t required_rank = 0;
                try {
                    required_rank = prerequisite["rank"].get<std::int64_t>();
                } catch (const std::exception &) {
                    return {ApiCallStatus::InvalidArgument, nullptr, "prerequisite rank is outside the integer range"};
                }
                if (required_rank < 1 || required_rank > 99) {
                    return {ApiCallStatus::InvalidArgument, nullptr, "prerequisite rank must be in the range [1, 99]"};
                }
                prerequisites.push_back({{"id", prerequisite_id}, {"rank", required_rank}});
            }
            Json normalized = {{"id", node_id}, {"title", node_title}, {"description", node_description},
                               {"max_rank", max_rank}, {"prerequisites", std::move(prerequisites)}};
            if (skill.contains("effects")) {
                const Json &effects = skill["effects"];
                if (effects.contains("enemy_damage_per_rank")) {
                    if (!effects["enemy_damage_per_rank"].is_number()) {
                        return {ApiCallStatus::InvalidArgument, nullptr,
                                "effects.enemy_damage_per_rank must be a finite number in [0, 1]"};
                    }
                    double per_rank = 0.0;
                    try {
                        per_rank = effects["enemy_damage_per_rank"].get<double>();
                    } catch (const std::exception &) {
                        return {ApiCallStatus::InvalidArgument, nullptr,
                                "effects.enemy_damage_per_rank is outside the numeric range"};
                    }
                    if (!std::isfinite(per_rank) || per_rank < 0.0 || per_rank > 1.0) {
                        return {ApiCallStatus::InvalidArgument, nullptr,
                                "effects.enemy_damage_per_rank must be a finite number in [0, 1]"};
                    }
                }
                normalized["effects"] = effects;
            }
            skill_definitions.emplace(node_id, normalized);
            skills.push_back(std::move(normalized));
        }

        for (const Json &skill : skills) {
            for (const Json &prerequisite : skill["prerequisites"]) {
                const auto found = skill_definitions.find(prerequisite["id"].get_ref<const std::string &>());
                if (found == skill_definitions.end() || prerequisite["rank"] > found->second["max_rank"]) {
                    return {ApiCallStatus::InvalidArgument, nullptr,
                            "prerequisites must name a skill in this tree and not exceed its max rank"};
                }
            }
        }
        std::set<std::string, std::less<>> visited;
        std::set<std::string, std::less<>> active;
        std::function<bool(const std::string &)> has_cycle = [&](const std::string &node_id) {
            if (active.contains(node_id)) return true;
            if (visited.contains(node_id)) return false;
            active.insert(node_id);
            for (const Json &prerequisite : skill_definitions.at(node_id)["prerequisites"]) {
                if (has_cycle(prerequisite["id"].get<std::string>())) return true;
            }
            active.erase(node_id);
            visited.insert(node_id);
            return false;
        };
        for (const std::string &node_id : skill_ids) {
            if (has_cycle(node_id)) {
                return {ApiCallStatus::InvalidArgument, nullptr, "skill prerequisites cannot contain a cycle"};
            }
        }

        const Json definition = {{"title", title}, {"description", description}, {"skills", std::move(skills)}};
        Json replacement = storage;
        if (!replacement.contains(std::string(kSkillStorageKey))) replacement[std::string(kSkillStorageKey)] = Json::object();
        Json &trees = replacement[std::string(kSkillStorageKey)];
        if (!trees.is_object()) return {ApiCallStatus::Failed, nullptr, "stored skill data is invalid"};
        const auto prior = trees.find(tree_id);
        if (prior != trees.end()) {
            if (prior->is_object() && prior->value("definition", Json()) == definition) {
                Json node_ids = Json::array();
                for (const Json &skill : definition["skills"]) {
                    node_ids.push_back(manifest.id + ":" + tree_id + "." + skill["id"].get<std::string>());
                }
                return {ApiCallStatus::Ok,
                        {{"id", manifest.id + ":" + tree_id}, {"skills", std::move(node_ids)},
                         {"status", "registered"}, {"idempotent", true}}, {}};
            }
            const Json old = prior->value("definition", Json::object());
            const Json old_nodes = old.value("skills", Json::array());
            const Json &new_nodes = definition.at("skills");
            // Mods may extend a registered tree without invalidating ranks already saved for
            // existing nodes. Removing/reordering nodes or changing their rank/prerequisite
            // contract remains a breaking update.
            bool compatible = old_nodes.is_array() && old_nodes.size() <= new_nodes.size();
            for (std::size_t i = 0; compatible && i < old_nodes.size(); ++i) {
                compatible = old_nodes[i].value("id", "") == new_nodes[i].value("id", "") &&
                             old_nodes[i].value("max_rank", 0) == new_nodes[i].value("max_rank", 0) &&
                             old_nodes[i].value("prerequisites", Json::array()) == new_nodes[i].value("prerequisites", Json::array());
            }
            if (!compatible) return {ApiCallStatus::Failed, nullptr, "skill tree update changes its rank or prerequisite structure"};
            Json &ranks = (*prior)["ranks"];
            if (!ranks.is_object()) return {ApiCallStatus::Failed, nullptr, "stored skill ranks are invalid"};
            for (const Json &node : new_nodes) {
                const std::string node_id = node.value("id", "");
                if (!ranks.contains(node_id)) ranks[node_id] = 0;
            }
            (*prior)["definition"] = definition;
            const ApiCallResult committed = CommitSkillStorage(std::move(replacement));
            if (committed.status != ApiCallStatus::Ok) return committed;
            return {ApiCallStatus::Ok, {{"id", manifest.id + ":" + tree_id}, {"status", "updated"}}, {}};
        }
        if (trees.size() >= 32) return {ApiCallStatus::InvalidArgument, nullptr, "mod skill tree limit reached"};
        Json ranks = Json::object();
        Json node_ids = Json::array();
        for (const Json &skill : definition["skills"]) {
            ranks[skill["id"].get<std::string>()] = 0;
            node_ids.push_back(manifest.id + ":" + tree_id + "." + skill["id"].get<std::string>());
        }
        trees[tree_id] = {{"definition", definition}, {"ranks", std::move(ranks)}};
        const ApiCallResult committed = CommitSkillStorage(std::move(replacement));
        if (committed.status != ApiCallStatus::Ok) return committed;
        return {ApiCallStatus::Ok,
                {{"id", manifest.id + ":" + tree_id}, {"skills", std::move(node_ids)},
                 {"status", "registered"}, {"idempotent", false}}, {}};
    }

    ApiCallResult GetSkills(const Json &arguments) const {
        if (!arguments.is_array() || arguments.size() > 1 ||
            (!arguments.empty() && (!arguments[0].is_object() || arguments[0].size() != 1 ||
                                    !arguments[0].contains("id") || !arguments[0]["id"].is_string()))) {
            return {ApiCallStatus::InvalidArgument, nullptr, "expected no arguments or {id = skill_id}"};
        }
        const auto tree_store = storage.find(std::string(kSkillStorageKey));
        if (tree_store == storage.end()) {
            if (!arguments.empty()) return {ApiCallStatus::Failed, nullptr, "skill is not registered"};
            return {ApiCallStatus::Ok, {{"skills", Json::array()}}, {}};
        }
        if (!tree_store->is_object()) return {ApiCallStatus::Failed, nullptr, "stored skill data is invalid"};
        try {
        auto build_view = [&](const std::string &tree_id, const std::string &node_id, const Json &node,
                              const Json &definition, const Json &ranks) -> Json {
            const auto rank_field = ranks.find(node_id);
            if (rank_field == ranks.end() || !rank_field->is_number_integer()) {
                throw std::runtime_error("skill rank is invalid");
            }
            const std::int64_t current_rank = rank_field->get<std::int64_t>();
            bool prerequisites_met = true;
            Json prerequisites = Json::array();
            for (const Json &requirement : node.at("prerequisites")) {
                const std::string required_id = requirement.at("id").get<std::string>();
                const std::int64_t required_rank = requirement.at("rank").get<std::int64_t>();
                const auto actual = ranks.find(required_id);
                const std::int64_t actual_rank = actual != ranks.end() && actual->is_number_integer()
                                                     ? actual->get<std::int64_t>() : 0;
                const bool met = actual_rank >= required_rank;
                prerequisites_met = prerequisites_met && met;
                prerequisites.push_back({{"id", manifest.id + ":" + tree_id + "." + required_id},
                                          {"required_rank", required_rank}, {"rank", actual_rank},
                                          {"met", met}});
            }
            const std::int64_t max_rank = node.at("max_rank").get<std::int64_t>();
            return {{"id", manifest.id + ":" + tree_id + "." + node_id},
                    {"tree_id", manifest.id + ":" + tree_id}, {"tree_title", definition.at("title")},
                    {"id_local", node_id}, {"title", node.at("title")},
                    {"description", node.value("description", "")}, {"rank", current_rank},
                    {"effects", node.value("effects", Json::object())},
                    {"max_rank", max_rank}, {"unlocked", current_rank > 0},
                    {"prerequisites_met", prerequisites_met},
                    {"available", current_rank < max_rank && prerequisites_met},
                    {"prerequisites", std::move(prerequisites)}};
        };
        if (!arguments.empty()) {
            std::string skill_id;
            std::string error;
            if (!ParseSkillId(arguments[0]["id"].get_ref<const std::string &>(), skill_id, error)) {
                return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
            }
            for (auto tree = tree_store->begin(); tree != tree_store->end(); ++tree) {
                const std::string prefix = tree.key() + ".";
                if (!skill_id.starts_with(prefix)) continue;
                const std::string node_id = skill_id.substr(prefix.size());
                if (!tree.value().is_object() || !tree.value().contains("definition") || !tree.value().contains("ranks")) {
                    return {ApiCallStatus::Failed, nullptr, "stored skill tree is invalid"};
                }
                const Json &definition = tree.value().at("definition");
                const Json &ranks = tree.value().at("ranks");
                const auto node = std::ranges::find(definition.at("skills"), node_id,
                                                    [](const Json &skill) -> const std::string & {
                                                        return skill.at("id").get_ref<const std::string &>();
                                                    });
                if (node == definition.at("skills").end()) continue;
                try {
                    return {ApiCallStatus::Ok, build_view(tree.key(), node_id, *node, definition, ranks), {}};
                } catch (const std::exception &) {
                    return {ApiCallStatus::Failed, nullptr, "stored skill record is invalid"};
                }
            }
            return {ApiCallStatus::Failed, nullptr, "skill is not registered"};
        }
        Json result = Json::array();
        for (auto tree = tree_store->begin(); tree != tree_store->end(); ++tree) {
            if (!tree.value().is_object() || !tree.value().contains("definition") || !tree.value().contains("ranks")) {
                return {ApiCallStatus::Failed, nullptr, "stored skill tree is invalid"};
            }
            const Json &definition = tree.value().at("definition");
            const Json &ranks = tree.value().at("ranks");
            for (const Json &node : definition.at("skills")) {
                try {
                    result.push_back(build_view(tree.key(), node.at("id").get<std::string>(), node, definition, ranks));
                } catch (const std::exception &) {
                    return {ApiCallStatus::Failed, nullptr, "stored skill record is invalid"};
                }
            }
        }
        return {ApiCallStatus::Ok, {{"skills", std::move(result)}}, {}};
        } catch (const std::exception &) {
            return {ApiCallStatus::Failed, nullptr, "stored skill data is invalid"};
        }
    }

    ApiCallResult SetSkillRank(const Json &arguments, bool increment) {
        const bool fields_valid = arguments.is_array() && arguments.size() == 1 && arguments[0].is_object() &&
                                  (increment ? HasOnlyFields(arguments[0], {"id"})
                                             : HasOnlyFields(arguments[0], {"id", "rank"}));
        if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object() ||
            !fields_valid ||
            !arguments[0].contains("id") || !arguments[0]["id"].is_string() ||
            (!increment && (!arguments[0].contains("rank") || !arguments[0]["rank"].is_number_integer()))) {
            return {ApiCallStatus::InvalidArgument, nullptr,
                    increment ? "expected {id = skill_id}" : "expected {id = skill_id, rank = integer}"};
        }
        std::string skill_id;
        std::string error;
        if (!ParseSkillId(arguments[0]["id"].get_ref<const std::string &>(), skill_id, error)) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::move(error)};
        }
        try {
        auto tree_store = storage.find(std::string(kSkillStorageKey));
        if (tree_store == storage.end() || !tree_store->is_object()) {
            return {ApiCallStatus::Failed, nullptr, "skill is not registered"};
        }
        std::string selected_tree;
        std::string selected_node;
        for (auto tree = tree_store->begin(); tree != tree_store->end(); ++tree) {
            const std::string prefix = tree.key() + ".";
            if (!skill_id.starts_with(prefix)) continue;
            const std::string node_id = skill_id.substr(prefix.size());
            if (!tree.value().is_object() || !tree.value().contains("definition") || !tree.value().contains("ranks")) {
                return {ApiCallStatus::Failed, nullptr, "stored skill tree is invalid"};
            }
            const Json &nodes = tree.value().at("definition").at("skills");
            if (std::ranges::any_of(nodes, [&](const Json &node) { return node.value("id", "") == node_id; })) {
                selected_tree = tree.key();
                selected_node = node_id;
                break;
            }
        }
        if (selected_tree.empty()) return {ApiCallStatus::Failed, nullptr, "skill is not registered"};
        Json replacement = storage;
        Json &tree = replacement[std::string(kSkillStorageKey)][selected_tree];
        Json &ranks = tree["ranks"];
        const Json &definition = tree.at("definition");
        const auto node = std::ranges::find(definition.at("skills"), selected_node,
                                            [](const Json &skill) -> const std::string & {
                                                return skill.at("id").get_ref<const std::string &>();
                                            });
        if (node == definition.at("skills").end()) return {ApiCallStatus::Failed, nullptr, "skill is not registered"};
        const std::int64_t old_rank = ranks.value(selected_node, std::int64_t{0});
        std::int64_t requested_rank = 0;
        if (increment) {
            if (old_rank >= node->at("max_rank").get<std::int64_t>()) {
                return {ApiCallStatus::Failed, nullptr, "skill is already at maximum rank"};
            }
            requested_rank = old_rank + 1;
        } else {
            try {
                requested_rank = arguments[0]["rank"].get<std::int64_t>();
            } catch (const std::exception &) {
                return {ApiCallStatus::InvalidArgument, nullptr, "skill rank is outside the integer range"};
            }
            if (requested_rank < 0 || requested_rank > node->at("max_rank").get<std::int64_t>()) {
                return {ApiCallStatus::InvalidArgument, nullptr, "skill rank is outside the registered range"};
            }
        }
        if (requested_rank == old_rank) {
            return {ApiCallStatus::Ok,
                    {{"id", manifest.id + ":" + skill_id}, {"rank", old_rank}, {"changed", false},
                     {"idempotent", true}}, {}};
        }
        if (requested_rank > 0) {
            for (const Json &prerequisite : node->at("prerequisites")) {
                const std::string required_id = prerequisite.at("id").get<std::string>();
                const std::int64_t required_rank = prerequisite.at("rank").get<std::int64_t>();
                if (ranks.value(required_id, std::int64_t{0}) < required_rank) {
                    return {ApiCallStatus::Failed, nullptr,
                            "skill prerequisite is not met: " + required_id + " rank " + std::to_string(required_rank)};
                }
            }
        }
        ranks[selected_node] = requested_rank;
        const ApiCallResult committed = CommitSkillStorage(std::move(replacement));
        if (committed.status != ApiCallStatus::Ok) return committed;
        const std::string qualified_id = manifest.id + ":" + skill_id;
        Emit("skills.rank_changed", {{"id", qualified_id}, {"previous_rank", old_rank},
                                     {"rank", requested_rank}});
        if (old_rank == 0 && requested_rank > 0) Emit("skills.unlocked", {{"id", qualified_id}, {"rank", requested_rank}});
        return {ApiCallStatus::Ok,
                {{"id", qualified_id}, {"rank", requested_rank}, {"changed", true}, {"idempotent", false}}, {}};
        } catch (const std::exception &) {
            return {ApiCallStatus::Failed, nullptr, "stored skill data is invalid"};
        }
    }

    ApiCallResult DebugProfile(const Json &arguments) const {
        if (!arguments.is_array() || !arguments.empty()) {
            return {ApiCallStatus::InvalidArgument, nullptr, "debug.profile expects no arguments"};
        }

        std::size_t subscription_count = 0;
        for (const auto &[event_name, subscriptions] : callbacks) {
            (void)event_name;
            subscription_count += subscriptions.size();
        }
        std::size_t translation_count = 0;
        for (const auto &[localization_id, entry] : localizations) {
            (void)localization_id;
            translation_count += entry.translations.size();
        }

        return {ApiCallStatus::Ok,
                {{"lua_memory", {{"used_bytes", memory.used}, {"limit_bytes", memory.limit}}},
                 {"events", {{"event_names", callbacks.size()}, {"subscriptions", subscription_count}}},
                 {"localization", {{"keys", localizations.size()}, {"translations", translation_count},
                                   {"bytes", localization_bytes}}},
                 {"resources", {{"handles", resources.size()}, {"bytes", resource_bytes},
                                {"handle_limit", kResourceHandleLimit}, {"byte_limit", kResourceTotalSizeLimit}}},
                 {"storage", {{"keys", storage.size()}, {"bytes", storage.dump().size()},
                              {"limit_bytes", kStorageSizeLimit}, {"dirty", storage_dirty}}}},
                {}};
    }


    // mods.*: what this session loaded, and other mods' settings (co-op's required mods and the host's values).
    ApiCallResult ModsApi(std::string_view path, const Json &arguments) {
        const Json none = Json::object();
        if (!arguments.is_array() || arguments.size() > 1 || (arguments.size() == 1 && !arguments[0].is_object())) {
            return {ApiCallStatus::InvalidArgument, nullptr, std::string(path) + " takes one table or nothing"};
        }
        const Json &args = arguments.empty() ? none : arguments[0];
        if (!config.mods_handler) return {ApiCallStatus::Unsupported, nullptr, "this runtime lists no mods"};
        if (path == "mods.list") return config.mods_handler(path, {}, none);
        const auto id_field = args.find("id");
        const bool has_id = id_field != args.end();
        if (has_id && (!id_field->is_string() || !IsValidModId(id_field->get_ref<const std::string &>()))) {
            return {ApiCallStatus::InvalidArgument, nullptr, "id must be a mod id"};
        }
        const std::string id = has_id ? id_field->get<std::string>() : std::string();
        if (path == "mods.settings_get") {
            const std::string target = id.empty() ? manifest.id : id;
            if (target != manifest.id && !HasPermission(manifest, "mods.settings")) {
                return {ApiCallStatus::PermissionDenied, nullptr, "manifest permission is required: mods.settings"};
            }
            return config.mods_handler(path, target, none);
        }
        if (path == "mods.settings_overlay") {
            if (id.empty()) return {ApiCallStatus::InvalidArgument, nullptr, "settings_overlay needs the mod's id"};
            const auto values = args.find("values");
            if (values == args.end()) return {ApiCallStatus::InvalidArgument, nullptr, "settings_overlay needs values"};
            return config.mods_handler(path, id, *values);
        }
        if (path == "mods.settings_overlay_clear") {
            return config.mods_handler(path, id, none);
        }
        return {ApiCallStatus::Unsupported, nullptr, "unknown API operation"};
    }

    ApiCallResult InvokeApi(std::string_view path, const Json &arguments) {
        const ApiOperationSpec *operation = FindApiOperation(path);
        if (operation == nullptr) {
            return {ApiCallStatus::Unsupported, nullptr, "unknown API operation"};
        }
        if (!operation->permission.empty() && !HasPermission(manifest, operation->permission)) {
            return {ApiCallStatus::PermissionDenied, nullptr,
                    "manifest permission is required: " + std::string(operation->permission)};
        }
        std::string reason;
        if (!Capability(path, reason)) {
            return {ApiCallStatus::Unsupported, nullptr, reason};
        }
        if (path == "info.get") {
            // The calendar day in UTC, so a daily challenge seed is the same for every player that day.
            const std::chrono::year_month_day today{std::chrono::floor<std::chrono::days>(std::chrono::system_clock::now())};
            return {ApiCallStatus::Ok,
                    {{"id", config.game_id}, {"name", config.game_name}, {"api_major", config.framework_api},
                     {"mod_id", manifest.id},
                     {"date", {{"year", static_cast<int>(today.year())}, {"month", static_cast<unsigned>(today.month())},
                               {"day", static_cast<unsigned>(today.day())}}}},
                    {}};
        }
        if (path.starts_with("mods.")) return ModsApi(path, arguments);
        if (path == "events.list") {
            if (!arguments.is_array() || !arguments.empty()) {
                return {ApiCallStatus::InvalidArgument, nullptr, "events.list does not accept arguments"};
            }
            Json events = Json::array();
            for (const EventSpec &event : EventCatalog()) {
                if (event.games != "both" && event.games != config.game_id) continue;
                events.push_back({{"name", event.name}, {"domain", event.domain}, {"summary", event.summary}});
            }
            return {ApiCallStatus::Ok, {{"game", config.game_id}, {"events", std::move(events)}}, {}};
        }
        if (path == "scripts.list_hooks") {
            if (!arguments.is_array() || !arguments.empty()) {
                return {ApiCallStatus::InvalidArgument, nullptr, "scripts.list_hooks does not accept arguments"};
            }
            Json hooks = Json::array({
                {{"id", "native.event.run"},
                 {"kind", "event_program"},
                 {"summary", "Runs before a native event program starts in the active scene."},
                 {"context", {"program_no"}},
                 {"original_call", "game.scripts.call({id = hook.id})"}},
                {{"id", "native.dialogue.show"},
                 {"kind", "dialogue"},
                 {"summary", "Runs before a native dialogue or message window opens."},
                 {"context", {"message_id", "speaker"}},
                 {"original_call", "game.scripts.call({id = hook.id})"}},
                {{"id", "native.combat.before_hit"},
                 {"kind", "combat_hit"},
                 {"summary", "Runs before a native combat hit applies damage."},
                 {"context", {"attacker", "target", "damage"}},
                 {"original_call", "game.scripts.call({id = hook.id})"}},
                {{"id", "native.loot.roll"},
                 {"kind", "loot_roll"},
                 {"summary", "Runs before a native monster drop or treasure chest loot roll completes."},
                 {"context", {"source", "item_no"}},
                 {"original_call", "game.scripts.call({id = hook.id})"}},
                {{"id", "native.transition.before_enter"},
                 {"kind", "scene_transition"},
                 {"summary", "Runs before a native map, town, or dungeon floor transition begins."},
                 {"context", {"target", "floor_id"}},
                 {"original_call", "game.scripts.call({id = hook.id})"}},
                {{"id", "native.monster.run"},
                 {"kind", "monster_program"},
                 {"summary", "Runs before a monster's AI script program (1 spawn, 50 revealed, 100 idle, 110 hit, 120 killed)."},
                 {"context", {"entity", "slot", "program_no", "model"}},
                 {"original_call", "game.scripts.call({id = hook.id})"}},
            });
            return {ApiCallStatus::Ok, {{"game", config.game_id}, {"hooks", std::move(hooks)}}, {}};
        }
        if (path == "scripts.call") {
            if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object() ||
                !HasOnlyFields(arguments[0], {"id"}) || !arguments[0].contains("id") ||
                !arguments[0]["id"].is_string()) {
                return {ApiCallStatus::InvalidArgument, nullptr,
                        "scripts.call requires {id = the active native hook id}"};
            }
            const std::string &hook_id = arguments[0]["id"].get_ref<const std::string &>();
            if (hook_id != "native.event.run" && hook_id != "native.dialogue.show" &&
                hook_id != "native.combat.before_hit" && hook_id != "native.loot.roll" &&
                hook_id != "native.transition.before_enter" && hook_id != "native.monster.run") {
                return {ApiCallStatus::InvalidArgument, nullptr, "scripts.call does not recognize this native hook id"};
            }
            if (!config.script_hook_original_call_handler) {
                return {ApiCallStatus::Unsupported, nullptr, "native script call-through is unavailable"};
            }
            return config.script_hook_original_call_handler(manifest.id, hook_id);
        }
        if (path == "localization.register") return RegisterLocalization(arguments);
        if (path == "localization.get") return GetLocalization(arguments);
        if (path == "resources.load") return LoadResource(arguments);
        if (path == "resources.resolve") return ResolveResource(arguments);
        if (path == "resources.release") return ReleaseResource(arguments);
        if (path == "resources.override") {
            return RegisterNativeResourceOverride(manifest.id, manifest.package_root, arguments);
        }
        if (path == "resources.unoverride") {
            if (config.api_resource_override_removal_guard) {
                const std::string reason = config.api_resource_override_removal_guard(manifest.id, arguments);
                if (!reason.empty()) return {ApiCallStatus::Failed, nullptr, reason};
            }
            return RemoveNativeResourceOverride(manifest.id, arguments);
        }
        if (path == "ui.register") return RegisterUiAction(arguments);
        if (path == "ui.remove") return RemoveUiAction(arguments);
        if (path == "ui.navigate") return NavigateUi(arguments);
        if (path == "ui.open_skills") {
            if (!arguments.is_array() || !arguments.empty()) return {ApiCallStatus::InvalidArgument,nullptr,"open_skills takes no arguments"};
            OpenManager();
            const auto mods=SharedRuntime().Mods();
            for (std::size_t i=0;i<mods.size();++i) if (mods[i].id==manifest.id) ManagerState().selected_mod=static_cast<int>(i);
            ManagerState().detail_tab=3; ManagerState().settings_focus=true;
            return {ApiCallStatus::Ok,{{"opened",true}}, {}};
        }
        if (path == "ui.notify") return NotifyUi(arguments);
        if (path == "ui.sheet_set") return SetCharacterSheetUi(arguments);
        if (path == "ui.sheet_clear") {
            if (!arguments.is_array() || !arguments.empty()) return {ApiCallStatus::InvalidArgument, nullptr, "sheet_clear takes no arguments"};
            return {ApiCallStatus::Ok, {{"cleared", ClearCharacterSheet(manifest.id)}}, {}};
        }
        if (path == "ui.sheet_open") {
            if (!arguments.is_array() || !arguments.empty()) return {ApiCallStatus::InvalidArgument, nullptr, "sheet_open takes no arguments"};
            if (ActiveCharacterSheet() == nullptr) {
                return {ApiCallStatus::Failed, nullptr, "no loaded mod has set a character sheet"};
            }
            RequestCharacterPageOpen();
            return {ApiCallStatus::Ok, {{"opened", true}}, {}};
        }
        if (path == "commands.execute") return ExecuteCommand(arguments);
        if (path == "commands.unregister") return UnregisterCommand(arguments);
        if (path == "scripts.list_functions") return ListScriptFunctions(arguments);
        if (path == "scripts.call_function") return CallScriptFunction(arguments);
        if (path == "scripts.unregister_function") return UnregisterScriptFunction(arguments);
        if (path == "skills.list" || path == "skills.get") return GetSkills(arguments);
        if (path == "skills.register") return RegisterSkillTree(arguments);
        if (path == "skills.unlock") return SetSkillRank(arguments, true);
        if (path == "skills.set_rank") return SetSkillRank(arguments, false);
        if (path == "quests.get") return GetQuests(arguments);
        if (path == "quests.register") return RegisterQuest(arguments);
        if (path == "quests.start") return StartQuest(arguments);
        if (path == "quests.advance") return AdvanceQuest(arguments);
        if (path == "debug.profile") return DebugProfile(arguments);
        if (path == "save.get") {
            if (!arguments.is_array() || arguments.empty() || !arguments[0].is_string() ||
                !ValidKey(arguments[0].get_ref<const std::string &>())) {
                return {ApiCallStatus::InvalidArgument, nullptr, "expected a save key with 1-128 bytes"};
            }
            const std::string key = arguments[0].get<std::string>();
            if (key == kQuestStorageKey || key == kSkillStorageKey) {
                return {ApiCallStatus::InvalidArgument, nullptr, "the quest and skill storage keys are reserved"};
            }
            if (key == kSaveMigrationVersionKey) {
                const auto version = storage.find(key);
                if (version == storage.end()) return {ApiCallStatus::Ok, 0, {}};
                if (!version->is_number_integer()) {
                    return {ApiCallStatus::Failed, nullptr, "stored save migration version is invalid"};
                }
                try {
                    const std::int64_t value = version->get<std::int64_t>();
                    if (value < 0 || value > std::numeric_limits<std::int32_t>::max()) {
                        return {ApiCallStatus::Failed, nullptr, "stored save migration version is invalid"};
                    }
                } catch (const std::exception &) {
                    return {ApiCallStatus::Failed, nullptr, "stored save migration version is invalid"};
                }
                return {ApiCallStatus::Ok, *version, {}};
            }
            const auto found = storage.find(key);
            return {ApiCallStatus::Ok, found == storage.end() ? Json(nullptr) : *found, {}};
        }
        if (path == "save.set") {
            if (!arguments.is_array() || arguments.size() < 2 || !arguments[0].is_string() ||
                !ValidKey(arguments[0].get_ref<const std::string &>())) {
                return {ApiCallStatus::InvalidArgument, nullptr, "expected a save key and JSON-compatible value"};
            }
            if (arguments[0].get_ref<const std::string &>() == kSaveMigrationVersionKey ||
                arguments[0].get_ref<const std::string &>() == kQuestStorageKey ||
                arguments[0].get_ref<const std::string &>() == kSkillStorageKey) {
                return {ApiCallStatus::InvalidArgument, nullptr, "the save key is reserved by MoonFactory"};
            }
            Json replacement = storage;
            replacement[arguments[0].get<std::string>()] = arguments[1];
            try {
                if (replacement.dump().size() > kStorageSizeLimit) {
                    return {ApiCallStatus::InvalidArgument, nullptr, "mod save data is limited to 128 KiB"};
                }
            } catch (const std::exception &) {
                return {ApiCallStatus::InvalidArgument, nullptr, "mod save value is not valid JSON"};
            }
            Json prior_storage = std::move(storage);
            const bool was_dirty = storage_dirty;
            storage = std::move(replacement);
            storage_dirty = true;
            std::string error;
            if (!FlushStorage(error)) {
                storage = std::move(prior_storage);
                storage_dirty = was_dirty;
                return {ApiCallStatus::Failed, nullptr, error};
            }
            return {ApiCallStatus::Ok, true, {}};
        }
        if (path == "save.migrate") {
            if (!save_migration_window) {
                return {ApiCallStatus::PhaseViolation, nullptr,
                        "save.migrate may only run from the mod's runtime.loaded callback"};
            }
            if (!arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object()) {
                return {ApiCallStatus::InvalidArgument, nullptr,
                        "expected {from_version, to_version, set = optional object, delete = optional string array}"};
            }
            const Json &request = arguments[0];
            const auto from_field = request.find("from_version");
            const auto to_field = request.find("to_version");
            if (from_field == request.end() || to_field == request.end() ||
                !from_field->is_number_integer() || !to_field->is_number_integer()) {
                return {ApiCallStatus::InvalidArgument, nullptr, "from_version and to_version must be integers"};
            }
            std::int64_t from_version = 0;
            std::int64_t to_version = 0;
            try {
                from_version = from_field->get<std::int64_t>();
                to_version = to_field->get<std::int64_t>();
            } catch (const std::exception &) {
                return {ApiCallStatus::InvalidArgument, nullptr, "migration versions are outside the supported range"};
            }
            if (from_version < 0 || from_version >= std::numeric_limits<std::int32_t>::max() ||
                to_version != from_version + 1) {
                return {ApiCallStatus::InvalidArgument, nullptr,
                        "migrations must advance exactly one version using non-negative 32-bit integers"};
            }

            const auto set_field = request.find("set");
            if (set_field != request.end() && !set_field->is_object()) {
                return {ApiCallStatus::InvalidArgument, nullptr, "set must be an object of save keys and values"};
            }
            const auto delete_field = request.find("delete");
            if (delete_field != request.end() && !delete_field->is_array()) {
                return {ApiCallStatus::InvalidArgument, nullptr, "delete must be an array of save keys"};
            }
            std::set<std::string, std::less<>> deleted_keys;
            if (delete_field != request.end()) {
                for (const Json &key : *delete_field) {
                    if (!key.is_string() || !ValidKey(key.get_ref<const std::string &>()) ||
                        key.get_ref<const std::string &>() == kSaveMigrationVersionKey ||
                        key.get_ref<const std::string &>() == kQuestStorageKey ||
                        key.get_ref<const std::string &>() == kSkillStorageKey) {
                        return {ApiCallStatus::InvalidArgument, nullptr, "delete contains an invalid or reserved save key"};
                    }
                    deleted_keys.insert(key.get<std::string>());
                }
            }
            if (set_field != request.end()) {
                for (auto it = set_field->begin(); it != set_field->end(); ++it) {
                    if (!ValidKey(it.key()) || it.key() == kSaveMigrationVersionKey ||
                        it.key() == kQuestStorageKey || it.key() == kSkillStorageKey ||
                        deleted_keys.contains(it.key())) {
                        return {ApiCallStatus::InvalidArgument, nullptr,
                                "set contains an invalid, reserved, or also-deleted save key"};
                    }
                }
            }

            const std::string version_key(kSaveMigrationVersionKey);
            const auto stored_version = storage.find(version_key);
            std::int64_t current_version = 0;
            if (stored_version != storage.end()) {
                if (!stored_version->is_number_integer()) {
                    return {ApiCallStatus::Failed, nullptr, "stored save migration version is invalid"};
                }
                try {
                    current_version = stored_version->get<std::int64_t>();
                } catch (const std::exception &) {
                    return {ApiCallStatus::Failed, nullptr, "stored save migration version is outside the supported range"};
                }
                if (current_version < 0 || current_version > std::numeric_limits<std::int32_t>::max()) {
                    return {ApiCallStatus::Failed, nullptr, "stored save migration version is invalid"};
                }
            }
            if (current_version >= to_version) {
                return {ApiCallStatus::Ok,
                        {{"migrated", false}, {"version", current_version}}, {}};
            }
            if (current_version != from_version) {
                return {ApiCallStatus::Failed, nullptr,
                        "save migration version conflict: expected " + std::to_string(from_version) +
                            ", found " + std::to_string(current_version)};
            }

            Json replacement = storage;
            for (const std::string &key : deleted_keys) replacement.erase(key);
            if (set_field != request.end()) {
                for (auto it = set_field->begin(); it != set_field->end(); ++it) replacement[it.key()] = it.value();
            }
            replacement[version_key] = to_version;
            try {
                if (replacement.dump().size() > kStorageSizeLimit) {
                    return {ApiCallStatus::InvalidArgument, nullptr, "mod save data is limited to 128 KiB"};
                }
            } catch (const std::exception &) {
                return {ApiCallStatus::InvalidArgument, nullptr, "migration values are not valid JSON"};
            }
            Json prior_storage = std::move(storage);
            const bool was_dirty = storage_dirty;
            storage = std::move(replacement);
            storage_dirty = true;
            std::string error;
            if (!FlushStorage(error)) {
                storage = std::move(prior_storage);
                storage_dirty = was_dirty;
                return {ApiCallStatus::Failed, nullptr, error};
            }
            return {ApiCallStatus::Ok,
                    {{"migrated", true}, {"from_version", from_version}, {"to_version", to_version}}, {}};
        }
        if (path == "api.list" || path == "compat.list") {
            return {ApiCallStatus::Ok, CapabilityList(), {}};
        }
        if (path == "api.supports" || path == "compat.supports") {
            if (!arguments.is_array() || arguments.empty() || !arguments[0].is_string()) {
                return {ApiCallStatus::InvalidArgument, nullptr, "expected an API operation path string"};
            }
            const std::string requested = arguments[0].get<std::string>();
            const ApiOperationSpec *target = FindApiOperation(requested);
            if (target == nullptr) {
                return {ApiCallStatus::Unsupported, nullptr, "unknown API operation"};
            }
            std::string target_reason;
            const bool supported = Capability(requested, target_reason);
            return {ApiCallStatus::Ok,
                    {{"path", requested}, {"supported", supported},
                     {"status", supported ? "supported" : "unsupported"}, {"reason", target_reason}},
                    {}};
        }
        if (path == "api.call") {
            if (!arguments.is_array() || arguments.empty() || !arguments[0].is_string()) {
                return {ApiCallStatus::InvalidArgument, nullptr, "expected an API operation path as the first argument"};
            }
            const std::string target_path = arguments[0].get<std::string>();
            const ApiOperationSpec *target = FindApiOperation(target_path);
            if (target == nullptr || target_path == "api.call") {
                return {ApiCallStatus::Unsupported, nullptr, "unknown or non-callable API operation"};
            }
            if (target_path == "scripts.override") {
                return {ApiCallStatus::InvalidArgument, nullptr,
                        "scripts.override requires a Lua callback; call game.scripts.override directly"};
            }
            Json target_arguments = Json::array();
            for (std::size_t index = 1; index < arguments.size(); ++index) {
                target_arguments.push_back(arguments[index]);
            }
            return InvokeApi(target_path, target_arguments);
        }
        const bool package_operation = operation->domain == "audio";
        if (package_operation && config.package_api_handler) {
            try {
                const std::string owner = manifest.id;
                ApiCallResult result = config.package_api_handler(owner, manifest.package_root, *operation, arguments);
                const std::size_t response_limit = path == "resources.resolve" ? kApiResourceResultBytesLimit
                                                                                 : kApiValueBytesLimit;
                if (result.status == ApiCallStatus::Ok && result.value.dump().size() > response_limit) {
                    return {ApiCallStatus::Failed, nullptr, "game API response exceeds its byte limit"};
                }
                return result;
            } catch (const std::exception &exception) {
                return {ApiCallStatus::Failed, nullptr, std::string("game API adapter failed: ") + exception.what()};
            } catch (...) {
                return {ApiCallStatus::Failed, nullptr, "game API adapter failed with an unknown exception"};
            }
        }
        if (!config.api_handler) {
            return {ApiCallStatus::Unsupported, nullptr, "this game adapter has not bound the operation"};
        }
        try {
            const std::string owner = manifest.id;
            ApiCallResult result = config.api_handler(owner, *operation, arguments);
            const std::size_t response_limit = path == "resources.resolve" ? kApiResourceResultBytesLimit
                                                                             : kApiValueBytesLimit;
            if (result.status == ApiCallStatus::Ok && result.value.dump().size() > response_limit) {
                return {ApiCallStatus::Failed, nullptr, "game API response exceeds its byte limit"};
            }
            return result;
        } catch (const std::exception &exception) {
            return {ApiCallStatus::Failed, nullptr, std::string("game API adapter failed: ") + exception.what()};
        } catch (...) {
            return {ApiCallStatus::Failed, nullptr, "game API adapter failed with an unknown exception"};
        }
    }

    static int PushApiResult(lua_State *lua, const ApiCallResult &result) {
        if (result.status == ApiCallStatus::Ok) {
            PushJson(lua, result.value);
            lua_pushnil(lua);
            return 2;
        }
        lua_pushnil(lua);
        lua_createtable(lua, 0, 2);
        const std::string_view code = ApiCallStatusName(result.status);
        lua_pushlstring(lua, code.data(), code.size());
        lua_setfield(lua, -2, "code");
        lua_pushlstring(lua, result.message.data(), result.message.size());
        lua_setfield(lua, -2, "message");
        return 2;
    }

    static int DispatchApi(lua_State *lua, Impl *self, std::string_view path, int first_argument) {
        if (self == nullptr || self->state != lua) return 0;
        if (path == "events.subscribe") return SubscribeEvent(lua, self, first_argument);
        if (path == "events.unsubscribe") return UnsubscribeEvent(lua, self, first_argument);
        if (path == "commands.register") return RegisterCommand(lua, self, first_argument);
        if (path == "scripts.register_function") return RegisterScriptFunction(lua, self, first_argument);
        if (path == "scripts.override") return RegisterScriptOverride(lua, self, first_argument);
        Json arguments = Json::array();
        std::set<const void *> active;
        std::size_t nodes = 0;
        std::string error;
        for (int index = first_argument; index <= lua_gettop(lua); ++index) {
            Json value;
            if (!ReadApiJson(lua, index, value, error, 0, nodes, active)) {
                return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr, std::move(error)});
            }
            arguments.push_back(std::move(value));
        }
        try {
            if (arguments.dump().size() > kApiValueBytesLimit) {
                return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr, "API arguments exceed 64 KiB"});
            }
        } catch (const std::exception &) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr, "API arguments are not valid JSON"});
        }
        ApiCallResult result = self->InvokeApi(path, arguments);
        // The whole catalogue (api.list) and resource payloads may be larger than an ordinary value.
        bool resource_payload = path == "resources.resolve" || path == "api.list" || path == "compat.list";
        if (path == "api.call" && arguments.size() >= 1 && arguments[0].is_string()) {
            resource_payload = arguments[0].get_ref<const std::string &>() == "resources.resolve";
        }
        const std::size_t response_limit = resource_payload ? kApiResourceResultBytesLimit : kApiValueBytesLimit;
        try {
            if (result.status == ApiCallStatus::Ok && result.value.dump().size() > response_limit) {
                result = {ApiCallStatus::Failed, nullptr, "API response exceeds its byte limit"};
            }
        } catch (const std::exception &) {
            result = {ApiCallStatus::Failed, nullptr, "API response is not valid JSON"};
        }
        return PushApiResult(lua, result);
    }

    static int ApiDispatch(lua_State *lua) {
        Impl *self = static_cast<Impl *>(lua_touserdata(lua, lua_upvalueindex(1)));
        const char *domain = lua_tostring(lua, lua_upvalueindex(2));
        const char *operation = lua_tostring(lua, lua_upvalueindex(3));
        if (domain == nullptr || operation == nullptr) return 0;
        return DispatchApi(lua, self, std::string(domain) + "." + operation, 1);
    }

    static int ApiDomainIndex(lua_State *lua) {
        Impl *self = static_cast<Impl *>(lua_touserdata(lua, lua_upvalueindex(1)));
        const char *domain = lua_tostring(lua, lua_upvalueindex(2));
        std::size_t operation_length = 0;
        const char *operation = lua_tolstring(lua, 2, &operation_length);
        if (self == nullptr || domain == nullptr || operation == nullptr || operation_length == 0 ||
            operation_length > 64) {
            lua_pushnil(lua);
            return 1;
        }
        for (std::size_t i = 0; i < operation_length; ++i) {
            const char c = operation[i];
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_')) {
                lua_pushnil(lua);
                return 1;
            }
        }
        lua_pushlightuserdata(lua, self);
        lua_pushstring(lua, domain);
        lua_pushlstring(lua, operation, operation_length);
        lua_pushcclosure(lua, ApiDispatch, 3);
        return 1;
    }

    static bool ValidEventName(std::string_view name) {
        if (name.size() < 2 || name.size() > 96) return false;
        for (const char c : name) {
            if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.' || c == '-')) {
                return false;
            }
        }
        return true;
    }

    int AddCallback(std::string event, int reference) {
        auto &list = callbacks[std::move(event)];
        if (list.size() >= callback_limit) return 0;
        if (next_event_token <= 0 || next_event_token == std::numeric_limits<int>::max()) next_event_token = 1;
        const int token = next_event_token++;
        list.push_back({token, reference, manifest.id});
        return token;
    }

    bool HasCallback(int token) const {
        for (const auto &[event, list] : callbacks) {
            (void)event;
            if (std::ranges::find(list, token, &EventCallback::token) != list.end()) return true;
        }
        return false;
    }

    bool RemoveCallback(int token) {
        for (auto it = callbacks.begin(); it != callbacks.end(); ++it) {
            auto &list = it->second;
            const auto found = std::ranges::find(list, token, &EventCallback::token);
            if (found == list.end()) continue;
            luaL_unref(state, LUA_REGISTRYINDEX, found->reference);
            list.erase(found);
            if (list.empty()) callbacks.erase(it);
            return true;
        }
        return false;
    }

    static bool RegisterCallback(lua_State *lua, Impl *self, int event_index, int callback_index,
                                 std::string &event_name, int &token, std::string &error) {
        std::size_t name_length = 0;
        const char *name = luaL_checklstring(lua, event_index, &name_length);
        event_name.assign(name, name_length);
        if (!ValidEventName(event_name)) {
            error = "invalid event name";
            return false;
        }
        luaL_checktype(lua, callback_index, LUA_TFUNCTION);
        const auto found = self->callbacks.find(event_name);
        if (found != self->callbacks.end() && found->second.size() >= self->callback_limit) {
            error = "event callback limit reached";
            return false;
        }
        lua_pushvalue(lua, callback_index);
        const int reference = luaL_ref(lua, LUA_REGISTRYINDEX);
        token = self->AddCallback(event_name, reference);
        if (token == 0) {
            luaL_unref(lua, LUA_REGISTRYINDEX, reference);
            error = "event callback limit reached";
            return false;
        }
        return true;
    }

    static int SubscribeEvent(lua_State *lua, Impl *self, int first_argument) {
        if (self == nullptr || self->state != lua || lua_gettop(lua) < first_argument + 1) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "events.subscribe expects an event name and callback"});
        }
        std::string event_name;
        std::string error;
        int token = 0;
        if (!RegisterCallback(lua, self, first_argument, first_argument + 1, event_name, token, error)) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr, std::move(error)});
        }
        lua_pushinteger(lua, token);
        lua_pushnil(lua);
        return 2;
    }

    static int UnsubscribeEvent(lua_State *lua, Impl *self, int first_argument) {
        if (self == nullptr || self->state != lua || !lua_isinteger(lua, first_argument)) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "events.unsubscribe expects a subscription token"});
        }
        const lua_Integer token = lua_tointeger(lua, first_argument);
        if (token <= 0 || token > std::numeric_limits<int>::max() || !self->RemoveCallback(static_cast<int>(token))) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr, "subscription token is not active"});
        }
        lua_pushboolean(lua, 1);
        lua_pushnil(lua);
        return 2;
    }

    static int RegisterCommand(lua_State *lua, Impl *self, int first_argument) {
        if (self == nullptr || self->state != lua || lua_gettop(lua) != first_argument + 1) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "commands.register expects a definition and callback"});
        }
        if (first_argument != 1) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "call commands.register through game.commands.register"});
        }
        const ApiOperationSpec *operation = FindApiOperation("commands.register");
        if (operation == nullptr) {
            return PushApiResult(lua, {ApiCallStatus::Unsupported, nullptr, "commands.register is not catalogued"});
        }
        if (!HasPermission(self->manifest, operation->permission)) {
            return PushApiResult(lua, {ApiCallStatus::PermissionDenied, nullptr,
                                       "manifest permission is required: " + std::string(operation->permission)});
        }
        std::string reason;
        if (!self->Capability(operation->path, reason)) {
            return PushApiResult(lua, {ApiCallStatus::Unsupported, nullptr, std::move(reason)});
        }
        if (lua_type(lua, first_argument + 1) != LUA_TFUNCTION) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr, "command callback must be a function"});
        }
        Json definition;
        std::string error;
        std::set<const void *> active;
        std::size_t nodes = 0;
        if (!ReadApiJson(lua, first_argument, definition, error, 0, nodes, active) || !definition.is_object()) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       error.empty() ? "commands.register requires a definition object" : std::move(error)});
        }
        if (!self->HasOnlyFields(definition, {"id", "label", "description"})) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr, "command definition contains an unknown field"});
        }
        const auto id_field = definition.find("id");
        const auto label_field = definition.find("label");
        const auto description_field = definition.find("description");
        if (id_field == definition.end() || !id_field->is_string() || label_field == definition.end() ||
            !label_field->is_string() ||
            (description_field != definition.end() && !description_field->is_string())) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "command definition requires string fields 'id' and 'label'"});
        }
        std::string local_id;
        if (!self->ParseCommandId(id_field->get_ref<const std::string &>(), local_id, error)) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr, std::move(error)});
        }
        if (self->commands.contains(local_id)) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "command id is already registered by this mod"});
        }
        const std::string action_id = "cmd." + local_id;
        if (!RegisterManagerAction(self->manifest.id, action_id, label_field->get<std::string>(),
                                   description_field == definition.end() ? std::string{}
                                                                         : description_field->get<std::string>(),
                                   error)) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr, std::move(error)});
        }
        lua_pushvalue(lua, first_argument + 1);
        const int reference = luaL_ref(lua, LUA_REGISTRYINDEX);
        self->commands.emplace(local_id, CommandCallback{reference});
        PushJson(lua, {{"id", self->manifest.id + ":" + local_id},
                       {"action_id", self->manifest.id + ":" + action_id}});
        lua_pushnil(lua);
        return 2;
    }

    static int RegisterScriptFunction(lua_State *lua, Impl *self, int first_argument) {
        if (self == nullptr || self->state != lua || lua_gettop(lua) != first_argument + 1) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "scripts.register_function expects a definition and callback"});
        }
        if (first_argument != 1) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "call scripts.register_function through game.scripts.register_function"});
        }
        const ApiOperationSpec *operation = FindApiOperation("scripts.register_function");
        if (operation == nullptr) {
            return PushApiResult(lua, {ApiCallStatus::Unsupported, nullptr,
                                       "scripts.register_function is not catalogued"});
        }
        if (!HasPermission(self->manifest, operation->permission)) {
            return PushApiResult(lua, {ApiCallStatus::PermissionDenied, nullptr,
                                       "manifest permission is required: " + std::string(operation->permission)});
        }
        std::string reason;
        if (!self->Capability(operation->path, reason)) {
            return PushApiResult(lua, {ApiCallStatus::Unsupported, nullptr, std::move(reason)});
        }
        if (lua_type(lua, first_argument + 1) != LUA_TFUNCTION) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "script function callback must be a function"});
        }
        Json definition;
        std::string error;
        std::set<const void *> active;
        std::size_t nodes = 0;
        if (!ReadApiJson(lua, first_argument, definition, error, 0, nodes, active) || !definition.is_object()) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       error.empty() ? "scripts.register_function requires a definition object"
                                                     : std::move(error)});
        }
        if (!self->HasOnlyFields(definition, {"id", "description"})) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "script function definition contains an unknown field"});
        }
        const auto id_field = definition.find("id");
        const auto description_field = definition.find("description");
        if (id_field == definition.end() || !id_field->is_string() ||
            (description_field != definition.end() && !description_field->is_string())) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "script function definition requires a string id and optional description"});
        }
        if (description_field != definition.end() && description_field->get_ref<const std::string &>().size() > 1024) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "script function description is limited to 1024 bytes"});
        }
        std::string local_id;
        if (!self->ParseCommandId(id_field->get_ref<const std::string &>(), local_id, error)) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr, std::move(error)});
        }
        if (self->script_functions.contains(local_id)) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "script function id is already registered by this mod"});
        }
        if (self->script_functions.size() >= kScriptFunctionLimit) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "a mod may expose at most 256 script functions"});
        }
        lua_pushvalue(lua, first_argument + 1);
        const int reference = luaL_ref(lua, LUA_REGISTRYINDEX);
        const std::string description = description_field == definition.end()
                                            ? std::string{}
                                            : description_field->get<std::string>();
        self->script_functions.emplace(local_id, ScriptFunctionCallback{reference, description});
        PushJson(lua, {{"id", self->manifest.id + ":" + local_id}, {"exposed", true}});
        lua_pushnil(lua);
        return 2;
    }

    static int RegisterScriptOverride(lua_State *lua, Impl *self, int first_argument) {
        if (self == nullptr || self->state != lua || lua_gettop(lua) < first_argument ||
            lua_gettop(lua) > first_argument + 1) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "scripts.override expects a definition and optional callback"});
        }
        if (first_argument != 1) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "call scripts.override through game.scripts.override"});
        }
        const ApiOperationSpec *operation = FindApiOperation("scripts.override");
        if (operation == nullptr) {
            return PushApiResult(lua, {ApiCallStatus::Unsupported, nullptr, "scripts.override is not catalogued"});
        }
        if (!HasPermission(self->manifest, operation->permission)) {
            return PushApiResult(lua, {ApiCallStatus::PermissionDenied, nullptr,
                                       "manifest permission is required: " + std::string(operation->permission)});
        }
        std::string reason;
        if (!self->Capability(operation->path, reason)) {
            return PushApiResult(lua, {ApiCallStatus::Unsupported, nullptr, std::move(reason)});
        }
        Json definition;
        std::string error;
        std::set<const void *> active;
        std::size_t nodes = 0;
        if (!ReadApiJson(lua, first_argument, definition, error, 0, nodes, active) || !definition.is_object() ||
            !self->HasOnlyFields(definition, {"id", "enabled"})) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       error.empty() ? "scripts.override requires {id, enabled?}"
                                                     : std::move(error)});
        }
        const auto id_field = definition.find("id");
        const auto enabled_field = definition.find("enabled");
        if (id_field == definition.end() || !id_field->is_string() ||
            (enabled_field != definition.end() && !enabled_field->is_boolean())) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "scripts.override requires a string id and optional boolean enabled"});
        }
        const std::string hook_id = id_field->get<std::string>();
        if (hook_id != "native.event.run" && hook_id != "native.dialogue.show" &&
            hook_id != "native.combat.before_hit" && hook_id != "native.loot.roll" &&
            hook_id != "native.transition.before_enter" && hook_id != "native.monster.run") {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "unknown hook id; inspect game.scripts.list_hooks()"});
        }
        const bool enabled = enabled_field == definition.end() || enabled_field->get<bool>();
        auto existing = self->script_overrides.find(hook_id);
        if (existing != self->script_overrides.end() && existing->second.owner != self->manifest.id) {
            // another part of this pack holds the hook: removing leaves it; taking it over says so
            if (!enabled) existing = self->script_overrides.end();
            else self->Log("warning", "takes native script hook '" + std::string(hook_id) + "' from " + existing->second.owner);
        }
        if (!enabled) {
            if (lua_gettop(lua) != first_argument) {
                return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                           "removing a script override does not accept a callback"});
            }
            const bool removed = existing != self->script_overrides.end();
            if (removed) {
                luaL_unref(lua, LUA_REGISTRYINDEX, existing->second.reference);
                self->script_overrides.erase(existing);
                if (hook_id == "native.monster.run") BumpMonsterScriptOverrideGeneration();
            }
            PushJson(lua, {{"id", hook_id}, {"enabled", false}, {"removed", removed}});
            lua_pushnil(lua);
            return 2;
        }
        if (lua_gettop(lua) != first_argument + 1 || lua_type(lua, first_argument + 1) != LUA_TFUNCTION) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "enabled script overrides require a Lua callback"});
        }
        if (existing == self->script_overrides.end() && self->script_overrides.size() >= 16) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr,
                                       "a mod may own at most 16 native script overrides"});
        }
        lua_pushvalue(lua, first_argument + 1);
        const int reference = luaL_ref(lua, LUA_REGISTRYINDEX);
        const bool replaced = existing != self->script_overrides.end();
        if (replaced) luaL_unref(lua, LUA_REGISTRYINDEX, existing->second.reference);
        self->script_overrides[hook_id] = ScriptOverrideCallback{reference, self->manifest.id};
        if (hook_id == "native.monster.run") BumpMonsterScriptOverrideGeneration();
        PushJson(lua, {{"id", hook_id}, {"enabled", true}, {"replaced", replaced}});
        lua_pushnil(lua);
        return 2;
    }

    static int ApiCallByPath(lua_State *lua) {
        Impl *self = Upvalue(lua);
        std::size_t path_length = 0;
        const char *path = luaL_checklstring(lua, 1, &path_length);
        if (path_length == 0 || path_length > 128) {
            return PushApiResult(lua, {ApiCallStatus::InvalidArgument, nullptr, "invalid API operation path"});
        }
        return DispatchApi(lua, self, std::string_view(path, path_length), 2);
    }

    static int ApiList(lua_State *lua) {
        Impl *self = Upvalue(lua);
        if (self == nullptr) {
            lua_pushnil(lua);
            return 1;
        }
        PushJson(lua, self->CapabilityList());
        return 1;
    }

    static int ApiSupports(lua_State *lua) {
        Impl *self = Upvalue(lua);
        std::size_t length = 0;
        const char *path = luaL_checklstring(lua, 1, &length);
        std::string reason;
        const ApiOperationSpec *operation = FindApiOperation(std::string_view(path, length));
        const bool supported = self != nullptr && operation != nullptr && self->Capability(std::string_view(path, length), reason);
        lua_pushboolean(lua, supported);
        if (operation == nullptr) reason = "unknown API operation";
        lua_pushlstring(lua, reason.data(), reason.size());
        return 2;
    }

    void RegisterGameApi() {
        lua_newtable(state);
        for (const ApiOperationSpec &operation : ApiCatalog()) {
            lua_getfield(state, -1, std::string(operation.domain).c_str());
            if (lua_isnil(state, -1)) {
                lua_pop(state, 1);
                lua_newtable(state);
                lua_newtable(state);
                lua_pushlightuserdata(state, this);
                lua_pushlstring(state, operation.domain.data(), operation.domain.size());
                lua_pushcclosure(state, ApiDomainIndex, 2);
                lua_setfield(state, -2, "__index");
                lua_setmetatable(state, -2);
                lua_pushlstring(state, operation.domain.data(), operation.domain.size());
                lua_pushvalue(state, -2);
                lua_settable(state, -4);
            }
            lua_pop(state, 1);
        }
        lua_setglobal(state, "game");
    }

    static int ApiOn(lua_State *lua) {
        Impl *self = Upvalue(lua);
        if (self == nullptr || self->state != lua) {
            lua_pushnil(lua);
            lua_pushliteral(lua, "invalid event subscription");
            return 2;
        }
        std::string event_name;
        std::string error;
        int token = 0;
        if (!RegisterCallback(lua, self, 1, 2, event_name, token, error)) {
            lua_pushnil(lua);
            lua_pushlstring(lua, error.data(), error.size());
            return 2;
        }
        lua_pushboolean(lua, 1);
        return 1;
    }

    static int ApiLog(lua_State *lua) {
        Impl *self = Upvalue(lua);
        const char *level = luaL_checkstring(lua, 1);
        std::size_t length = 0;
        const char *text = luaL_checklstring(lua, 2, &length);
        if (self == nullptr) return 0;
        constexpr std::string_view levels[] = {"debug", "info", "warning", "error"};
        if (std::ranges::find(levels, std::string_view(level)) == std::end(levels)) {
            lua_pushnil(lua);
            lua_pushliteral(lua, "log level must be debug, info, warning, or error");
            return 2;
        }
        self->Log(level, std::string_view(text, std::min<std::size_t>(length, 8192)));
        return 0;
    }

    static int ApiSettingGet(lua_State *lua) {
        Impl *self = Upvalue(lua);
        std::size_t length = 0;
        const char *key = luaL_checklstring(lua, 1, &length);
        if (self == nullptr || length == 0 || length > 64) {
            lua_pushnil(lua);
            return 1;
        }
        const Json value = self->setting_reader(self->manifest.id, std::string_view(key, length));
        PushJson(lua, value);
        return 1;
    }

    static int ApiStorageGet(lua_State *lua) {
        Impl *self = Upvalue(lua);
        std::size_t length = 0;
        const char *key = luaL_checklstring(lua, 1, &length);
        if (self == nullptr || !HasPermission(self->manifest, "storage.read") || !ValidKey({key, length}) ||
            ReservedStorageKey({key, length})) {
            lua_pushnil(lua);
            return 1;
        }
        const auto found = self->storage.find(std::string(key, length));
        if (found == self->storage.end()) lua_pushnil(lua);
        else StoragePush(lua, *found);
        return 1;
    }

    bool FlushStorage(std::string &error) {
        if (!storage_dirty) return true;
        std::error_code ec;
        if (moonmod::ContainsSymlinkBelow(storage_file, config.save_root)) {
            error = "mod storage path contains a symbolic link";
            return false;
        }
        std::filesystem::create_directories(storage_file.parent_path(), ec);
        if (ec) {
            error = "could not create mod storage directory: " + ec.message();
            return false;
        }
        auto temp = storage_file;
        temp += ".tmp";
        if (moonmod::ContainsSymlinkBelow(storage_file, config.save_root) ||
            moonmod::ContainsSymlinkBelow(temp, config.save_root)) {
            error = "mod storage path contains a symbolic link";
            return false;
        }
        {
            std::ofstream output(temp, std::ios::binary | std::ios::trunc);
            if (!output) {
                error = "could not write mod storage";
                return false;
            }
            output << storage.dump(2);
            output.flush();
            if (!output) {
                error = "could not finish writing mod storage";
                return false;
            }
        }
        std::filesystem::rename(temp, storage_file, ec);
        if (ec) {
            std::error_code remove_error;
            std::filesystem::remove(storage_file, remove_error);
            ec.clear();
            std::filesystem::rename(temp, storage_file, ec);
        }
        if (ec) {
            error = "could not replace mod storage: " + ec.message();
            return false;
        }
        storage_dirty = false;
        return true;
    }

    static int ApiStorageSet(lua_State *lua) {
        Impl *self = Upvalue(lua);
        std::size_t key_length = 0;
        const char *key = luaL_checklstring(lua, 1, &key_length);
        if (self == nullptr || !HasPermission(self->manifest, "storage.write") || !ValidKey({key, key_length})) {
            lua_pushnil(lua);
            lua_pushliteral(lua, "storage.write permission is required and keys must contain 1-128 bytes");
            return 2;
        }
        if (ReservedStorageKey({key, key_length})) {
            lua_pushnil(lua);
            lua_pushliteral(lua, "reserved internal storage key");
            return 2;
        }
        bool valid = false;
        Json value = StorageValue(lua, 2, valid);
        if (!valid) {
            lua_pushnil(lua);
            lua_pushliteral(lua, "storage accepts tables, strings up to 4096 bytes, booleans, and finite numbers");
            return 2;
        }
        auto replacement = self->storage;
        replacement[std::string(key, key_length)] = std::move(value);
        if (replacement.dump().size() > kStorageSizeLimit) {
            lua_pushnil(lua);
            lua_pushliteral(lua, "mod storage is limited to 128 KiB");
            return 2;
        }
        self->storage = std::move(replacement);
        self->storage_dirty = true;
        std::string error;
        if (!self->FlushStorage(error)) {
            self->Log("error", error);
            lua_pushnil(lua);
            lua_pushlstring(lua, error.data(), error.size());
            return 2;
        }
        lua_pushboolean(lua, 1);
        return 1;
    }

    static int ApiStorageDelete(lua_State *lua) {
        Impl *self = Upvalue(lua);
        std::size_t length = 0;
        const char *key = luaL_checklstring(lua, 1, &length);
        if (self == nullptr || !HasPermission(self->manifest, "storage.write") || !ValidKey({key, length})) {
            lua_pushnil(lua);
            lua_pushliteral(lua, "storage.write permission is required and keys must contain 1-128 bytes");
            return 2;
        }
        if (ReservedStorageKey({key, length})) {
            lua_pushnil(lua);
            lua_pushliteral(lua, "reserved internal storage key");
            return 2;
        }
        if (self->storage.erase(std::string(key, length)) == 0) {
            lua_pushboolean(lua, 0);
            return 1;
        }
        self->storage_dirty = true;
        std::string error;
        if (!self->FlushStorage(error)) {
            self->Log("error", error);
            lua_pushnil(lua);
            lua_pushlstring(lua, error.data(), error.size());
            return 2;
        }
        lua_pushboolean(lua, 1);
        return 1;
    }

    static int OpenBase(lua_State *lua) { return luaopen_base(lua); }

    void OpenSafeLibraries() {
        luaL_requiref(state, "_G", OpenBase, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_TABLIBNAME, luaopen_table, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_STRLIBNAME, luaopen_string, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_MATHLIBNAME, luaopen_math, 1);
        lua_pop(state, 1);
        luaL_requiref(state, LUA_UTF8LIBNAME, luaopen_utf8, 1);
        lua_pop(state, 1);
        // These base functions could load code, bytecode, files, native modules, or arbitrary C libraries.
        constexpr const char *blocked[] = {"dofile", "load", "loadfile", "require", "collectgarbage"};
        for (const char *name : blocked) {
            lua_pushnil(state);
            lua_setglobal(state, name);
        }
        lua_pushcfunction(state, Print);
        lua_setglobal(state, "print");
        lua_pushnil(state);
        lua_setglobal(state, "io");
        lua_pushnil(state);
        lua_setglobal(state, "os");
        lua_pushnil(state);
        lua_setglobal(state, "debug");
        lua_pushnil(state);
        lua_setglobal(state, "package");
        lua_pushnil(state);
        lua_setglobal(state, "coroutine");
    }

    void RegisterApi() {
        lua_newtable(state);
        SetFunction(state, "on", ApiOn, this);
        SetFunction(state, "log", ApiLog, this);

        lua_newtable(state);
        SetFunction(state, "get", ApiSettingGet, this);
        lua_setfield(state, -2, "settings");

        lua_newtable(state);
        SetFunction(state, "get", ApiStorageGet, this);
        SetFunction(state, "set", ApiStorageSet, this);
        SetFunction(state, "delete", ApiStorageDelete, this);
        lua_setfield(state, -2, "storage");

        lua_newtable(state);
        SetFunction(state, "list", ApiList, this);
        SetFunction(state, "supports", ApiSupports, this);
        SetFunction(state, "call", ApiCallByPath, this);
        lua_setfield(state, -2, "api");

        lua_newtable(state);
        lua_pushlstring(state, config.game_id.data(), config.game_id.size());
        lua_setfield(state, -2, "id");
        lua_pushinteger(state, config.framework_api);
        lua_setfield(state, -2, "api_major");
        lua_pushlstring(state, manifest.id.data(), manifest.id.size());
        lua_setfield(state, -2, "mod_id");
        lua_setfield(state, -2, "game");

        lua_pushlstring(state, config.game_id.data(), config.game_id.size());
        lua_setglobal(state, "game_id");
        if (!owners.empty()) {
            lua_newtable(state);
            SetFunction(state, "run", ApiPartRun, this);
            SetFunction(state, "enabled", ApiPartEnabled, this);
            SetFunction(state, "list", ApiPartList, this);
            lua_setfield(state, -2, "part");
        }
        lua_setglobal(state, "moonmod");
        RegisterGameApi();
    }

    bool LoadStorage(std::string &error) {
        std::error_code ec;
        if (moonmod::ContainsSymlinkBelow(storage_file, config.save_root)) {
            error = "mod storage path contains a symbolic link";
            return false;
        }
        if (!std::filesystem::is_regular_file(storage_file, ec)) return true;
        const auto size = std::filesystem::file_size(storage_file, ec);
        if (ec || size > kStorageSizeLimit) {
            error = "stored data exceeds the 128 KiB limit";
            storage = Json::object();
            return false;
        }
        try {
            std::ifstream input(storage_file, std::ios::binary);
            input >> storage;
            if (!storage.is_object()) storage = Json::object();
            return true;
        } catch (const std::exception &exception) {
            error = std::string("stored data could not be parsed: ") + exception.what();
            storage = Json::object();
            return false;
        }
    }

    bool ProtectedCall(int arguments, int results, std::string &error) {
        const int previous_budget = budget_remaining;
        const bool previous_exceeded = budget_exceeded;
        const lua_Hook previous_hook = lua_gethook(state);
        const int previous_mask = lua_gethookmask(state);
        const int previous_count = lua_gethookcount(state);
        budget_remaining = config.lua_instructions_per_callback;
        budget_exceeded = false;
        lua_sethook(state, InstructionHook, LUA_MASKCOUNT, kHookInterval);
        const int status = lua_pcall(state, arguments, results, 0);
        const bool exceeded = budget_exceeded;
        budget_remaining = previous_budget;
        budget_exceeded = previous_exceeded;
        lua_sethook(state, previous_hook, previous_mask, previous_count);
        if (status == LUA_OK && !exceeded) return true;
        if (status == LUA_OK) {
            if (results > 0) lua_pop(state, results);
            error = "Lua instruction budget exceeded";
            return false;
        }
        const char *message = lua_tostring(state, -1);
        error = message == nullptr ? "unknown Lua error" : message;
        lua_pop(state, 1);
        return false;
    }

    void AddModuleCallback(const char *field, std::string event) {
        lua_getfield(state, -1, field);
        if (lua_isfunction(state, -1)) {
            const int reference = luaL_ref(state, LUA_REGISTRYINDEX);
            if (AddCallback(std::move(event), reference) == 0) luaL_unref(state, LUA_REGISTRYINDEX, reference);
        } else {
            lua_pop(state, 1);
        }
    }

    void Emit(std::string_view event_name, const Json &payload, std::string_view only_owner = {}) {
        if (state == nullptr) return;
        if (event_name.starts_with("game.")) {
            if (owners.empty()) {
                AdvanceQuestsFromEvent(event_name, payload);
            } else {
                for (const std::string &owner : OwnerIds()) {
                    if (!only_owner.empty() && owner != only_owner) continue;
                    const auto slot = owners.find(owner);
                    const bool current = owner == manifest.id;
                    if (!current && (slot == owners.end() || !slot->second.storage.contains(std::string(kQuestStorageKey)))) {
                        continue;
                    }
                    OwnerScope scope(*this, owner);
                    AdvanceQuestsFromEvent(event_name, payload);
                }
            }
        }
        OwnerScope target_scope(*this, only_owner);
        if (event_name == "ui.action" && payload.is_object() && payload.contains("id") && payload["id"].is_string()) {
            const std::string &action_id = payload["id"].get_ref<const std::string &>();
            if (action_id.starts_with("cmd.")) {
                const std::string_view local_id(action_id.data() + 4, action_id.size() - 4);
                if (commands.contains(local_id)) {
                    const ApiCallResult result = InvokeCommand(local_id, Json::object());
                    if (result.status != ApiCallStatus::Ok) {
                        Log("error", "manager command '" + std::string(local_id) + "' failed: " + result.message);
                    }
                }
            }
        }
        const auto found = callbacks.find(event_name);
        if (found == callbacks.end()) return;
        const std::vector<EventCallback> event_callbacks = found->second;
        const bool previous_migration_window = save_migration_window;
        save_migration_window = event_name == "runtime.loaded";
        for (const EventCallback &callback : event_callbacks) {
            if (!only_owner.empty() && callback.owner != only_owner) continue;
            if (!HasCallback(callback.token)) continue;
            OwnerScope scope(*this, callback.owner);
            lua_rawgeti(state, LUA_REGISTRYINDEX, callback.reference);
            PushJson(state, payload);
            std::string error;
            if (!ProtectedCall(1, 0, error)) {
                Log("error", std::string("event '") + std::string(event_name) + "' failed: " + error);
            }
        }
        save_migration_window = previous_migration_window;
    }
};

LuaMod::LuaMod(const RuntimeConfig &config, const ModManifest &manifest,
               std::function<Json(std::string_view, std::string_view)> setting_reader)
    : impl_(std::make_unique<Impl>(config, manifest, std::move(setting_reader))) {}

LuaMod::~LuaMod() { Unload(); }

bool LuaMod::Load(std::string &error) {
    Impl &self = *impl_;
    if (self.state != nullptr) return true;
    std::string storage_error;
    if (!self.LoadStorage(storage_error)) self.Log("warning", storage_error);
    self.state = lua_newstate(Impl::Allocate, &self.memory, LuaSeed(self.manifest.id));
    if (self.state == nullptr) {
        error = "could not create Lua state within the configured memory budget";
        return false;
    }
    *static_cast<Impl **>(lua_getextraspace(self.state)) = &self;
    self.ResetOwners();
    self.OpenSafeLibraries();
    self.RegisterApi();

    std::ifstream input(self.manifest.entry_file, std::ios::binary);
    if (!input) {
        error = "entry script could not be opened";
        Unload();
        return false;
    }
    std::string source((std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
    if (source.empty() || source.size() > kScriptSizeLimit) {
        error = "entry script is empty or exceeds 1 MiB";
        Unload();
        return false;
    }
    if (luaL_loadbufferx(self.state, source.data(), source.size(), self.manifest.entry.c_str(), "t") != LUA_OK) {
        const char *message = lua_tostring(self.state, -1);
        error = message == nullptr ? "could not compile entry script" : message;
        lua_pop(self.state, 1);
        Unload();
        return false;
    }
    if (!self.ProtectedCall(0, 1, error)) {
        Unload();
        return false;
    }
    if (lua_istable(self.state, -1)) {
        self.AddModuleCallback("on_load", "runtime.loaded");
        self.AddModuleCallback("on_unload", "runtime.unloaded");
        self.AddModuleCallback("on_tick", "game.tick");
    }
    lua_settop(self.state, 0);
    return true;
}

void LuaMod::Unload() {
    if (!impl_ || impl_->state == nullptr) return;
    Impl &self = *impl_;
    self.SwitchOwner(self.home_id);
    Emit("runtime.unloaded", Json::object());
    for (const std::string &part : std::vector<std::string>(self.running_parts.begin(), self.running_parts.end())) {
        self.StopPart(part, false);
    }
    self.ReleaseOwner(self.manifest.id);
    std::string error;
    if (!self.FlushStorage(error)) self.Log("error", error);
    lua_close(self.state);
    self.state = nullptr;
    self.callbacks.clear();
    if (self.script_overrides.contains("native.monster.run")) BumpMonsterScriptOverrideGeneration();
    self.script_overrides.clear();
    self.owners.clear();
    self.part_functions.clear();
    self.part_globals.clear();
    self.memory.used = 0;
}

void LuaMod::Emit(std::string_view event_name, const Json &payload, std::string_view owner) {
    impl_->Emit(event_name, payload, owner);
}

void LuaMod::SetEnabledParts(std::set<std::string, std::less<>> parts) { impl_->enabled_parts = std::move(parts); }

bool LuaMod::StartPart(std::string_view part, std::string &error) {
    impl_->enabled_parts.insert(std::string(part));
    if (impl_->state == nullptr) {
        error = "the pack is not running";
        return false;
    }
    return impl_->StartPart(std::string(part), error);
}

void LuaMod::StopPart(std::string_view part) {
    impl_->enabled_parts.erase(std::string(part));
    if (impl_->state != nullptr) impl_->StopPart(std::string(part), true);
}

bool LuaMod::PartRunning(std::string_view part) const { return impl_->running_parts.contains(part); }

std::string LuaMod::PartError(std::string_view part) const {
    const auto found = impl_->part_errors.find(part);
    return found == impl_->part_errors.end() ? std::string{} : found->second;
}

bool LuaMod::OwnsHook(std::string_view owner) const {
    return std::ranges::any_of(impl_->script_overrides, [&](const auto &entry) { return entry.second.owner == owner; });
}

ApiCallResult LuaMod::GetQuests(std::string_view owner) const {
    if (impl_ == nullptr || impl_->state == nullptr) {
        return {ApiCallStatus::Failed, nullptr, "quest owner is not loaded"};
    }
    Impl::OwnerScope scope(*impl_, owner);
    return impl_->GetQuests(Json::array());
}

ApiCallResult LuaMod::GetSkills(std::string_view owner) const {
    if (impl_ == nullptr || impl_->state == nullptr) {
        return {ApiCallStatus::Failed, nullptr, "skill owner is not loaded"};
    }
    Impl::OwnerScope scope(*impl_, owner);
    return impl_->GetSkills(Json::array());
}

ApiCallResult LuaMod::InvokeFunction(std::string_view owner, std::string_view function_id, const Json &arguments) {
    if (impl_ == nullptr || impl_->state == nullptr) {
        return {ApiCallStatus::Failed, nullptr, "script function owner is not loaded"};
    }
    Impl::OwnerScope scope(*impl_, owner);
    return impl_->InvokeScriptFunction(function_id, arguments);
}

ApiCallResult LuaMod::ListFunctions(std::string_view owner) const {
    if (impl_ == nullptr || impl_->state == nullptr) {
        return {ApiCallStatus::Failed, nullptr, "script function owner is not loaded"};
    }
    Impl::OwnerScope scope(*impl_, owner);
    return impl_->ListOwnScriptFunctions();
}

std::optional<ApiCallResult> LuaMod::InvokeScriptOverride(std::string_view hook_id, const Json &payload) {
    if (impl_ == nullptr || impl_->state == nullptr) return std::nullopt;
    const auto found = impl_->script_overrides.find(hook_id);
    if (found == impl_->script_overrides.end()) return std::nullopt;
    try {
        if (payload.dump().size() > kApiValueBytesLimit) {
            return ApiCallResult{ApiCallStatus::InvalidArgument, nullptr,
                                 "native script hook context exceeds 64 KiB"};
        }
    } catch (const std::exception &) {
        return ApiCallResult{ApiCallStatus::InvalidArgument, nullptr,
                             "native script hook context is not valid JSON"};
    }
    Impl::OwnerScope scope(*impl_, found->second.owner);
    lua_rawgeti(impl_->state, LUA_REGISTRYINDEX, found->second.reference);
    PushJson(impl_->state, payload);
    std::string error;
    if (!impl_->ProtectedCall(1, 1, error)) {
        return ApiCallResult{ApiCallStatus::Failed, nullptr, "native script override callback failed: " + error};
    }
    Json result;
    std::set<const void *> active;
    std::size_t nodes = 0;
    if (!ReadApiJson(impl_->state, -1, result, error, 0, nodes, active)) {
        lua_pop(impl_->state, 1);
        return ApiCallResult{ApiCallStatus::Failed, nullptr,
                             "native script override returned an invalid value: " + error};
    }
    lua_pop(impl_->state, 1);
    try {
        if (result.dump().size() > kApiValueBytesLimit) {
            return ApiCallResult{ApiCallStatus::Failed, nullptr, "native script override result exceeds 64 KiB"};
        }
    } catch (const std::exception &) {
        return ApiCallResult{ApiCallStatus::Failed, nullptr, "native script override result is not valid JSON"};
    }
    return ApiCallResult{ApiCallStatus::Ok, std::move(result), {}};
}

ApiCallResult LuaMod::CallApi(std::string_view path, const Json &arguments) {
    if (impl_ == nullptr || impl_->state == nullptr) {
        return {ApiCallStatus::Failed, nullptr, "native API caller is not loaded"};
    }
    return impl_->InvokeApi(path, arguments);
}

} // namespace moonmod

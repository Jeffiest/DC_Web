#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <set>
#include <optional>
#include <string>
#include <string_view>

#include "moonmod/manifest.hpp"
#include "moonmod/runtime.hpp"

namespace moonmod {

class LuaMod {
public:
    // setting_reader(owner, setting id): owner is the mod, or the pack part whose code asks.
    LuaMod(const RuntimeConfig &config, const ModManifest &manifest,
           std::function<Json(std::string_view, std::string_view)> setting_reader);
    ~LuaMod();
    LuaMod(const LuaMod &) = delete;
    LuaMod &operator=(const LuaMod &) = delete;

    bool Load(std::string &error);
    void Unload();
    // owner: only that owner's callbacks (a pack's part, or the mod itself); empty: everyone's.
    void Emit(std::string_view event_name, const Json &payload, std::string_view owner = {});
    ApiCallResult GetQuests(std::string_view owner = {}) const;
    ApiCallResult GetSkills(std::string_view owner = {}) const;
    ApiCallResult InvokeFunction(std::string_view owner, std::string_view function_id, const Json &arguments);
    ApiCallResult ListFunctions(std::string_view owner = {}) const;

    // A merged pack's parts (manifest "parts"): which run at Load, and starting or stopping one while the pack runs.
    void SetEnabledParts(std::set<std::string, std::less<>> parts);
    bool StartPart(std::string_view part, std::string &error);
    void StopPart(std::string_view part);
    bool PartRunning(std::string_view part) const;
    std::string PartError(std::string_view part) const;
    bool OwnsHook(std::string_view owner) const;
    std::optional<ApiCallResult> InvokeScriptOverride(std::string_view hook_id, const Json &payload);
    ApiCallResult CallApi(std::string_view path, const Json &arguments);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moonmod

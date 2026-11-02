#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>

#include "moonmod/manifest.hpp"
#include "moonmod/runtime.hpp"

namespace moonmod {

class LuaMod {
public:
    LuaMod(const RuntimeConfig &config, const ModManifest &manifest,
           std::function<Json(std::string_view)> setting_reader);
    ~LuaMod();
    LuaMod(const LuaMod &) = delete;
    LuaMod &operator=(const LuaMod &) = delete;

    bool Load(std::string &error);
    void Unload();
    void Emit(std::string_view event_name, const Json &payload);
    ApiCallResult GetQuests() const;
    ApiCallResult GetSkills() const;
    ApiCallResult InvokeFunction(std::string_view function_id, const Json &arguments);
    ApiCallResult ListFunctions() const;
    std::optional<ApiCallResult> InvokeScriptOverride(std::string_view hook_id, const Json &payload);
    ApiCallResult CallApi(std::string_view path, const Json &arguments);

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moonmod

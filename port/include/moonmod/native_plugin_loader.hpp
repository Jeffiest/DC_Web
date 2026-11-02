#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "moonmod/api.hpp"
#include "moonmod/native_plugin.h"

namespace moonmod {

struct NativePluginLoadConfig {
    std::filesystem::path library;
    std::string plugin_id;
    std::string game_id;
    std::string build_id = "development";
    std::string platform;
    std::vector<std::string> manifest_permissions;
    std::vector<ApiCapability> api_capabilities;
    ApiCallHandler api_handler;
    std::function<void(std::string_view level, std::string_view mod_id, std::string_view message)> log;
};

class NativePluginInstance {
public:
    NativePluginInstance();
    ~NativePluginInstance();
    NativePluginInstance(const NativePluginInstance &) = delete;
    NativePluginInstance &operator=(const NativePluginInstance &) = delete;

    bool Load(NativePluginLoadConfig config, std::string &error);
    void Publish(std::string_view event_name, const Json &payload, std::uint64_t frame);
    void Unload();
    bool Loaded() const;
    std::string_view PluginId() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace moonmod

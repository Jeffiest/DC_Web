#pragma once

#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#pragma push_macro("B")
#pragma push_macro("R")
#pragma push_macro("G")
#pragma push_macro("A")
#undef B
#undef R
#undef G
#undef A
#include <nlohmann/json.hpp>
#pragma pop_macro("A")
#pragma pop_macro("G")
#pragma pop_macro("R")
#pragma pop_macro("B")

namespace moonmod {

using Json = nlohmann::json;

enum class SettingType { Boolean, Integer, Number, String, Choice };

struct SettingDefinition {
    std::string id;
    std::string name;
    std::string description;
    SettingType type = SettingType::Boolean;
    Json        default_value = false;
    double      minimum = 0.0;
    double      maximum = 1.0;
    double      step = 1.0;
    std::vector<std::string> choices;
};

struct ModManifest {
    std::string id;
    std::string name;
    std::string version;
    std::string author;
    std::string description;
    std::string entry;
    int         schema_version = 1;
    int         framework_api = 1;
    std::vector<std::string> games;
    std::vector<std::string> dependencies;
    std::vector<std::string> optional_dependencies;
    std::vector<std::string> load_after;
    std::vector<std::string> load_before;
    std::vector<std::string> permissions;
    std::vector<SettingDefinition> settings;
    std::filesystem::path package_root;
    std::filesystem::path entry_file;
    std::filesystem::path native_library;
};

std::optional<ModManifest> ReadManifest(const std::filesystem::path &package_root, std::string &error);
bool                      IsValidModId(std::string_view id);
std::string               SettingTypeName(SettingType type);
std::string               SettingValueText(const SettingDefinition &setting, const Json &value);

} // namespace moonmod

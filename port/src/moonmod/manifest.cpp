#include "moonmod/manifest.hpp"

#include "moonmod/api.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <format>
#include <fstream>
#include <initializer_list>
#include <limits>
#include <regex>
#include <set>
#include <string_view>



namespace moonmod {
namespace {

constexpr std::size_t kManifestLimit = 256 * 1024;

bool IsString(const Json &value, std::size_t limit) {
    return value.is_string() && value.get_ref<const std::string &>().size() <= limit;
}

bool ReadString(const Json &object, const char *key, std::string &out, std::size_t limit, bool required,
                std::string &error) {
    const auto found = object.find(key);
    if (found == object.end()) {
        if (!required) {
            return true;
        }
        error = std::string("missing required field '") + key + "'";
        return false;
    }
    if (!IsString(*found, limit)) {
        error = std::string("field '") + key + "' must be a string no longer than " + std::to_string(limit) + " bytes";
        return false;
    }
    out = found->get<std::string>();
    if (required && out.empty()) {
        error = std::string("field '") + key + "' cannot be empty";
        return false;
    }
    return true;
}

bool ReadStringList(const Json &object, const char *key, std::vector<std::string> &out, std::size_t max_count,
                    bool required, std::string &error) {
    const auto found = object.find(key);
    if (found == object.end()) {
        if (!required) {
            return true;
        }
        error = std::string("missing required field '") + key + "'";
        return false;
    }
    if (!found->is_array() || found->size() > max_count) {
        error = std::string("field '") + key + "' must be an array with at most " + std::to_string(max_count) + " entries";
        return false;
    }
    std::set<std::string> unique;
    for (const Json &entry : *found) {
        if (!IsString(entry, 96)) {
            error = std::string("field '") + key + "' contains a non-string or oversized value";
            return false;
        }
        const std::string value = entry.get<std::string>();
        if (value.empty() || !unique.insert(value).second) {
            error = std::string("field '") + key + "' contains an empty or duplicate value";
            return false;
        }
        out.push_back(value);
    }
    return true;
}

bool RejectUnknownFields(const Json &object, std::initializer_list<std::string_view> allowed, std::string &error) {
    for (auto item = object.begin(); item != object.end(); ++item) {
        if (std::ranges::find(allowed, item.key()) == allowed.end()) {
            error = "unknown field '" + item.key() + "'";
            return false;
        }
    }
    return true;
}

std::optional<SettingType> ParseSettingType(std::string_view value) {
    if (value == "boolean") return SettingType::Boolean;
    if (value == "integer") return SettingType::Integer;
    if (value == "number") return SettingType::Number;
    if (value == "string") return SettingType::String;
    if (value == "choice") return SettingType::Choice;
    return std::nullopt;
}

bool ValidSettingDefault(const SettingDefinition &setting) {
    switch (setting.type) {
        case SettingType::Boolean:
            return setting.default_value.is_boolean();
        case SettingType::Integer:
            return setting.default_value.is_number_integer() && setting.minimum <= setting.maximum && setting.step > 0.0;
        case SettingType::Number:
            return setting.default_value.is_number() && std::isfinite(setting.default_value.get<double>()) &&
                   setting.minimum <= setting.maximum && setting.step > 0.0;
        case SettingType::String:
            return setting.default_value.is_string() && setting.default_value.get_ref<const std::string &>().size() <= 256;
        case SettingType::Choice:
            return setting.default_value.is_string() &&
                   std::ranges::find(setting.choices, setting.default_value.get<std::string>()) != setting.choices.end();
    }
    return false;
}

bool ReadSettings(const Json &object, std::vector<SettingDefinition> &settings, std::string &error) {
    const auto found = object.find("settings");
    if (found == object.end()) {
        return true;
    }
    if (!found->is_array() || found->size() > 128) {
        error = "field 'settings' must be an array with at most 128 entries";
        return false;
    }
    std::set<std::string> ids;
    for (const Json &value : *found) {
        if (!value.is_object()) {
            error = "each settings entry must be an object";
            return false;
        }
        if (!RejectUnknownFields(value, {"id", "name", "description", "type", "default", "min", "max", "step", "choices"}, error)) {
            return false;
        }
        SettingDefinition setting;
        if (!ReadString(value, "id", setting.id, 64, true, error) ||
            !ReadString(value, "name", setting.name, 96, true, error) ||
            !ReadString(value, "description", setting.description, 512, false, error)) {
            return false;
        }
        static const std::regex kSettingIdPattern("[a-z][a-z0-9._-]{0,63}");
        if (!std::regex_match(setting.id, kSettingIdPattern) || !ids.insert(setting.id).second) {
            error = "setting ids must be unique lowercase ids beginning with a letter";
            return false;
        }
        std::string type;
        if (!ReadString(value, "type", type, 16, true, error)) {
            return false;
        }
        const auto parsed = ParseSettingType(type);
        if (!parsed) {
            error = "setting '" + setting.id + "' has an unknown type";
            return false;
        }
        setting.type = *parsed;
        const auto default_value = value.find("default");
        if (default_value == value.end()) {
            error = "setting '" + setting.id + "' is missing its default";
            return false;
        }
        setting.default_value = *default_value;
        const bool integer = setting.type == SettingType::Integer;
        const double low_default = integer ? static_cast<double>(std::numeric_limits<std::int64_t>::min())
                                           : std::numeric_limits<double>::lowest();
        const double high_default = integer
                                        ? std::nextafter(static_cast<double>(std::numeric_limits<std::int64_t>::max()), 0.0)
                                        : std::numeric_limits<double>::max();
        auto read_number = [&](const char *key, double &out, double fallback) {
            const auto number = value.find(key);
            out = fallback;
            if (number == value.end()) return true;
            if (!number->is_number() || !std::isfinite(number->get<double>())) return false;
            out = number->get<double>();
            return true;
        };
        if (!read_number("min", setting.minimum, low_default) || !read_number("max", setting.maximum, high_default) ||
            !read_number("step", setting.step, 1.0)) {
            error = "setting '" + setting.id + "' has a non-finite numeric bound";
            return false;
        }
        if (integer && (std::trunc(setting.minimum) != setting.minimum ||
                        std::trunc(setting.maximum) != setting.maximum ||
                        static_cast<long double>(setting.minimum) <
                            static_cast<long double>(std::numeric_limits<std::int64_t>::min()) ||
                        static_cast<long double>(setting.maximum) >
                            static_cast<long double>(std::numeric_limits<std::int64_t>::max()))) {
            error = "integer setting '" + setting.id + "' bounds must be integral and fit a signed 64-bit value";
            return false;
        }
        if (setting.type == SettingType::Choice) {
            if (!ReadStringList(value, "choices", setting.choices, 32, true, error) || setting.choices.empty()) {
                if (error.empty()) error = "choice setting '" + setting.id + "' must have at least one choice";
                return false;
            }
        }
        if (!ValidSettingDefault(setting)) {
            error = "setting '" + setting.id + "' has a default that does not match its type or bounds";
            return false;
        }
        if ((setting.type == SettingType::Integer || setting.type == SettingType::Number) &&
            (setting.default_value.get<double>() < setting.minimum || setting.default_value.get<double>() > setting.maximum)) {
            error = "setting '" + setting.id + "' default is outside its range";
            return false;
        }
        settings.push_back(std::move(setting));
    }
    return true;
}

bool SafeRelativeEntry(std::string_view entry) {
    if (entry.empty() || entry.size() > 128 || entry.find(':') != std::string_view::npos ||
        entry.find('\\') != std::string_view::npos || entry.front() == '/' || entry.front() == '.') {
        return false;
    }
    const std::filesystem::path path(entry);
    if (path.is_absolute()) return false;
    for (const auto &part : path) {
        if (part == "." || part == "..") return false;
    }
    return path.extension() == ".lua";
}

bool SafeRelativeNativeLibrary(std::string_view entry) {
    if (entry.empty() || entry.size() > 192 || entry.find(':') != std::string_view::npos ||
        entry.find('\\') != std::string_view::npos || entry.front() == '/' || entry.front() == '.') {
        return false;
    }
    const std::filesystem::path path(entry);
    if (path.is_absolute()) return false;
    for (const auto &part : path) {
        if (part == "." || part == "..") return false;
    }
    const auto extension = path.extension().string();
    return extension == ".dll" || extension == ".so" || extension == ".dylib";
}

bool IsWithin(const std::filesystem::path &root, const std::filesystem::path &path) {
    const auto relative = path.lexically_relative(root);
    if (relative.empty() || relative.is_absolute()) return false;
    const auto first = relative.begin();
    return first != relative.end() && *first != "..";
}

} // namespace

bool IsValidModId(std::string_view id) {
    if (id.size() < 3 || id.size() > 64) return false;
    if (!(id.front() >= 'a' && id.front() <= 'z') && !(id.front() >= '0' && id.front() <= '9')) return false;
    return std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

std::optional<ModManifest> ReadManifest(const std::filesystem::path &package_root, std::string &error) {
    error.clear();
    std::error_code ec;
    const auto canonical_root = std::filesystem::canonical(package_root, ec);
    if (ec || !std::filesystem::is_directory(canonical_root, ec) || ec) {
        error = "package directory is missing or unreadable";
        return std::nullopt;
    }
    const auto manifest_file = std::filesystem::canonical(canonical_root / "mod.json", ec);
    if (ec || !IsWithin(canonical_root, manifest_file) || !std::filesystem::is_regular_file(manifest_file, ec) || ec) {
        error = "mod.json is missing";
        return std::nullopt;
    }
    const auto size = std::filesystem::file_size(manifest_file, ec);
    if (ec || size == 0 || size > kManifestLimit) {
        error = "mod.json is empty, unreadable, or larger than 256 KiB";
        return std::nullopt;
    }
    Json document;
    try {
        std::ifstream input(manifest_file, std::ios::binary);
        if (!input) {
            error = "mod.json could not be opened";
            return std::nullopt;
        }
        input >> document;
    } catch (const std::exception &exception) {
        error = std::string("mod.json could not be parsed: ") + exception.what();
        return std::nullopt;
    }
    if (!document.is_object()) {
        error = "mod.json must contain one JSON object";
        return std::nullopt;
    }
    if (!RejectUnknownFields(document, {"schema_version", "id", "name", "version", "author", "description",
                                        "framework_api", "games", "entry", "dependencies", "optional_dependencies",
                                        "load_after", "load_before", "permissions", "settings", "native_extension", "coop"},
                            error)) {
        return std::nullopt;
    }

    ModManifest manifest;
    manifest.package_root = canonical_root;
    const auto schema = document.find("schema_version");
    if (schema == document.end() || !schema->is_number_integer() || schema->get<int>() != 1) {
        error = "schema_version must be 1";
        return std::nullopt;
    }
    if (!ReadString(document, "id", manifest.id, 64, true, error) ||
        !ReadString(document, "name", manifest.name, 96, true, error) ||
        !ReadString(document, "version", manifest.version, 64, true, error) ||
        !ReadString(document, "author", manifest.author, 96, false, error) ||
        !ReadString(document, "description", manifest.description, 1024, false, error) ||
        !ReadString(document, "entry", manifest.entry, 128, true, error) ||
        !ReadString(document, "coop", manifest.coop, 16, false, error)) {
        return std::nullopt;
    }
    // Online co-op: "required" (the default) every player has the mod at the host's version and the host's settings;
    // "host_only" only the host needs it; "local" each player's own, never asked for.
    if (manifest.coop.empty()) manifest.coop = "required";
    if (manifest.coop != "required" && manifest.coop != "host_only" && manifest.coop != "local") {
        error = "coop must be required, host_only, or local";
        return std::nullopt;
    }
    if (!IsValidModId(manifest.id)) {
        error = "id must be 3-64 lowercase letters, digits, dots, underscores, or hyphens";
        return std::nullopt;
    }
    static const std::regex kSemverPattern("[0-9]+\\.[0-9]+\\.[0-9]+([+-][A-Za-z0-9.-]+)?");
    if (!std::regex_match(manifest.version, kSemverPattern)) {
        error = "version must use semantic version form MAJOR.MINOR.PATCH";
        return std::nullopt;
    }
    if (!SafeRelativeEntry(manifest.entry)) {
        error = "entry must be a relative .lua file inside the package";
        return std::nullopt;
    }
    const auto api = document.find("framework_api");
    if (api == document.end() || !api->is_number_integer() || api->get<int>() < 1) {
        error = "framework_api must be a positive integer";
        return std::nullopt;
    }
    manifest.framework_api = api->get<int>();
    if (!ReadStringList(document, "games", manifest.games, 2, true, error) ||
        !ReadStringList(document, "dependencies", manifest.dependencies, 64, false, error) ||
        !ReadStringList(document, "optional_dependencies", manifest.optional_dependencies, 64, false, error) ||
        !ReadStringList(document, "load_after", manifest.load_after, 64, false, error) ||
        !ReadStringList(document, "load_before", manifest.load_before, 64, false, error) ||
        !ReadStringList(document, "permissions", manifest.permissions, 64, false, error) ||
        !ReadSettings(document, manifest.settings, error)) {
        return std::nullopt;
    }
    if (const auto native = document.find("native_extension"); native != document.end()) {
        if (!native->is_object() || !RejectUnknownFields(*native, {"library"}, error)) {
            if (error.empty()) error = "native_extension must be an object containing a library path";
            return std::nullopt;
        }
        std::string library;
        if (!ReadString(*native, "library", library, 192, true, error) || !SafeRelativeNativeLibrary(library)) {
            if (error.empty()) error = "native_extension.library must be a safe relative .dll, .so, or .dylib path";
            return std::nullopt;
        }
        if (std::ranges::find(manifest.permissions, "native.extension") == manifest.permissions.end()) {
            error = "native_extension requires the native.extension manifest permission";
            return std::nullopt;
        }
        std::error_code native_error;
        manifest.native_library = std::filesystem::canonical(canonical_root / std::filesystem::path(library), native_error);
        if (native_error || !IsWithin(canonical_root, manifest.native_library) ||
            !std::filesystem::is_regular_file(manifest.native_library, native_error) || native_error) {
            error = "native extension library is missing or escapes the package";
            return std::nullopt;
        }
        const auto native_size = std::filesystem::file_size(manifest.native_library, native_error);
        if (native_error || native_size == 0 || native_size > 256 * 1024 * 1024) {
            error = "native extension library is empty, unreadable, or larger than 256 MiB";
            return std::nullopt;
        }
    } else if (std::ranges::find(manifest.permissions, "native.extension") != manifest.permissions.end()) {
        error = "native.extension permission requires a native_extension library declaration";
        return std::nullopt;
    }
    for (const std::string &permission : manifest.permissions) {
        if (!IsKnownPermission(permission)) {
            error = "unsupported permission '" + permission + "' for framework API major 1";
            return std::nullopt;
        }
    }
    if (manifest.games.empty()) {
        error = "games must contain at least one supported game id";
        return std::nullopt;
    }
    for (const std::string &game : manifest.games) {
        if (game != "chronicle" && game != "chronicle-two") {
            error = "games may contain only 'chronicle' and 'chronicle-two'";
            return std::nullopt;
        }
    }
    for (const auto *ids : {&manifest.dependencies, &manifest.optional_dependencies,
                            &manifest.load_after, &manifest.load_before}) {
        for (const std::string &id : *ids) {
            if (!IsValidModId(id)) {
                error = "dependency and load-order references must use valid mod ids";
                return std::nullopt;
            }
        }
    }
    manifest.entry_file = std::filesystem::canonical(canonical_root / std::filesystem::path(manifest.entry), ec);
    if (ec || !IsWithin(canonical_root, manifest.entry_file) || !std::filesystem::is_regular_file(manifest.entry_file, ec) || ec) {
        error = "entry script is missing";
        return std::nullopt;
    }
    const auto entry_size = std::filesystem::file_size(manifest.entry_file, ec);
    if (ec || entry_size == 0 || entry_size > 1024 * 1024) {
        error = "entry script is empty, unreadable, or larger than 1 MiB";
        return std::nullopt;
    }
    return manifest;
}

std::string SettingTypeName(SettingType type) {
    switch (type) {
        case SettingType::Boolean: return "boolean";
        case SettingType::Integer: return "integer";
        case SettingType::Number: return "number";
        case SettingType::String: return "string";
        case SettingType::Choice: return "choice";
    }
    return "unknown";
}

std::string SettingValueText(const SettingDefinition &setting, const Json &value) {
    switch (setting.type) {
        case SettingType::Boolean:
            return value.is_boolean() && value.get<bool>() ? "On" : "Off";
        case SettingType::Integer:
            return value.is_number_integer() ? std::to_string(value.get<std::int64_t>()) : setting.default_value.dump();
        case SettingType::Number:
            return value.is_number() ? std::format("{:.6g}", value.get<double>()) : setting.default_value.dump();
        case SettingType::String:
            return value.is_string() ? value.get<std::string>() : setting.default_value.get<std::string>();
        case SettingType::Choice:
            return value.is_string() ? value.get<std::string>() : setting.default_value.get<std::string>();
    }
    return {};
}

} // namespace moonmod

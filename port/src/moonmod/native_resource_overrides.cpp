#include "moonmod/native_resource_overrides.hpp"
#include "moonmod/symlinks.hpp"

#include <algorithm>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <map>
#include <mutex>
#include <ranges>
#include <string>
#include <system_error>



namespace moonmod {
namespace {

constexpr std::size_t kPerModOverrideLimit = 64;
constexpr std::size_t kGlobalOverrideLimit = 512;
constexpr std::uintmax_t kOverrideFileSizeLimit = 128ull * 1024ull * 1024ull;
constexpr std::uintmax_t kMapAliasOverrideSizeLimit = 8ull * 1024ull * 1024ull;

struct OverrideRecord {
    std::string owner;
    std::string resource_path;
    std::filesystem::path file;
    std::uintmax_t size = 0;
    bool persistent = false;
    bool pinned_by_alias = false;
    std::filesystem::file_time_type written{};
};

struct ResourcePathAlias {
    std::string owner;
    std::string target_prefix;
    std::string source_prefix;
};

std::mutex g_override_mutex;
std::map<std::string, OverrideRecord, std::less<>> g_overrides;
std::map<std::string, ResourcePathAlias, std::less<>> g_path_aliases;

bool IsPathBelowPrefix(std::string_view path, std::string_view prefix) {
    return path.size() > prefix.size() && path.starts_with(prefix) && path[prefix.size()] == '/';
}

bool IsChronicleMapDirectory(std::string_view path, bool target) {
    if (path.size() != 9 || !path.starts_with("gedit/s") || path[7] < '0' || path[7] > '9' ||
        path[8] < '0' || path[8] > '9') {
        return false;
    }
    const int directory_id = (path[7] - '0') * 10 + (path[8] - '0');
    return target ? directory_id >= 60 && directory_id <= 69 : directory_id < 70;
}

bool HasOnlyFields(const Json &object, std::initializer_list<std::string_view> fields) {
    for (auto field = object.begin(); field != object.end(); ++field) {
        if (std::ranges::find(fields, field.key()) == fields.end()) return false;
    }
    return true;
}

bool NormalizeGamePath(std::string_view input, std::string &normalized) {
    if (input.empty() || input.size() > 240) return false;
    normalized.clear();
    normalized.reserve(input.size());
    std::string component;
    for (unsigned char character : input) {
        if (character < 0x21 || character > 0x7e || character == ':' || character == '*' || character == '?' ||
            character == '"' || character == '<' || character == '>' || character == '|') {
            return false;
        }
        if (character == '/' || character == '\\') {
            if (component.empty() || component == "." || component == "..") return false;
            normalized.push_back('/');
            component.clear();
            continue;
        }
        const char folded = character >= 'A' && character <= 'Z'
                                ? static_cast<char>(character + ('a' - 'A'))
                                : static_cast<char>(character);
        normalized.push_back(folded);
        component.push_back(folded);
    }
    return !component.empty() && component != "." && component != ".." && normalized.front() != '/' &&
           normalized.back() != '/';
}

bool ValidResourcePath(std::string_view input, std::filesystem::path &relative) {
    if (input.size() < 11 || input.size() > 240 || !input.starts_with("resources/") ||
        std::ranges::any_of(input, [](unsigned char character) {
            return character < 0x20 || character == '\\' || character == ':' || character == '*' ||
                   character == '?' || character == '"' || character == '<' || character == '>' ||
                   character == '|';
        })) {
        return false;
    }
    relative = std::filesystem::path(input);
    return !relative.is_absolute() && !relative.has_root_name() && !relative.has_root_directory() &&
           !std::ranges::any_of(relative, [](const std::filesystem::path &part) {
               return part.empty() || part == "." || part == "..";
           });
}

} // namespace

ApiCallResult RegisterNativeResourceOverride(std::string_view owner, const std::filesystem::path &package_root,
                                             const Json &arguments) {
    if (owner.empty() || !arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object() ||
        arguments[0].size() != 2 || !HasOnlyFields(arguments[0], {"path", "game_path"})) {
        return {ApiCallStatus::InvalidArgument, nullptr,
                "expected {path = package resource path, game_path = native asset path}"};
    }
    const Json &definition = arguments[0];
    const auto path_field = definition.find("path");
    const auto game_path_field = definition.find("game_path");
    if (path_field == definition.end() || !path_field->is_string() || game_path_field == definition.end() ||
        !game_path_field->is_string()) {
        return {ApiCallStatus::InvalidArgument, nullptr,
                "path and game_path must be strings"};
    }
    const std::string &resource_path = path_field->get_ref<const std::string &>();
    const std::string &raw_game_path = game_path_field->get_ref<const std::string &>();
    std::filesystem::path relative_path;
    std::string game_path;
    if (!ValidResourcePath(resource_path, relative_path)) {
        return {ApiCallStatus::InvalidArgument, nullptr,
                "path must be a safe package-relative file below resources/"};
    }
    if (!NormalizeGamePath(raw_game_path, game_path)) {
        return {ApiCallStatus::InvalidArgument, nullptr,
                "game_path must be a normalized relative native asset path without traversal segments"};
    }

    std::error_code error;
    const std::filesystem::path canonical_root = std::filesystem::canonical(package_root, error);
    if (error || moonmod::ContainsSymlinkBelow(package_root, package_root.parent_path())) {
        return {ApiCallStatus::Failed, nullptr, "mod package path is missing or contains a symbolic link"};
    }
    const std::filesystem::path candidate = package_root / relative_path;
    if (moonmod::ContainsSymlinkBelow(candidate, package_root)) {
        return {ApiCallStatus::Failed, nullptr, "resource path contains a symbolic link"};
    }
    const std::filesystem::path resolved = std::filesystem::canonical(candidate, error);
    if (error) return {ApiCallStatus::Failed, nullptr, "resource file could not be resolved"};
    const std::filesystem::path within_package = resolved.lexically_relative(canonical_root);
    if (within_package.empty() || within_package.is_absolute() ||
        (!within_package.empty() && *within_package.begin() == "..")) {
        return {ApiCallStatus::Failed, nullptr, "resource resolves outside this mod package"};
    }
    if (!std::filesystem::is_regular_file(resolved, error) || error) {
        return {ApiCallStatus::Failed, nullptr, "resource path is not a regular file"};
    }
    const std::uintmax_t size = std::filesystem::file_size(resolved, error);
    if (error) return {ApiCallStatus::Failed, nullptr, "resource size could not be read"};
    if (size == 0 || size > kOverrideFileSizeLimit ||
        size > static_cast<std::uintmax_t>(std::numeric_limits<std::int32_t>::max())) {
        return {ApiCallStatus::InvalidArgument, nullptr,
                "native replacement resources must contain 1 byte through 128 MiB"};
    }

    std::lock_guard lock(g_override_mutex);
    const auto path_alias = std::ranges::find_if(g_path_aliases, [&game_path](const auto &entry) {
        return IsPathBelowPrefix(game_path, entry.first);
    });
    if (path_alias != g_path_aliases.end()) {
        if (path_alias->second.owner != owner) {
            return {ApiCallStatus::PermissionDenied, nullptr, "registered map assets belong to another mod"};
        }
        if (size > kMapAliasOverrideSizeLimit) {
            return {ApiCallStatus::InvalidArgument, nullptr, "map assets cannot exceed 8 MiB"};
        }
    }
    const auto existing = g_overrides.find(game_path);
    if (existing != g_overrides.end() && existing->second.owner != owner) {
        return {ApiCallStatus::PermissionDenied, nullptr,
                "another mod already owns an override for this native asset path"};
    }
    if (existing == g_overrides.end()) {
        const std::size_t owned = std::ranges::count_if(g_overrides, [owner](const auto &entry) {
            return entry.second.owner == owner;
        });
        if (owned >= kPerModOverrideLimit || g_overrides.size() >= kGlobalOverrideLimit) {
            return {ApiCallStatus::InvalidArgument, nullptr, "native resource override limit reached"};
        }
    }
    const bool pinned_by_alias = path_alias != g_path_aliases.end() ||
                                 (existing != g_overrides.end() && existing->second.pinned_by_alias);
    const bool persistent = existing != g_overrides.end() && existing->second.persistent;
    std::error_code time_error;
    const std::filesystem::file_time_type written = std::filesystem::last_write_time(resolved, time_error);
    g_overrides.insert_or_assign(
        game_path, OverrideRecord{std::string(owner), within_package.generic_string(), resolved, size,
                                  persistent, pinned_by_alias, written});
    return {ApiCallStatus::Ok,
            {{"game_path", game_path}, {"path", within_package.generic_string()}, {"size", size},
             {"status", "registered"}},
            {}};
}

ApiCallResult RemoveNativeResourceOverride(std::string_view owner, const Json &arguments) {
    if (owner.empty() || !arguments.is_array() || arguments.size() != 1 || !arguments[0].is_object() ||
        arguments[0].size() != 1 || !HasOnlyFields(arguments[0], {"game_path"}) ||
        !arguments[0].contains("game_path") || !arguments[0]["game_path"].is_string()) {
        return {ApiCallStatus::InvalidArgument, nullptr, "expected {game_path = native asset path}"};
    }
    std::string game_path;
    if (!NormalizeGamePath(arguments[0]["game_path"].get_ref<const std::string &>(), game_path)) {
        return {ApiCallStatus::InvalidArgument, nullptr, "game_path is not a valid relative native asset path"};
    }
    std::lock_guard lock(g_override_mutex);
    const auto found = g_overrides.find(game_path);
    if (found == g_overrides.end()) {
        return {ApiCallStatus::Failed, nullptr, "no mod resource override exists for this path"};
    }
    if (found->second.owner != owner) {
        return {ApiCallStatus::PermissionDenied, nullptr, "native resource override belongs to another mod"};
    }
    if (found->second.pinned_by_alias) {
        return {ApiCallStatus::Failed, nullptr, "native resource override is pinned by a registered map"};
    }
    if (found->second.persistent) {
        return {ApiCallStatus::PermissionDenied, nullptr,
                "native resource override is required by a registered native content definition"};
    }
    g_overrides.erase(found);
    return {ApiCallStatus::Ok, {{"game_path", game_path}, {"removed", true}}, {}};
}

std::optional<std::filesystem::path> FindNativeResourceOverride(std::string_view game_path) {
    std::string normalized;
    if (!NormalizeGamePath(game_path, normalized)) return std::nullopt;
    std::lock_guard lock(g_override_mutex);
    const auto found = g_overrides.find(normalized);
    if (found == g_overrides.end()) return std::nullopt;
    return found->second.file;
}

bool NativeResourceOverrideOwnedBy(std::string_view owner, std::string_view game_path) {
    if (owner.empty()) return false;
    std::string normalized;
    if (!NormalizeGamePath(game_path, normalized)) return false;
    std::lock_guard lock(g_override_mutex);
    const auto found = g_overrides.find(normalized);
    return found != g_overrides.end() && found->second.owner == owner;
}

std::vector<std::string> RefreshChangedNativeResourceOverrides() {
    std::vector<std::string> changed;
    std::lock_guard lock(g_override_mutex);
    for (auto &[game_path, record] : g_overrides) {
        std::error_code error;
        const auto written = std::filesystem::last_write_time(record.file, error);
        if (error || written == record.written) continue;
        const std::uintmax_t size = std::filesystem::file_size(record.file, error);
        if (error || size == 0 || size > kOverrideFileSizeLimit) continue;
        record.written = written;
        record.size = size;
        changed.push_back(game_path);
    }
    return changed;
}

std::optional<std::uintmax_t> NativeResourceOverrideSize(std::string_view game_path) {
    std::string normalized;
    if (!NormalizeGamePath(game_path, normalized)) return std::nullopt;
    std::lock_guard lock(g_override_mutex);
    const auto found = g_overrides.find(normalized);
    if (found == g_overrides.end()) return std::nullopt;
    return found->second.size;
}

bool SetNativeResourceOverridePersistent(std::string_view owner, std::string_view game_path, bool persistent) {
    std::string normalized;
    if (!NormalizeGamePath(game_path, normalized)) return false;
    std::lock_guard lock(g_override_mutex);
    const auto found = g_overrides.find(normalized);
    if (found == g_overrides.end() || found->second.owner != owner) return false;
    found->second.persistent = persistent;
    return true;
}

bool RegisterNativeResourcePathAlias(std::string_view owner, std::string_view target_prefix,
                                     std::string_view source_prefix, std::uintmax_t max_override_size,
                                     std::string &error) {
    error.clear();
    std::string target;
    std::string source;
    if (owner.empty() || !NormalizeGamePath(target_prefix, target) || !NormalizeGamePath(source_prefix, source) ||
        !IsChronicleMapDirectory(target, true) || !IsChronicleMapDirectory(source, false) || target == source) {
        error = "map resource aliases require distinct gedit map directories";
        return false;
    }

    std::lock_guard lock(g_override_mutex);
    const auto existing = g_path_aliases.find(target);
    if (existing != g_path_aliases.end() &&
        (existing->second.owner != owner || existing->second.source_prefix != source)) {
        error = "another map registration already owns this map directory";
        return false;
    }
    for (const auto &[path, override_record] : g_overrides) {
        if (!IsPathBelowPrefix(path, target)) continue;
        if (override_record.owner != owner) {
            error = "a map asset path is overridden by another mod: " + path;
            return false;
        }
        if (override_record.size > max_override_size) {
            error = "map asset exceeds the per-file size limit: " + path;
            return false;
        }
    }
    for (auto &[path, override_record] : g_overrides) {
        if (IsPathBelowPrefix(path, target)) override_record.pinned_by_alias = true;
    }
    g_path_aliases.insert_or_assign(target, ResourcePathAlias{std::string(owner), target, source});
    return true;
}

std::optional<std::string> FindNativeResourcePathAlias(std::string_view game_path) {
    std::string normalized;
    if (!NormalizeGamePath(game_path, normalized)) return std::nullopt;
    std::lock_guard lock(g_override_mutex);
    const ResourcePathAlias *best = nullptr;
    for (const auto &[prefix, alias] : g_path_aliases) {
        if (IsPathBelowPrefix(normalized, prefix) && (best == nullptr || prefix.size() > best->target_prefix.size())) {
            best = &alias;
        }
    }
    if (best == nullptr) return std::nullopt;
    return best->source_prefix + normalized.substr(best->target_prefix.size());
}

void RemoveNativeResourceOverridesByOwner(std::string_view owner) {
    std::lock_guard lock(g_override_mutex);
    std::erase_if(g_overrides, [owner](const auto &entry) {
        return entry.second.owner == owner && !entry.second.persistent && !entry.second.pinned_by_alias;
    });
}

void ResetNativeResourceOverridesForTests() {
    std::lock_guard lock(g_override_mutex);
    g_overrides.clear();
    g_path_aliases.clear();
}

} // namespace moonmod

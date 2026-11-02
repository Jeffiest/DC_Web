#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "moonmod/api.hpp"

namespace moonmod {

ApiCallResult RegisterNativeResourceOverride(std::string_view owner, const std::filesystem::path &package_root,
                                             const Json &arguments);
ApiCallResult RemoveNativeResourceOverride(std::string_view owner, const Json &arguments);
std::optional<std::filesystem::path> FindNativeResourceOverride(std::string_view game_path);
bool NativeResourceOverrideOwnedBy(std::string_view owner, std::string_view game_path);
std::optional<std::uintmax_t> NativeResourceOverrideSize(std::string_view game_path);
bool SetNativeResourceOverridePersistent(std::string_view owner, std::string_view game_path, bool persistent);
bool RegisterNativeResourcePathAlias(std::string_view owner, std::string_view target_prefix,
                                     std::string_view source_prefix, std::uintmax_t max_override_size,
                                     std::string &error);
std::optional<std::string> FindNativeResourcePathAlias(std::string_view game_path);
void RemoveNativeResourceOverridesByOwner(std::string_view owner);
// Hot reload: the game paths whose override file was written since it was registered or last
// checked. Their size is read again; the game reads the new bytes the next time it loads the path.
std::vector<std::string> RefreshChangedNativeResourceOverrides();
void ResetNativeResourceOverridesForTests();

} // namespace moonmod

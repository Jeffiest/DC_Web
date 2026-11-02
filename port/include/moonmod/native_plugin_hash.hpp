#pragma once

#include <filesystem>
#include <string>

namespace moonmod {

// Returns an empty string when the file cannot be read or exceeds the trusted-library size limit.
std::string NativePluginFileSha256(const std::filesystem::path &path);

} // namespace moonmod

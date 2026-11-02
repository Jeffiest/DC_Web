#pragma once

#include <filesystem>
#include <system_error>

namespace moonmod {

// Whether a component of path below base is a symbolic link, or can't be inspected: such a link, planted in the
// save folder or a mod's package, could send a read or write somewhere else. base and the folders above it are the
// player's or the system's own and aren't checked: macOS's /var is a link to /private/var, and a home or save folder
// can sit behind a link or junction. A path that leaves base is checked from where it leaves.
inline bool ContainsSymlinkBelow(const std::filesystem::path &path, const std::filesystem::path &base) {
    std::filesystem::path trusted = base.lexically_normal();
    if (!trusted.has_filename()) trusted = trusted.parent_path();
    auto                  next_trusted = trusted.begin();
    bool                  within = !trusted.empty();
    std::filesystem::path current;
    for (const auto &part : path.lexically_normal()) {
        current /= part;
        if (within && next_trusted != trusted.end() && *next_trusted == part) {
            ++next_trusted;
            continue;
        }
        within = false;
        if (part.empty() || part == "." || !current.has_relative_path()) continue;
        std::error_code ec;
        const auto      status = std::filesystem::symlink_status(current, ec);
        if (ec && ec != std::errc::no_such_file_or_directory) return true;
        if (!ec && std::filesystem::is_symlink(status)) return true;
    }
    return false;
}

} // namespace moonmod

#pragma once

#include <filesystem>
#include <system_error>

namespace moonmod {

// The one folder mods are read from (one, so a package never shows up twice):
// - mods/ in the Chronicle checkout the game was built in, the folder holding kazarov.conf above the executable.
//   The MoonMod release adds it there;
// - otherwise mods/ beside the executable, for a build copied somewhere else;
// - otherwise <save>/mods.
inline std::filesystem::path ModsDirectory(const std::filesystem::path &executable_dir,
                                           const std::filesystem::path &save_root) {
    namespace fs = std::filesystem;
    std::error_code error;
    for (fs::path dir = executable_dir; !dir.empty(); dir = dir.parent_path()) {
        if (fs::is_regular_file(dir / "kazarov.conf", error)) {
            if (fs::is_directory(dir / "mods", error)) return dir / "mods";
            break;
        }
        if (dir == dir.parent_path()) break;
    }
    if (!executable_dir.empty() && fs::is_directory(executable_dir / "mods", error)) return executable_dir / "mods";
    return save_root / "mods";
}

} // namespace moonmod

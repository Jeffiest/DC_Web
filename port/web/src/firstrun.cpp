// The web's first run (docs/WEB.md): the page stores what the player chose beside the data directory in the
// browser's storage.
//
// - data.zip, or a data directory with files: nothing to do.
// - disc.iso, a disc image, or disc/, a directory holding DATA.DAT and DATA.HD2: the game reads the disc's
//   files in place (DataArchive::OpenDisc), and the data directory holds only what dcdata writes beside them
//   (normalized/, data.hd2, languages.json and the aliases). Those are made here once, from the files they
//   come from, which are copied into memory, not the browser's storage: making a file there takes seconds on a
//   phone. Only what is made is written there. languages.json, written last, marks it done; until it is
//   there, it is made again from the start.
//
// Progress goes to stdout as "extract: <files> <total files> <bytes> <total bytes>", which the page shows.

#include "platform/firstrun.hpp"

#include <chrono>
#include <cstring>
#include <fcntl.h>
#include <unistd.h>
#include <cstdio>
#include <exception>
#include <string_view>
#include <system_error>
#include <unordered_set>
#include <vector>

#include <emscripten/wasmfs.h>

#include "platform/data_archive.hpp"
#include "platform/paths.hpp"

namespace dc::platform {

namespace fs = std::filesystem;

namespace {

// The files dcdata's Normalize, FillFloorPlates and their helpers read; it must follow them.
bool NeededToNormalize(std::string_view path) {
    for (std::string_view name : {"meswin/mes_tex.pak", "meswin/systeme.bin", "meswin/system14_1.mes",
                                  "dun/pack/teximg2.pac", "gedit/system/editsys.bin", "opdat/usa/fconv.bin",
                                  "titledat/title.pak", "opdat/dungeon/dungeon.pim", "opdat/norn/norn.pak",
                                  "opdat/norn2/norn2.pim", "opdat/toan/toan.pim"}) {
        if (path == name) {
            return true;
        }
    }
    return path.starts_with("commenu/") || path.starts_with("dun/img/us/dname0") ||
           (path.starts_with("gedit/system/esys") && path.ends_with(".pak")) ||
           (path.starts_with("dun/script/") && path.ends_with("/event.stb")) ||
           (path.starts_with("rmdat/rmdat") && path.ends_with(".pak")) ||
           path.find("/dungeon/dunmenu5.pak") != std::string_view::npos;
}

void Report(std::size_t files, std::size_t total_files, std::uint64_t bytes, std::uint64_t total_bytes) {
    std::printf("extract: %zu %zu %llu %llu\n", files, total_files, static_cast<unsigned long long>(bytes),
                static_cast<unsigned long long>(total_bytes));
    std::fflush(stdout);
}

} // namespace

void FirstRunIfNoData(bool) {
    const fs::path &root = PathsDataRoot();
    const fs::path  disc = DataArchiveDisc(root);
    std::error_code error;
    if (disc.empty() || fs::is_regular_file(root / dcdata::kLanguagesFile, error)) {
        return;
    }
    // What a data directory holds is the disc's own copy or an earlier start's; both are made again.
    const fs::path staging = "/prepare";
    fs::remove_all(root, error);
    fs::remove_all(root.parent_path() / "prepare", error); // where an earlier version staged them
    fs::remove_all(staging, error);
    if (int failed = wasmfs_create_directory(staging.c_str(), 0777, wasmfs_create_memory_backend()); failed != 0) {
        std::fprintf(stderr, "cannot make %s in memory: %s\n", staging.c_str(), std::strerror(-failed));
        return;
    }
    std::printf("extract: preparing the game data from %s\n", disc.filename().string().c_str());
    std::fflush(stdout);
    try {
        const dcdata::Archive             archive = dcdata::OpenArchive(disc);
        const std::vector<unsigned char>  hd2 = dcdata::ReadExtent(archive.hd2);
        const std::vector<dcdata::Record> records = dcdata::ParseIndex(hd2);
        std::size_t                       duplicates = 0;
        std::vector<const dcdata::Record *> needed;
        std::uint64_t                       total_bytes = 0;
        for (const dcdata::Record *record : dcdata::Reachable(records, nullptr, &duplicates)) {
            if (NeededToNormalize(record->path)) {
                needed.push_back(record);
                total_bytes += record->size;
            }
        }
        const auto started = std::chrono::steady_clock::now();
        const int  dat = ::open(archive.dat.file.c_str(), O_RDONLY | O_CLOEXEC);
        if (dat < 0) {
            dcdata::Fail("cannot open {}", archive.dat.file.string());
        }
        std::vector<unsigned char> bytes;
        std::uint64_t              done_bytes = 0;
        auto                       last = std::chrono::steady_clock::time_point{};
        for (std::size_t index = 0; index < needed.size(); ++index) {
            const dcdata::Record *record = needed[index];
            bytes.resize(record->size);
            if (!DataArchiveReadAt(dat, archive.dat.offset + record->sector * dcdata::kSector, bytes.data(),
                                   bytes.size())) {
                ::close(dat);
                dcdata::Fail("cannot read {} from the disc", record->path);
            }
            dcdata::WriteFile(staging / record->path, bytes);
            done_bytes += record->size;
            const auto now = std::chrono::steady_clock::now();
            if (index + 1 < needed.size() && now - last >= std::chrono::milliseconds(500)) {
                last = now;
                Report(index + 1, needed.size(), done_bytes, total_bytes);
            }
        }
        ::close(dat);
        Report(needed.size(), needed.size(), done_bytes, total_bytes);
        const auto read = std::chrono::steady_clock::now();

        dcdata::Normalize(staging, records);
        const dcdata::Release release = dcdata::ReleaseOf(hd2, records);
        if (release.blank_floor_plates) {
            dcdata::FillFloorPlates(staging);
        }
        dcdata::WriteFile(staging / "data.hd2", hd2);

        // Kept: what the disc does not hold. The disc's own files are read from it.
        std::unordered_set<std::string> on_disc;
        for (const dcdata::Record &record : records) {
            on_disc.insert(record.path);
        }
        std::vector<fs::path> kept;
        for (fs::recursive_directory_iterator it(staging), end; it != end; ++it) {
            if (it->is_regular_file() &&
                !on_disc.contains(dcdata::FoldPath(it->path().lexically_relative(staging).generic_string()))) {
                kept.push_back(it->path());
            }
        }
        for (const fs::path &path : kept) {
            const fs::path target = root / path.lexically_relative(staging);
            dcdata::WriteFile(target, dcdata::ReadFile(path));
        }
        dcdata::WriteLanguages(root, release);
        const auto seconds = [](auto from, auto to) { return std::chrono::duration<double>(to - from).count(); };
        std::printf("extract: the disc is %s; read %zu files (%.0f MiB) in %.1f s, prepared in %.1f s\n",
                    std::string(release.name).c_str(), needed.size(), done_bytes / 1048576.0,
                    seconds(started, read), seconds(read, std::chrono::steady_clock::now()));
    } catch (const std::exception &failure) {
        // Left on the page over the game, which starts without what failed and tries again next time.
        std::printf("extract: preparing the game data from %s failed: %s\n", disc.filename().string().c_str(),
                    failure.what());
        std::fflush(stdout);
        fs::remove_all(staging, error);
        return;
    }
    fs::remove_all(staging, error);
    std::printf("extract: done\n");
    std::fflush(stdout);
}

} // namespace dc::platform

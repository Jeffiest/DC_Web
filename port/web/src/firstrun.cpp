// The web's first run (docs/WEB.md): the page stores what the player chose beside the data directory in the
// browser's storage, and the game extracts a disc from there itself, as the desktop's first run does.
//
// - data.zip, or a data directory with files: nothing to do.
// - disc.iso, a disc image, or disc/, a directory holding DATA.DAT and DATA.HD2: extracted into the data
//   directory with dcdata, then checked file by file. The disc stays until every file checks out, so an
//   extraction cut short resumes at the next start (dcdata keeps what is already whole); then it goes, so the
//   game's data is not stored twice.
//
// Progress goes to stdout as "extract: <files> <total files> <bytes> <total bytes>", which the page shows.

#include "platform/firstrun.hpp"

#include <chrono>
#include <cstdio>
#include <exception>
#include <system_error>
#include <vector>

#include "platform/paths.hpp"

namespace fs = std::filesystem;

namespace {

bool HasRegularFile(const fs::path &root) {
    std::error_code error;
    if (!fs::is_directory(root, error)) {
        return false;
    }
    auto options = fs::directory_options::skip_permission_denied;
    for (fs::recursive_directory_iterator it(root, options, error), end; !error && it != end; it.increment(error)) {
        std::error_code entry_error;
        if (it->is_regular_file(entry_error)) {
            return true;
        }
    }
    return false;
}

// The jobs one after another: a browser tab gains little from more threads writing the same storage.
void RunInOrder(std::vector<dcdata::ClassJob> jobs) {
    for (dcdata::ClassJob &job : jobs) {
        job.run();
    }
}

void Report(const dcdata::Progress &progress) {
    std::printf("extract: %zu %zu %llu %llu\n", progress.files, progress.total_files,
                static_cast<unsigned long long>(progress.bytes), static_cast<unsigned long long>(progress.total_bytes));
    std::fflush(stdout);
}

} // namespace

void FirstRunIfNoData(bool) {
    const fs::path &root = PathsDataRoot();
    const fs::path  home = root.parent_path();
    std::error_code error;
    fs::path        source;
    for (const fs::path &candidate : {home / "disc.iso", home / "disc"}) {
        if (fs::exists(candidate, error)) {
            source = candidate;
            break;
        }
    }
    // A disc still here means its extraction has not finished, whatever the data directory holds.
    if (source.empty() || fs::is_regular_file(home / "data.zip", error)) {
        return;
    }
    if (HasRegularFile(root)) {
        std::printf("extract: resuming the extraction of %s\n", source.filename().string().c_str());
    }
    try {
        dcdata::Archive archive = dcdata::OpenArchive(source);
        auto            last = std::chrono::steady_clock::time_point{};
        dcdata::Extract(
            archive, root, nullptr,
            [&](const dcdata::Progress &progress) {
                const auto now = std::chrono::steady_clock::now();
                if (now - last >= std::chrono::milliseconds(500) || progress.files == progress.total_files) {
                    last = now;
                    Report(progress);
                }
                return true;
            },
            RunInOrder);
        std::vector<unsigned char>          hd2 = dcdata::ReadExtent(archive.hd2);
        std::vector<dcdata::Record>         records = dcdata::ParseIndex(hd2);
        std::vector<const dcdata::Record *> bad = dcdata::Mismatched(records, root);
        if (!bad.empty()) {
            dcdata::Fail("{} files are missing or the wrong size after extraction, {} among them", bad.size(),
                         bad.front()->path);
        }
    } catch (const std::exception &failure) {
        std::fprintf(stderr, "extracting %s failed: %s\n", source.filename().string().c_str(), failure.what());
        return;
    }
    fs::remove_all(source, error);
    std::printf("extract: done\n");
    std::fflush(stdout);
}

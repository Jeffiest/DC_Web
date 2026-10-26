#include "kazarov/extract_job.hpp"
#include "kazarov_bridge/rust/host_jobs.h"
#include "kazarov_bridge/rust/lib.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <exception>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include "dcdata.hpp"

namespace {
namespace fs = std::filesystem;

std::string Utf8Path(const fs::path &path) {
    const auto bytes = path.u8string();
    return {reinterpret_cast<const char *>(bytes.data()), bytes.size()};
}

unsigned WorkerCount() {
    return std::clamp(std::thread::hardware_concurrency(), 1u, 64u);
}

// Hands the asset classes to the extraction scheduler shared with the other Kazarov tools.
void RunOnKazarov(std::vector<dcdata::ClassJob> jobs) {
    rust::Box<kazarov::JobSet> set = kazarov::new_job_set();
    for (dcdata::ClassJob &job : jobs) {
        kazarov::job_set_add(*set, static_cast<std::uint8_t>(job.asset_class), job.label, job.work,
                             kazarov::make_extract_job(std::move(job.run)));
    }
    kazarov::job_set_run(std::move(set), WorkerCount());
}

void Pack(const fs::path &source, const fs::path &output) {
    const kazarov::PackReport report =
        kazarov::pack_directory_stored(Utf8Path(source), Utf8Path(output), WorkerCount());
    dcdata::Log(stdout, "packed {} files ({} bytes) into {}\n", report.files, report.bytes, output.string());
}

class TextureStage {
public:
    TextureStage() {
        const auto suffix = std::chrono::steady_clock::now().time_since_epoch().count();
        root = fs::temp_directory_path() / ("chronicle-dcdata-textures-" + std::to_string(suffix));
        if (!fs::create_directory(root)) {
            throw std::runtime_error("cannot create texture conversion staging directory");
        }
    }

    ~TextureStage() {
        std::error_code error;
        fs::remove_all(root, error);
    }

    fs::path root;
};

std::vector<unsigned char> ReadBytes(const fs::path &path) {
    std::ifstream stream(path, std::ios::binary);
    if (!stream) {
        throw std::runtime_error("cannot open " + path.string());
    }
    return {std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>()};
}

void MergeTextureAssets(const fs::path &stage, const fs::path &data) {
    const fs::path staged_textures = stage / "data" / "textures";
    const fs::path target_textures = data / "textures";
    std::error_code error;
    if (!fs::exists(staged_textures, error)) {
        if (error) {
            throw std::runtime_error("cannot inspect converted textures: " + error.message());
        }
        return;
    }
    if (!fs::is_directory(staged_textures, error) || error) {
        throw std::runtime_error("converted texture path is not a directory");
    }
    std::unordered_map<std::string, fs::path> existing;
    if (fs::exists(target_textures, error)) {
        if (!fs::is_directory(target_textures, error) || error) {
            throw std::runtime_error("existing texture path is not a directory");
        }
        for (fs::recursive_directory_iterator it(target_textures), end; it != end; ++it) {
            if (it->is_symlink(error) || error) {
                throw std::runtime_error("existing texture tree contains a symbolic link");
            }
            if (it->is_directory(error)) {
                if (error) {
                    throw std::runtime_error("cannot inspect existing texture tree: " + error.message());
                }
                continue;
            }
            if (!it->is_regular_file(error) || error) {
                throw std::runtime_error("existing texture tree contains a non-file entry");
            }
            const fs::path relative = it->path().lexically_relative(target_textures);
            const auto utf8 = relative.generic_u8string();
            const std::string name(utf8.begin(), utf8.end());
            if (!existing.emplace(dcdata::FoldPath(name), it->path()).second) {
                throw std::runtime_error("existing texture paths differ only by ASCII case: " + name);
            }
        }
    } else if (error) {
        throw std::runtime_error("cannot inspect existing texture tree: " + error.message());
    }
    struct PendingTexture {
        fs::path source;
        fs::path relative;
        std::string folded;
    };
    std::vector<PendingTexture> pending;
    std::unordered_map<std::string, bool> planned;
    for (fs::recursive_directory_iterator it(staged_textures), end; it != end; ++it) {
        if (it->is_symlink(error) || error) {
            throw std::runtime_error("converted texture tree contains a symbolic link");
        }
        if (it->is_directory(error)) {
            if (error) {
                throw std::runtime_error("cannot inspect converted texture tree: " + error.message());
            }
            continue;
        }
        if (!it->is_regular_file(error) || error) {
            throw std::runtime_error("converted texture tree contains a non-file entry");
        }
        const fs::path relative = it->path().lexically_relative(staged_textures);
        const auto utf8 = relative.generic_u8string();
        const std::string name(utf8.begin(), utf8.end());
        const std::string folded = dcdata::FoldPath(name);
        if (!dcdata::IsSafeRelative(name) || (!folded.ends_with(".png") && !folded.ends_with(".json"))) {
            throw std::runtime_error("unsafe converted texture path: " + name);
        }
        planned.emplace(folded, true);
        const auto collision = existing.find(folded);
        if (collision != existing.end()) {
            if (ReadBytes(it->path()) != ReadBytes(collision->second)) {
                throw std::runtime_error("converted texture collides with existing data: " + name);
            }
            continue;
        }
        pending.push_back({it->path(), relative, folded});
    }
    for (const auto &[name, path] : existing) {
        if (!planned.contains(name)) {
            throw std::runtime_error("existing texture is not part of this DC1 conversion; preserving it: " +
                                     path.string());
        }
    }
    for (const PendingTexture &texture : pending) {
        const fs::path target = target_textures / texture.relative;
        fs::create_directories(target.parent_path());
        fs::copy_file(texture.source, target, fs::copy_options::none);
        existing.emplace(texture.folded, target);
    }
}

void BakeTextures(const fs::path &raw, const fs::path &data) {
    TextureStage stage;
    const unsigned jobs = WorkerCount();
    const auto textures = kazarov::textures_convert_dc1(Utf8Path(raw), Utf8Path(stage.root), jobs);
    kazarov::textures_verify(Utf8Path(stage.root / "data" / "textures"));
    MergeTextureAssets(stage.root, data);
    dcdata::Log(stdout, "baked {} texture sources into {} PNGs\n", textures.sources, textures.pngs);
}

int Usage() {
    std::fputs("usage: dcdata extract <iso-or-directory> <data-directory>\n"
               "       dcdata extract-raw <iso-or-directory> <data-directory>\n"
               "       dcdata bake-textures <raw-directory> <data-directory>\n"
               "       dcdata texture-map <raw-directory> <mapping.json>\n"
               "       dcdata pack <data-directory> [output.zip]\n"
               "       dcdata list <iso-or-directory>\n",
               stderr);
    return 2;
}

void WriteTextureMap(const fs::path &raw, const fs::path &mapping) {
    const fs::path raw_root = fs::weakly_canonical(raw);
    const fs::path map_path = fs::weakly_canonical(mapping.parent_path()) / mapping.filename();
    const fs::path relative = map_path.lexically_relative(raw_root);
    if (!relative.empty() && *relative.begin() != "..") {
        throw std::runtime_error("texture mapping output must be outside the raw source tree");
    }
    if (fs::exists(mapping)) {
        throw std::runtime_error("texture mapping output already exists: " + mapping.string());
    }
    TextureStage stage;
    const unsigned jobs = WorkerCount();
    const rust::String rust_contents =
        kazarov::textures_document_map_dc1(Utf8Path(raw), Utf8Path(stage.root), jobs);
    const std::string contents = static_cast<std::string>(rust_contents);
    std::ofstream stream(mapping, std::ios::binary | std::ios::out | std::ios::trunc);
    if (!stream) {
        throw std::runtime_error("cannot create texture mapping output: " + mapping.string());
    }
    stream.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    stream.put('\n');
    if (!stream) {
        throw std::runtime_error("cannot write texture mapping output: " + mapping.string());
    }
}

void Describe(const dcdata::Archive &archive) {
    dcdata::Log(stdout, "source: {} ({})\n", archive.source.string(),
                archive.image ? "ISO 9660 image" : "directory");
    dcdata::Log(stdout, "DATA.DAT: {} bytes, DATA.HD2: {} bytes\n", archive.dat.size, archive.hd2.size);
}

int List(const std::filesystem::path &source) {
    dcdata::Archive             archive = dcdata::OpenArchive(source);
    std::vector<unsigned char>  hd2 = dcdata::ReadExtent(archive.hd2);
    std::vector<dcdata::Record> records = dcdata::ParseIndex(hd2);
    std::uint64_t               bytes = 0;
    for (const dcdata::Record &record : records) {
        dcdata::Log(stdout, "{:8} {:10} {}\n", record.sector, record.size, record.path);
        bytes += record.size;
    }
    Describe(archive);
    dcdata::Log(stdout, "{} files, {} bytes\n", records.size(), bytes);
    return 0;
}

int ExtractRaw(const std::filesystem::path &source, const std::filesystem::path &out) {
    dcdata::Archive archive = dcdata::OpenArchive(source);
    Describe(archive);
    dcdata::Summary summary = dcdata::Extract(archive, out, stdout, {}, RunOnKazarov);

    std::vector<unsigned char>          hd2 = dcdata::ReadExtent(archive.hd2);
    std::vector<dcdata::Record>         records = dcdata::ParseIndex(hd2);
    std::vector<const dcdata::Record *> bad = dcdata::Mismatched(records, out);
    std::size_t                         reachable = summary.records - summary.duplicates;
    dcdata::Log(stdout, "index: {} records, {} files, {} bytes\n", summary.records, reachable, summary.bytes);
    dcdata::Log(stdout, "wrote {} files ({} bytes), kept {} already present\n", summary.written,
                summary.written_bytes, summary.kept);
    if (summary.written + summary.kept != reachable || !bad.empty()) {
        for (const dcdata::Record *record : bad) {
            dcdata::Log(stderr, "missing or wrong size: {}\n", record->path);
        }
        dcdata::Log(stderr, "verified {}/{} files in {}\n", reachable - bad.size(), reachable, out.string());
        return 1;
    }

    dcdata::Log(stdout, "verified {}/{} files in {}{}\n", reachable, reachable, out.string(),
                summary.warnings ? std::format(" ({} warnings)", summary.warnings) : "");
    return 0;
}

int Extract(const std::filesystem::path &source, const std::filesystem::path &out) {
    const int result = ExtractRaw(source, out);
    if (result != 0) {
        return result;
    }
    BakeTextures(out, out);
    return 0;
}

} // namespace

int main(int argc, char **argv) {
    try {
        if (argc == 4 && std::strcmp(argv[1], "extract") == 0) {
            return Extract(argv[2], argv[3]);
        }
        if (argc == 4 && std::strcmp(argv[1], "extract-raw") == 0) {
            return ExtractRaw(argv[2], argv[3]);
        }
        if (argc == 3 && std::strcmp(argv[1], "list") == 0) {
            return List(argv[2]);
        }
        if (argc == 4 && std::strcmp(argv[1], "bake-textures") == 0) {
            BakeTextures(argv[2], argv[3]);
            return 0;
        }
        if (argc == 4 && std::strcmp(argv[1], "texture-map") == 0) {
            WriteTextureMap(argv[2], argv[3]);
            return 0;
        }
        if ((argc == 3 || argc == 4) && std::strcmp(argv[1], "pack") == 0) {
            const std::filesystem::path source = argv[2];
            const std::filesystem::path output = argc == 4 ? std::filesystem::path(argv[3])
                                                            : source.parent_path() / "data.zip";
            Pack(source, output);
            return 0;
        }
        return Usage();
    } catch (const std::exception &error) {
        std::fprintf(stderr, "dcdata: %s\n", error.what());
        return 1;
    }
}

#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <functional>
#include <iterator>
#include <map>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "../../../tools/dcdata/dcdata.hpp"
#include "data/data_read.hpp"
#include "data/model_gltf.hpp"
#include "platform/paths.hpp"
#include "platform_fixture.hpp"

namespace dc::test {

using namespace dc::data;

namespace {

// The bytes a source alias names in the raw tree: a file, the pack members inside it, or a scene
// at an offset of either ("gedit/s26/scene.scn#offset:4016").
std::vector<std::uint8_t> Original(const std::filesystem::path &raw, const std::string &alias) {
    std::vector<std::string> parts;
    for (std::size_t start = 0;;) {
        const std::size_t slash = alias.find('/', start);
        parts.push_back(alias.substr(start, slash - start));
        if (slash == std::string::npos) {
            break;
        }
        start = slash + 1;
    }
    std::filesystem::path file = raw;
    std::size_t           next = 0;
    for (; next < parts.size(); next++) {
        file /= parts[next].substr(0, parts[next].find('#'));
        if (std::filesystem::is_regular_file(file)) {
            break;
        }
    }
    std::ifstream             stream(file, std::ios::binary);
    std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(stream)), std::istreambuf_iterator<char>());
    for (std::size_t part = next; part < parts.size(); part++) {
        const std::string &name = parts[part];
        const std::size_t  marker = name.find('#');
        if (part > next) {
            const auto        members = dcdata::ReadPack(bytes);
            const std::string base = name.substr(0, marker);
            std::size_t       wanted = members.size();
            if (marker != std::string::npos && name.compare(marker, 7, "#entry:") == 0) {
                wanted = std::stoul(name.substr(marker + 7));
            } else {
                for (std::size_t m = 0; m < members.size(); m++) {
                    if (members[m].name == base) {
                        wanted = m;
                        break;
                    }
                }
            }
            if (wanted >= members.size()) {
                return {};
            }
            bytes.assign(members[wanted].data.begin(), members[wanted].data.end());
        }
        if (marker != std::string::npos && name.compare(marker, 8, "#offset:") == 0) {
            bytes.erase(bytes.begin(), bytes.begin() + std::stoul(name.substr(marker + 8)));
        }
    }
    return bytes;
}

// Whether this process checks the sources of `bundle`: DC_MODEL_PART=k/parts splits a disc's bundles
// across processes run side by side, each parsing only its own.
bool InPart(const std::string &bundle) {
    // DC_MODEL_ONLY narrows a run to the sources of one bundle.
    if (const char *only = std::getenv("DC_MODEL_ONLY")) {
        return bundle == only;
    }
    const std::size_t n = std::hash<std::string>{}(bundle);
    const char       *part = std::getenv("DC_MODEL_PART");
    unsigned          k = 0;
    unsigned          parts = 1;
    if (part == nullptr || std::sscanf(part, "%u/%u", &k, &parts) != 2 || parts == 0) {
        return true;
    }
    return n % parts == k;
}

struct Channel {
    std::int32_t                      type = 0;
    std::vector<std::uint32_t>        frames;
    std::vector<std::array<float, 4>> values;
};

// A MOT's channels by frame, type and target.
std::map<std::array<std::int64_t, 3>, Channel> ReadChannels(const std::vector<std::uint8_t> &bytes) {
    std::map<std::array<std::int64_t, 3>, Channel> channels;
    std::size_t                                    at = 0;
    while (at + 32 <= bytes.size()) {
        std::int32_t header[8];
        std::memcpy(header, bytes.data() + at, sizeof header);
        Channel &channel = channels[{header[0], header[2], header[1]}];
        channel.type = header[2];
        at += 32;
        for (std::int32_t k = 0; k < header[4] && at + 32 <= bytes.size(); k++, at += 32) {
            std::uint32_t        frame;
            std::array<float, 4> value;
            std::memcpy(&frame, bytes.data() + at, 4);
            std::memcpy(value.data(), bytes.data() + at + 16, 16);
            channel.frames.push_back(frame);
            channel.values.push_back(value);
        }
        if (header[5] == 0) {
            break;
        }
    }
    return channels;
}

// A channel's value at a frame, as the retail evaluator interpolates its keys: linearly, and a
// rotation along the arc between its normalized keys.
std::array<float, 4> Sample(const Channel &channel, float frame) {
    const std::size_t upper = static_cast<std::size_t>(
        std::upper_bound(channel.frames.begin(), channel.frames.end(), frame,
                         [](float at, std::uint32_t key) { return at < float(key); }) -
        channel.frames.begin());
    auto normal = [&](std::array<float, 4> q) {
        if (channel.type != 0) {
            return q;
        }
        const float length = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2] + q[3] * q[3]);
        for (float &v : q) {
            v /= length > 1e-20f ? length : 1.0f;
        }
        return q;
    };
    if (upper == 0) {
        return normal(channel.values.front());
    }
    if (upper == channel.frames.size()) {
        return normal(channel.values.back());
    }
    // Visibility holds its value until the next key.
    if (channel.type == 50 || channel.type == 51) {
        return channel.values[upper - 1];
    }
    const float          t = (frame - channel.frames[upper - 1]) / float(channel.frames[upper] - channel.frames[upper - 1]);
    std::array<float, 4> a = normal(channel.values[upper - 1]);
    std::array<float, 4> b = normal(channel.values[upper]);
    if (channel.type == 0) {
        float cosine = a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3];
        if (cosine < 0) {
            cosine = -cosine;
            for (float &v : b) {
                v = -v;
            }
        }
        float from = 1 - t;
        float to = t;
        if (1 - cosine > 0.01f) {
            const float angle = std::acos(std::min(cosine, 1.0f));
            from = std::sin((1 - t) * angle) / std::sin(angle);
            to = std::sin(t * angle) / std::sin(angle);
        }
        return normal({from * a[0] + to * b[0], from * a[1] + to * b[1], from * a[2] + to * b[2], from * a[3] + to * b[3]});
    }
    return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t, a[3] + (b[3] - a[3]) * t};
}

float Difference(const Channel &channel, const std::array<float, 4> &a, const std::array<float, 4> &b) {
    if (channel.type == 0) {
        return 1 - std::abs(a[0] * b[0] + a[1] * b[1] + a[2] * b[2] + a[3] * b[3]);
    }
    float largest = 0;
    // Translations, scales and colours use three lanes; a rotation all four.
    for (std::size_t c = 0; c < (channel.type <= 2 ? 3u : 4u); c++) {
        largest = std::max(largest, std::abs(a[c] - b[c]) / std::max(1.0f, std::abs(a[c])));
    }
    return largest;
}

} // namespace

TEST(ModelGltf, EveryPublishedSourceRebuildsByteForByte) {
    const char *data = std::getenv("DC_MODEL_DATA");
    const char *raw = std::getenv("DC_MODEL_RAW");
    if (data == nullptr || raw == nullptr) {
        GTEST_SKIP() << "Set DC_MODEL_DATA to data converted by dcdata convert-models from the raw tree in DC_MODEL_RAW";
    }
    dc::test::EnvSnapshot environment = dc::test::SaveEnv("DC_DATA");
    dc::test::SetEnv("DC_DATA", data, 1);
    PathsReset();
    PathsConfigure({.app_dir = "chronicle", .env_data = "DC_DATA", .env_save = "DC_SAVE"});
    DataReadReset();
    model_gltf::Reset();

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(DataReadReadFile("models.json", bytes));
    const nlohmann::json       index = nlohmann::json::parse(bytes.begin(), bytes.end());
    std::map<std::string, int> failures;
    int                        checked = 0;
    for (const auto &[alias, entry] : index.items()) {
        // Motions too: the game reads a motion's vertex channels by their place in it.
        if (entry.at("kind") == "cloths" || entry.at("kind") == "children" || !InPart(entry.at("bundle"))) {
            continue;
        }
        checked++;
        const std::vector<std::uint8_t> *rebuilt = model_gltf::Rebuild(alias);
        std::vector<std::uint8_t>        original = Original(raw, alias);
        std::string                      failure;
        if (rebuilt == nullptr) {
            failure = "not rebuilt";
        } else if (original.size() < rebuilt->size()) {
            failure = "longer than the source";
        } else {
            original.resize(rebuilt->size());
            if (original != *rebuilt) {
                failure = "differs";
            }
        }
        if (!failure.empty() && failures[entry.at("kind").get<std::string>() + " " + failure]++ < 5) {
            ADD_FAILURE() << alias << ": " << failure;
        }
    }
    EXPECT_GT(checked, 0);
    for (const auto &[failure, count] : failures) {
        ADD_FAILURE() << count << " sources " << failure;
    }
    model_gltf::Reset();
    dc::test::RestoreEnv(environment);
    PathsReset();
    PathsConfigure({.app_dir = "chronicle", .env_data = "DC_DATA", .env_save = "DC_SAVE"});
    DataReadReset();
}

TEST(ModelGltf, EveryMotionKeepsItsPosesInEveryAction) {
    const char *data = std::getenv("DC_MODEL_DATA");
    const char *raw = std::getenv("DC_MODEL_RAW");
    if (data == nullptr || raw == nullptr) {
        GTEST_SKIP() << "Set DC_MODEL_DATA to data converted by dcdata convert-models from the raw tree in DC_MODEL_RAW";
    }
    dc::test::EnvSnapshot environment = dc::test::SaveEnv("DC_DATA");
    dc::test::SetEnv("DC_DATA", data, 1);
    PathsReset();
    PathsConfigure({.app_dir = "chronicle", .env_data = "DC_DATA", .env_save = "DC_SAVE"});
    DataReadReset();
    model_gltf::Reset();

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(DataReadReadFile("models.json", bytes));
    const nlohmann::json                  index = nlohmann::json::parse(bytes.begin(), bytes.end());
    std::map<std::string, nlohmann::json> sidecars;
    int                                   motions = 0;
    int                                   bad = 0;
    int                                   larger = 0;
    double                                worst = 0;
    for (const auto &[alias, entry] : index.items()) {
        if ((entry.at("kind") != "animations" && entry.at("kind") != "motion_channels") || !InPart(entry.at("bundle"))) {
            continue;
        }
        motions++;
        const std::string bundle = entry.at("bundle");
        if (!sidecars.contains(bundle)) {
            const std::string sidecar = dcdata::PublishedPath(bundle.substr(0, bundle.size() - 5) + ".game.json",
                                                              [](std::string_view prefix) { return DataFileSize(prefix) > 0; });
            ASSERT_TRUE(DataReadReadFile(sidecar, bytes)) << sidecar;
            sidecars[bundle] = nlohmann::json::parse(bytes.begin(), bytes.end());
        }
        // A scene's own motion plays whole; a character's, action by action.
        const bool            scene = entry.at("kind") == "motion_channels";
        const nlohmann::json &resource =
            scene ? sidecars[bundle] : sidecars[bundle].at("resources").at("animations").at(entry.at("index").get<std::size_t>());
        const std::vector<std::uint8_t> *rebuilt = model_gltf::Rebuild(alias);
        if (rebuilt == nullptr) {
            if (bad++ < 5) {
                ADD_FAILURE() << alias << ": not rebuilt";
            }
            continue;
        }
        // DC_MODEL_DUMP=<directory> keeps each rebuilt motion there, to compare with the disc's.
        if (const char *dump = std::getenv("DC_MODEL_DUMP")) {
            std::string name = alias;
            std::replace(name.begin(), name.end(), '/', '_');
            std::ofstream(std::filesystem::path(dump) / name, std::ios::binary)
                .write(reinterpret_cast<const char *>(rebuilt->data()), static_cast<std::streamsize>(rebuilt->size()));
        }
        const auto  original = ReadChannels(Original(raw, alias));
        const auto  copy = ReadChannels(*rebuilt);
        std::size_t original_keys = 0;
        std::size_t copy_keys = 0;
        for (const auto &[_, channel] : original) {
            original_keys += channel.frames.size();
        }
        for (const auto &[_, channel] : copy) {
            copy_keys += channel.frames.size();
        }
        // The game reads every vertex channel of a frame at the first one's key and blends towards
        // keys by frame, so each channel keeps the disc's key frames, and none is added.
        bool same_frames = true;
        for (const auto &[id, channel] : original) {
            const auto found = copy.find(id);
            if (!channel.frames.empty() && (found == copy.end() || found->second.frames != channel.frames)) {
                same_frames = false;
            }
        }
        for (const auto &[id, channel] : copy) {
            if (!channel.frames.empty() && (!original.contains(id) || original.at(id).frames.empty())) {
                same_frames = false;
            }
        }
        if (!same_frames && larger++ < 5) {
            ADD_FAILURE() << alias << ": key frames differ from the disc's (" << copy_keys << " keys, the disc's "
                          << original_keys << ")";
        }
        float         largest = 0;
        std::string   where;
        std::uint32_t last = 0;
        for (const auto &[_, channel] : original) {
            for (std::uint32_t frame : channel.frames) {
                last = std::max(last, frame);
            }
        }
        const nlohmann::json actions =
            scene ? nlohmann::json::array({
                        {{"start", 0}, {"end", last}}
        })
                  : resource.at("actions");
        for (const auto &[id, channel] : original) {
            if (channel.frames.empty()) {
                continue;
            }
            const auto found = copy.find(id);
            for (const nlohmann::json &action : actions) {
                for (std::uint32_t f = action.at("start"); f <= action.at("end").get<std::uint32_t>(); f++) {
                    float difference = 1;
                    if (found != copy.end() && !found->second.frames.empty()) {
                        difference = Difference(channel, Sample(channel, f), Sample(found->second, f));
                    }
                    if (difference > largest) {
                        largest = difference;
                        where = std::format("frame {} type {} target {} at {}", id[0], id[1], id[2], f);
                    }
                }
            }
        }
        worst = std::max(worst, double(largest));
        if (largest > 1e-3f && bad++ < 10) {
            ADD_FAILURE() << alias << ": " << largest << " at " << where;
        }
    }
    EXPECT_GT(motions, 0);
    EXPECT_EQ(bad, 0) << "of " << motions << " motions";
    EXPECT_EQ(larger, 0) << "motions rebuilt with other key frames than the disc's";
    std::printf("%d motions, largest difference %g\n", motions, worst);
    model_gltf::Reset();
    dc::test::RestoreEnv(environment);
    PathsReset();
    PathsConfigure({.app_dir = "chronicle", .env_data = "DC_DATA", .env_save = "DC_SAVE"});
    DataReadReset();
}

TEST(ModelGltf, SplicingPutsAnEmptiedPackMemberBack) {
    const char *data = std::getenv("DC_MODEL_DATA");
    const char *raw = std::getenv("DC_MODEL_RAW");
    if (data == nullptr || raw == nullptr) {
        GTEST_SKIP() << "Set DC_MODEL_DATA to data converted by dcdata convert-models from the raw tree in DC_MODEL_RAW";
    }
    dc::test::EnvSnapshot environment = dc::test::SaveEnv("DC_DATA");
    dc::test::SetEnv("DC_DATA", data, 1);
    PathsReset();
    PathsConfigure({.app_dir = "chronicle", .env_data = "DC_DATA", .env_save = "DC_SAVE"});
    DataReadReset();

    // Toan's pack with his model and weights emptied, as stripping the converted data leaves them.
    std::vector<std::uint8_t> original = Original(raw, "chara/c01d.chr");
    ASSERT_FALSE(original.empty());
    std::vector<std::uint8_t> stripped = original;
    for (const char *name : {"c01d.mds", "c01dbas.wgt", "c01dbas.bbp"}) {
        std::size_t at = 0;
        while (at + 76 <= stripped.size() && std::strcmp(reinterpret_cast<char *>(stripped.data() + at), name) != 0) {
            std::uint32_t next;
            std::memcpy(&next, stripped.data() + at + 72, 4);
            at += next;
        }
        ASSERT_LT(at + 76, stripped.size()) << name;
        std::uint32_t offset;
        std::uint32_t size;
        std::memcpy(&offset, stripped.data() + at + 64, 4);
        std::memcpy(&size, stripped.data() + at + 68, 4);
        std::fill_n(stripped.begin() + at + offset, size, 0);
        std::fill_n(stripped.begin() + at + 68, 4, 0);
    }
    model_gltf::Splice("chara/c01d.chr", stripped);
    const auto members = dcdata::ReadPack(stripped);
    for (const auto &member : dcdata::ReadPack(original)) {
        if (member.name == "c01d.mds" || member.name.ends_with(".wgt") || member.name.ends_with(".bbp")) {
            const auto found = std::find_if(members.begin(), members.end(),
                                            [&](const auto &other) { return other.name == member.name; });
            ASSERT_NE(found, members.end());
            EXPECT_TRUE(found->data == member.data) << member.name;
        }
    }
    model_gltf::Reset();
    dc::test::RestoreEnv(environment);
    PathsReset();
    PathsConfigure({.app_dir = "chronicle", .env_data = "DC_DATA", .env_save = "DC_SAVE"});
    DataReadReset();
}

namespace {

// The byte ranges of the motion members inside a pack file, by walking its packs as the game does.
void MotionRanges(std::span<const std::uint8_t> bytes, std::size_t base, std::vector<std::pair<std::size_t, std::size_t>> &out,
                  int depth = 0) {
    std::size_t at = 0;
    while (depth < 6 && at + 76 <= bytes.size() && bytes[at] != 0) {
        std::uint32_t offset;
        std::uint32_t size;
        std::uint32_t next;
        std::memcpy(&offset, bytes.data() + at + 64, 4);
        std::memcpy(&size, bytes.data() + at + 68, 4);
        std::memcpy(&next, bytes.data() + at + 72, 4);
        if (offset < 76 || next < 76 || at + offset + size > bytes.size() || std::memchr(bytes.data() + at, 0, 64) == nullptr) {
            return;
        }
        const std::string_view name(reinterpret_cast<const char *>(bytes.data() + at));
        if (name.ends_with(".mot") || name.ends_with(".MOT")) {
            out.emplace_back(base + at + 68, base + at + 72);
            out.emplace_back(base + at + offset, base + at + next);
        } else {
            MotionRanges(bytes.subspan(at + offset, size), base + at + offset, out, depth + 1);
        }
        at += next;
    }
}

} // namespace

// The data with its converted sources emptied (dcdata strip-models) reads back as the disc: every
// file, spliced from the published models as the game reads it, equals the disc's, but for the
// motions, which come back from their actions and are compared pose by pose above.
// The game reads every vertex channel of a motion at its first channel's key, so a motion's channels
// must stay in the disc's order: rebuilt in another, Saget's vertex animation in the attract movie wrote
// past his mesh and crashed. Every motion inside a pack comes back as the disc has it.
TEST(ModelGltf, MotionsGoInOnlyAsTheDiscHasThem) {
    const char *data = std::getenv("DC_MODEL_DATA");
    const char *raw = std::getenv("DC_MODEL_RAW");
    if (data == nullptr || raw == nullptr) {
        GTEST_SKIP() << "Set DC_MODEL_DATA to data converted by dcdata convert-models from the raw tree in DC_MODEL_RAW";
    }
    dc::test::EnvSnapshot environment = dc::test::SaveEnv("DC_DATA");
    dc::test::SetEnv("DC_DATA", data, 1);
    PathsReset();
    PathsConfigure({.app_dir = "chronicle", .env_data = "DC_DATA", .env_save = "DC_SAVE"});
    DataReadReset();
    model_gltf::Reset();

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(DataReadReadFile("models.json", bytes));
    const nlohmann::json  index = nlohmann::json::parse(bytes.begin(), bytes.end());
    std::set<std::string> files;
    for (const auto &[alias, entry] : index.items()) {
        if (entry.at("kind") != "animations" && entry.at("kind") != "motion_channels") {
            continue;
        }
        std::string file;
        std::size_t start = 0;
        for (;;) {
            const std::size_t slash = alias.find('/', start);
            const std::string part = alias.substr(start, slash - start);
            file += (file.empty() ? "" : "/") + part.substr(0, part.find('#'));
            if (std::filesystem::is_regular_file(std::filesystem::path(raw) / file) || slash == std::string::npos) {
                break;
            }
            start = slash + 1;
        }
        if (file != alias && InPart(file)) {
            files.insert(file);
        }
    }
    int differing = 0;
    for (const std::string &file : files) {
        std::vector<std::uint8_t> read;
        ASSERT_TRUE(DataReadReadFile(file, read)) << file;
        model_gltf::Splice(file, read);
        const std::vector<std::uint8_t>                  original = Original(raw, file);
        std::vector<std::pair<std::size_t, std::size_t>> motions;
        MotionRanges(original, 0, motions);
        for (const auto &[first, last] : motions) {
            if (last > read.size() || !std::equal(read.begin() + first, read.begin() + last, original.begin() + first)) {
                if (differing++ < 10) {
                    ADD_FAILURE() << file << ": a motion differs from the disc's at byte " << first;
                }
                break;
            }
        }
    }
    EXPECT_GT(files.size(), 0u);
    EXPECT_EQ(differing, 0) << "of " << files.size() << " files";
    model_gltf::Reset();
    dc::test::RestoreEnv(environment);
    PathsReset();
    PathsConfigure({.app_dir = "chronicle", .env_data = "DC_DATA", .env_save = "DC_SAVE"});
    DataReadReset();
}

TEST(ModelGltf, StrippedDataReadsBackAsTheDisc) {
    const char *data = std::getenv("DC_MODEL_STRIPPED");
    const char *raw = std::getenv("DC_MODEL_RAW");
    if (data == nullptr || raw == nullptr) {
        GTEST_SKIP() << "Set DC_MODEL_STRIPPED to data stripped by dcdata strip-models, and DC_MODEL_RAW to the disc's";
    }
    dc::test::EnvSnapshot environment = dc::test::SaveEnv("DC_DATA");
    dc::test::SetEnv("DC_DATA", data, 1);
    PathsReset();
    PathsConfigure({.app_dir = "chronicle", .env_data = "DC_DATA", .env_save = "DC_SAVE"});
    DataReadReset();

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(DataReadReadFile("models.json", bytes));
    const nlohmann::json  index = nlohmann::json::parse(bytes.begin(), bytes.end());
    std::set<std::string> files;
    for (const auto &[alias, entry] : index.items()) {
        std::string file;
        std::size_t start = 0;
        for (;;) {
            const std::size_t slash = alias.find('/', start);
            const std::string part = alias.substr(start, slash - start);
            file += (file.empty() ? "" : "/") + part.substr(0, part.find('#'));
            if (std::filesystem::is_regular_file(std::filesystem::path(raw) / file) || slash == std::string::npos) {
                break;
            }
            start = slash + 1;
        }
        if (InPart(file)) {
            files.insert(file);
        }
    }
    int differing = 0;
    for (const std::string &file : files) {
        std::vector<std::uint8_t> read;
        ASSERT_TRUE(DataReadReadFile(file, read)) << file;
        model_gltf::Splice(file, read);
        // Motions too: they come back as the disc has them, channel order and all.
        const std::vector<std::uint8_t> original = Original(raw, file);
        std::size_t                     first = original.size();
        for (std::size_t at = 0; at < std::min(original.size(), read.size()); at++) {
            if (read[at] != original[at]) {
                first = at;
                break;
            }
        }
        if ((first < original.size() || read.size() != original.size()) && differing++ < 10) {
            ADD_FAILURE() << file << ": differs from the disc at byte " << first;
        }
    }
    EXPECT_EQ(differing, 0) << "of " << files.size() << " files";
    model_gltf::Reset();
    dc::test::RestoreEnv(environment);
    PathsReset();
    PathsConfigure({.app_dir = "chronicle", .env_data = "DC_DATA", .env_save = "DC_SAVE"});
    DataReadReset();
}

} // namespace dc::test

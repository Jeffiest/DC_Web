#include "data/model_gltf.hpp"

#include <fastgltf/core.hpp>
#include <fastgltf/tools.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <tuple>
#include <unordered_map>

#include "../../../tools/dcdata/dcdata.hpp"
#include "data/data_read.hpp"

namespace dc::data::model_gltf {
namespace {

using Json = nlohmann::json;

// A published bundle: the glTF with its buffers attached, and the sidecar beside it.
struct Bundle {
    fastgltf::Asset asset;
    Json            sidecar;
};

struct State {
    std::mutex                                                                  mutex;
    bool                                                                        indexed = false;
    std::uint64_t                                                               generation = 0;
    std::map<std::string, Json>                                                 index;
    std::unordered_map<std::string, std::shared_ptr<const Bundle>>              bundles;
    std::deque<std::string>                                                     recent;
    std::set<std::string>                                                       failed;
    bool                                                                        stripped = false;
    std::unordered_map<std::string, std::unique_ptr<std::vector<std::uint8_t>>> rebuilt;
    std::deque<std::string>                                                     rebuilt_order;
    std::size_t                                                                 rebuilt_bytes = 0;
};

// Rebuilt sources are kept, up to this many bytes, for the files the game reads again.
constexpr std::size_t kKeptRebuiltBytes = 128u << 20;

State &Shared() {
    static State state;
    return state;
}

// Kazarov names sources as the disc spells them; the port's loaders fold paths as the game does.
std::string Key(std::string_view alias) {
    return dcdata::FoldPath(alias);
}

void LoadIndex(State &state);

// Reads the index, again when the data has been indexed again since.
void Refresh(State &state) {
    // Reading the index may index the data, so the generation is taken after.
    if (!state.indexed || state.generation != DataReadGeneration()) {
        LoadIndex(state);
        state.generation = DataReadGeneration();
    }
}

// Forgets everything read from the data, with the state locked.
void Forget(State &state) {
    state.indexed = false;
    state.index.clear();
    state.bundles.clear();
    state.recent.clear();
    state.failed.clear();
    state.stripped = false;
    state.rebuilt.clear();
    state.rebuilt_order.clear();
    state.rebuilt_bytes = 0;
}

// Stops the game: data whose sources were emptied can't run without the models that rebuild them.
[[noreturn]] void Unrecoverable(const std::string &why) {
    std::fprintf(stderr,
                 "%s\nThe game data's models were stripped (dcdata strip-models), so the game can't load them "
                 "from the disc's files either. Extract the data again, or run dcdata bake-assets on it.\n",
                 why.c_str());
    std::fflush(stderr);
    std::abort();
}

void LoadIndex(State &state) {
    // Indexing the data reads it anew; what was read from it before may be stale.
    Forget(state);
    state.indexed = true;
    state.stripped = DataFileExists("models.stripped");
    std::vector<std::uint8_t> bytes;
    if (!DataReadReadFile("models.json", bytes)) {
        if (state.stripped) {
            Unrecoverable("models.json is missing");
        }
        return;
    }
    const Json index = Json::parse(bytes.begin(), bytes.end(), nullptr, false);
    if (!index.is_object()) {
        if (state.stripped) {
            Unrecoverable("models.json can't be read");
        }
        std::fprintf(stderr, "models.json: not a source index\n");
        return;
    }
    for (const auto &[alias, entry] : index.items()) {
        state.index.emplace(Key(alias), entry);
    }
}

// Where a bundle's file sits in the data (dcdata::PublishedPath).
std::string OnDisk(std::string_view path) {
    return dcdata::PublishedPath(path, [](std::string_view prefix) { return DataFileExists(prefix); });
}

std::string Directory(const std::string &path) {
    const std::size_t slash = path.rfind('/');
    return slash == std::string::npos ? std::string() : path.substr(0, slash + 1);
}

// Bundles are parsed once and kept while they are among the last used: a character's model,
// motions and weights come from one bundle, a placed motion's from two, and a scene file holds many.
constexpr std::size_t kKeptBundles = 16;

std::shared_ptr<const Bundle> LoadBundle(State &state, const std::string &path) {
    if (auto found = state.bundles.find(path); found != state.bundles.end()) {
        std::erase(state.recent, path);
        state.recent.push_back(path);
        return found->second;
    }
    std::vector<std::uint8_t> gltf;
    std::vector<std::uint8_t> sidecar;
    const std::string         stem = path.substr(0, path.size() - std::string_view(".gltf").size());
    if (!DataReadReadFile(OnDisk(path), gltf) || !DataReadReadFile(OnDisk(stem + ".game.json"), sidecar)) {
        throw std::runtime_error("missing bundle " + path);
    }
    auto input = fastgltf::GltfDataBuffer::FromBytes(reinterpret_cast<const std::byte *>(gltf.data()), gltf.size());
    if (input.error() != fastgltf::Error::None) {
        throw std::runtime_error("cannot read " + path);
    }
    fastgltf::Parser parser;
    // Buffers are read through the data's own reader below, archived or loose alike.
    auto parsed = parser.loadGltf(input.get(), {}, fastgltf::Options::None);
    if (parsed.error() != fastgltf::Error::None) {
        throw std::runtime_error(path + ": " + std::string(fastgltf::getErrorMessage(parsed.error())));
    }
    if (fastgltf::validate(parsed.get()) != fastgltf::Error::None) {
        throw std::runtime_error(path + ": invalid glTF");
    }
    auto bundle = std::make_shared<Bundle>(Bundle{std::move(parsed.get()), Json::parse(sidecar.begin(), sidecar.end())});
    for (fastgltf::Buffer &buffer : bundle->asset.buffers) {
        const auto *uri = std::get_if<fastgltf::sources::URI>(&buffer.data);
        if (uri == nullptr) {
            continue;
        }
        std::vector<std::uint8_t> data;
        if (!DataReadReadFile(OnDisk(Directory(path) + std::string(uri->uri.path())), data) ||
            data.size() < buffer.byteLength) {
            throw std::runtime_error("missing buffer of " + path);
        }
        fastgltf::sources::Vector storage;
        storage.bytes.resize(data.size());
        std::memcpy(storage.bytes.data(), data.data(), data.size());
        buffer.data = std::move(storage);
    }
    // Holders keep their bundles alive past an eviction.
    while (state.recent.size() >= kKeptBundles) {
        state.bundles.erase(state.recent.front());
        state.recent.pop_front();
    }
    state.recent.push_back(path);
    state.bundles[path] = bundle;
    return bundle;
}

// A retail record being written: words and floats at the offsets the source file had them.
class Record {
public:
    void Size(std::size_t size) {
        if (bytes_.size() < size) {
            bytes_.resize(size);
        }
    }

    void Word(std::size_t at, std::uint32_t value) { Put(at, &value, 4); }

    void Float(std::size_t at, float value) { Put(at, &value, 4); }

    void Bytes(std::size_t at, std::string_view value) { Put(at, value.data(), value.size()); }

    std::vector<std::uint8_t> Take() { return std::move(bytes_); }

private:
    void Put(std::size_t at, const void *value, std::size_t size) {
        Size(at + size);
        std::memcpy(bytes_.data() + at, value, size);
    }

    std::vector<std::uint8_t> bytes_;
};

// The CRC-32 (IEEE) of some bytes, as models.json records each source's.
std::uint32_t Crc32(std::span<const std::uint8_t> bytes) {
    static const std::array<std::uint32_t, 256> table = [] {
        std::array<std::uint32_t, 256> out{};
        for (std::uint32_t n = 0; n < 256; n++) {
            std::uint32_t c = n;
            for (int k = 0; k < 8; k++) {
                c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            }
            out[n] = c;
        }
        return out;
    }();
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::uint8_t b : bytes) {
        crc = table[(crc ^ b) & 0xFF] ^ (crc >> 8);
    }
    return crc ^ 0xFFFFFFFFu;
}

std::uint32_t U32(const Json &value) {
    return value.get<std::uint32_t>();
}

void Padding(Record &record, std::size_t base, const Json &holder) {
    for (const Json &word : holder.value("padding_u32", Json::array())) {
        record.Word(base + U32(word[0]), U32(word[1]));
    }
}

// The values of one retail stream, by source index, as the glTF primitives' corners carry them.
using Stream = std::map<std::uint32_t, std::array<float, 4>>;

template <class Vector, std::size_t Width>
void Gather(const fastgltf::Asset &asset, std::size_t accessor, const Json &sources, Stream &stream) {
    fastgltf::iterateAccessorWithIndex<Vector>(asset, asset.accessors[accessor], [&](Vector value, std::size_t corner) {
        std::array<float, 4> &entry = stream[U32(sources.at(corner))];
        for (std::size_t component = 0; component < Width; component++) {
            entry[component] = value[component];
        }
    });
}

// Rebuilds one MDT at `base`: its header and mesh-section words from the sidecar, its streams from
// the glTF corners and the sidecar's overrides, its strips from the corners' source indices.
void Mesh(Record &record, std::size_t base, const Bundle &bundle, const Json &mesh, const fastgltf::Node *node) {
    const Json &words = mesh.at("record_u32");
    for (std::size_t k = 0; k < 16; k++) {
        record.Word(base + k * 4, U32(words[k]));
    }
    const std::size_t face = U32(words[10]);
    for (std::size_t k = 0; k < 4; k++) {
        record.Word(base + face + k * 4, U32(words[16 + k]));
    }
    record.Size(base + U32(words[2]));
    auto                               signed_word = [&](std::size_t k) { return static_cast<std::int32_t>(U32(words[k])); };
    const std::array<std::uint32_t, 4> counts = {U32(words[3]), U32(words[5]), U32(words[11]), U32(words[7])};
    const std::array<std::int32_t, 4>  offsets = {signed_word(4), signed_word(6), signed_word(12), signed_word(8)};

    std::array<Stream, 4> streams;
    const bool            collision = mesh.value("kind", "") == "collision";
    if (!collision && node != nullptr && node->meshIndex) {
        const fastgltf::Mesh &gltf_mesh = bundle.asset.meshes[*node->meshIndex];
        const Json           &primitives = mesh.at("primitives");
        for (std::size_t p = 0; p < gltf_mesh.primitives.size() && p < primitives.size(); p++) {
            const fastgltf::Primitive &primitive = gltf_mesh.primitives[p];
            const Json                &sources = primitives[p];
            if (auto *a = primitive.findAttribute("POSITION"); a != primitive.attributes.end()) {
                Gather<fastgltf::math::fvec3, 3>(bundle.asset, a->accessorIndex, sources.at("source_vertices"), streams[0]);
            }
            if (auto *a = primitive.findAttribute("NORMAL"); a != primitive.attributes.end()) {
                Gather<fastgltf::math::fvec3, 3>(bundle.asset, a->accessorIndex, sources.at("source_normals"), streams[1]);
            }
            if (auto *a = primitive.findAttribute("TEXCOORD_0"); a != primitive.attributes.end()) {
                Gather<fastgltf::math::fvec2, 2>(bundle.asset, a->accessorIndex, sources.at("source_uvs"), streams[2]);
            }
            if (auto *a = primitive.findAttribute("COLOR_0"); a != primitive.attributes.end()) {
                Gather<fastgltf::math::fvec4, 4>(bundle.asset, a->accessorIndex, sources.at("source_colours"), streams[3]);
            }
        }
    }
    // A component the sidecar doesn't override is the glTF's, or the standard default: 1 for a
    // position's w, 0 elsewhere.
    constexpr std::array<const char *, 4> kNames = {"positions", "normals", "uvs", "colours"};
    const Json                            overrides = mesh.value("stream_overrides", Json::object());
    for (std::size_t s = 0; s < 4; s++) {
        if (offsets[s] <= 0) {
            continue;
        }
        for (std::uint32_t i = 0; i < counts[s]; i++) {
            std::array<float, 4> value = {0, 0, 0, 0};
            if (auto found = streams[s].find(i); found != streams[s].end()) {
                value = found->second;
                if (s == 0) {
                    value[3] = 1;
                }
            }
            for (std::size_t c = 0; c < 4; c++) {
                record.Float(base + offsets[s] + i * 16 + c * 4, value[c]);
            }
        }
        for (const Json &entry : overrides.value(kNames[s], Json::array())) {
            record.Float(base + offsets[s] + U32(entry[0]) * 16 + U32(entry[1]) * 4, entry[2].get<float>());
        }
    }

    const std::size_t info = U32(words[14]);
    if (collision) {
        const Json &polygons = mesh.at("polygons");
        record.Word(base + face + 16, U32(mesh.at("face_reserved_u32")[2]));
        record.Word(base + face + 20, static_cast<std::uint32_t>(polygons.size()));
        for (std::size_t k = 0; k < polygons.size(); k++) {
            for (std::size_t w = 0; w < 5; w++) {
                record.Word(base + face + 24 + k * 20 + w * 4, U32(polygons[k][w]));
            }
        }
        const Json &records = mesh.at("info");
        for (std::size_t k = 0; k < records.size(); k++) {
            for (std::size_t w = 0; w < 4; w++) {
                record.Word(base + info + k * 16 + w * 4, U32(records[k][w]));
            }
        }
    } else {
        // Every corner indexes a position, a normal and a texture coordinate, and a colour when the
        // mesh has colours; a strip with no corners keeps its place.
        const std::size_t                     stride = offsets[3] > 0 ? 4 : 3;
        std::map<std::uint32_t, const Json *> empty;
        for (const Json &strip : mesh.value("empty_strips", Json::array())) {
            empty[U32(strip.at("index"))] = &strip;
        }
        const Json &primitives = mesh.at("primitives");
        std::size_t at = base + face + 16;
        std::size_t next = 0;
        for (std::uint32_t s = 0; s < U32(words[18]); s++) {
            if (auto found = empty.find(s); found != empty.end()) {
                record.Word(at, U32(found->second->at("type")));
                record.Word(at + 4, 0);
                record.Word(at + 8, U32(found->second->at("material")));
                at += 12;
                continue;
            }
            const Json &strip = primitives.at(next++);
            const Json &v = strip.at("source_vertices");
            record.Word(at, U32(strip.at("type")));
            record.Word(at + 4, static_cast<std::uint32_t>(v.size()));
            record.Word(at + 8, U32(strip.at("material")));
            at += 12;
            for (std::size_t c = 0; c < v.size(); c++) {
                record.Word(at, U32(v[c]));
                record.Word(at + 4, U32(strip.at("source_normals")[c]));
                record.Word(at + 8, U32(strip.at("source_uvs")[c]));
                if (stride == 4) {
                    record.Word(at + 12, U32(strip.at("source_colours")[c]));
                }
                at += stride * 4;
            }
        }
        const Json &materials = mesh.at("materials");
        for (std::size_t k = 0; k < materials.size(); k++) {
            const Json &material = materials[k].at("record_u32");
            for (std::size_t w = 0; w < 24; w++) {
                record.Word(base + info + k * 96 + w * 4, U32(material[w]));
            }
        }
    }
    Padding(record, base, mesh);
}

// Rebuilds an MDS: its header and object records, each object's MDT where it was.
std::vector<std::uint8_t> Scene(const Bundle &bundle, const Json &model) {
    Record      record;
    const Json &words = model.at("record_u32");
    for (std::size_t k = 0; k < 4; k++) {
        record.Word(k * 4, U32(words[k]));
    }
    const std::size_t node_offset = model.value("node_offset", 0u);
    std::size_t       at = U32(words[3]);
    const Json       &frames = model.at("frames");
    for (std::size_t f = 0; f < frames.size(); f++) {
        const Json           &frame = frames[f];
        const std::uint32_t   step = U32(frame.at("record_u32")[0]);
        const std::uint32_t   mdt = U32(frame.at("record_u32")[1]);
        const fastgltf::Node &node = bundle.asset.nodes.at(node_offset + f);
        record.Word(at, U32(frame.at("reserved_u32")[0]));
        record.Word(at + 4, step);
        record.Size(at + 40);
        record.Bytes(at + 8, std::string_view(node.name).substr(0, 32));
        record.Word(at + 40, mdt);
        record.Word(at + 44, static_cast<std::uint32_t>(frame.at("parent").get<std::int32_t>()));
        const Json &matrix = frame.at("local_matrix");
        for (std::size_t k = 0; k < 16; k++) {
            record.Float(at + 48 + k * 4, matrix[k].get<float>());
        }
        if (!frame.at("mdt").is_null()) {
            // A frame whose transform needs a shear helper hangs its mesh on the helper.
            const std::size_t     mesh_node = frame.value("rotation_node", node_offset + f);
            const fastgltf::Node *holder = &bundle.asset.nodes.at(mesh_node);
            Mesh(record, mdt, bundle, model.at("meshes").at(frame.at("mdt").get<std::size_t>()), holder);
        }
        at += step;
    }
    Padding(record, 0, model);
    // A scene inside another file spans what its records cover, which can end before its last MDT's
    // header says it does.
    std::vector<std::uint8_t> bytes = record.Take();
    if (const auto size = model.find("size"); size != model.end()) {
        bytes.resize(size->get<std::size_t>());
    }
    return bytes;
}

// Rebuilds a channel file (a MOT or a WGT): 32-byte channel records, each followed by its 32-byte
// keys; a channel's sixth word is the distance to the next, 0 on the last.
std::vector<std::uint8_t> Channels(const std::vector<const Json *> &channels) {
    Record      record;
    std::size_t at = 0;
    for (std::size_t c = 0; c < channels.size(); c++) {
        const Json &channel = *channels[c];
        const Json &keys = channel.at("keys");
        const Json &reserved = channel.at("reserved_u32");
        record.Word(at, static_cast<std::uint32_t>(channel.at("frame").get<std::int32_t>()));
        record.Word(at + 4, static_cast<std::uint32_t>(channel.at("target").get<std::int32_t>()));
        record.Word(at + 8, static_cast<std::uint32_t>(channel.at("type").get<std::int32_t>()));
        record.Word(at + 12, U32(reserved[0]));
        record.Word(at + 16, static_cast<std::uint32_t>(keys.size()));
        record.Word(at + 20, c + 1 < channels.size() ? static_cast<std::uint32_t>(32 + keys.size() * 32) : 0);
        record.Word(at + 24, U32(reserved[1]));
        record.Word(at + 28, U32(reserved[2]));
        at += 32;
        for (const Json &key : keys) {
            record.Word(at, U32(key.at("frame")));
            for (std::size_t w = 0; w < 3; w++) {
                record.Word(at + 4 + w * 4, U32(key.at("reserved_u32")[w]));
            }
            // A key whose unused lanes held bits that aren't numbers keeps its words.
            if (const auto words = key.find("value_u32"); words != key.end()) {
                for (std::size_t w = 0; w < 4; w++) {
                    record.Word(at + 16 + w * 4, U32((*words)[w]));
                }
            } else {
                for (std::size_t w = 0; w < 4; w++) {
                    record.Float(at + 16 + w * 4, key.at("value")[w].get<float>());
                }
            }
            at += 32;
        }
    }
    return record.Take();
}

std::vector<std::uint8_t> Matrices(const Json &matrices) {
    Record record;
    for (std::size_t m = 0; m < matrices.size(); m++) {
        for (std::size_t k = 0; k < 16; k++) {
            record.Float(m * 64 + k * 4, matrices[m][k].get<float>());
        }
    }
    return record.Take();
}

std::vector<const Json *> Pointers(const Json &list) {
    std::vector<const Json *> pointers;
    for (const Json &item : list) {
        pointers.push_back(&item);
    }
    return pointers;
}

// The entry of a standalone sidecar's list that came from `alias`.
const Json &FromSource(const Json &list, std::string_view alias) {
    for (const Json &item : list) {
        if (Key(item.value("alias", "")) == Key(alias)) {
            return item;
        }
    }
    throw std::runtime_error("bundle lacks its source");
}

// One retail key being rebuilt: its reserved and value words.
struct RetailKey {
    std::array<std::uint32_t, 3> reserved{};
    std::array<std::uint32_t, 4> value{};
};

// One retail channel being rebuilt: its header's reserved words and its keys by source frame.
struct Track {
    std::uint32_t                      frame;
    std::uint32_t                      target;
    std::int32_t                       type;
    std::array<std::uint32_t, 3>       reserved{};
    std::map<std::uint32_t, RetailKey> keys;
};

using TrackId = std::tuple<std::uint32_t, std::int32_t, std::uint32_t>;

// The channels being rebuilt, and the order the sidecar lists them in, the disc's.
struct Tracks {
    std::map<TrackId, Track> by_id;
    std::vector<TrackId>     order;
};

Track &TrackOf(Tracks &tracks, std::uint32_t frame, std::int32_t type, std::uint32_t target) {
    Track &track = tracks.by_id[{frame, type, target}];
    track.frame = frame;
    track.type = type;
    track.target = target;
    return track;
}

std::uint32_t Bits(float value) {
    return std::bit_cast<std::uint32_t>(value);
}

// Adds a channel in its place, with its words and its keys, their frames shifted by `start`.
void AddRetail(Tracks &tracks, const Json &channel, std::uint32_t start) {
    const TrackId id = {static_cast<std::uint32_t>(channel.at("frame").get<std::int32_t>()), channel.at("type").get<std::int32_t>(),
                        static_cast<std::uint32_t>(channel.value("target", 0))};
    Track        &track = TrackOf(tracks, std::get<0>(id), std::get<1>(id), std::get<2>(id));
    tracks.order.push_back(id);
    if (const auto reserved = channel.find("reserved_u32"); reserved != channel.end()) {
        for (std::size_t w = 0; w < 3; w++) {
            track.reserved[w] = U32((*reserved)[w]);
        }
    }
    for (const Json &key : channel.at("keys")) {
        RetailKey retail;
        if (const auto reserved = key.find("reserved_u32"); reserved != key.end()) {
            for (std::size_t w = 0; w < 3; w++) {
                retail.reserved[w] = U32((*reserved)[w]);
            }
        }
        // A key a clip can't give back bit for bit keeps its words.
        if (const auto words = key.find("value_u32"); words != key.end()) {
            for (std::size_t w = 0; w < 4; w++) {
                retail.value[w] = U32((*words)[w]);
            }
        } else {
            for (std::size_t w = 0; w < 4; w++) {
                retail.value[w] = Bits(key.at("value")[w].get<float>());
            }
        }
        track.keys[start + U32(key.at("frame"))] = retail;
    }
}

// Reads one action's transform keys back out of its glTF clip: translations and scales as they
// are, rotations from glTF's normalized x, y, z, w to retail's scalar-first, conjugated form.
// Channels the sidecar keeps whole, by frame and type.
using Whole = std::set<std::pair<std::uint32_t, std::int32_t>>;

void AddClip(Tracks &tracks, const Bundle &bundle, const Json &model, std::size_t node_offset, std::size_t clip,
             std::uint32_t start, std::uint32_t end, const Whole &whole) {
    std::map<std::size_t, std::pair<std::uint32_t, bool>> frames;
    const Json                                           &model_frames = model.at("frames");
    for (std::uint32_t f = 0; f < model_frames.size(); f++) {
        frames[node_offset + f] = {f, false};
        if (const auto helper = model_frames[f].find("rotation_node"); helper != model_frames[f].end()) {
            frames[helper->get<std::size_t>()] = {f, true};
        }
    }
    const fastgltf::Animation &animation = bundle.asset.animations.at(clip);
    for (const fastgltf::AnimationChannel &channel : animation.channels) {
        if (!channel.nodeIndex) {
            continue;
        }
        const auto found = frames.find(*channel.nodeIndex);
        if (found == frames.end()) {
            continue;
        }
        const auto [frame, helper] = found->second;
        std::int32_t type;
        switch (channel.path) {
            case fastgltf::AnimationPath::Rotation:
                type = 0;
                break;
            case fastgltf::AnimationPath::Scale:
                // A helper's scale keys only undo its authored scale.
                if (helper) {
                    continue;
                }
                type = 1;
                break;
            case fastgltf::AnimationPath::Translation:
                type = 2;
                break;
            default:
                continue;
        }
        // A rebaked frame's rotation and scale clips hold its pose, not its keys; the sidecar keeps those.
        if (whole.contains({frame, type}) || (type != 2 && model_frames[frame].value("rebaked", false))) {
            continue;
        }
        const fastgltf::AnimationSampler &sampler = animation.samplers[channel.samplerIndex];
        std::vector<float>                times;
        fastgltf::iterateAccessor<float>(bundle.asset, bundle.asset.accessors[sampler.inputAccessor],
                                         [&](float time) { times.push_back(time); });
        // The clip gives back the keys inside its action; the sidecar keeps those on its ends, and
        // those the clip can't give back bit for bit, which win.
        Track &track = TrackOf(tracks, frame, type, 0);
        auto   inside = [&](std::size_t k) -> RetailKey   *{
            const std::uint32_t at = start + static_cast<std::uint32_t>(std::lround(times.at(k) * 30.0f));
            if (at <= start || at >= end || track.keys.contains(at)) {
                return nullptr;
            }
            return &track.keys[at];
        };
        if (type == 0) {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec4>(
                bundle.asset, bundle.asset.accessors[sampler.outputAccessor], [&](fastgltf::math::fvec4 q, std::size_t k) {
                    if (RetailKey *key = inside(k)) {
                        key->value = {Bits(q[3]), Bits(-q[0]), Bits(-q[1]), Bits(-q[2])};
                    }
                });
        } else {
            fastgltf::iterateAccessorWithIndex<fastgltf::math::fvec3>(
                bundle.asset, bundle.asset.accessors[sampler.outputAccessor], [&](fastgltf::math::fvec3 v, std::size_t k) {
                    if (RetailKey *key = inside(k)) {
                        key->value = {Bits(v[0]), Bits(v[1]), Bits(v[2]), Bits(0.0f)};
                    }
                });
        }
    }
}

// Adds the sidecar's retail keys of a motion, noting the channels it keeps whole.
void AddResidue(Tracks &tracks, Whole &whole, const Json &channels) {
    for (const Json &channel : channels) {
        AddRetail(tracks, channel, 0);
        if (channel.value("complete", false)) {
            whole.insert({static_cast<std::uint32_t>(channel.at("frame").get<std::int32_t>()),
                          channel.at("type").get<std::int32_t>()});
        }
    }
}

std::vector<std::uint8_t> Write(Tracks &tracks);

// Rebuilds a character's MOT from its actions: each action's clip for the transforms, the
// sidecar's channels for everything else, and the retail keys of frames glTF can't give back.
std::vector<std::uint8_t> Motion(State &state, const Bundle &own, const Json &resource) {
    Tracks tracks;
    Whole  whole;
    AddResidue(tracks, whole, resource.value("retail_channels", Json::array()));
    const Bundle                 *bundle = &own;
    std::shared_ptr<const Bundle> target;
    const Json                   *placed = &resource;
    // A motion placed into its targets refers to its placements, unless this is one of them.
    if (const auto external = resource.find("external");
        external != resource.end() && resource.value("variants", Json::array()).empty()) {
        placed = nullptr;
        const std::string key = external->get<std::string>();
        for (const Json &path : resource.value("targets", Json::array())) {
            target = LoadBundle(state, path.get<std::string>());
            const Bundle &candidate = *target;
            for (const Json &entry : candidate.sidecar.at("resources").at("animations")) {
                // A motion placed into models of its own bundle sits beside itself there.
                if (entry.value("external", "") == key &&
                    !entry.value("variants", Json::array()).empty()) {
                    bundle = &candidate;
                    placed = &entry;
                    break;
                }
            }
            if (placed != nullptr) {
                break;
            }
        }
    }
    if (placed != nullptr && !placed->value("variants", Json::array()).empty()) {
        const Json       &variant = placed->at("variants")[0];
        const Json       &model = bundle->sidecar.at("models").at(variant.at("model").get<std::size_t>());
        const std::size_t node_offset = variant.at("node_offset").get<std::size_t>();
        const Json       &actions = placed->at("actions");
        const Json       &clips = variant.at("animations");
        for (std::size_t a = 0; a < actions.size(); a++) {
            const Json         &action = actions[a];
            const std::uint32_t start = U32(action.at("start"));
            if (a < clips.size() && !clips[a].is_null()) {
                AddClip(tracks, *bundle, model, node_offset, clips[a].get<std::size_t>(), start, U32(action.at("end")),
                        whole);
            }
        }
    }
    return Write(tracks);
}

// Writes rebuilt tracks as a MOT. Its channels keep the disc's key frames, and its order, which the
// sidecar lists: the game reads every vertex channel of a frame at the key of the motion's first
// channel, and blends towards keys by frame. Saget's vertex animation in the attract movie, rebuilt in
// another order, wrote past his mesh and crashed its draw.
std::vector<std::uint8_t> Write(Tracks &tracks) {
    std::vector<const Track *> channels;
    std::set<TrackId>          listed;
    for (const TrackId &id : tracks.order) {
        if (listed.insert(id).second) {
            channels.push_back(&tracks.by_id.at(id));
        }
    }
    // A sidecar from before the order was kept lists only the channels keeping keys, and leaves out a
    // clip's channel whose keys all sit on its actions' ends.
    for (const auto &[id, track] : tracks.by_id) {
        if (!listed.contains(id) && !track.keys.empty()) {
            channels.push_back(&track);
        }
    }
    Record      record;
    std::size_t at = 0;
    for (std::size_t c = 0; c < channels.size(); c++) {
        const Track &track = *channels[c];
        record.Word(at, track.frame);
        record.Word(at + 4, track.target);
        record.Word(at + 8, static_cast<std::uint32_t>(track.type));
        record.Word(at + 12, track.reserved[0]);
        record.Word(at + 16, static_cast<std::uint32_t>(track.keys.size()));
        record.Word(at + 20, c + 1 < channels.size() ? static_cast<std::uint32_t>(32 + track.keys.size() * 32) : 0);
        record.Word(at + 24, track.reserved[1]);
        record.Word(at + 28, track.reserved[2]);
        at += 32;
        for (const auto &[frame, key] : track.keys) {
            record.Word(at, frame);
            for (std::size_t w = 0; w < 3; w++) {
                record.Word(at + 4 + w * 4, key.reserved[w]);
            }
            for (std::size_t w = 0; w < 4; w++) {
                record.Word(at + 16 + w * 4, key.value[w]);
            }
            at += 32;
        }
    }
    return record.Take();
}

// Rebuilds a motion a scene's own configuration links to it, published beside the scene: its
// clip, named after the motion, covers it from frame 0, and the sidecar keeps the rest.
std::vector<std::uint8_t> SceneMotion(const Bundle &bundle, std::string_view alias) {
    Tracks tracks;
    Whole  whole;
    Json   own = Json::array();
    for (const Json &channel : bundle.sidecar.at("motion_channels")) {
        if (Key(channel.value("alias", "")) == Key(alias)) {
            own.push_back(channel);
        }
    }
    AddResidue(tracks, whole, own);
    std::string_view  leaf = alias.substr(alias.rfind('/') + 1);
    const std::string clip_name = std::string(leaf.substr(0, leaf.rfind('.'))) + "#0";
    for (std::size_t a = 0; a < bundle.asset.animations.size(); a++) {
        if (std::string_view(bundle.asset.animations[a].name) == clip_name) {
            const fastgltf::Animation &animation = bundle.asset.animations[a];
            std::uint32_t              end = 0;
            for (const fastgltf::AnimationSampler &sampler : animation.samplers) {
                fastgltf::iterateAccessor<float>(bundle.asset, bundle.asset.accessors[sampler.inputAccessor], [&](float time) {
                    end = std::max(end, static_cast<std::uint32_t>(std::lround(time * 30.0f)));
                });
            }
            AddClip(tracks, bundle, bundle.sidecar, 0, a, 0, end, whole);
            break;
        }
    }
    return Write(tracks);
}

const Json &ModelOf(const Bundle &bundle, const Json &entry) {
    if (bundle.sidecar.value("kind", "") == "character") {
        return bundle.sidecar.at("models").at(entry.at("index").get<std::size_t>());
    }
    return bundle.sidecar;
}

std::unique_ptr<std::vector<std::uint8_t>> Build(State &state, std::string_view alias, const Json &entry) {
    const std::shared_ptr<const Bundle> holder = LoadBundle(state, entry.at("bundle").get<std::string>());
    const Bundle                       &bundle = *holder;
    const std::string                  &kind = entry.at("kind").get_ref<const std::string &>();
    const bool                          character = bundle.sidecar.value("kind", "") == "character";
    auto                                bytes = [](std::vector<std::uint8_t> value) {
        return std::make_unique<std::vector<std::uint8_t>>(std::move(value));
    };
    if (kind == "model") {
        return bytes(Scene(bundle, ModelOf(bundle, entry)));
    }
    // A motion is as long as the disc's, its tail zeros.
    auto padded = [&](std::vector<std::uint8_t> motion) {
        motion.resize(std::max<std::size_t>(motion.size(), entry.value("size", std::size_t{0})));
        return motion;
    };
    if (character && kind == "animations") {
        return bytes(padded(Motion(state, bundle, bundle.sidecar.at("resources").at(kind).at(entry.at("index").get<std::size_t>()))));
    }
    if (character && (kind == "weights" || kind == "bind_poses")) {
        const Json &resource = bundle.sidecar.at("resources").at(kind).at(entry.at("index").get<std::size_t>());
        return bytes(kind == "weights" ? Channels(Pointers(resource.at("channels"))) : Matrices(resource.at("matrices")));
    }
    if (kind == "weights") {
        return bytes(Channels(Pointers(FromSource(bundle.sidecar.at("weights"), alias).at("channels"))));
    }
    if (kind == "bind_poses") {
        return bytes(Matrices(FromSource(bundle.sidecar.at("bind_poses"), alias).at("matrices")));
    }
    if (kind == "motion_channels") {
        return bytes(padded(SceneMotion(bundle, alias)));
    }
    return nullptr;
}

} // namespace

bool Publishes(std::string_view alias) {
    State                      &state = Shared();
    std::lock_guard<std::mutex> lock(state.mutex);
    Refresh(state);
    return state.index.contains(Key(alias));
}

namespace {

// Rebuild, with the state locked. The bytes live until the next rebuild.
const std::vector<std::uint8_t> *RebuildLocked(State &state, std::string_view alias) {
    Refresh(state);
    const std::string key = Key(alias);
    if (auto found = state.rebuilt.find(key); found != state.rebuilt.end()) {
        return found->second.get();
    }
    auto entry = state.index.find(key);
    // A source that failed once fails again; it isn't parsed anew at every read of its file.
    if (entry == state.index.end() || state.failed.contains(key)) {
        return nullptr;
    }
    std::unique_ptr<std::vector<std::uint8_t>> bytes;
    try {
        bytes = Build(state, alias, entry->second);
        // Models, weights and bind poses come back as the disc has them, or not at all.
        const std::string kind = entry->second.value("kind", "");
        if (bytes != nullptr && kind != "animations" && kind != "motion_channels" && entry->second.contains("crc32") &&
            Crc32(*bytes) != entry->second["crc32"].get<std::uint32_t>()) {
            throw std::runtime_error("rebuilt bytes don't match the disc's");
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "%.*s: cannot rebuild from its glTF: %s\n", static_cast<int>(alias.size()), alias.data(),
                     error.what());
        bytes.reset();
    }
    if (bytes == nullptr) {
        state.failed.insert(key);
        return nullptr;
    }
    while (!state.rebuilt_order.empty() && state.rebuilt_bytes + bytes->size() > kKeptRebuiltBytes) {
        auto oldest = state.rebuilt.find(state.rebuilt_order.front());
        state.rebuilt_bytes -= oldest->second->size();
        state.rebuilt.erase(oldest);
        state.rebuilt_order.pop_front();
    }
    state.rebuilt_bytes += bytes->size();
    state.rebuilt_order.push_back(key);
    return state.rebuilt.emplace(key, std::move(bytes)).first->second.get();
}

} // namespace

const std::vector<std::uint8_t> *Rebuild(std::string_view alias) {
    State                      &state = Shared();
    std::lock_guard<std::mutex> lock(state.mutex);
    return RebuildLocked(state, alias);
}

namespace {

// The place of one pack member in a buffer: its entry, its bytes and the room before the next entry.
struct Place {
    std::size_t entry;
    std::size_t start;
    std::size_t room;
};

std::uint32_t Read32(std::span<const std::uint8_t> bytes, std::size_t at) {
    std::uint32_t value = 0;
    std::memcpy(&value, bytes.data() + at, 4);
    return value;
}

// Finds a member in the pack spanning `bytes`, by name or, after "#entry:", by position.
std::optional<Place> Member(std::span<const std::uint8_t> bytes, std::string_view name) {
    std::optional<std::size_t> wanted;
    if (const std::size_t marker = name.find("#entry:"); marker != std::string_view::npos) {
        wanted = std::stoul(std::string(name.substr(marker + 7)));
        name = name.substr(0, marker);
    }
    std::size_t at = 0;
    for (std::size_t index = 0; at + 76 <= bytes.size() && bytes[at] != 0; index++) {
        const std::size_t offset = Read32(bytes, at + 64);
        const std::size_t size = Read32(bytes, at + 68);
        const std::size_t next = Read32(bytes, at + 72);
        if (offset < 76 || next < 76 || next > bytes.size() - at || offset > next) {
            return std::nullopt;
        }
        const char *entry_name = reinterpret_cast<const char *>(bytes.data() + at);
        const bool  match = wanted ? index == *wanted
                                   : Key(std::string_view(entry_name, strnlen(entry_name, 64))) == Key(name);
        if (match) {
            // A converted member may have been emptied from the data; its room remains.
            return Place{at, at + offset, std::max(next - offset, size)};
        }
        at += next;
    }
    return std::nullopt;
}

} // namespace

namespace {

// Writes one source back where the disc has it in `bytes`: each "/member" of `rest` enters a pack,
// "#offset:n" a scene inside the file or member.
void SpliceOne(State &state, const std::string &alias, const Json &entry, const std::string &rest,
               std::span<std::uint8_t> bytes) {
    std::span<std::uint8_t> place = bytes;
    std::optional<Place>    member;
    for (std::size_t at = 0; at < rest.size();) {
        const std::size_t end = rest.find('/', at + 1);
        const std::string part = rest.substr(at, end == std::string::npos ? std::string::npos : end - at);
        at = end == std::string::npos ? rest.size() : end;
        std::string name = part.front() == '/' ? part.substr(1) : part;
        std::size_t offset = 0;
        if (const std::size_t marker = name.find("#offset:"); marker != std::string::npos) {
            offset = std::stoul(name.substr(marker + 8));
            name = name.substr(0, marker);
        }
        if (!name.empty()) {
            member = Member(place, name);
            if (!member) {
                return;
            }
            place = place.subspan(member->start, std::min(member->room, place.size() - member->start));
        }
        if (offset > 0) {
            if (offset > place.size()) {
                return;
            }
            place = place.subspan(offset);
            member.reset();
        }
    }
    // Only the bytes the source was converted from, or the zeros stripping left of them, are
    // replaced: a file a mod or another release changed keeps what it has.
    const std::size_t size = std::min<std::size_t>(entry.value("size", place.size()), place.size());
    const auto        current = place.first(size);
    const bool        emptied = std::all_of(current.begin(), current.end(), [](std::uint8_t b) { return b == 0; });
    if (!emptied && entry.contains("crc32") && Crc32(current) != entry["crc32"].get<std::uint32_t>()) {
        std::fprintf(stderr, "%s: the data's bytes aren't those its glTF was converted from; kept as read\n", alias.c_str());
        return;
    }
    const std::vector<std::uint8_t> *rebuilt = RebuildLocked(state, alias);
    if (rebuilt == nullptr || rebuilt->size() > place.size()) {
        if (emptied && state.stripped) {
            Unrecoverable(alias + " can't be rebuilt from its glTF");
        }
        if (rebuilt != nullptr) {
            std::fprintf(stderr, "%s: rebuilt source is larger than its place, kept as read\n", alias.c_str());
        }
        return;
    }
    // A motion goes in only as the disc has it. The sidecar doesn't yet keep the order of a motion's
    // channels, and the game reads every vertex channel at the first channel's key: rebuilt in another
    // order, Saget's vertex animation in the attract movie wrote past his mesh and crashed its draw.
    const std::string &kind = entry.at("kind").get_ref<const std::string &>();
    if ((kind == "animations" || kind == "motion_channels") && entry.contains("crc32") &&
        Crc32(*rebuilt) != entry["crc32"].get<std::uint32_t>()) {
        if (emptied && state.stripped) {
            Unrecoverable(alias + " can't be rebuilt from its glTF as the disc has it");
        }
        return;
    }
    std::memcpy(place.data(), rebuilt->data(), rebuilt->size());
    // A pack member's size becomes the rebuilt one; the game reads motions by their own chain.
    if (member) {
        const std::uint32_t size = static_cast<std::uint32_t>(rebuilt->size());
        std::memcpy(place.data() - (member->start - member->entry) + 68, &size, 4);
    }
}

} // namespace

void Splice(std::string_view path, std::span<std::uint8_t> bytes) {
    State                      &state = Shared();
    std::lock_guard<std::mutex> lock(state.mutex);
    Refresh(state);
    std::vector<std::tuple<std::string, std::string, const Json *>> sources;
    const std::string                                               file = Key(path);
    for (auto it = state.index.lower_bound(file); it != state.index.end() && it->first.starts_with(file); ++it) {
        const std::string_view rest = std::string_view(it->first).substr(file.size());
        const std::string      kind = it->second.value("kind", "");
        const bool             rebuildable = kind == "model" || kind == "animations" || kind == "weights" ||
                                             kind == "bind_poses" || kind == "motion_channels";
        if (rebuildable && (rest.empty() || rest.front() == '/' || rest.front() == '#')) {
            sources.emplace_back(it->first, std::string(rest), &it->second);
        }
    }
    for (const auto &[alias, rest, entry] : sources) {
        // Nothing a source can't be rebuilt for reaches the game: the file keeps what it read.
        try {
            SpliceOne(state, alias, *entry, rest, bytes);
        } catch (const std::exception &error) {
            std::fprintf(stderr, "%s: cannot rebuild from its glTF: %s\n", alias.c_str(), error.what());
        }
    }
}

void Reset() {
    State                      &state = Shared();
    std::lock_guard<std::mutex> lock(state.mutex);
    Forget(state);
}

} // namespace dc::data::model_gltf

// Mod framework phase 1b: 3D model dump and replacement (Windows fork).
//
//   DC_DUMP_MODELS=1 (run_win.ps1 -DumpModels)   writes mods/_dump/models/<texture>__<N>v.obj for every model built
//   mods/<mod>/models/<texture>.obj|.glb          replaces every model whose first texture is <texture>
//   mods/<mod>/models/<texture>__<N>v.obj         replaces only the model that has N vertices (what the dump writes)
//   mods/<mod>/models/<same name>.json            optional: {"scale":1,"rotate_deg":[0,0,0],"offset":[0,0,0],"flip_v":false}
//
// The replacement keeps the first material of the model it replaces (so the PNG texture mods repaint it) and is drawn
// with one strip. OBJ files are read in the dump's own axes and units, so a dump edited in Blender and exported with the
// same axis settings round-trips exactly. A .glb (glTF binary: Unity, Unreal, Blender) is used as it is, with the
// sidecar .json to scale/rotate/offset it into the game's units.

#include "moonmod/mod_model.hpp"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <exception>
#include <array>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include "render/draw3d.hpp"
#include "moonmod/mods_directory.hpp"
#include "moonmod/runtime.hpp"
#include "platform/paths.hpp"

namespace dc::game {}
using namespace dc::game;
using namespace dc::render;

namespace fs = std::filesystem;

namespace {

struct ModelFile {
    fs::path path;
    int      verts = -1; // -1: any model with the texture
};

struct Model {
    bool                       ok = false;
    std::vector<gfx::Vertex3D> vertices;
    std::vector<uint32_t>      indices;
    // OBJ `usemtl strip<N>` groups, as the dump writes them: which original strip's material each run of indices uses.
    struct Group {
        int      strip;
        uint32_t first_index;
        uint32_t index_count;
    };
    std::vector<Group> groups; // empty: the whole mesh takes the original's first material
};

struct State {
    bool                                     inited = false;
    bool                                     dump = false;
    fs::path                                 root;
    std::map<std::string, std::vector<ModelFile>> by_name; // later mods last
    std::map<std::string, Model>             cache;
    std::set<std::string>                    dumped;
    int                                      replaced = 0;
    std::map<const void *, std::string>      skin_name; // DC_DEBUG_SKIN=1: which replaced models the game re-skins
    std::set<const void *>                   skin_seen;
    std::map<const void *, std::vector<const void *>> skin_blocks; // an original model's vertex array -> blocks of its replaced visuals
    std::map<const Draw3DVisual *, std::vector<const void *>> skin_bindings;
};

State g;

void BindSkinBlock(const Draw3DVisual &visual, const float (*original)[4], const void *block) {
    if (original == nullptr || block == nullptr) return;
    auto &blocks = g.skin_blocks[original];
    if (std::find(blocks.begin(), blocks.end(), block) == blocks.end()) blocks.push_back(block);
    auto &bindings = g.skin_bindings[&visual];
    if (std::find(bindings.begin(), bindings.end(), block) == bindings.end()) bindings.push_back(block);
}

std::string Fold(std::string s) {
    for (char &c : s) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }
    return s;
}

std::string Utf8(const fs::path &p) {
    auto u = p.u8string();
    return std::string(u.begin(), u.end());
}

std::string StripExtension(std::string name) {
    size_t dot = name.find('.');
    if (dot != std::string::npos) {
        name.erase(dot);
    }
    return name;
}

// "name__123v" -> name, 123; otherwise the stem and -1.
void SplitStem(const std::string &stem, std::string &name, int &verts) {
    name = stem;
    verts = -1;
    if (stem.empty()) return;
    size_t at = stem.rfind("__");
    if (at == std::string::npos || stem.back() != 'v') {
        return;
    }
    std::string digits = stem.substr(at + 2, stem.size() - at - 3);
    if (digits.empty() || !std::all_of(digits.begin(), digits.end(), [](unsigned char c) { return std::isdigit(c); })) {
        return;
    }
    name = stem.substr(0, at);
    verts = std::atoi(digits.c_str());
}

void Init() {
    g.inited = true;
    const char *dump = std::getenv("DC_DUMP_MODELS");
    g.dump = dump != nullptr && dump[0] != '0';
    g.root = moonmod::ModsDirectory(PathsExecutable().parent_path(), PathsSaveRoot());
    std::error_code error;
    if (!fs::is_directory(g.root, error)) {
        return;
    }
    // Use the same resolved load order and active package set as MoonFactory.
    const auto records = moonmod::SharedRuntime().Mods();
    for (const std::string &mod_id : moonmod::SharedRuntime().LoadedModOrder()) {
        const auto found = std::ranges::find(records, mod_id, &moonmod::ModRecord::id);
        if (found == records.end() || !found->enabled || found->state != moonmod::ModState::Loaded) continue;
        const fs::path &mod = found->package_root;
        fs::path models = mod / "models";
        if (!fs::is_directory(models, error)) {
            continue;
        }
        int count = 0;
        for (const auto &entry : fs::directory_iterator(models, error)) {
            std::string ext = Fold(Utf8(entry.path().extension()));
            if (ext != ".obj" && ext != ".glb") {
                continue;
            }
            ModelFile file;
            file.path = entry.path();
            std::string name;
            SplitStem(Fold(Utf8(entry.path().stem())), name, file.verts);
            g.by_name[name].push_back(file);
            count++;
        }
        std::fprintf(stderr, "mods: %s: %d model override(s)\n", Utf8(mod.filename()).c_str(), count);
    }
}

// ---- Transform sidecar -----------------------------------------------------------------------------------------------

struct Transform {
    float scale = 1.0f;
    float rotate[3] = {0, 0, 0}; // degrees
    float offset[3] = {0, 0, 0};
    bool  flip_v = false;
};

Transform ReadTransform(const fs::path &model) {
    Transform t;
    fs::path sidecar = model;
    sidecar.replace_extension(".json");
    std::ifstream in(sidecar);
    if (!in) {
        return t;
    }
    auto json = nlohmann::json::parse(in, nullptr, false, true);
    if (json.is_discarded() || !json.is_object()) {
        std::fprintf(stderr, "mods: %s is not valid JSON; ignored\n", Utf8(sidecar).c_str());
        return t;
    }
    if ((json.contains("scale") && !json["scale"].is_number()) ||
        (json.contains("flip_v") && !json["flip_v"].is_boolean())) {
        std::fprintf(stderr, "mods: %s has invalid transform fields; defaults used\n", Utf8(sidecar).c_str());
        return t;
    }
    t.scale = json.value("scale", 1.0f);
    t.flip_v = json.value("flip_v", false);
    for (const char *key : {"rotate_deg", "offset"}) {
        if (json.contains(key) && json[key].is_array() && json[key].size() == 3) {
            float *target = std::strcmp(key, "offset") == 0 ? t.offset : t.rotate;
            for (int i = 0; i < 3; i++) {
                if (!json[key][i].is_number()) {
                    std::fprintf(stderr, "mods: %s has invalid transform fields; defaults used\n", Utf8(sidecar).c_str());
                    return Transform{};
                }
                target[i] = json[key][i].get<float>();
            }
        }
    }
    if (!std::isfinite(t.scale) || std::abs(t.scale) > 1000000.0f) {
        std::fprintf(stderr, "mods: %s has an invalid scale; defaults used\n", Utf8(sidecar).c_str());
        return Transform{};
    }
    for (int i = 0; i < 3; i++) {
        if (!std::isfinite(t.rotate[i]) || !std::isfinite(t.offset[i]) ||
            std::abs(t.rotate[i]) > 1000000.0f || std::abs(t.offset[i]) > 1000000.0f) {
            std::fprintf(stderr, "mods: %s has invalid transform values; defaults used\n", Utf8(sidecar).c_str());
            return Transform{};
        }
    }
    return t;
}

void Rotate(float v[3], const float deg[3]) {
    const float k = 3.14159265358979f / 180.0f;
    float       x = v[0], y = v[1], z = v[2];
    float       c = std::cos(deg[0] * k), s = std::sin(deg[0] * k);
    float       y1 = y * c - z * s, z1 = y * s + z * c;
    y = y1;
    z = z1;
    c = std::cos(deg[1] * k);
    s = std::sin(deg[1] * k);
    float x2 = x * c + z * s, z2 = -x * s + z * c;
    x = x2;
    z = z2;
    c = std::cos(deg[2] * k);
    s = std::sin(deg[2] * k);
    float x3 = x * c - y * s, y3 = x * s + y * c;
    v[0] = x3;
    v[1] = y3;
    v[2] = z;
}

void Normalize(float v[3]) {
    float len = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (len > 1e-8f) {
        for (int i = 0; i < 3; i++) {
            v[i] /= len;
        }
    } else {
        v[0] = 0;
        v[1] = 1;
        v[2] = 0;
    }
}

void ApplyTransform(Model &m, const Transform &t) {
    for (gfx::Vertex3D &v : m.vertices) {
        Rotate(v.position, t.rotate);
        Rotate(v.normal, t.rotate);
        for (int i = 0; i < 3; i++) {
            v.position[i] = v.position[i] * t.scale + t.offset[i];
        }
        if (t.flip_v) {
            v.uv[1] = 1.0f - v.uv[1];
        }
    }
}

void ComputeNormals(Model &m) {
    for (gfx::Vertex3D &v : m.vertices) {
        v.normal[0] = v.normal[1] = v.normal[2] = 0;
    }
    for (size_t i = 0; i + 2 < m.indices.size(); i += 3) {
        gfx::Vertex3D &a = m.vertices[m.indices[i]];
        gfx::Vertex3D &b = m.vertices[m.indices[i + 1]];
        gfx::Vertex3D &c = m.vertices[m.indices[i + 2]];
        float e1[3], e2[3], n[3];
        for (int k = 0; k < 3; k++) {
            e1[k] = b.position[k] - a.position[k];
            e2[k] = c.position[k] - a.position[k];
        }
        n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        for (int k = 0; k < 3; k++) {
            a.normal[k] += n[k];
            b.normal[k] += n[k];
            c.normal[k] += n[k];
        }
    }
    for (gfx::Vertex3D &v : m.vertices) {
        Normalize(v.normal);
    }
}

// ---- OBJ ---------------------------------------------------------------------------------------------------------------

Model LoadObj(const fs::path &file) {
    Model         m;
    std::ifstream in(file);
    if (!in) {
        return m;
    }
    std::vector<std::array<float, 3>> positions, normals;
    std::vector<std::array<float, 2>> uvs;
    std::map<std::tuple<int, int, int>, uint32_t> seen;
    bool        any_normal = false;
    int         group = -1;
    bool        any_group = false;
    std::map<int, std::vector<uint32_t>> grouped; // strip number -> indices (-1: before any usemtl)
    std::string line;
    while (std::getline(in, line)) {
        std::istringstream s(line);
        std::string        tag;
        s >> tag;
        if (tag == "usemtl") {
            std::string name;
            s >> name;
            if (name.rfind("strip", 0) == 0 && name.size() > 5 &&
                std::all_of(name.begin() + 5, name.end(), [](unsigned char c) { return std::isdigit(c); })) {
                group = std::atoi(name.c_str() + 5);
                any_group = true;
            }
        } else if (tag == "v") {
            std::array<float, 3> p{};
            s >> p[0] >> p[1] >> p[2];
            positions.push_back(p);
        } else if (tag == "vn") {
            std::array<float, 3> n{};
            s >> n[0] >> n[1] >> n[2];
            normals.push_back(n);
        } else if (tag == "vt") {
            std::array<float, 2> t{};
            s >> t[0] >> t[1];
            uvs.push_back(t);
        } else if (tag == "f") {
            std::vector<uint32_t> face;
            std::string           token;
            while (s >> token) {
                int  idx[3] = {0, 0, 0};
                int  part = 0;
                std::string number;
                for (size_t i = 0; i <= token.size(); i++) {
                    if (i == token.size() || token[i] == '/') {
                        if (!number.empty() && part < 3) {
                            idx[part] = std::atoi(number.c_str());
                        }
                        number.clear();
                        part++;
                    } else {
                        number += token[i];
                    }
                }
                auto fix = [](int i, size_t n) { return i > 0 ? i - 1 : (i < 0 ? static_cast<int>(n) + i : -1); };
                int  pi = fix(idx[0], positions.size());
                int  ti = fix(idx[1], uvs.size());
                int  ni = fix(idx[2], normals.size());
                if (pi < 0 || pi >= static_cast<int>(positions.size())) {
                    continue;
                }
                auto key = std::make_tuple(pi, ti, ni);
                auto it = seen.find(key);
                if (it == seen.end()) {
                    gfx::Vertex3D v = {};
                    std::memcpy(v.position, positions[pi].data(), sizeof(v.position));
                    if (ti >= 0 && ti < static_cast<int>(uvs.size())) {
                        v.uv[0] = uvs[ti][0];
                        v.uv[1] = 1.0f - uvs[ti][1]; // OBJ v points up, the game's down
                    }
                    if (ni >= 0 && ni < static_cast<int>(normals.size())) {
                        std::memcpy(v.normal, normals[ni].data(), sizeof(v.normal));
                        any_normal = true;
                    }
                    std::memset(v.color, 0x80, sizeof(v.color));
                    it = seen.emplace(key, static_cast<uint32_t>(m.vertices.size())).first;
                    m.vertices.push_back(v);
                }
                face.push_back(it->second);
            }
            for (size_t i = 1; i + 1 < face.size(); i++) { // fan
                auto &target = grouped[group];
                target.insert(target.end(), {face[0], face[i], face[i + 1]});
            }
        }
    }
    if (!any_group) {
        m.indices = grouped[-1];
    } else {
        std::map<int, std::vector<uint32_t>> merged; // faces before the first usemtl count as strip 0
        for (auto &[key, list] : grouped) {
            auto &dst = merged[key < 0 ? 0 : key];
            dst.insert(dst.end(), list.begin(), list.end());
        }
        for (auto &[key, list] : merged) {
            m.groups.push_back({key, static_cast<uint32_t>(m.indices.size()), static_cast<uint32_t>(list.size())});
            m.indices.insert(m.indices.end(), list.begin(), list.end());
        }
    }
    if (m.vertices.empty() || m.indices.empty()) {
        return m;
    }
    if (!any_normal) {
        ComputeNormals(m);
    }
    m.ok = true;
    return m;
}

// ---- GLB ---------------------------------------------------------------------------------------------------------------

int AccessorComponentSize(int type) {
    return type == 5126 || type == 5125 ? 4 :
           (type == 5122 || type == 5123 ? 2 :
            (type == 5120 || type == 5121 ? 1 : 0));
}

bool ReadBufferView(const nlohmann::json &doc, const std::vector<uint8_t> &bin, int view_index,
                    size_t byte_offset, size_t byte_length, size_t &offset) {
    if (!doc.contains("bufferViews") || !doc["bufferViews"].is_array() || view_index < 0 ||
        view_index >= static_cast<int>(doc["bufferViews"].size())) return false;
    const auto &view = doc["bufferViews"][view_index];
    if (!view.is_object()) return false;
    const size_t view_offset = view.value("byteOffset", size_t{0});
    const size_t view_length = view.value("byteLength", size_t{0});
    if (view_offset > bin.size() || view_length > bin.size() - view_offset || byte_offset > view_length ||
        byte_length > view_length - byte_offset || view_offset + byte_offset > bin.size()) return false;
    offset = view_offset + byte_offset;
    return true;
}

float ReadAccessorComponent(const uint8_t *bytes, int type, bool normalized) {
    if (type == 5126) {
        float value;
        std::memcpy(&value, bytes, sizeof(value));
        return value;
    }
    if (type == 5123) {
        uint16_t value;
        std::memcpy(&value, bytes, sizeof(value));
        return normalized ? value / 65535.0f : static_cast<float>(value);
    }
    if (type == 5121) return normalized ? bytes[0] / 255.0f : static_cast<float>(bytes[0]);
    if (type == 5122) {
        int16_t value;
        std::memcpy(&value, bytes, sizeof(value));
        return normalized ? std::max(-1.0f, value / 32767.0f) : static_cast<float>(value);
    }
    int8_t value;
    std::memcpy(&value, bytes, sizeof(value));
    return normalized ? std::max(-1.0f, value / 127.0f) : static_cast<float>(value);
}

bool ReadSparseIndices(const nlohmann::json &doc, const std::vector<uint8_t> &bin,
                       const nlohmann::json &sparse, size_t accessor_count,
                       std::vector<uint32_t> &indices) {
    if (!sparse.is_object() || !sparse.contains("count") ||
        (!sparse["count"].is_number_unsigned() && !sparse["count"].is_number_integer()) ||
        !sparse.contains("indices") || !sparse["indices"].is_object()) return false;
    const size_t count = sparse["count"].get<size_t>();
    const auto &definition = sparse["indices"];
    if (count == 0 || count > accessor_count || !definition.contains("bufferView") ||
        !definition.contains("componentType")) return false;
    const int type = definition["componentType"].get<int>();
    const int size = type == 5121 ? 1 : (type == 5123 ? 2 : (type == 5125 ? 4 : 0));
    if (size == 0) return false;
    const size_t relative = definition.value("byteOffset", size_t{0});
    size_t offset = 0;
    if (count > std::numeric_limits<size_t>::max() / static_cast<size_t>(size) ||
        !ReadBufferView(doc, bin, definition["bufferView"].get<int>(), relative,
                        count * static_cast<size_t>(size), offset)) return false;
    indices.resize(count);
    uint32_t previous = 0;
    for (size_t i = 0; i < count; ++i) {
        uint32_t value = 0;
        std::memcpy(&value, &bin[offset + i * static_cast<size_t>(size)], static_cast<size_t>(size));
        if (value >= accessor_count || (i != 0 && value <= previous)) return false;
        indices[i] = value;
        previous = value;
    }
    return true;
}

bool ReadAccessor(const nlohmann::json &doc, const std::vector<uint8_t> &bin, int index, int want_components, std::vector<float> &out) {
    if (!doc.contains("accessors") || !doc["accessors"].is_array() || index < 0 ||
        index >= static_cast<int>(doc["accessors"].size()) || want_components <= 0 || want_components > 4) return false;
    const auto &acc = doc["accessors"][index];
    const char *expected_type = want_components == 1 ? "SCALAR" :
                                (want_components == 2 ? "VEC2" : (want_components == 3 ? "VEC3" : "VEC4"));
    if (!acc.is_object() || !acc.contains("count") || !acc.contains("componentType") ||
        !acc.contains("type") || !acc["type"].is_string() || acc["type"] != expected_type) return false;
    const size_t count = acc["count"].get<size_t>();
    const int type = acc["componentType"].get<int>();
    const int component_size = AccessorComponentSize(type);
    const bool normalized = acc.value("normalized", false);
    if (component_size == 0 || type == 5125 || count == 0 || count > 4000000 ||
        count > std::numeric_limits<size_t>::max() / static_cast<size_t>(want_components)) return false;
    const size_t element_size = static_cast<size_t>(component_size) * want_components;
    out.assign(count * want_components, 0.0f);
    if (acc.contains("bufferView")) {
        const int view_index = acc["bufferView"].get<int>();
        const size_t accessor_offset = acc.value("byteOffset", size_t{0});
        if (!doc.contains("bufferViews") || !doc["bufferViews"].is_array() || view_index < 0 ||
            view_index >= static_cast<int>(doc["bufferViews"].size())) return false;
        const auto &view = doc["bufferViews"][view_index];
        if (!view.is_object()) return false;
        const size_t view_length = view.value("byteLength", size_t{0});
        if (accessor_offset > view_length || element_size > view_length - accessor_offset) return false;
        size_t offset = 0;
        if (!ReadBufferView(doc, bin, view_index, accessor_offset, view_length - accessor_offset, offset)) return false;
        const size_t stride = view.value("byteStride", element_size);
        if (stride < element_size || (count - 1) > (view_length - accessor_offset - element_size) / stride ||
            (count - 1) > (bin.size() - offset - element_size) / stride) return false;
        for (size_t i = 0; i < count; ++i) {
            for (int c = 0; c < want_components; ++c) {
                const size_t at = offset + i * stride + static_cast<size_t>(c) * component_size;
                out[i * want_components + c] = ReadAccessorComponent(&bin[at], type, normalized);
            }
        }
    } else if (acc.contains("byteOffset")) {
        return false;
    }
    if (acc.contains("sparse")) {
        std::vector<uint32_t> sparse_indices;
        if (!ReadSparseIndices(doc, bin, acc["sparse"], count, sparse_indices)) return false;
        const auto &values = acc["sparse"]["values"];
        if (!values.is_object() || !values.contains("bufferView") ||
            sparse_indices.size() > std::numeric_limits<size_t>::max() / element_size) return false;
        const size_t value_bytes = sparse_indices.size() * element_size;
        size_t value_offset = 0;
        if (!ReadBufferView(doc, bin, values["bufferView"].get<int>(), values.value("byteOffset", size_t{0}),
                            value_bytes, value_offset)) return false;
        for (size_t i = 0; i < sparse_indices.size(); ++i) {
            const size_t destination = static_cast<size_t>(sparse_indices[i]) * want_components;
            const size_t source = value_offset + i * element_size;
            for (int c = 0; c < want_components; ++c) {
                out[destination + c] = ReadAccessorComponent(&bin[source + static_cast<size_t>(c) * component_size], type, normalized);
            }
        }
    }
    return true;
}

bool ReadIndexAccessor(const nlohmann::json &doc, const std::vector<uint8_t> &bin, int index, std::vector<uint32_t> &out) {
    if (!doc.contains("accessors") || !doc["accessors"].is_array() || index < 0 ||
        index >= static_cast<int>(doc["accessors"].size())) return false;
    const auto &acc = doc["accessors"][index];
    if (!acc.is_object() || !acc.contains("count") || !acc.contains("componentType") ||
        !acc.contains("type") || !acc["type"].is_string() || acc["type"] != "SCALAR" ||
        acc.value("normalized", false)) return false;
    const size_t count = acc["count"].get<size_t>();
    const int type = acc["componentType"].get<int>();
    const int size = type == 5125 ? 4 : (type == 5123 ? 2 : (type == 5121 ? 1 : 0));
    if (size == 0 || count == 0 || count > 4000000) return false;
    out.assign(count, 0);
    if (acc.contains("bufferView")) {
        const size_t accessor_offset = acc.value("byteOffset", size_t{0});
        const int view_index = acc["bufferView"].get<int>();
        if (!doc.contains("bufferViews") || !doc["bufferViews"].is_array() || view_index < 0 ||
            view_index >= static_cast<int>(doc["bufferViews"].size())) return false;
        const size_t view_length = doc["bufferViews"][view_index].value("byteLength", size_t{0});
        if (accessor_offset > view_length || count > (view_length - accessor_offset) / static_cast<size_t>(size)) return false;
        size_t offset = 0;
        if (!ReadBufferView(doc, bin, view_index, accessor_offset, view_length - accessor_offset, offset) ||
            count > (bin.size() - offset) / static_cast<size_t>(size)) return false;
        for (size_t i = 0; i < count; ++i) {
            std::memcpy(&out[i], &bin[offset + i * static_cast<size_t>(size)], static_cast<size_t>(size));
        }
    } else if (acc.contains("byteOffset")) {
        return false;
    }
    if (acc.contains("sparse")) {
        std::vector<uint32_t> sparse_indices;
        if (!ReadSparseIndices(doc, bin, acc["sparse"], count, sparse_indices)) return false;
        const auto &values = acc["sparse"]["values"];
        size_t value_offset = 0;
        if (!values.is_object() || !values.contains("bufferView") ||
            !ReadBufferView(doc, bin, values["bufferView"].get<int>(), values.value("byteOffset", size_t{0}),
                            sparse_indices.size() * static_cast<size_t>(size), value_offset)) return false;
        for (size_t i = 0; i < sparse_indices.size(); ++i) {
            uint32_t value = 0;
            std::memcpy(&value, &bin[value_offset + i * static_cast<size_t>(size)], static_cast<size_t>(size));
            out[sparse_indices[i]] = value;
        }
    }
    return true;
}

Model LoadGlb(const fs::path &file) {
    Model         m;
    std::ifstream in(file, std::ios::binary);
    if (!in) {
        return m;
    }
    std::vector<uint8_t> data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (data.size() < 20 || std::memcmp(data.data(), "glTF", 4) != 0) {
        std::fprintf(stderr, "mods: %s is not a .glb file (export glTF Binary)\n", Utf8(file).c_str());
        return m;
    }
    std::string          json_text;
    std::vector<uint8_t> bin;
    size_t               at = 12;
    while (at + 8 <= data.size()) {
        uint32_t length, type;
        std::memcpy(&length, &data[at], 4);
        std::memcpy(&type, &data[at + 4], 4);
        at += 8;
        if (at + length > data.size()) {
            break;
        }
        if (type == 0x4E4F534A) {
            json_text.assign(reinterpret_cast<const char *>(&data[at]), length);
        } else if (type == 0x004E4942 && bin.empty()) {
            bin.assign(data.begin() + at, data.begin() + at + length);
        }
        at += (length + 3u) & ~3u;
    }
    auto doc = nlohmann::json::parse(json_text, nullptr, false);
    if (doc.is_discarded() || !doc.contains("meshes") || !doc["meshes"].is_array() || doc["meshes"].empty()) {
        std::fprintf(stderr, "mods: %s has no mesh\n", Utf8(file).c_str());
        return m;
    }

    // glTF matrices are column-major. Build each node's local matrix from its matrix or TRS,
    // then traverse the selected scene so instanced meshes and authored transforms survive.
    using Matrix = std::array<float, 16>;
    auto identity = [] {
        Matrix result{};
        result[0] = result[5] = result[10] = result[15] = 1.0f;
        return result;
    };
    auto multiply = [](const Matrix &a, const Matrix &b) {
        Matrix result{};
        for (int c = 0; c < 4; ++c) {
            for (int r = 0; r < 4; ++r) {
                for (int k = 0; k < 4; ++k) result[c * 4 + r] += a[k * 4 + r] * b[c * 4 + k];
            }
        }
        return result;
    };
    auto number_array = [](const nlohmann::json &value, size_t size, float *out) {
        if (!value.is_array() || value.size() != size) return false;
        for (size_t i = 0; i < size; ++i) {
            if (!value[i].is_number()) return false;
            out[i] = value[i].get<float>();
            if (!std::isfinite(out[i])) return false;
        }
        return true;
    };
    auto node_local = [&](const nlohmann::json &node) {
        Matrix result = identity();
        if (node.contains("matrix")) {
            float values[16];
            if (number_array(node["matrix"], 16, values)) std::copy(values, values + 16, result.begin());
            return result;
        }
        float t[3] = {0, 0, 0}, s[3] = {1, 1, 1}, q[4] = {0, 0, 0, 1};
        float values[4];
        if (node.contains("translation") && number_array(node["translation"], 3, values)) std::copy(values, values + 3, t);
        if (node.contains("scale") && number_array(node["scale"], 3, values)) std::copy(values, values + 3, s);
        if (node.contains("rotation") && number_array(node["rotation"], 4, values)) std::copy(values, values + 4, q);
        const float qlen = std::sqrt(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
        if (qlen > 1e-8f) for (float &v : q) v /= qlen;
        const float x=q[0], y=q[1], z=q[2], w=q[3];
        result[0]=(1-2*(y*y+z*z))*s[0]; result[1]=(2*(x*y+z*w))*s[0]; result[2]=(2*(x*z-y*w))*s[0];
        result[4]=(2*(x*y-z*w))*s[1]; result[5]=(1-2*(x*x+z*z))*s[1]; result[6]=(2*(y*z+x*w))*s[1];
        result[8]=(2*(x*z+y*w))*s[2]; result[9]=(2*(y*z-x*w))*s[2]; result[10]=(1-2*(x*x+y*y))*s[2];
        result[12]=t[0]; result[13]=t[1]; result[14]=t[2];
        return result;
    };
    auto transform_vertex = [](gfx::Vertex3D &v, const Matrix &matrix) {
        const float x=v.position[0], y=v.position[1], z=v.position[2];
        v.position[0]=matrix[0]*x+matrix[4]*y+matrix[8]*z+matrix[12];
        v.position[1]=matrix[1]*x+matrix[5]*y+matrix[9]*z+matrix[13];
        v.position[2]=matrix[2]*x+matrix[6]*y+matrix[10]*z+matrix[14];
        const float a00=matrix[0], a01=matrix[4], a02=matrix[8];
        const float a10=matrix[1], a11=matrix[5], a12=matrix[9];
        const float a20=matrix[2], a21=matrix[6], a22=matrix[10];
        const float c00=a11*a22-a12*a21, c01=a12*a20-a10*a22, c02=a10*a21-a11*a20;
        const float c10=a02*a21-a01*a22, c11=a00*a22-a02*a20, c12=a01*a20-a00*a21;
        const float c20=a01*a12-a02*a11, c21=a02*a10-a00*a12, c22=a00*a11-a01*a10;
        const float determinant=a00*c00+a01*c01+a02*c02;
        const float inv_det=std::abs(determinant)>1e-12f ? 1.0f/determinant : 0.0f;
        const float nx=(c00*v.normal[0]+c01*v.normal[1]+c02*v.normal[2])*inv_det;
        const float ny=(c10*v.normal[0]+c11*v.normal[1]+c12*v.normal[2])*inv_det;
        const float nz=(c20*v.normal[0]+c21*v.normal[1]+c22*v.normal[2])*inv_det;
        v.normal[0]=nx; v.normal[1]=ny; v.normal[2]=nz;
        Normalize(v.normal);
    };

    std::vector<std::pair<int, Matrix>> instances;
    bool has_active_scene = false;
    if (doc.contains("nodes") && doc["nodes"].is_array() && !doc["nodes"].empty()) {
        const auto &nodes = doc["nodes"];
        std::vector<bool> active(nodes.size(), false);
        std::function<void(int, const Matrix &, int)> visit = [&](int node_index, const Matrix &parent, int depth) {
            if (node_index < 0 || node_index >= static_cast<int>(nodes.size()) || depth > 256 || active[node_index]) return;
            const auto &node = nodes[node_index];
            if (!node.is_object()) return;
            active[node_index] = true;
            const Matrix world = multiply(parent, node_local(node));
            if (node.contains("mesh") && node["mesh"].is_number_integer()) {
                const int mesh_index = node["mesh"].get<int>();
                if (mesh_index >= 0 && mesh_index < static_cast<int>(doc["meshes"].size())) instances.emplace_back(mesh_index, world);
            }
            if (node.contains("children") && node["children"].is_array()) {
                for (const auto &child : node["children"]) if (child.is_number_integer()) visit(child.get<int>(), world, depth + 1);
            }
            active[node_index] = false;
        };
        int scene_index = doc.value("scene", 0);
        if (doc.contains("scenes") && doc["scenes"].is_array() && scene_index >= 0 && scene_index < static_cast<int>(doc["scenes"].size()) &&
            doc["scenes"][scene_index].contains("nodes") && doc["scenes"][scene_index]["nodes"].is_array()) {
            has_active_scene = true;
            for (const auto &root : doc["scenes"][scene_index]["nodes"]) if (root.is_number_integer()) visit(root.get<int>(), identity(), 0);
        }
        // A valid GLB may omit scenes. Preserve utility for simple, unparented node files.
        if (instances.empty() && (!doc.contains("scenes") || !doc["scenes"].is_array())) {
            std::vector<bool> child(nodes.size(), false);
            for (const auto &node : nodes) if (node.is_object() && node.contains("children") && node["children"].is_array())
                for (const auto &index : node["children"]) if (index.is_number_integer() && index.get<int>() >= 0 && index.get<int>() < static_cast<int>(nodes.size())) child[index.get<int>()] = true;
            for (int i = 0; i < static_cast<int>(nodes.size()); ++i) if (!child[i]) visit(i, identity(), 0);
        }
    }
    if (instances.empty() && !has_active_scene) {
        // Also accept meshes-only GLBs, where no node graph is provided.
        for (int i = 0; i < static_cast<int>(doc["meshes"].size()); ++i) instances.emplace_back(i, identity());
    }

    bool needs_normals = false;
    for (const auto &[mesh_index, node_matrix] : instances) {
      const auto &mesh = doc["meshes"][mesh_index];
      if (!mesh.is_object() || !mesh.contains("primitives") || !mesh["primitives"].is_array()) continue;
      for (const auto &prim : mesh["primitives"]) {
        const int primitive_mode = prim.value("mode", 4);
        if ((primitive_mode != 4 && primitive_mode != 5 && primitive_mode != 6) ||
            !prim.contains("attributes") || !prim["attributes"].contains("POSITION")) {
            continue;
        }
        std::vector<float> pos, nor, uv;
        if (!ReadAccessor(doc, bin, prim["attributes"]["POSITION"].get<int>(), 3, pos)) {
            continue;
        }
        bool has_n = prim["attributes"].contains("NORMAL") &&
                     ReadAccessor(doc, bin, prim["attributes"]["NORMAL"].get<int>(), 3, nor);
        bool has_uv = prim["attributes"].contains("TEXCOORD_0") &&
                      ReadAccessor(doc, bin, prim["attributes"]["TEXCOORD_0"].get<int>(), 2, uv);
        const size_t vertex_count = pos.size() / 3;
        has_n = has_n && nor.size() == pos.size();
        needs_normals = needs_normals || !has_n;
        has_uv = has_uv && uv.size() == vertex_count * 2;
        constexpr size_t kMaxReplacementVertices = 2u * 1024u * 1024u;
        constexpr size_t kMaxReplacementIndices = 6u * 1024u * 1024u;
        if (vertex_count == 0 || vertex_count > kMaxReplacementVertices - std::min(m.vertices.size(), kMaxReplacementVertices)) {
            std::fprintf(stderr, "mods: %s exceeds the replacement vertex limit\n", Utf8(file).c_str());
            return Model{};
        }
        uint32_t base = static_cast<uint32_t>(m.vertices.size());
        for (size_t i = 0; i < pos.size() / 3; i++) {
            gfx::Vertex3D v = {};
            std::memcpy(v.position, &pos[i * 3], sizeof(v.position));
            if (has_n) {
                std::memcpy(v.normal, &nor[i * 3], sizeof(v.normal));
            }
            if (has_uv) {
                std::memcpy(v.uv, &uv[i * 2], sizeof(v.uv));
            }
            std::memset(v.color, 0x80, sizeof(v.color));
            transform_vertex(v, node_matrix);
            m.vertices.push_back(v);
        }
        std::vector<uint32_t> idx;
        if (prim.contains("indices")) {
            if (!ReadIndexAccessor(doc, bin, prim["indices"].get<int>(), idx)) {
                m.vertices.resize(base);
                continue;
            }
        } else {
            for (uint32_t i = 0; i < pos.size() / 3; i++) {
                idx.push_back(i);
            }
        }
        if (idx.size() < 3 ||
            std::any_of(idx.begin(), idx.end(), [vertex_count](uint32_t i) { return i >= vertex_count; })) {
            m.vertices.resize(base);
            continue;
        }
        const size_t remaining_indices = kMaxReplacementIndices - std::min(m.indices.size(), kMaxReplacementIndices);
        const size_t triangle_count = primitive_mode == 4 ? idx.size() / 3 : idx.size() - 2;
        if ((primitive_mode == 4 && idx.size() % 3 != 0) || triangle_count > remaining_indices / 3) {
            m.vertices.resize(base);
            continue;
        }
        if (primitive_mode == 5) {
            std::vector<uint32_t> triangles;
            triangles.reserve((idx.size() - 2) * 3);
            for (size_t i = 2; i < idx.size(); ++i) {
                uint32_t a = idx[i - 2], b = idx[i - 1], c = idx[i];
                if (i & 1u) std::swap(a, b);
                if (a != b && b != c && a != c) {
                    triangles.insert(triangles.end(), {a, b, c});
                }
            }
            idx = std::move(triangles);
        } else if (primitive_mode == 6) {
            std::vector<uint32_t> triangles;
            triangles.reserve((idx.size() - 2) * 3);
            for (size_t i = 2; i < idx.size(); ++i) {
                const uint32_t a = idx[0], b = idx[i - 1], c = idx[i];
                if (a != b && b != c && a != c) {
                    triangles.insert(triangles.end(), {a, b, c});
                }
            }
            idx = std::move(triangles);
        }
        if (idx.empty() || idx.size() % 3 != 0 ||
            idx.size() > kMaxReplacementIndices - std::min(m.indices.size(), kMaxReplacementIndices)) {
            m.vertices.resize(base);
            continue;
        }
        for (uint32_t i : idx) {
            m.indices.push_back(base + i);
        }
      }
    }
    if (m.vertices.empty() || m.indices.empty()) {
        return m;
    }
    if (needs_normals) {
        ComputeNormals(m);
    }
    m.ok = true;
    return m;
}

const Model &Load(const ModelFile &file) {
    std::string key = Utf8(file.path);
    auto        it = g.cache.find(key);
    if (it != g.cache.end()) {
        return it->second;
    }
    Model m;
    try {
        constexpr std::uintmax_t kMaxModelFileBytes = 64u * 1024u * 1024u;
        std::error_code error;
        const std::uintmax_t file_size = fs::file_size(file.path, error);
        if (error || file_size == 0 || file_size > kMaxModelFileBytes) {
            std::fprintf(stderr, "mods: model file is missing, empty, or exceeds 64 MiB: %s\n", key.c_str());
        } else {
            m = Fold(Utf8(file.path.extension())) == ".glb" ? LoadGlb(file.path) : LoadObj(file.path);
            if (m.ok) ApplyTransform(m, ReadTransform(file.path));
        }
    } catch (const std::exception &error) {
        std::fprintf(stderr, "mods: invalid model %s: %s\n", key.c_str(), error.what());
        m = {};
    } catch (...) {
        std::fprintf(stderr, "mods: invalid model %s\n", key.c_str());
        m = {};
    }
    if (!m.ok) {
        std::fprintf(stderr, "mods: replacement skipped; native model retained: %s\n", key.c_str());
    }
    return g.cache.emplace(key, std::move(m)).first->second;
}

void Dump(const Draw3DVisual &visual, const std::string &name) {
    if (visual.indices.empty() || visual.vertices.empty()) {
        return;
    }
    std::string stem = (name.empty() ? std::string("untextured") : name) + "__" + std::to_string(visual.vertices.size()) + "v";
    if (!g.dumped.insert(stem).second) {
        return;
    }
    fs::path    dir = g.root / "_dump" / "models";
    std::error_code error;
    fs::create_directories(dir, error);
    std::ofstream out(dir / (stem + ".obj"));
    out << "# dumped by Dark Cloud (Windows fork): texture " << name << ", " << visual.vertices.size() << " vertices\n";
    char buffer[160];
    for (const gfx::Vertex3D &v : visual.vertices) {
        std::snprintf(buffer, sizeof(buffer), "v %.6f %.6f %.6f\n", v.position[0], v.position[1], v.position[2]);
        out << buffer;
    }
    for (const gfx::Vertex3D &v : visual.vertices) {
        std::snprintf(buffer, sizeof(buffer), "vt %.6f %.6f\n", v.uv[0], 1.0f - v.uv[1]);
        out << buffer;
    }
    for (const gfx::Vertex3D &v : visual.vertices) {
        std::snprintf(buffer, sizeof(buffer), "vn %.6f %.6f %.6f\n", v.normal[0], v.normal[1], v.normal[2]);
        out << buffer;
    }
    // One group per original strip (`usemtl strip<N>`): a replacement keeps strip N's material (texture, colours) for the faces
    // under that group, so a character with several materials (body, boots, hair) stays correctly painted. Keep the lines.
    size_t strips = std::max<size_t>(visual.strips.size(), 1);
    for (size_t k = 0; k < strips; k++) {
        size_t first = visual.strips.empty() ? 0 : visual.strips[k].first_index;
        size_t count = visual.strips.empty() ? visual.indices.size() : visual.strips[k].index_count;
        if (count == 0 || first >= visual.indices.size()) {
            continue;
        }
        out << "usemtl strip" << k << "\n";
        for (size_t i = first; i + 2 < first + count && i + 2 < visual.indices.size(); i += 3) {
            uint32_t a = visual.indices[i] + 1, b = visual.indices[i + 1] + 1, c = visual.indices[i + 2] + 1;
            std::snprintf(buffer, sizeof(buffer), "f %u/%u/%u %u/%u/%u %u/%u/%u\n", a, a, a, b, b, b, c, c, c);
            out << buffer;
        }
    }
}

} // namespace

namespace {

constexpr int kSkinNeighbours = 4;

// For every replacement vertex, the kSkinNeighbours nearest vertices of the original model (by bind pose) and weights that
// sum to 1 (inverse distance). The original is skinned by the game each tick; the replacement just follows its neighbours.
void BuildSkin(Draw3DVisual &visual, const float (*original)[4], int count) {
    visual.skin_bind.clear();
    visual.skin_base.clear();
    visual.skin_src.clear();
    visual.skin_weight.clear();
    if (original == nullptr || count <= 0) {
        return;
    }
    count = std::min(count, 3000); // MotionProc2's native deformation buffer has 3000 entries.
    visual.skin_bind.resize(static_cast<size_t>(count) * 3);
    for (int i = 0; i < count; i++) {
        for (int k = 0; k < 3; k++) {
            visual.skin_bind[static_cast<size_t>(i) * 3 + k] = original[i][k];
        }
    }
    const size_t n = visual.vertices.size();
    visual.skin_base.resize(n * 3);
    visual.skin_src.assign(n * kSkinNeighbours, 0);
    visual.skin_weight.assign(n * kSkinNeighbours, 0.0f);
    for (size_t j = 0; j < n; j++) {
        const float *p = visual.vertices[j].position;
        std::copy(p, p + 3, &visual.skin_base[j * 3]);
        float best_d[kSkinNeighbours];
        int   best_i[kSkinNeighbours];
        for (int k = 0; k < kSkinNeighbours; k++) {
            best_d[k] = 1e30f;
            best_i[k] = 0;
        }
        for (int i = 0; i < count; i++) {
            float dx = p[0] - original[i][0], dy = p[1] - original[i][1], dz = p[2] - original[i][2];
            float d = dx * dx + dy * dy + dz * dz;
            if (d >= best_d[kSkinNeighbours - 1]) {
                continue;
            }
            int at = kSkinNeighbours - 1;
            while (at > 0 && best_d[at - 1] > d) {
                best_d[at] = best_d[at - 1];
                best_i[at] = best_i[at - 1];
                at--;
            }
            best_d[at] = d;
            best_i[at] = i;
        }
        float sum = 0.0f;
        for (int k = 0; k < kSkinNeighbours; k++) {
            float d = std::sqrt(best_d[k]) + 0.01f; // cubed: a vertex that sits on an original one follows it exactly
            float w = 1.0f / (d * d * d);
            visual.skin_src[j * kSkinNeighbours + k] = best_i[k];
            visual.skin_weight[j * kSkinNeighbours + k] = w;
            sum += w;
        }
        for (int k = 0; k < kSkinNeighbours; k++) {
            visual.skin_weight[j * kSkinNeighbours + k] /= sum;
        }
    }
    visual.skin_by_orig.assign(static_cast<size_t>(count), {});
    for (size_t j = 0; j < n; j++) {
        for (int k = 0; k < kSkinNeighbours; k++) {
            const int source_vertex = visual.skin_src[j * kSkinNeighbours + k];
            visual.skin_by_orig[static_cast<size_t>(source_vertex)].push_back({static_cast<int>(j), k});
        }
    }
}

} // namespace

namespace {

void Apply4(float out[4], const float m[4][4], const float v[4]) {
    float r[4];
    for (int i = 0; i < 4; i++) {
        r[i] = m[0][i] * v[0];
        r[i] = r[i] + m[1][i] * v[1];
        r[i] = r[i] + m[2][i] * v[2];
        r[i] = r[i] + m[3][i] * v[3];
    }
    for (int i = 0; i < 4; i++) {
        out[i] = r[i];
    }
}

// the game's SkinVertex (port/src/gameutil_motion.cpp) on one replacement vertex
void SkinPoint(float *out, float *deformed, const float *source, const float base[4][4], const float bone[4][4], const float inverse[4][4],
               float weight) {
    float local[4];
    for (int i = 0; i < 3; i++) {
        local[i] = source[i] - base[3][i];
    }
    local[3] = 1.0f;
    float accumulated[4];
    Apply4(accumulated, base, local);
    for (int i = 0; i < 3; i++) {
        local[i] = accumulated[i] - base[3][i];
    }
    Apply4(accumulated, bone, local);
    float moved[3];
    for (int i = 0; i < 3; i++) {
        moved[i] = accumulated[i] - source[i];
    }
    for (int i = 0; i < 3; i++) {
        deformed[i] = deformed[i] + moved[i] * weight;
    }
    float result[4];
    Apply4(result, inverse, deformed);
    for (int i = 0; i < 4; i++) {
        out[i] = result[i];
    }
}

} // namespace

void ModsSkinBegin(const float (*mdt)[4]) {
    static int debug_calls = 0;
    if (std::getenv("DC_DEBUG_SKIN") != nullptr && debug_calls < 6 && !g.skin_blocks.empty()) {
        debug_calls++;
        std::fprintf(stderr, "skin: MotionProc2 begins a pose for %p; %zu replaced model(s) registered, match %d\n", static_cast<const void *>(mdt),
                     g.skin_blocks.size(), g.skin_blocks.count(mdt) ? 1 : 0);
    }
    auto it = g.skin_blocks.find(mdt);
    if (it == g.skin_blocks.end()) {
        return;
    }
    for (const void *block : it->second) {
        Draw3DVisual *v = Draw3DFindVisual(block);
        if (v == nullptr || !v->replaced || v->skin_by_orig.empty()) {
            continue;
        }
        const size_t n = v->vertices.size();
        v->skin_def.assign(n * 4, 0.0f);
        v->skin_out.assign(n * 4, 0.0f);
        for (size_t j = 0; j < n; j++) {
            for (int k = 0; k < 3; k++) {
                v->skin_def[j * 4 + k] = v->skin_base[j * 3 + k];
                v->skin_out[j * 4 + k] = v->skin_base[j * 3 + k];
            }
            v->skin_def[j * 4 + 3] = 1.0f;
            v->skin_out[j * 4 + 3] = 1.0f;
        }
        v->skin_fresh = true;
    }
}

void ModsSkinVertex(const float (*mdt)[4], unsigned vertex, const float base[4][4], const float bone[4][4], const float inverse[4][4],
                    float weight) {
    auto it = g.skin_blocks.find(mdt);
    if (it == g.skin_blocks.end()) {
        return;
    }
    for (const void *block : it->second) {
        Draw3DVisual *v = Draw3DFindVisual(block);
        if (v == nullptr || !v->replaced || !v->skin_fresh || vertex >= v->skin_by_orig.size()) {
            continue;
        }
        for (const auto &[j, neighbour] : v->skin_by_orig[vertex]) {
            SkinPoint(&v->skin_out[static_cast<size_t>(j) * 4], &v->skin_def[static_cast<size_t>(j) * 4],
                      &v->skin_base[static_cast<size_t>(j) * 3], base, bone, inverse,
                      weight * v->skin_weight[static_cast<size_t>(j) * kSkinNeighbours + neighbour]);
        }
    }
}

void ModsModelSkin(Draw3DVisual &visual, const float (*vertex)[4], const void *block) {
    if (visual.skin_src.empty() || vertex == nullptr) {
        return;
    }
    if (block != nullptr) { // the live vertex array the game poses (not the one the visual was built from): MotionProc2 finds us by it
        BindSkinBlock(visual, vertex, block);
    }
    if (visual.skin_fresh) { // exact: posed by the game's own bone matrices in MotionProc2
        visual.skin_fresh = false;
        if (std::getenv("DC_DEBUG_SKIN") != nullptr) {
            static int shown = 0;
            size_t     posed = 0, covered = 0;
            for (size_t j = 0; j < visual.vertices.size(); j++) {
                if (visual.skin_out[j * 4] != visual.skin_base[j * 3] || visual.skin_out[j * 4 + 1] != visual.skin_base[j * 3 + 1] ||
                    visual.skin_out[j * 4 + 2] != visual.skin_base[j * 3 + 2]) {
                    posed++;
                }
                covered += 0;
            }
            if (shown++ < 4) {
                std::fprintf(stderr, "skin: exact pose: %zu of %zu replacement vertices moved from bind\n", posed, visual.vertices.size());
            }
        }
        bool changed = false;
        for (size_t j = 0; j < visual.vertices.size(); j++) {
            float *out = visual.vertices[j].position;
            for (int a = 0; a < 3; a++) {
                if (out[a] != visual.skin_out[j * 4 + a]) {
                    out[a] = visual.skin_out[j * 4 + a];
                    changed = true;
                }
            }
        }
        if (changed && visual.mesh != gfx::kNullMesh) {
            gfx::UpdateMeshVertices(visual.mesh, 0, visual.vertices);
        }
        return;
    }
    if (std::getenv("DC_DEBUG_SKIN") != nullptr && g.skin_seen.insert(&visual).second) {
        std::fprintf(stderr, "skin: %s re-skinned by the game (%zu vertices)\n",
                     g.skin_name.count(&visual) ? g.skin_name[&visual].c_str() : "?", visual.vertices.size());
    }
    const size_t n = visual.vertices.size();
    bool         changed = false;
    for (size_t j = 0; j < n; j++) {
        float moved[3] = {0, 0, 0};
        for (int k = 0; k < kSkinNeighbours; k++) {
            int          s = visual.skin_src[j * kSkinNeighbours + k];
            float        w = visual.skin_weight[j * kSkinNeighbours + k];
            const float *bind = &visual.skin_bind[static_cast<size_t>(s) * 3];
            for (int a = 0; a < 3; a++) {
                moved[a] += w * (vertex[s][a] - bind[a]);
            }
        }
        float *out = visual.vertices[j].position;
        for (int a = 0; a < 3; a++) {
            float v = visual.skin_base[j * 3 + a] + moved[a];
            if (out[a] != v) {
                out[a] = v;
                changed = true;
            }
        }
    }
    if (changed && visual.mesh != gfx::kNullMesh) {
        gfx::UpdateMeshVertices(visual.mesh, 0, visual.vertices);
    }
}

void ModsModelHook(Draw3DVisual &visual, const char *name_c, const float (*original)[4], int original_count, const void *block) {
    if (!g.inited) {
        Init();
    }
    std::string name = Fold(StripExtension(name_c != nullptr ? name_c : ""));
    if (g.dump) {
        Dump(visual, name);
    }
    auto it = g.by_name.find(name);
    if (it == g.by_name.end() || visual.indices.empty()) {
        return;
    }
    const int verts = static_cast<int>(visual.vertices.size());
    const ModelFile *chosen = nullptr;
    for (auto f = it->second.rbegin(); f != it->second.rend(); ++f) { // last mod wins; an exact vertex count beats "any"
        if (f->verts == verts) {
            chosen = &*f;
            break;
        }
        if (f->verts < 0 && chosen == nullptr) {
            chosen = &*f;
        }
    }
    if (chosen == nullptr) {
        return;
    }
    const Model &model = Load(*chosen);
    if (!model.ok) {
        return;
    }
    const std::vector<Draw3DStrip> old_strips = visual.strips;
    auto material_of = [&](int k) { // the original strip's material, else its first
        if (old_strips.empty()) {
            return Draw3DStrip{};
        }
        return old_strips[k >= 0 && k < static_cast<int>(old_strips.size()) ? k : 0];
    };
    visual.vertices = model.vertices;
    visual.indices = model.indices;
    visual.strips.clear();
    if (model.groups.empty()) {
        Draw3DStrip strip = material_of(0);
        strip.first_index = 0;
        strip.index_count = static_cast<uint32_t>(model.indices.size());
        visual.strips.push_back(strip);
    } else {
        for (const Model::Group &gr : model.groups) {
            Draw3DStrip strip = material_of(gr.strip);
            strip.first_index = gr.first_index;
            strip.index_count = gr.index_count;
            visual.strips.push_back(strip);
        }
    }
    visual.replaced = true;
    BuildSkin(visual, original, original_count);
    g.skin_name[&visual] = name;
    BindSkinBlock(visual, original, block);
    if (g.replaced++ < 50) {
        std::fprintf(stderr, "mods: replaced model %s (%d -> %zu vertices)\n", name.c_str(), verts, model.vertices.size());
    }
}

void ModsModelVisualReleased(Draw3DVisual &visual) {
    g.skin_name.erase(&visual);
    g.skin_seen.erase(&visual);
    const auto bindings = g.skin_bindings.find(&visual);
    if (bindings != g.skin_bindings.end()) {
        for (const void *block : bindings->second) {
            for (auto entry = g.skin_blocks.begin(); entry != g.skin_blocks.end();) {
                std::erase(entry->second, block);
                if (entry->second.empty()) entry = g.skin_blocks.erase(entry);
                else ++entry;
            }
        }
        g.skin_bindings.erase(bindings);
    }
}

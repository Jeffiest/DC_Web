#include "cloth.hpp"

#include <libvu0.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <memory>
#include <numbers>
#include <span>
#include <unordered_map>
#include <vector>

#include "bound.hpp"
#include "chararead.hpp"
#include "frame.hpp"
#include "framevu1.hpp"
#include "mdt.hpp"
#include "mglib.hpp"
#include "platform/config.hpp"
#include "render/draw3d.hpp"
#include "renderinfo.hpp"
#include "texture.hpp"
#include "visualvu1.hpp"
#include "vu0_ops.hpp"
#include "wind.hpp"

namespace dc::game {

using namespace dc::platform;
using namespace dc::render;

void CCloth::Draw() {
    CFrameVu1 draw_frame;

    draw_frame.SetVisual(this);
    draw_frame.max[0] = 10000.0f;
    draw_frame.max[1] = 10000.0f;
    draw_frame.max[2] = 10000.0f;
    draw_frame.min[0] = -10000.0f;
    draw_frame.min[1] = -10000.0f;
    draw_frame.min[2] = -10000.0f;

    for (int corner_no = 0; corner_no < 8; corner_no++) {
        const float *bounds[2] = {draw_frame.min, draw_frame.max};

        draw_frame.corner[corner_no][3] = 1.0f;
        draw_frame.corner[corner_no][0] = bounds[(corner_no & 1) != 0][0];
        draw_frame.corner[corner_no][1] = bounds[(corner_no & 2) != 0][1];
        draw_frame.corner[corner_no][2] = bounds[(corner_no & 4) != 0][2];
    }

    CFrameAttr attr;

    attr.fog_enable = false;
    attr.program_option = 0;
    attr.clip_enable = false;
    float origin = 0.0f;

    draw_frame.SetAttr(attr, 1, 0);
    draw_frame.SetPosition(origin, origin, origin);
    MGDraw(&draw_frame);
}

void CCloth::Clear() {
    sceVu0FMATRIX matrix;

    if (frame != nullptr) {
        frame->GetLWMatrix(matrix);
    }

    sceVu0ApplyMatrix(last_position, matrix, position);

    for (int j = 0; j < num_j; j++) {
        for (int i = 0; i < num_i; i++) {
            float *velocity = speed[i][j];
            velocity[0] = 0.0f;
            velocity[1] = 0.0f;
            velocity[2] = 0.0f;

            if (frame != nullptr) {
                sceVu0ApplyMatrix(point[i][j], matrix, home[i][j]);
                sceVu0CopyVector(last[i][j], point[i][j]);
            }
        }
    }
}

namespace {

// Cut interpolation for attachment or local deformation jumps beyond the simulation's reset distance.
constexpr float kClothResetDistance = 10.0f;

void ClothDrawMatrix(CFrame *frame, sceVu0FMATRIX matrix) {
    sceVu0UnitMatrix(matrix);
    if (frame == nullptr) {
        return;
    }
    sceVu0FMATRIX world;
    frame->GetLWMatrix(world);
    sceVu0CopyVector(matrix[3], world[3]);

    // Use a rigid basis so scale and shear stay in the vertices and the inverse remains exact.
    sceVu0FVECTOR x;
    sceVu0FVECTOR y;
    sceVu0FVECTOR z;
    sceVu0Normalize(x, world[0]);
    sceVu0OuterProduct(z, x, world[1]);
    if (sceVu0InnerProduct(x, x) == 0.0f || sceVu0InnerProduct(z, z) == 0.0f) {
        return;
    }
    sceVu0Normalize(z, z);
    sceVu0OuterProduct(y, z, x);
    sceVu0CopyVector(matrix[0], x);
    sceVu0CopyVector(matrix[1], y);
    sceVu0CopyVector(matrix[2], z);
    matrix[0][3] = matrix[1][3] = matrix[2][3] = 0.0f;
}

void Follow(float *vertex, float *velocity, float *world_home, const float *home, const float *delta,
            const float matrix[4][4], const float *stiffness) {
    float carried[4];
    vu0::Apply(carried, matrix, home);
    vertex[3] = 1.0f;
    velocity[3] = 1.0f;
    vu0::Copy(world_home, carried);

    float force[4];
    for (int i = 0; i < 4; i++) {
        force[i] = delta[i] + (carried[i] - vertex[i]) * stiffness[i];
    }

    for (int i = 0; i < 3; i++) {
        vertex[i] = (vertex[i] + velocity[i]) + force[i];
        velocity[i] = 0.0f - force[i];
    }
}

bool StartsWith(const char *name, const char *prefix) {
    return std::strncmp(name, prefix, std::strlen(prefix)) == 0;
}

using cloth_body::Capsule;

std::array<float, 3> WorldPosition(CFrame *frame) {
    sceVu0FMATRIX matrix;
    frame->GetLWMatrix(matrix);
    return {matrix[3][0], matrix[3][1], matrix[3][2]};
}

// The body as capsules. A limb is a chain with two joints ("jntN_1" to "jntN_2" to "effN", an arm or a
// leg): a capsule along each joint, a fifth of its length thick. The torso is a chain with one joint whose
// end the other chains branch from (the pelvis, the spine up to the shoulders): a capsule along it, 0.4 of
// its length thick until FitTorsos shapes it to the mesh (Toan's chest lies 0.65 to 1.27 from his 3.1-long
// spine). The other chains with one joint (the head, the hands, the feet) go in `others`, when asked for.
void BodyCapsules(CFrame *frame, std::vector<Capsule> &capsules, std::vector<Capsule> *others = nullptr) {
    CFrame *root = frame;
    while (root->parent != nullptr && root->reference == 0) {
        root = root->parent;
    }
    auto joint_child = [](CFrame *joint) -> CFrame * {
        for (CFrame *child = joint->child; child != nullptr; child = child->brother) {
            if (StartsWith(child->name, "jnt") || StartsWith(child->name, "eff")) {
                return child;
            }
        }
        return nullptr;
    };
    std::vector<CFrame *> pending = {root};
    while (!pending.empty()) {
        CFrame *frame_at = pending.back();
        pending.pop_back();
        for (CFrame *child = frame_at->child; child != nullptr; child = child->brother) {
            pending.push_back(child);
        }
        CFrame *middle = StartsWith(frame_at->name, "jnt") ? joint_child(frame_at) : nullptr;
        if (middle == nullptr || StartsWith(frame_at->parent->name, "jnt")) {
            continue;
        }
        if (StartsWith(middle->name, "eff")) {
            int branches = 0;
            for (CFrame *child = middle->child; child != nullptr; child = child->brother) {
                branches += StartsWith(child->name, "chn") ? 1 : 0;
            }
            if (branches >= 2) {
                const std::array<float, 3> a = WorldPosition(frame_at);
                const std::array<float, 3> b = WorldPosition(middle);
                const float                length = std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
                Capsule                    torso = {a, b, length * 0.4f, true};
                // The joint's axis least along the spine, square to it: the fitted shape turns with the body.
                sceVu0FMATRIX matrix;
                frame_at->GetLWMatrix(matrix);
                const float axis[3] = {(b[0] - a[0]) / length, (b[1] - a[1]) / length, (b[2] - a[2]) / length};
                int         across = 0;
                float       along = 2.0f;
                for (int row = 0; row < 3; row++) {
                    const float dot = std::fabs(matrix[row][0] * axis[0] + matrix[row][1] * axis[1] + matrix[row][2] * axis[2]) /
                                      std::hypot(matrix[row][0], matrix[row][1], matrix[row][2]);
                    if (dot < along) {
                        along = dot;
                        across = row;
                    }
                }
                const float dot = matrix[across][0] * axis[0] + matrix[across][1] * axis[1] + matrix[across][2] * axis[2];
                float       side[3];
                for (int k = 0; k < 3; k++) {
                    side[k] = matrix[across][k] - dot * axis[k];
                }
                const float side_length = std::hypot(side[0], side[1], side[2]);
                torso.side = {side[0] / side_length, side[1] / side_length, side[2] / side_length};
                capsules.push_back(torso);
            } else if (others != nullptr) {
                const std::array<float, 3> a = WorldPosition(frame_at);
                const std::array<float, 3> b = WorldPosition(middle);
                const float                length = std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
                others->push_back({a, b, std::clamp(length * 0.2f, 0.4f, 1.4f)});
            }
            continue;
        }
        // A limb starts at a joint whose next is a joint too.
        for (CFrame *joint = frame_at, *next = middle; next != nullptr; joint = next, next = joint_child(next)) {
            const std::array<float, 3> a = WorldPosition(joint);
            const std::array<float, 3> b = WorldPosition(next);
            const float                length = std::hypot(b[0] - a[0], b[1] - a[1], b[2] - a[2]);
            capsules.push_back({a, b, std::clamp(length * 0.2f, 0.4f, 1.4f)});
            if (!StartsWith(next->name, "jnt")) {
                break;
            }
        }
    }
}

std::array<float, 3> NearestOnCapsule(const Capsule &capsule, const float *p) {
    const float ab[3] = {capsule.b[0] - capsule.a[0], capsule.b[1] - capsule.a[1], capsule.b[2] - capsule.a[2]};
    const float length2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    float       t = 0.0f;
    if (length2 > 1e-12f) {
        t = std::clamp(((p[0] - capsule.a[0]) * ab[0] + (p[1] - capsule.a[1]) * ab[1] + (p[2] - capsule.a[2]) * ab[2]) / length2,
                       0.0f, 1.0f);
    }
    return {capsule.a[0] + ab[0] * t, capsule.a[1] + ab[1] * t, capsule.a[2] + ab[2] * t};
}

// The torso capsule whose surface lies nearest `p`, the point of its line nearest `p`, the distance to it
// and how far the surface lies from the line that way, or null when the skeleton has none.
struct TorsoNearest {
    const Capsule       *capsule = nullptr;
    std::array<float, 3> at;
    float                distance = 1e9f;
    float                reach = 0.0f;
};

TorsoNearest NearestTorso(std::span<const Capsule> capsules, const float *p) {
    TorsoNearest nearest;
    for (const Capsule &capsule : capsules) {
        if (capsule.torso) {
            const std::array<float, 3> at = NearestOnCapsule(capsule, p);
            const float                distance = std::hypot(p[0] - at[0], p[1] - at[1], p[2] - at[2]);
            const float                reach = cloth_body::Reach(capsule, p);
            if (nearest.capsule == nullptr || distance - reach < nearest.distance - nearest.reach) {
                nearest = {&capsule, at, distance, reach};
            }
        }
    }
    return nearest;
}

// How far along `ray` (a unit direction) from `origin` it crosses `face`, or -1 where it misses.
float RayHit(const float *origin, const float *ray, const std::array<std::array<float, 3>, 3> &face) {
    float edge1[3];
    float edge2[3];
    float to[3];
    for (int k = 0; k < 3; k++) {
        edge1[k] = face[1][k] - face[0][k];
        edge2[k] = face[2][k] - face[0][k];
        to[k] = origin[k] - face[0][k];
    }
    const float cross[3] = {ray[1] * edge2[2] - ray[2] * edge2[1], ray[2] * edge2[0] - ray[0] * edge2[2],
                            ray[0] * edge2[1] - ray[1] * edge2[0]};
    const float det = edge1[0] * cross[0] + edge1[1] * cross[1] + edge1[2] * cross[2];
    if (std::fabs(det) < 1e-9f) {
        return -1.0f;
    }
    const float u = (to[0] * cross[0] + to[1] * cross[1] + to[2] * cross[2]) / det;
    if (u < 0.0f || u > 1.0f) {
        return -1.0f;
    }
    const float q[3] = {to[1] * edge1[2] - to[2] * edge1[1], to[2] * edge1[0] - to[0] * edge1[2], to[0] * edge1[1] - to[1] * edge1[0]};
    const float v = (ray[0] * q[0] + ray[1] * q[1] + ray[2] * q[2]) / det;
    if (v < 0.0f || u + v > 1.0f) {
        return -1.0f;
    }
    const float t = (edge2[0] * q[0] + edge2[1] * q[1] + edge2[2] * q[2]) / det;
    return t >= 0.0f ? t : -1.0f;
}

// Where `p` lies round the capsule's axis, from 0 to 1 starting at its side, and along it from a to b.
std::array<float, 2> AroundAndAlong(const Capsule &capsule, const float *p) {
    const float ab[3] = {capsule.b[0] - capsule.a[0], capsule.b[1] - capsule.a[1], capsule.b[2] - capsule.a[2]};
    const float length2 = ab[0] * ab[0] + ab[1] * ab[1] + ab[2] * ab[2];
    const float ap[3] = {p[0] - capsule.a[0], p[1] - capsule.a[1], p[2] - capsule.a[2]};
    const float t = length2 > 1e-12f ? (ap[0] * ab[0] + ap[1] * ab[1] + ap[2] * ab[2]) / length2 : 0.0f;
    float       out[3];
    for (int k = 0; k < 3; k++) {
        out[k] = ap[k] - ab[k] * t;
    }
    const float length = std::sqrt(length2);
    const float axis[3] = {ab[0] / length, ab[1] / length, ab[2] / length};
    const float other[3] = {axis[1] * capsule.side[2] - axis[2] * capsule.side[1],
                            axis[2] * capsule.side[0] - axis[0] * capsule.side[2],
                            axis[0] * capsule.side[1] - axis[1] * capsule.side[0]};
    const float x = out[0] * capsule.side[0] + out[1] * capsule.side[1] + out[2] * capsule.side[2];
    const float y = out[0] * other[0] + out[1] * other[1] + out[2] * other[2];
    float       around = std::atan2(y, x) / (2.0f * std::numbers::pi_v<float>);
    if (around < 0.0f) {
        around += 1.0f;
    }
    return {around, t};
}

// The body's mesh in world space, as drawn: the skinned meshes, which keep their vertices for the motions
// to move. A cloth's frame isn't drawn (clothread.cpp's CommandFRAME), its mesh only the cloth's start.
void BodyMesh(CFrame *frame, cloth_body::Mesh &body) {
    CFrame *root = frame;
    while (root->parent != nullptr && root->reference == 0) {
        root = root->parent;
    }
    std::vector<CFrame *> pending = {root};
    while (!pending.empty()) {
        CFrame *frame_at = pending.back();
        pending.pop_back();
        const bool drawn = (frame_at->attr.draw_on & 1) != 0;
        if (drawn || (frame_at->attr.draw_on & 2) == 0) {
            for (CFrame *child = frame_at->child; child != nullptr; child = child->brother) {
                pending.push_back(child);
            }
        }
        // A character's frames are CFrameVu1 (dataset.cpp builds them as an array of them).
        CVisualVu1 *visual = static_cast<CFrameVu1 *>(frame_at)->GetVisual();
        if (!drawn || visual == nullptr) {
            continue;
        }
        const auto *model = reinterpret_cast<const MDT_HEADER *>(visual->GetMDTDataAddress());
        if (model == nullptr) {
            continue;
        }
        const auto   *local = reinterpret_cast<const sceVu0FVECTOR *>(reinterpret_cast<const u_char *>(model) + model->vertex_ofs);
        sceVu0FMATRIX matrix;
        frame_at->GetLWMatrix(matrix);
        const auto first = static_cast<std::uint32_t>(body.vertices.size());
        for (int k = 0; k < model->vertex_num; k++) {
            const float point[4] = {local[k][0], local[k][1], local[k][2], 1.0f};
            float       world[4];
            vu0::Apply(world, matrix, point);
            body.vertices.push_back({world[0], world[1], world[2]});
        }
        // The strips as visualvu1.cpp reads them: each a primitive, a count and a material, then for each
        // corner its vertex, normal and texture coordinate indices, and colour's when the model has them.
        const auto  *mesh = reinterpret_cast<const u_int *>(reinterpret_cast<const u_char *>(model) + model->mesh_ofs);
        const int    stride = model->colour_ofs > 0 ? 4 : 3;
        const u_int *index = mesh + 4;
        for (u_int strip = 0; strip < mesh[2]; strip++) {
            const u_int prim = index[0];
            const u_int count = index[1];
            index += 3;
            auto corner = [&](u_int k) { return index[k * stride]; };
            for (u_int k = 0; k + 2 < count; k += prim == 4 ? 1 : 3) {
                const u_int a = corner(k);
                const u_int b = corner(k + 1);
                const u_int c = corner(k + 2);
                if (std::max({a, b, c}) < static_cast<u_int>(model->vertex_num)) {
                    body.triangles.push_back({first + a, first + b, first + c});
                }
            }
            index += count * stride;
        }
    }
}

constexpr float kCapsuleMargin = 0.05f;

// What a cloth takes from the body when it settles on it: each vertex's clearance from each limb, and the
// torsos' shapes.
struct Settled {
    std::vector<float>   clearances;
    std::vector<Capsule> torsos;
};

std::unordered_map<const CCloth *, Settled> &SettledOn() {
    static std::unordered_map<const CCloth *, Settled> settled;
    return settled;
}

// The Options screen's Enhanced cloth: the PS2's step, then the free vertices pushed out of the body's
// capsules as the step pushes them out of the boxes, so the body no longer passes through the cloth.
// How near each limb a vertex may come, and the torso's shape, are taken when the cloth first steps on the
// body, or after a reset: the cloth is authored resting on the body as it stands.
template <class At>
void PushClothOutOfBody(CCloth &cloth, At at) {
    std::vector<Capsule> capsules;
    std::vector<Capsule> others;
    BodyCapsules(cloth.frame, capsules, &others);
    const std::size_t   count = capsules.size();
    Settled            &settled = SettledOn()[&cloth];
    std::vector<float> &clearances = settled.clearances;
    if (clearances.size() != static_cast<std::size_t>(cloth.num_i * cloth.num_j) * count) {
        settled.torsos.clear();
        if (std::ranges::any_of(capsules, &Capsule::torso)) {
            cloth_body::Mesh body;
            BodyMesh(cloth.frame, body);
            cloth_body::FitTorsos(capsules, others, body);
            std::ranges::copy_if(capsules, std::back_inserter(settled.torsos), &Capsule::torso);
        }
        clearances.resize(static_cast<std::size_t>(cloth.num_i * cloth.num_j) * count);
        for (int i = 0; i < cloth.num_i; i++) {
            for (int j = 0; j < cloth.num_j; j++) {
                for (std::size_t c = 0; c < count; c++) {
                    clearances[(i * cloth.num_j + j) * count + c] = cloth_body::Clearance(capsules[c], cloth.world_home[i][j]);
                }
            }
        }
    }
    auto torso = settled.torsos.begin();
    for (Capsule &capsule : capsules) {
        if (capsule.torso && torso != settled.torsos.end()) {
            capsule.shape = torso->shape;
            capsule.fitted = torso->fitted;
            ++torso;
        }
    }
    for (int i = 0; i < cloth.num_i; i++) {
        for (int j = 1; j < cloth.num_j; j++) {
            cloth_body::PushOutOfBody(capsules, std::span(clearances).subspan((i * cloth.num_j + j) * count, count),
                                      cloth.world_home[i][j], at(i, j));
        }
    }
}

} // namespace

float cloth_body::Reach(const Capsule &capsule, const float *p) {
    if (!capsule.fitted) {
        return capsule.radius;
    }
    const auto [around, along] = AroundAndAlong(capsule, p);
    const float sector = around * kTorsoSectors - 0.5f;
    const float band = std::clamp(along * kTorsoBands - 0.5f, 0.0f, static_cast<float>(kTorsoBands - 1));
    const int   sector0 = static_cast<int>(std::floor(sector));
    const int   band0 = std::min(static_cast<int>(band), kTorsoBands - 2);
    const float s = sector - static_cast<float>(sector0);
    const float h = std::min(band - static_cast<float>(band0), 1.0f);
    auto        at = [&](int sector_at, int band_at) {
        return capsule.shape[std::min(band_at, kTorsoBands - 1) * kTorsoSectors + (sector_at + kTorsoSectors) % kTorsoSectors];
    };
    const float low = at(sector0, band0) * (1.0f - s) + at(sector0 + 1, band0) * s;
    const float high = at(sector0, band0 + 1) * (1.0f - s) + at(sector0 + 1, band0 + 1) * s;
    return low * (1.0f - h) + high * h;
}

void cloth_body::FitTorsos(std::span<Capsule> capsules, std::span<const Capsule> others, const Mesh &body) {
    // Too few vertices, or faces in too few directions, to say what shape a torso is, and it keeps its
    // round one.
    constexpr int kFewest = 8;
    constexpr int kFewestCrossed = kTorsoSectors * kTorsoBands / 2;

    std::vector<std::size_t> owners(body.vertices.size(), capsules.size());
    std::vector<int>         counts(capsules.size());
    for (std::size_t v = 0; v < body.vertices.size(); v++) {
        const float *vertex = body.vertices[v].data();
        auto         outside = [&](const Capsule &capsule) {
            const std::array<float, 3> at = NearestOnCapsule(capsule, vertex);
            return std::hypot(vertex[0] - at[0], vertex[1] - at[1], vertex[2] - at[2]) - capsule.radius;
        };
        std::size_t owner = capsules.size();
        float       nearest = 1e9f;
        for (std::size_t c = 0; c < capsules.size(); c++) {
            if (outside(capsules[c]) < nearest) {
                nearest = outside(capsules[c]);
                owner = c;
            }
        }
        if (owner < capsules.size() && capsules[owner].torso &&
            std::ranges::none_of(others, [&](const Capsule &other) { return outside(other) < nearest; })) {
            owners[v] = owner;
            counts[owner]++;
        }
    }
    for (std::size_t c = 0; c < capsules.size(); c++) {
        Capsule &torso = capsules[c];
        if (!torso.torso || counts[c] < kFewest) {
            continue;
        }
        // The torso's faces: those with most of their corners on it, not an arm's or a hand's.
        std::vector<std::array<std::array<float, 3>, 3>> faces;
        for (const std::array<std::uint32_t, 3> &triangle : body.triangles) {
            const int on = static_cast<int>(std::ranges::count_if(triangle, [&](std::uint32_t v) { return owners[v] == c; }));
            if (on >= 2) {
                faces.push_back({body.vertices[triangle[0]], body.vertices[triangle[1]], body.vertices[triangle[2]]});
            }
        }
        const float length = std::hypot(torso.b[0] - torso.a[0], torso.b[1] - torso.a[1], torso.b[2] - torso.a[2]);
        const float axis[3] = {(torso.b[0] - torso.a[0]) / length, (torso.b[1] - torso.a[1]) / length,
                               (torso.b[2] - torso.a[2]) / length};
        const float other[3] = {axis[1] * torso.side[2] - axis[2] * torso.side[1], axis[2] * torso.side[0] - axis[0] * torso.side[2],
                                axis[0] * torso.side[1] - axis[1] * torso.side[0]};
        // Each direction and height looks out from the axis to the furthest face it crosses.
        std::array<float, kTorsoSectors * kTorsoBands> shape;
        shape.fill(-1.0f);
        for (int band = 0; band < kTorsoBands; band++) {
            const float along = (static_cast<float>(band) + 0.5f) / static_cast<float>(kTorsoBands);
            float       origin[3];
            for (int k = 0; k < 3; k++) {
                origin[k] = torso.a[k] + (torso.b[k] - torso.a[k]) * along;
            }
            for (int sector = 0; sector < kTorsoSectors; sector++) {
                const float angle = 2.0f * std::numbers::pi_v<float> * (static_cast<float>(sector) + 0.5f) / kTorsoSectors;
                float       ray[3];
                for (int k = 0; k < 3; k++) {
                    ray[k] = torso.side[k] * std::cos(angle) + other[k] * std::sin(angle);
                }
                float &cell = shape[band * kTorsoSectors + sector];
                for (const auto &face : faces) {
                    cell = std::max(cell, RayHit(origin, ray, face));
                }
            }
        }
        if (std::ranges::count_if(shape, [](float reach) { return reach >= 0.0f; }) < kFewestCrossed) {
            continue;
        }
        // A direction no face lies in takes the shape between its neighbours round the band; a band with
        // none takes the nearest band's.
        for (int band = 0; band < kTorsoBands; band++) {
            float                           *row = &shape[band * kTorsoSectors];
            std::array<float, kTorsoSectors> filled;
            std::copy(row, row + kTorsoSectors, filled.begin());
            for (int sector = 0; sector < kTorsoSectors; sector++) {
                if (row[sector] >= 0.0f) {
                    continue;
                }
                int before = 1;
                while (before < kTorsoSectors && row[(sector - before + kTorsoSectors) % kTorsoSectors] < 0.0f) {
                    before++;
                }
                if (before == kTorsoSectors) {
                    break;
                }
                int after = 1;
                while (row[(sector + after) % kTorsoSectors] < 0.0f) {
                    after++;
                }
                const float from = row[(sector - before + kTorsoSectors) % kTorsoSectors];
                const float to = row[(sector + after) % kTorsoSectors];
                filled[sector] = from + (to - from) * static_cast<float>(before) / static_cast<float>(before + after);
            }
            std::copy(filled.begin(), filled.end(), row);
        }
        for (int band = 0; band < kTorsoBands; band++) {
            for (int step = 1; shape[band * kTorsoSectors] < 0.0f && step < kTorsoBands; step++) {
                for (const int from : {band - step, band + step}) {
                    if (from >= 0 && from < kTorsoBands && shape[from * kTorsoSectors] >= 0.0f) {
                        std::copy_n(&shape[from * kTorsoSectors], kTorsoSectors, &shape[band * kTorsoSectors]);
                        break;
                    }
                }
            }
        }
        torso.shape = shape;
        torso.fitted = true;
    }
}

float cloth_body::Clearance(const Capsule &limb, const float *home) {
    const std::array<float, 3> at = NearestOnCapsule(limb, home);
    return std::min(limb.radius + kCapsuleMargin, std::hypot(home[0] - at[0], home[1] - at[1], home[2] - at[2]));
}

// The PS2's step lets the cloth through the chest when the body turns quickly (an about-turn swings
// Toan's chest through the poncho over a dozen ticks) and when an arm presses it there. Carrying the cloth
// round with the turn instead took most of its swing away; keeping it off the chest keeps the swing.
// Squeezed between an arm and the chest, the cloth stays on the chest and the arm overlaps it.
void cloth_body::PushOutOfBody(std::span<const Capsule> capsules, std::span<const float> clearances, const float *home,
                               float *vertex) {
    for (const Capsule &capsule : capsules) {
        if (!capsule.torso) {
            continue;
        }
        // Cloth authored resting inside the torso (Toan's collar) goes no deeper than it rests: pushed out
        // to the surface, it puffed; left be, it sank deeper into his chest in the attract movie's poses.
        const std::array<float, 3> home_at = NearestOnCapsule(capsule, home);
        const float                rest = std::hypot(home[0] - home_at[0], home[1] - home_at[1], home[2] - home_at[2]);
        const float                reach = std::min(cloth_body::Reach(capsule, vertex) + kCapsuleMargin, rest);
        const std::array<float, 3> nearest = NearestOnCapsule(capsule, vertex);
        const float                out[3] = {vertex[0] - nearest[0], vertex[1] - nearest[1], vertex[2] - nearest[2]};
        const float                distance = std::hypot(out[0], out[1], out[2]);
        if (distance < reach && distance > 1e-6f) {
            for (int axis = 0; axis < 3; axis++) {
                vertex[axis] = nearest[axis] + out[axis] * reach / distance;
            }
        }
    }
    for (std::size_t c = 0; c < capsules.size(); c++) {
        const Capsule &capsule = capsules[c];
        if (capsule.torso) {
            continue;
        }
        const float                reach = c < clearances.size() ? clearances[c] : capsule.radius + kCapsuleMargin;
        const std::array<float, 3> nearest = NearestOnCapsule(capsule, vertex);
        const float                out[3] = {vertex[0] - nearest[0], vertex[1] - nearest[1], vertex[2] - nearest[2]};
        const float                distance = std::hypot(out[0], out[1], out[2]);
        if (distance >= reach || distance <= 1e-6f) {
            continue;
        }
        float pushed[3];
        for (int axis = 0; axis < 3; axis++) {
            pushed[axis] = nearest[axis] + out[axis] * reach / distance;
        }
        const TorsoNearest torso = NearestTorso(capsules, pushed);
        if (torso.capsule != nullptr && torso.distance > 1e-6f) {
            const float floor = std::min(torso.reach + kCapsuleMargin, NearestTorso(capsules, home).distance);
            if (torso.distance < floor) {
                for (int axis = 0; axis < 3; axis++) {
                    pushed[axis] = torso.at[axis] + (pushed[axis] - torso.at[axis]) * floor / torso.distance;
                }
            }
        }
        std::copy(pushed, pushed + 3, vertex);
    }
}

void CCloth::Step(int step) {
    if (num_i <= 0 || num_i > 16 || num_j <= 0 || num_j > 16) {
        return;
    }

    if (step < 0) {
        SettledOn().erase(this);
        Clear();
        return;
    }

    sceVu0FVECTOR root;
    sceVu0FVECTOR delta = {0.0f, 0.0f, 0.0f, 0.0f};
    sceVu0FMATRIX matrix = {};
    sceVu0FVECTOR scratch = {};

    // The simulation works on a copy of the grid; point is only updated once the step completes.
    auto work = std::make_unique<sceVu0FVECTOR[]>(static_cast<std::size_t>(num_i) * static_cast<std::size_t>(num_j));
    auto work_at = [&](int row, int column) -> float * {
        return work[(static_cast<std::size_t>(row) * static_cast<std::size_t>(num_j)) + static_cast<std::size_t>(column)];
    };
    auto  stretch_params = std::make_unique<sceVu0FVECTOR[]>(static_cast<std::size_t>(num_j));
    float hit_point[4] = {};

    bool        reset = false;
    const float ground = floor_y;

    for (int i = 0; i < num_i; i++) {
        for (int j = 0; j < num_j; j++) {
            vu0::Copy(work_at(i, j), point[i][j]);
        }
    }

    if (frame != nullptr) {
        frame->GetLWMatrix(matrix);
        sceVu0ApplyMatrix(root, matrix, this->position);
        sceVu0SubVector(delta, root, last_position);

        if (vuabs(delta) > 10.0f || stop != 0) {
            reset = true;
        } else {
            delta[0] *= follow[0];
            delta[1] *= follow[1];
            delta[2] *= follow[2];
        }

        last_position[0] = root[0];
        last_position[1] = root[1];
        last_position[2] = root[2];
    }

    float stiffness_held[4];
    vu0::Copy(stiffness_held, stiffness);

    for (int j = 0; j < num_j; j++) {
        for (int i = 0; i < num_i; i++) {
            float *vertex = work_at(i, j);
            float *velocity = speed[i][j];

            if (reset) {
                vertex[0] += delta[0];
                vertex[1] += delta[1];
                vertex[2] += delta[2];
            } else if (frame != nullptr) {
                Follow(vertex, velocity, world_home[i][j], home[i][j], delta, matrix, stiffness_held);
            } else {
                vertex[0] += velocity[0];
                vertex[1] += velocity[1];
                vertex[2] += velocity[2];
            }
        }
    }

    scratch[1] = 0.5f;
    scratch[2] = 0.5f;

    for (int j = 0; j < num_j - 1; j++) {
        stretch_params[j][1] = 0.3f + 0.2f * (static_cast<float>(j) / static_cast<float>(num_j));
        stretch_params[j][2] = 1.0f - stretch_params[j][1];
    }

    for (int pass = 0; pass < 4 && !reset; pass++) {
        for (int j = 0; j < num_j - 1; j++) {
            for (int i = 0; i < num_i; i++) {
                if (i > 1) {
                    scratch[0] = 2.0f * rest[i][j][0];
                    StretchBind2(work_at(i, j), work_at(i - 2, j), scratch);
                } else {
                    scratch[0] = 0.0f;
                }

                stretch_params[j][0] = rest[i][j][1];
                StretchBind2(work_at(i, j), work_at(i, j + 1), stretch_params[j]);
            }
        }

        for (int row = 0; row < num_i; row++) {
            if (frame == nullptr) {
                sceVu0CopyVector(work_at(row, 0), home[row][0]);
            } else {
                vu0::Copy(work_at(row, 0), world_home[row][0]);
            }
        }
    }

    if (bound != nullptr) {
        bound->UpDate();
    }

    for (int j = 1; j < num_j; j++) {
        for (int i = 0; i < num_i; i++) {
            bool          touched = false;
            float         hit_count = 0.0f;
            sceVu0FVECTOR hit_sum = {};
            float         friction = 0.0f;

            for (CBound *box = bound; box != nullptr; box = box->next) {
                if ((mask[i][j] & (1 << box->mask_bit)) != 0 && box->InCheck(work_at(i, j), hit_point) != 0) {
                    hit_count += 1.0f;
                    sceVu0AddVector(hit_sum, hit_sum, hit_point);
                    friction += box->friction;
                    touched = true;
                }
            }

            if (touched) {
                work_at(i, j)[0] = hit_sum[0] / hit_count;
                work_at(i, j)[1] = hit_sum[1] / hit_count;
                work_at(i, j)[2] = hit_sum[2] / hit_count;
                rest[i][j][3] = friction / hit_count;
            } else {
                rest[i][j][3] = -1.0f;
            }
        }
    }

    if (ConfigGet().enhanced_cloth && frame != nullptr && !reset) {
        PushClothOutOfBody(*this, work_at);
    } else if (reset) {
        // A cut or a teleport puts the cloth back on its rest shape, on the body as it stands now.
        SettledOn().erase(this);
    }

    sceVu0FVECTOR wind_force = {0.0f, 0.0f, 0.0f, 0.0f};

    for (int i = 0; i < num_i; i++) {
        if (wind != nullptr) {
            static_cast<CWind *>(wind)->GetWindNoise(wind_force);
            sceVu0ScaleVector(wind_force, wind_force, wind_effect);
        }

        for (int j = 0; j < num_j; j++) {
            speed[i][j][0] += gravity[0] + (work_at(i, j)[0] - last[i][j][0]);
            speed[i][j][1] += gravity[1] + (work_at(i, j)[1] - last[i][j][1]);
            speed[i][j][2] += gravity[2] + (work_at(i, j)[2] - last[i][j][2]);
            sceVu0CopyVector(last[i][j], work_at(i, j));
            sceVu0Normalize(scratch, speed[i][j]);
            float facing = sceVu0InnerProduct(scratch, normal_grid[i][j]);

            if (facing < 0.0f) {
                facing *= -1.0f;
            }

            const float damping = std::min(0.6f + (1.0f - facing), 1.0f);

            speed[i][j][0] += facing * wind_force[0];
            speed[i][j][1] += facing * wind_force[1];
            speed[i][j][2] += facing * wind_force[2];
            speed[i][j][0] *= damping;
            speed[i][j][1] *= damping;
            speed[i][j][2] *= damping;

            if (reset) {
                speed[i][j][0] = 0.0f;
                speed[i][j][1] = 0.0f;
                speed[i][j][2] = 0.0f;
            } else {
                if (rest[i][j][3] > 0.0f) {
                    speed[i][j][0] *= rest[i][j][3];
                    speed[i][j][1] *= rest[i][j][3];
                    speed[i][j][2] *= rest[i][j][3];
                }

                if (work_at(i, j)[1] <= ground && floor_on != 0) {
                    work_at(i, j)[1] = ground;
                    speed[i][j][0] *= 0.4f;
                    speed[i][j][2] *= 0.4f;
                }
            }
        }
    }

    sceVu0FVECTOR edge[4];
    sceVu0FVECTOR normal = {};
    sceVu0FVECTOR cross[4];

    for (int j = 0; j < num_j; j++) {
        for (int i = 0; i < num_i; i++) {
            const int next_i = std::min(i + 1, num_i - 1);
            const int prev_i = std::max(i - 1, 0);
            const int prev_j = std::max(j - 1, 0);
            const int next_j = std::min(j + 1, num_j - 1);

            sceVu0SubVector(edge[0], work_at(next_i, j), work_at(i, j));
            sceVu0SubVector(edge[1], work_at(prev_i, j), work_at(i, j));
            sceVu0SubVector(edge[2], work_at(i, prev_j), work_at(i, j));
            sceVu0SubVector(edge[3], work_at(i, next_j), work_at(i, j));
            sceVu0OuterProduct(cross[0], edge[3], edge[0]);
            sceVu0OuterProduct(cross[1], edge[1], edge[3]);
            sceVu0OuterProduct(cross[2], edge[2], edge[1]);
            sceVu0OuterProduct(cross[3], edge[0], edge[2]);
            sceVu0AddVector(normal, cross[0], cross[1]);
            sceVu0AddVector(normal, normal, cross[2]);
            sceVu0AddVector(normal, normal, cross[3]);
            sceVu0ScaleVector(normal, normal, normal_scale);
            sceVu0Normalize(normal_grid[i][j], normal);
        }
    }

    for (int i = 0; i < num_i; i++) {
        for (int j = 0; j < num_j; j++) {
            vu0::Copy(point[i][j], work_at(i, j));
        }
    }
}

int CCloth::DrawVu1(u_int * /*packet*/, float (* /*matrix*/)[4], RenderInfo *info, VU1_PROGRAM program,
                    u_long128 * /*draw_state*/, int /*unknown1*/, int /*unknown2*/) {
    CreateVUData(vu_block[DBuffID]);
    vu_data = vu_block[DBuffID];
    if (const Draw3DVisual *visual = Draw3DFindVisual(vu_data)) {
        sceVu0FMATRIX model;
        ClothDrawMatrix(frame, model);
        Draw3DIdentityScope identity(this, kClothResetDistance, true);
        Draw3DDrawVisual(*visual, model, *info, program);
    }
    return 0;
}

int CCloth::DrawVu1(sceVif1Packet * /*packet*/, float (*matrix)[4], RenderInfo *info, VU1_PROGRAM program,
                    u_long128 *draw_state, int unknown1, int unknown2) {
    return CCloth::DrawVu1(static_cast<u_int *>(nullptr), matrix, info, program, draw_state, unknown1, unknown2);
}

// The grid is rebuilt every frame from the simulation, so it is drawn as an immediate triangle
// list: one strip per row pair, each row drawn from the far side (wound the other way) when
// polygon_divide says so, all with the fixed cloth material and the model's texture. The vertices
// are relative to the attachment frame, so display frames carry them with the body's rotation
// while interpolating the simulated deformation separately.
int CCloth::CreateVUData(u_int *packet) {
    if (packet == nullptr) {
        return kDraw3DBlockQuads;
    }
    if (num_i <= 0 || num_i > 16 || num_j <= 0 || num_j > 16) {
        return kDraw3DBlockQuads;
    }
    Draw3DVisual &visual = Draw3DRegisterVisual(packet, true);
    sceVu0FMATRIX model;
    sceVu0FMATRIX inverse;
    ClothDrawMatrix(frame, model);
    sceVu0InversMatrix(inverse, model);

    Draw3DStrip strip;
    float       cloth_ambient[4] = {0.3f, 0.3f, 0.3f, 0.0f};
    std::memcpy(strip.ambient, cloth_ambient, sizeof(strip.ambient));
    for (float &value : strip.specular) {
        value = 1.0f;
    }
    float     su = 1.0f;
    float     sv = 1.0f;
    CTexture *texture = TexManager.GetTexture(material.texture, -1);
    if (texture != nullptr) {
        strip.texture = TexManager.GetTextureHandle(material.texture, -1);
        strip.tex0 = texture->tex0;
        strip.tex1 = texture->tex1;
        if (texture->width > 0 && texture->height > 0) {
            su = static_cast<float>(1u << ((texture->tex0 >> 26) & 0xF)) / static_cast<float>(texture->width);
            sv = static_cast<float>(1u << ((texture->tex0 >> 30) & 0xF)) / static_cast<float>(texture->height);
        }
    }

    auto add = [&](int i, int j) {
        gfx::Vertex3D vertex = {};
        sceVu0FVECTOR world_point = {point[i][j][0], point[i][j][1], point[i][j][2], 1.0f};
        sceVu0FVECTOR local_point;
        sceVu0FVECTOR local_normal;
        sceVu0ApplyMatrix(local_point, inverse, world_point);
        sceVu0FVECTOR world_normal = {normal_grid[i][j][0], normal_grid[i][j][1], normal_grid[i][j][2], 0.0f};
        sceVu0ApplyMatrix(local_normal, inverse, world_normal);
        std::memcpy(vertex.position, local_point, sizeof(vertex.position));
        std::memcpy(vertex.normal, local_normal, sizeof(vertex.normal));
        vertex.uv[0] = texture_coord[i][j][0] * su;
        vertex.uv[1] = texture_coord[i][j][1] * sv;
        vertex.color[0] = vertex.color[1] = vertex.color[2] = vertex.color[3] = 0x80;
        visual.vertices.push_back(vertex);
    };

    for (int i = 0; i < num_i - 1; i++) {
        auto first = static_cast<uint32_t>(visual.vertices.size());
        for (int j = 0; j < num_j; j++) {
            if (polygon_divide[i] != 0) {
                add(i + 1, j);
                add(i, j);
            } else {
                add(i, j);
                add(i + 1, j);
            }
        }
        Draw3DStripToList(visual.indices, first, static_cast<uint32_t>(num_j * 2));
    }
    strip.index_count = static_cast<uint32_t>(visual.indices.size());
    visual.strips.push_back(strip);
    return kDraw3DBlockQuads;
}

void CCloth::InitParam() {
    float side_follow = 0.9f;
    num_i = 16;
    num_j = 16;
    pitch = 1.0f;
    init_20 = 0;
    vu_block[1] = nullptr;
    vu_block[0] = nullptr;
    frame = nullptr;
    init_40 = 0;
    flags = 0;
    wind = nullptr;
    wind_effect = 1.0f;
    floor_y = 0.0f;
    last_position[0] = 0.0f;
    last_position[1] = 0.0f;
    last_position[2] = 0.0f;
    stiffness[0] = 0.1f;
    stiffness[1] = 0.1f;
    stiffness[2] = 0.1f;
    follow[0] = side_follow;
    follow[1] = 0.4f;
    follow[2] = side_follow;
    gravity[0] = 0.0f;
    gravity[1] = -0.1f;
    gravity[2] = 0.0f;
    normal_scale = 1.0f;
    stop = false;
    floor_on = true;
    sceVu0FVECTOR zero = {0.0f, 0.0f, 0.0f, 0.0f};

    for (int i = 0; i < num_i; i++) {
        for (int j = 0; j < num_j; j++) {
            mask[i][j] = 0xFFFF;
            sceVu0CopyVector(home[i][j], zero);
            sceVu0CopyVector(point[i][j], zero);
            sceVu0CopyVector(last[i][j], zero);
            sceVu0CopyVector(rest[i][j], zero);
            sceVu0CopyVector(speed[i][j], zero);
            sceVu0CopyVector(normal_grid[i][j], zero);
            sceVu0CopyVector(texture_coord[i][j], zero);
        }
    }

    bound = nullptr;
    material.texture[0] = 0;
}

CCloth::CCloth(int grid_i, int grid_j, float grid_pitch) {
    InitParam();
    num_i = grid_i;
    num_j = grid_j;
    pitch = grid_pitch;
}

void CCloth::Initialize(CDataAlloc2<1> *alloc) {
    if (alloc == nullptr) {
        alloc = &VisualData;
    }

    alloc->Align64();
    vu_block[0] = reinterpret_cast<u_int *>(&alloc->base[static_cast<std::ptrdiff_t>(alloc->used) * 16]);
    vu_size = CreateVUData(vu_block[0]);
    alloc->Alloc(vu_size);
    vu_block[1] = reinterpret_cast<u_int *>(alloc->Alloc64(vu_size));
    vu_size = CreateVUData(vu_block[1]);

    for (int i = 0; i < num_i; i++) {
        polygon_divide[i] = 0;

        for (int j = 0; j < num_j; j++) {
            sceVu0FVECTOR zero = {0.0f, 0.0f, 0.0f, 0.0f};
            sceVu0CopyVector(last[j][i], zero);
            sceVu0CopyVector(speed[j][i], zero);
            sceVu0CopyVector(normal_grid[j][i], zero);
            sceVu0CopyVector(rest[j][i], zero);
            point[j][i][3] = 1.0f;
            last[j][i][3] = 1.0f;
        }
    }

    for (int i = 0; i < num_j; i++) {
        for (int j = 0; j < num_i; j++) {
            sceVu0CopyVector(last[j][i], point[j][i]);
            sceVu0CopyVector(home[j][i], point[j][i]);
        }
    }

    sceVu0FVECTOR centre = {0.0f, 0.0f, 0.0f, 0.0f};

    for (int i = 0; i < num_i; i++) {
        sceVu0AddVector(centre, centre, home[0][i]);
    }

    position[0] = centre[0] / static_cast<float>(num_i);
    position[1] = centre[1] / static_cast<float>(num_i);
    position[2] = centre[2] / static_cast<float>(num_i);
    position[3] = 1.0f;

    // Record each vertex's rest distance to its neighbours down and across the grid.
    sceVu0FVECTOR edge;

    for (int i = 0; i < num_j; i++) {
        for (int j = 0; j < num_i; j++) {
            int next_j = j + 1;
            int prev_j = j - 1;
            int prev_i = i - 1;
            int next_i = i + 1;

            if (prev_j > 0 || next_j >= num_i) {
                sceVu0SubVector(edge, point[j][i], point[prev_j][i]);
            } else {
                sceVu0SubVector(edge, point[j][i], point[next_j][i]);
            }

            rest[j][i][0] = vuabs(edge);

            if (next_i >= num_j) {
                sceVu0SubVector(edge, point[j][i], point[j][prev_i]);
            } else {
                sceVu0SubVector(edge, point[j][i], point[j][next_i]);
            }

            rest[j][i][1] = vuabs(edge);
        }
    }
}

void CCloth::Initialize(MDT_HEADER *header, CDataAlloc2<1> *alloc) {
    if (header == nullptr) {
        return;
    }

    auto *resource = reinterpret_cast<u_char *>(header);
    auto *vertices = reinterpret_cast<sceVu0FVECTOR *>(&resource[header->vertex_ofs]);
    auto *uvs = reinterpret_cast<sceVu0FVECTOR *>(&resource[header->uv[1]]);
    auto *materials = reinterpret_cast<MDT_MATERIAL *>(&resource[header->info_ofs]);
    auto *mesh = reinterpret_cast<int *>(&resource[header->mesh_ofs]);
    // Each strip: a three-word header (word 1 corner count, word 2 material), then three-word corners
    // (word 0 vertex, word 2 texture coordinate).
    const int *strips = &mesh[4];
    int        strip_count = mesh[2];

    for (int i = 0; i < num_i; i++) {
        for (int j = 0; j < num_j; j++) {
            int vertex_no = j + i * num_j;
            sceVu0CopyVector(point[i][j], vertices[vertex_no]);
            int strip_pos = 0;

            for (int strip_no = 0; strip_no < strip_count; strip_no++) {
                int corner_count = strips[strip_pos + 1];
                material = materials[strips[strip_pos + 2]];
                strip_pos += 3;

                for (int corner_no = 0; corner_no < corner_count; corner_no++) {
                    const int *corner = &strips[strip_pos + corner_no * 3];

                    if (corner[0] == vertex_no) {
                        sceVu0CopyVector(texture_coord[i][j], uvs[corner[2]]);
                    }
                }

                strip_pos += corner_count * 3;
            }
        }
    }

    Initialize(alloc);
}

} // namespace dc::game

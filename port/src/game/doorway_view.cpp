#include "doorway_view.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "dungeon.h"
#include "frame.hpp"
#include "framevu1.hpp"
#include "gamemode.hpp"
#include "main.hpp"
#include "mapparts.hpp"
#include "mglib.hpp"
#include "moonmod/game_hooks.hpp"
#include "render/draw3d.hpp"
#include "renderinfo.hpp"
#include "texture.hpp"

namespace dc::game {

namespace {

constexpr float kGsCentre = 2048.0f;
constexpr float kCaveWindowBehind = 8.0f; // how far behind the wall's face the cave's window stands
constexpr int   kGrid = 12; // the window is drawn as a grid so its picture follows the other side's perspective

// One side of a doorway: where its door is, and a picture of it seen from the door.
struct Side {
    bool               placed = false;
    float              width = 0.0f; // the window, when the door has its own (else Shape's)
    float              height = 0.0f;
    float              origin[3] = {}; // the party at the door
    float              out[3] = {};    // level and unit: from the door toward the other place
    bool               pictured = false;
    RenderInfo         camera = {};    // the camera the picture was taken with
    float              eye[3] = {};    // and where it stood
    gfx::TextureHandle picture = gfx::kNullTexture;
};

struct Doorway {
    Side town;
    Side dungeon;
};

std::map<int, Doorway> g_doorways; // by town map * 1000 + dungeon map
int                    g_leaving = -1; // the map a jump is leaving, until its picture is taken
int                    g_leaving_to = -1;
bool                   g_have_last = false;
float                  g_last_position[3] = {};
RenderInfo             g_last_camera = {};
// A dungeon is built again on each visit, its way in somewhere else: once the party has walked in (stood still a
// moment after the held picture went), its door is where it stands, the way out where the camera looks.
// The live view (L4): the last town and dungeon drawn (the ones whose places are kept), and this tick's view of the
// other side, drawn by DoorwayLive for DoorwayDraw.
int                    g_last_town = -1;
int                    g_last_dungeon = -1;
gfx::TextureHandle     g_live = gfx::kNullTexture;
const void            *g_live_side = nullptr; // the side whose door shows g_live this tick
bool                   g_walking_through = false; // the jump under way is the party walking through a door
int                    g_arriving = 0; // frames left to stand the party at the town's door (the first is undone)     // a walk out of a dungeon's way out, to stand at the town's door
float                  g_arrive_at[3] = {};
float                  g_arrive_heading = 0.0f;
int                    g_entering = -1; // walked through the town's door into dungeon map g_entering, its doorway placed
float                  g_enter_at[3] = {};
float                  g_enter_heading = 0.0f;
int                    g_settling = -1; // the dungeon map being settled into, or -1
int                    g_settle_frames = 0;
int                    g_still_frames = 0;
float                  g_settle_position[3] = {};
bool                   g_have_walk_out = false;
float                  g_walk_out[3] = {}; // the camera's look as the walk-in begins: at the stairs, the way out


// The picture windows are off: the user wants the live places, both loaded (O2 C1), not pictures of them. The
// doors are still found (where the party left and walked in), for the live view to use.
bool PicturesOn() { return std::getenv("DC_DOORWAY_PICTURES") != nullptr; }

Doorway *Find(int a, int b, bool create) {
    const int town = IsDungeonMap(a) ? b : a;
    const int dungeon = IsDungeonMap(a) ? a : b;
    const int key = town * 1000 + dungeon;
    auto      it = g_doorways.find(key);
    if (it != g_doorways.end()) return &it->second;
    return create ? &g_doorways[key] : nullptr;
}

Side &SideOf(Doorway &doorway, int map_no) { return IsDungeonMap(map_no) ? doorway.dungeon : doorway.town; }

// The camera's look, level and unit: world to eye is column-major, so the eye's z axis is row 2.
bool CameraForward(const RenderInfo &camera, float forward[3]) {
    forward[0] = camera.view[0][2];
    forward[1] = 0.0f;
    forward[2] = camera.view[2][2];
    const float length = std::sqrt(forward[0] * forward[0] + forward[2] * forward[2]);
    if (!(length > 1e-4f)) return false;
    forward[0] /= length;
    forward[2] /= length;
    return true;
}

// Tunables while the doorways are fitted (DC_DOORWAY="distance,width,height,back,viewer").
void Shape(float &distance, float &width, float &height, float &back, float &viewer) {
    distance = 2.0f;
    width = 50.0f;
    height = 70.0f;
    back = -5.0f;
    viewer = 80.0f;
    if (const char *text = std::getenv("DC_DOORWAY")) {
        float d = 0.0f, w = 0.0f, h = 0.0f, b = 0.0f, v = 0.0f;
        if (std::sscanf(text, "%f,%f,%f,%f,%f", &d, &w, &h, &b, &v) == 5) {
            distance = d;
            width = w;
            height = h;
            back = b;
            viewer = v;
        }
    }
}

// The logical frame point (0-640, 0-480) a world point lands on under camera, as MGRotTransPers2D gives it.
void Project(const RenderInfo &camera, const float point[3], float &u, float &v) {
    const auto &m = camera.view_scaled;
    const float x = m[0][0] * point[0] + m[1][0] * point[1] + m[2][0] * point[2] + m[3][0];
    const float y = m[0][1] * point[0] + m[1][1] * point[1] + m[2][1] * point[2] + m[3][1];
    float       z = m[0][2] * point[0] + m[1][2] * point[1] + m[2][2] * point[2] + m[3][2];
    if (z < 1.0f) z = 1.0f;
    const float centre_x = camera.offset[0] - (kGsCentre - 320.0f);
    const float centre_y = camera.offset[1] - (kGsCentre - 240.0f);
    u = (centre_x + camera.scale[0] * x / z) / 640.0f;
    v = (centre_y + camera.scale[1] * y / z) / 480.0f;
}

void Multiply(float out[4][4], const float a[4][4], const float b[4][4]) {
    float result[4][4];
    for (int column = 0; column < 4; column++) {
        for (int row = 0; row < 4; row++) {
            result[column][row] = a[0][row] * b[column][0] + a[1][row] * b[column][1] + a[2][row] * b[column][2] +
                                  a[3][row] * b[column][3];
        }
    }
    for (int column = 0; column < 4; column++) {
        for (int row = 0; row < 4; row++) out[column][row] = result[column][row];
    }
}

void Translation(float m[4][4], float x, float y, float z) {
    for (int column = 0; column < 4; column++) {
        for (int row = 0; row < 4; row++) m[column][row] = column == row ? 1.0f : 0.0f;
    }
    m[3][0] = x;
    m[3][1] = y;
    m[3][2] = z;
}

// The doorway's move: a point here, taken through the door, lands there with the door planes laid on each other
// (walking out of here through it is walking into there, against its way out).
struct Move {
    float r[3][3]; // [row][column], here to there
    float here[3];
    float there[3];
};

Move DoorMove(const Side &here, const Side &there) {
    const float f[3] = {here.out[0], 0.0f, here.out[2]};
    const float g[3] = {-there.out[0], 0.0f, -there.out[2]};
    const float rf[3] = {f[2], 0.0f, -f[0]};
    const float rg[3] = {g[2], 0.0f, -g[0]};
    const float up[3] = {0.0f, 1.0f, 0.0f};
    Move        move = {};
    for (int row = 0; row < 3; row++) {
        for (int column = 0; column < 3; column++) {
            move.r[row][column] = rg[row] * rf[column] + up[row] * up[column] + g[row] * f[column];
        }
    }
    for (int i = 0; i < 3; i++) {
        move.here[i] = here.origin[i];
        move.there[i] = there.origin[i];
    }
    return move;
}

void MovePoint(const Move &move, const float p[3], float out[3]) {
    float d[3];
    for (int i = 0; i < 3; i++) d[i] = p[i] - move.here[i];
    for (int row = 0; row < 3; row++) {
        out[row] = move.there[row] + move.r[row][0] * d[0] + move.r[row][1] * d[1] + move.r[row][2] * d[2];
    }
}

// The window's corners, here.
void WindowCorners(const Side &here, float corners[4][3]) {
    float distance, width, height, back, viewer;
    Shape(distance, width, height, back, viewer);
    if (here.width > 0.0f) {
        width = here.width;
        height = here.height;
    }
    const float right[3] = {here.out[2], 0.0f, -here.out[0]};
    for (int c = 0; c < 4; c++) {
        const float across = (c & 1 ? 0.5f : -0.5f) * width;
        for (int i = 0; i < 3; i++) corners[c][i] = here.origin[i] + here.out[i] * distance + right[i] * across;
        corners[c][1] = here.origin[1] + (c & 2 ? height : 0.0f);
    }
}

// Draws the other place into g_live under this tick's camera taken through the door.
bool DrawLive(const Side &here, const Side &there, bool there_is_town) {
    const float *eye = mgRenderInfo.view_position;
    float        distance, width, height, back, viewer;
    Shape(distance, width, height, back, viewer);
    // looked at from the front, and near enough to matter
    const float ahead = (here.origin[0] + here.out[0] * distance - eye[0]) * here.out[0] +
                        (here.origin[2] + here.out[2] * distance - eye[2]) * here.out[2];
    if (ahead < 0.0f || ahead > 3000.0f) return false;
    // the eye's look (world to eye is column-major: the eye's axes are the rows)
    const float look[3] = {mgRenderInfo.view[0][2], mgRenderInfo.view[1][2], mgRenderInfo.view[2][2]};
    float       corners[4][3];
    WindowCorners(here, corners);
    float nearest = 1e9f;
    for (const auto &corner : corners) {
        nearest = std::min(nearest, (corner[0] - eye[0]) * look[0] + (corner[1] - eye[1]) * look[1] +
                                        (corner[2] - eye[2]) * look[2]);
    }
    if (!(nearest > 0.0f)) return false;

    if (g_live == gfx::kNullTexture) {
        g_live = gfx::NamedRenderTarget("moonmod doorway live", 640, SCREEN_HEIGHT, false, false, true);
        if (g_live == gfx::kNullTexture) return false;
    }

    // the camera through the door: there's eye is here's moved; there's view takes a point back here, then on
    const Move move = DoorMove(here, there);
    float      new_eye[3];
    MovePoint(move, eye, new_eye);
    float back_move[4][4] = {};
    for (int column = 0; column < 3; column++) {
        for (int row = 0; row < 3; row++) back_move[column][row] = move.r[column][row]; // the transpose
    }
    for (int row = 0; row < 3; row++) {
        back_move[3][row] = move.here[row] - (move.r[0][row] * move.there[0] + move.r[1][row] * move.there[1] +
                                              move.r[2][row] * move.there[2]);
    }
    back_move[3][3] = 1.0f;
    float view[4][4];
    Multiply(view, mgRenderInfo.view, back_move);
    float look_there[3];
    for (int row = 0; row < 3; row++) {
        look_there[row] = move.r[row][0] * look[0] + move.r[row][1] * look[1] + move.r[row][2] * look[2];
    }
    const float ref[3] = {new_eye[0] + look_there[0] * 100.0f, new_eye[1] + look_there[1] * 100.0f,
                          new_eye[2] + look_there[2] * 100.0f};

    const RenderInfo         saved = mgRenderInfo;
    const s32                saved_clear = mgClearBackFlag;
    sceVu0FVECTOR            saved_back;
    sceVu0CopyVector(saved_back, mgBackColor);
    const gfx::TextureHandle saved_target = gfx::CurrentRenderTarget();
    // everything on this side of the door is clipped away by the near plane; a dungeon's rock stands proud of the
    // wall the door was set in, so into a dungeon the cut goes deeper; a town's door is where the party stood as it
    // walked in, inside the dark hollow a town builds behind its door (Norune's cave mouth), so out to a town the cut
    // goes past that hollow to the threshold
    const float cut = there_is_town ? -70.0f : -30.0f;
    MGSetRenderInfo(mgRenderInfo.projection, std::max(saved.near[2], nearest - cut), saved.far[2]);
    sceVu0FVECTOR eye_there = {new_eye[0], new_eye[1], new_eye[2], 1.0f};
    MGSetViewMatrixQuiet(view, eye_there);
    gfx::SetRenderTarget(g_live);
    const int world = moonmod::ChronicleConnectedWorld(there_is_town ? g_last_town : g_last_dungeon);
    TextureManagerSwap(world);
    if (there_is_town) {
        EdDrawForDoorway(new_eye, ref);
    } else {
        DunDrawForDoorway(new_eye, ref);
    }
    TextureManagerSwap(world);
    gfx::SetRenderTarget(saved_target);
    mgRenderInfo = saved;
    mgClearBackFlag = saved_clear;
    sceVu0CopyVector(mgBackColor, saved_back);
    render::MGPortRestoreRegisters();
    return true;
}

void TakePicture(Side &side, int map_no, int other_map) {
    if (side.picture == gfx::kNullTexture) {
        const std::string name =
            std::string("moonmod doorway ") + std::to_string(map_no) + " " + std::to_string(other_map);
        side.picture = gfx::NamedRenderTarget(name.c_str(), 640, SCREEN_HEIGHT, false, false, true);
        if (side.picture == gfx::kNullTexture) return;
    }
    gfx::BlitTexture(gfx::kMainTarget, gfx::Rect{0, 0, 640, SCREEN_HEIGHT}, side.picture,
                     gfx::Rect{0, 0, 640, SCREEN_HEIGHT}, gfx::Filter::Linear);
    side.camera = mgRenderInfo;
    for (int i = 0; i < 3; i++) side.eye[i] = mgRenderInfo.view_position[i];
    side.pictured = true;
}

// Draws the other side's picture in this side's door: a point of the window, taken to the other side with the door
// planes laid on each other, is looked up where the other side's camera saw it.
void DrawWindow(const Side &here, const Side &there, bool live) {
    float distance, width, height, back, viewer;
    Shape(distance, width, height, back, viewer);
    if (live && here.width > 0.0f) {
        width = here.width;
        height = here.height;
    }
    // only from the side it is looked through: a camera past the door would see the picture's back
    const float *eye = mgRenderInfo.view_position;
    if ((eye[0] - here.origin[0]) * here.out[0] + (eye[2] - here.origin[2]) * here.out[2] > distance) return;
    // a viewer `viewer` in front of the window at the picture's eye height looks through it: each window point is
    // that viewer's ray, cast again from where the other side's camera stood
    const float eye_height = there.eye[1] - there.origin[1];
    // looking out of here, and into there (against its way out): the right hands match
    const float here_right[3] = {here.out[2], 0.0f, -here.out[0]};
    const float there_right[3] = {-there.out[2], 0.0f, there.out[0]};
    std::vector<gfx::Vertex3D> vertices;
    std::vector<uint32_t>      indices;
    vertices.reserve((kGrid + 1) * (kGrid + 1));
    for (int row = 0; row <= kGrid; row++) {
        for (int column = 0; column <= kGrid; column++) {
            const float   across = (static_cast<float>(column) / kGrid - 0.5f) * width;
            const float   up = static_cast<float>(row) / kGrid * height;
            gfx::Vertex3D vertex = {};
            for (int i = 0; i < 3; i++) {
                vertex.position[i] = here.origin[i] + here.out[i] * distance + here_right[i] * across;
            }
            vertex.position[1] = here.origin[1] + up;
            float far_point[3];
            for (int i = 0; i < 3; i++) far_point[i] = there.eye[i] - there.out[i] * viewer + there_right[i] * across;
            far_point[1] = there.eye[1] + up - eye_height;
            if (live) {
                Project(mgRenderInfo, vertex.position, vertex.uv[0], vertex.uv[1]);
            } else {
                Project(there.camera, far_point, vertex.uv[0], vertex.uv[1]);
            }
            vertex.normal[2] = 1.0f;
            vertex.color[0] = vertex.color[1] = vertex.color[2] = vertex.color[3] = 0x80;
            vertices.push_back(vertex);
        }
    }
    for (int row = 0; row < kGrid; row++) {
        for (int column = 0; column < kGrid; column++) {
            const uint32_t a = static_cast<uint32_t>(row * (kGrid + 1) + column);
            const uint32_t b = a + 1;
            const uint32_t c = a + kGrid + 1;
            const uint32_t d = c + 1;
            indices.insert(indices.end(), {a, b, c, b, d, c});
        }
    }

    static const float kWhite[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    static const float kBlack[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const float        model[4][4] = {{1, 0, 0, 0}, {0, 1, 0, 0}, {0, 0, 1, 0}, {0, 0, 0, 1}};
    gfx::MeshConstants constants = {};
    gfx::MeshTransform transform = {};
    render::Draw3DSceneConstants(constants, mgRenderInfo, model, &transform);
    render::Draw3DMaterial(constants, mgRenderInfo, kWhite, kWhite, kBlack);
    constants.flags = 0;
    gfx::DrawState state = render::Draw3DState(mgRenderInfo, false);
    state.blend = false;
    state.cull = gfx::CullMode::None;
    gfx::TextureBinding binding;
    binding.texture = live ? g_live : there.picture;
    binding.filter = gfx::Filter::Linear;
    binding.wrap_u = gfx::Wrap::Clamp;
    binding.wrap_v = gfx::Wrap::Clamp;
    const bool unkeyed = gfx::CurrentInterpKey() == 0;
    if (unkeyed) gfx::SetInterpKey(reinterpret_cast<uintptr_t>(&here));
    gfx::DrawMeshImmediate(vertices, indices, constants, binding, state, &transform);
    if (unkeyed) gfx::SetInterpKey(0);
}

} // namespace

namespace {
std::set<int> g_open_town_doors; // towns whose door into their dungeon has opened once (since the save was loaded)
std::set<int> g_prebuild_failed; // towns whose dungeon would not build unseen (since the save was loaded)
} // namespace

void DoorwayJumped(int from_map, int to_map) {
    if (!IsDungeonMap(from_map) && to_map >= 0 && DoorwayTownDungeon(from_map) == to_map) g_open_town_doors.insert(from_map);
    g_settling = IsDungeonMap(to_map) ? to_map : -1;
    g_have_walk_out = false;
    g_settle_frames = 0;
    g_still_frames = 0;
    float forward[3];
    if (!g_have_last || !CameraForward(g_last_camera, forward)) return;
    // leaving, the party walks into the door with the camera behind it; a dungeon's door, walked through, stays
    Side &side = SideOf(*Find(from_map, to_map, true), from_map);
    if (IsDungeonMap(from_map) && side.placed) {
        g_leaving = from_map;
        g_leaving_to = to_map;
        // the party comes out of the town's door, a few steps outside it (room for the camera behind), facing away
        const Side &town = Find(from_map, to_map, false)->town;
        g_arriving = g_walking_through && town.placed ? 3 : 0;
        if (g_arriving) {
            for (int i = 0; i < 3; i++) g_arrive_at[i] = town.origin[i] - town.out[i] * 110.0f;
            g_arrive_heading = std::atan2(-town.out[0], -town.out[2]);
        }
        g_walking_through = false;
        return;
    }
    for (int i = 0; i < 3; i++) side.origin[i] = g_last_position[i];
    side.out[0] = forward[0];
    side.out[1] = 0.0f;
    side.out[2] = forward[2];
    side.placed = true;
    g_entering = -1;
    if (!IsDungeonMap(from_map)) {
        // a town's door: the window fills its opening, from where the party walked into it
        side.width = 70.0f;
        side.height = 100.0f;
        // into a dungeon whose doorway is placed: the party comes out of it, a few steps in, facing away from it
        const Side &cave = Find(from_map, to_map, false)->dungeon;
        if (IsDungeonMap(to_map) && cave.placed) {
            g_entering = to_map;
            for (int i = 0; i < 3; i++) g_enter_at[i] = cave.origin[i] - cave.out[i] * (80.0f + kCaveWindowBehind + 3.0f);
            g_enter_heading = std::atan2(-cave.out[0], -cave.out[2]);
        }
    }
    g_leaving = from_map;
    g_leaving_to = to_map;
}

namespace {
// A dungeon's way out, behind the party as it stands walked in (`back`, level and unit): the door is the wall behind,
// so the camera following the party does not look through it.
void PlaceCave(Side &cave, const float position[3], const float back[3]) {
    float forward[3] = {back[0], 0.0f, back[2]};
    float wall[3], axis[3];
    float origin[3] = {position[0], position[1], position[2]};
    float width = 0.0f, height = 0.0f;
    if (DunDoorwayWall(position, forward, wall, axis)) {
        float distance, shape_width, shape_height, shape_back, viewer;
        Shape(distance, shape_width, shape_height, shape_back, viewer);
        // the window stands just behind the wall's face, wider and taller than any way out: the rock around the
        // opening is in front of it and hides it, so only the opening shows the other place
        for (int i = 0; i < 3; i++) {
            forward[i] = axis[i];
            origin[i] = wall[i] + axis[i] * (kCaveWindowBehind - distance);
        }
        origin[1] = position[1];
        width = 260.0f;
        height = 220.0f;
    }
    DunHideWayOutDoor(origin);
    for (int i = 0; i < 3; i++) cave.origin[i] = origin[i];
    cave.out[0] = forward[0];
    cave.out[1] = 0.0f;
    cave.out[2] = forward[2];
    cave.width = width;
    cave.height = height;
    cave.placed = true;
}

// The towns' ways into their dungeons, for the dungeon to be seen through them before the first visit: where the
// party stands as the jump starts and the way it faces walking in (Norune's cave door, program 130, stands it at 962,
// -499 facing +x; Matataki's forest path, program 3, walks it to -1287, -375, away from the camera it sets at -1132,
// -339), and a door's two halves as its event slides them open (Norune's: fixed part 3's "door1" and "door2", 18 each
// way along z; Matataki's path has none, fixed_part -1). The other towns have no way in on foot: their dungeons are
// reached from the world map, so there is no door to see through.
struct TownDoor {
    int         town;
    int         dungeon;
    float       x, y, z;
    float       heading;
    int         fixed_part;
    const char *halves[2];
    float       slide;
};
constexpr TownDoor kTownDoors[] = {
    {0, DUNGEON_MAP_BASE + 0, 962.0f, 0.0f, -499.0f, 1.5708f, 3, {"door1", "door2"}, 18.0f}, // Norune: Divine Beast Cave
    {1, DUNGEON_MAP_BASE + 1, -1287.0f, 31.1f, -375.0f, -1.7990f, -1, {nullptr, nullptr}, 0.0f}, // Matataki: Wise Owl Forest
};
} // namespace

int DoorwayPrebuildWanted(int town_map) {
    if (!moonmod::ChronicleQuickPassageOn() || g_prebuild_failed.contains(town_map)) return -1;
    for (const TownDoor &door : kTownDoors) {
        if (door.town != town_map) continue;
        const Doorway *doorway = Find(door.town, door.dungeon, false);
        const bool     placed = doorway != nullptr && doorway->town.placed && doorway->dungeon.placed;
        return placed ? -2 : door.dungeon;
    }
    return -1;
}

int DoorwayTownDungeon(int town_map) {
    for (const TownDoor &door : kTownDoors) {
        if (door.town == town_map) return door.dungeon;
    }
    return -1;
}

namespace {
const TownDoor *OpenDoor(int town_map) {
    if (!moonmod::ChronicleQuickPassageOn() || !g_open_town_doors.contains(town_map)) return nullptr;
    for (const TownDoor &door : kTownDoors) {
        if (door.town != town_map || door.fixed_part < 0) continue;
        const Doorway *doorway = Find(door.town, door.dungeon, false);
        return doorway != nullptr && doorway->town.placed && doorway->dungeon.placed ? &door : nullptr;
    }
    return nullptr;
}
} // namespace

void DoorwayStandTownDoorOpen(int town_map, CMapParts *fixed_parts, int fixed_parts_count) {
    const TownDoor *door = OpenDoor(town_map);
    if (door == nullptr || fixed_parts == nullptr || door->fixed_part >= fixed_parts_count) return;
    CMapParts &parts = fixed_parts[door->fixed_part];
    // every frame tree the event's handle moves (SetObjHandle's), the collision's among them, so the party walks in
    CFrame *roots[] = {parts.frame[0], parts.frame[1], parts.frame[2], parts.frame[3], parts.GetCollisionFrame(),
                       parts.shadow_frame, parts.shade_frame, parts.ripple_frame};
    for (int half = 0; half < 2; half++) {
        const float z = half == 0 ? -door->slide : door->slide;
        for (CFrame *root : roots) {
            if (root == nullptr) continue;
            if (CFrame *frame = root->SearchFrame(const_cast<char *>(door->halves[half]))) frame->SetPosition(0.0f, 0.0f, z);
        }
    }
}

int DoorwayOpenTownDoor(int town_map, const float facing[3]) {
    const TownDoor *door = OpenDoor(town_map);
    if (door == nullptr) return -2;
    const float length = std::sqrt(facing[0] * facing[0] + facing[2] * facing[2]);
    if (length < 1e-3f) return -1;
    const float toward = (facing[0] * std::sin(door->heading) + facing[2] * std::cos(door->heading)) / length;
    return toward > 0.6f ? door->dungeon : -1;
}

void DoorwayCloseTownDoors() {
    g_open_town_doors.clear();
    g_prebuild_failed.clear();
}

namespace {

// Where the world map sets the party down in Norune, Queens and Yellow Drops (their start program 128), and the way
// it faces in. Matataki's and Muska Lacka's gates are the ground their program 1 runs from, beside that spot (80, -880;
// 807, 405); Norune's spot faces a fence.
struct TownGate {
    int   town;
    float x, z;
    float heading;
};
constexpr TownGate kTownGates[] = {
    {0, -780.0f, -342.0f, 1.57f}, // Norune
    {2, 1407.0f, 44.0f, -1.57f},  // Queens
    {4, 0.0f, -560.0f, 0.0f},     // Yellow Drops
};
constexpr float kGateReach = 90.0f;  // the party stands this near the gate to take the road
constexpr float kGateLeft = 200.0f;  // and has walked this far off it, or faced in, since last time

int  g_road_town = -1;
bool g_road_armed = false;

} // namespace

bool DoorwayTownRoad(int town_map, const float position[3], const float facing[3]) {
    const TownGate *gate = nullptr;
    for (const TownGate &each : kTownGates) {
        if (each.town == town_map) gate = &each;
    }
    if (gate == nullptr) return false;
    if (town_map != g_road_town) {
        g_road_town = town_map;
        g_road_armed = false;
    }
    const float length = std::sqrt(facing[0] * facing[0] + facing[2] * facing[2]);
    if (length < 1e-3f) return false;
    const float out = -(facing[0] * std::sin(gate->heading) + facing[2] * std::cos(gate->heading)) / length;
    const float distance = std::hypot(position[0] - gate->x, position[2] - gate->z);
    if (out < 0.2f || distance > kGateLeft) g_road_armed = true;
    if (!g_road_armed || distance > kGateReach || out < 0.6f) return false;
    g_road_armed = false;
    return true;
}

void DoorwayPrebuildFailed(int town_map) {
    g_prebuild_failed.insert(town_map);
}

void DoorwayPrebuilt(int town_map, int dungeon_map, const float position[3], const float facing[3]) {
    for (const TownDoor &door : kTownDoors) {
        if (door.town != town_map || door.dungeon != dungeon_map) continue;
        Doorway &doorway = *Find(town_map, dungeon_map, true);
        if (!doorway.town.placed) {
            doorway.town.origin[0] = door.x;
            doorway.town.origin[1] = door.y;
            doorway.town.origin[2] = door.z;
            doorway.town.out[0] = std::sin(door.heading);
            doorway.town.out[1] = 0.0f;
            doorway.town.out[2] = std::cos(door.heading);
            doorway.town.width = 70.0f;
            doorway.town.height = 100.0f;
            doorway.town.placed = true;
        }
        const float length = std::sqrt(facing[0] * facing[0] + facing[2] * facing[2]);
        if (length > 1e-3f) {
            const float back[3] = {-facing[0] / length, 0.0f, -facing[2] / length};
            PlaceCave(doorway.dungeon, position, back);
        }
        g_last_dungeon = dungeon_map;
        g_last_town = town_map;
        g_settling = -1;
    }
}

void DoorwayCamera() {
    if (!PicturesOn()) return;
    if (g_leaving < 0 || g_leaving != MapNo || !moonmod::ChronicleHoldingPicture()) return;
    const Doorway *doorway = Find(g_leaving, g_leaving_to, false);
    if (doorway == nullptr) return;
    const Side &side = IsDungeonMap(g_leaving) ? doorway->dungeon : doorway->town;
    float       old_forward[3];
    if (!CameraForward(mgRenderInfo, old_forward)) return;
    float distance, width, height, back, viewer;
    Shape(distance, width, height, back, viewer);
    // the camera turned about the upright to look back in, standing just outside the door at its own height
    const float *old_eye = mgRenderInfo.view_position;
    const float  new_eye[3] = {side.origin[0] + side.out[0] * back, old_eye[1] - g_last_position[1] + side.origin[1],
                               side.origin[2] + side.out[2] * back};
    const float  turn = std::atan2(-side.out[2], -side.out[0]) - std::atan2(old_forward[2], old_forward[0]);
    const float  c = std::cos(turn), s = std::sin(turn);
    // the new view is the old one after taking the world back through the move: T(old) * Ry(-turn) * T(-new)
    float unturn[4][4] = {{c, 0.0f, -s, 0.0f}, {0.0f, 1.0f, 0.0f, 0.0f}, {s, 0.0f, c, 0.0f}, {0.0f, 0.0f, 0.0f, 1.0f}};
    float to_old[4][4], from_new[4][4], move[4][4], view[4][4];
    Translation(to_old, old_eye[0], old_eye[1], old_eye[2]);
    Translation(from_new, -new_eye[0], -new_eye[1], -new_eye[2]);
    Multiply(move, unturn, from_new);
    Multiply(move, to_old, move);
    Multiply(view, mgRenderInfo.view, move);
    sceVu0FVECTOR eye = {new_eye[0], new_eye[1], new_eye[2], 1.0f};
    MGSetViewMatrix(view, eye);
}

void DoorwayDraw(const float player_position[3], const float *facing) {
    if (!moonmod::ChronicleQuickPassageOn()) return;

    // the frame after a jump is under the held picture: the place being left, seen from its door
    if (g_leaving >= 0 && g_leaving != MapNo) g_leaving = -1;
    if (g_leaving >= 0 && !PicturesOn()) g_leaving = -1;
    if (g_leaving >= 0) {
        if (Doorway *doorway = Find(g_leaving, g_leaving_to, false)) {
            TakePicture(SideOf(*doorway, g_leaving), g_leaving, g_leaving_to);
        }
        g_leaving = -1;
        return;
    }

    // the dungeon's way out, walked into, takes the party back to the town it came from
    if (g_have_last && IsDungeonMap(MapNo) && g_last_town >= 0 && NextMapNo < 0 && !moonmod::ChronicleHoldingPicture()) {
        auto it = g_doorways.find(g_last_town * 1000 + MapNo);
        if (it != g_doorways.end() && it->second.dungeon.placed) {
            const Side &door = it->second.dungeon;
            float       distance, width, height, back, viewer;
            Shape(distance, width, height, back, viewer);
            if (door.width > 0.0f) width = door.width;
            const float right[3] = {door.out[2], 0.0f, -door.out[0]};
            const float to_party[3] = {player_position[0] - door.origin[0], 0.0f, player_position[2] - door.origin[2]};
            const float ahead = distance - (to_party[0] * door.out[0] + to_party[2] * door.out[2]);
            const float across = to_party[0] * right[0] + to_party[2] * right[2];
            // facing it, as near as the wall lets the party come
            const float facing_length =
                facing != nullptr ? std::sqrt(facing[0] * facing[0] + facing[2] * facing[2]) : 0.0f;
            const float toward =
                facing_length > 1e-3f ? (facing[0] * door.out[0] + facing[2] * door.out[2]) / facing_length : 0.0f;
            if (ahead < 70.0f && ahead > -40.0f && std::fabs(across) < width * 0.5f && toward > 0.6f) {
                g_walking_through = true;
                MapJump(g_last_town, -1);
            }
        }
    }

    for (int i = 0; i < 3; i++) g_last_position[i] = player_position[i];
    g_last_camera = mgRenderInfo;
    g_have_last = true;
    (IsDungeonMap(MapNo) ? g_last_dungeon : g_last_town) = MapNo;

    if (g_settling >= 0 && g_settling == MapNo && !moonmod::ChronicleHoldingPicture()) {
        const float moved = std::fabs(player_position[0] - g_settle_position[0]) +
                            std::fabs(player_position[2] - g_settle_position[2]);
        // the party stands at the world's origin until a floor's walk-in places it
        const bool placed = std::fabs(player_position[0]) + std::fabs(player_position[2]) > 1.0f;
        g_still_frames = placed && moved < 0.5f ? g_still_frames + 1 : 0;
        float forward[3];
        const float facing_length = facing != nullptr ? std::sqrt(facing[0] * facing[0] + facing[2] * facing[2]) : 0.0f;
            if (placed && facing_length > 1e-3f) {
            // the way out is behind the party as it stands, walked in
            g_walk_out[0] = -facing[0] / facing_length;
            g_walk_out[1] = 0.0f;
            g_walk_out[2] = -facing[2] / facing_length;
            g_have_walk_out = true;
        }
        for (int i = 0; i < 3; i++) g_settle_position[i] = player_position[i];
        if (g_still_frames >= 20 && g_have_walk_out) {
            for (int i = 0; i < 3; i++) forward[i] = g_walk_out[i];
            for (auto &[key, doorway] : g_doorways) {
                if (key % 1000 != MapNo) continue;
                PlaceCave(doorway.dungeon, player_position, forward);
            }
            g_settling = -1;
        } else if (++g_settle_frames > 1200) {
            g_settling = -1;
        }
    }

    for (auto &[key, doorway] : g_doorways) {
        const int town = key / 1000;
        const int dungeon = key % 1000;
        if (MapNo != town && MapNo != dungeon) continue;
        const Side &here = MapNo == town ? doorway.town : doorway.dungeon;
        const Side &there = MapNo == town ? doorway.dungeon : doorway.town;
        if (g_live_side == &here) {
            DrawWindow(here, there, true);
        } else if (PicturesOn() && here.placed && there.placed && there.pictured) {
            DrawWindow(here, there, false);
        }
    }
    g_live_side = nullptr;
}

void DoorwayLive() {
    g_live_side = nullptr;
    if (!moonmod::ChronicleQuickPassageOn() || PicturesOn() || moonmod::ChronicleHoldingPicture()) return;
    const bool in_dungeon = IsDungeonMap(MapNo);
    const int  other = in_dungeon ? g_last_town : g_last_dungeon;
    if (other < 0) return;
    const int key = in_dungeon ? other * 1000 + MapNo : MapNo * 1000 + other;
    auto      it = g_doorways.find(key);
    if (it == g_doorways.end()) return;
    const Side &here = in_dungeon ? it->second.dungeon : it->second.town;
    const Side &there = in_dungeon ? it->second.town : it->second.dungeon;
    if (!here.placed || !there.placed) return;
    // the other place is kept (its world, its textures) only while connected worlds are on
    const int world = moonmod::ChronicleConnectedWorld(other);
    if (world == 0 || !TextureManagerKept(world)) return;
    if (DrawLive(here, there, in_dungeon)) g_live_side = &here;
}

bool DoorwayArrival(float position[3], float &heading) {
    if (g_arriving <= 0 || IsDungeonMap(MapNo)) return false;
    g_arriving--;
    for (int i = 0; i < 3; i++) position[i] = g_arrive_at[i];
    heading = g_arrive_heading;
    return true;
}

bool DoorwayEnteringKept(int map) {
    return g_entering >= 0 && map == g_entering;
}

bool DoorwayDungeonArrival(float position[3], float &heading) {
    if (g_entering < 0) return false;
    g_entering = -1;
    for (int i = 0; i < 3; i++) position[i] = g_enter_at[i];
    heading = g_enter_heading;
    return true;
}

bool DoorwayWalkingInto(int map) {
    return g_arriving > 0 && map == g_last_town && !IsDungeonMap(map);
}

void DoorwayReset() {
    g_doorways.clear();
    g_leaving = g_leaving_to = -1;
    g_settling = -1;
    g_have_last = false;
    g_last_town = g_last_dungeon = -1;
    g_live_side = nullptr;
    g_walking_through = false;
    g_arriving = 0;
    g_entering = -1;
}

} // namespace dc::game

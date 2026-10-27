#pragma once

#include "common.h"

#include <libvu0.h>

#include <array>
#include <cstdint>
#include <span>
#include <vector>

#include "dataalloc.hpp"
#include "mdt.hpp"
#include "visualvu1.hpp"

namespace dc::game {

class CBound;
class CFrame;

/**
 * Simulates and draws a fixed-capacity grid of cloth vertices.
 */
class CCloth : public CVisualVu1 {
public:
    int           unk_10;
    int           unk_14;
    u_int        *vu_data;               /**< VU packet the base class draws, pointed at this frame's buffer. */
    u_int         vu_size;               /**< Size of one VU packet buffer in quadwords. */
    int           init_20;               /**< Cleared when the parameters reset and never read. */
    u_int        *vu_block[2];           /**< VU packet data for each display buffer. */
    int           num_i;                 /**< Number of active grid rows. */
    int           num_j;                 /**< Number of active grid columns. */
    float         pitch;                 /**< Rest spacing between adjacent vertices. */
    int           stop;                  /**< Carries the vertices rigidly with the frame, without simulation, while set. */
    CFrame       *frame;                 /**< Frame from which the cloth hangs. */
    int           init_40;               /**< Cleared when the parameters reset and never read. */
    CBound       *bound;                 /**< First exclusion box in the chain the vertices are pushed out of. */
    int           floor_on;              /**< Whether the vertices stop at the floor height. */
    float         floor_y;               /**< Height of the floor the vertices rest on. */
    void         *wind;                  /**< Wind whose noise pushes the cloth, or null. */
    float         wind_effect;           /**< Scale applied to the wind's push. */
    float         normal_scale;          /**< Scale applied to each rebuilt normal before it is normalised. */
    MDT_MATERIAL  material;              /**< Material the cloth draws with, taken from its model's mesh. */
    sceVu0FVECTOR gravity;               /**< Pull added to every vertex's speed each step. */
    sceVu0FVECTOR follow;                /**< Share of the frame's movement each axis passes on to the vertices. */
    sceVu0FVECTOR stiffness;             /**< Strength of the spring pulling each vertex towards its rest position on the frame. */
    sceVu0FVECTOR last_position;         /**< World position of the anchor on the previous step. */
    sceVu0FVECTOR position;              /**< Anchor point in the frame's space: the average rest position of the first row. */
    sceVu0FVECTOR home[16][16];          /**< Rest position of each vertex in the frame's space. */
    sceVu0FVECTOR point[16][16];         /**< World position of each vertex. */
    sceVu0FVECTOR last[16][16];          /**< World position of each vertex on the previous step. */
    sceVu0FVECTOR rest[16][16];          /**< Rest lengths to the neighbours in x and y, and in w the friction of the boxes touched, or -1. */
    sceVu0FVECTOR speed[16][16];         /**< Velocity of each vertex. */
    sceVu0FVECTOR normal_grid[16][16];   /**< Normal of each vertex. */
    sceVu0FVECTOR texture_coord[16][16]; /**< Texture coordinate of each vertex. */
    int           polygon_divide[16];    /**< Whether each row draws its strip from the far side. */
    int           mask[16][16];          /**< Exclusion boxes, by mask bit, that each vertex collides with. */
    sceVu0FVECTOR world_home[16][16];    /**< Rest position of each vertex carried into world space by the frame this step. */

    /**
     * Draws the simulated cloth through a temporary world-space frame.
     */
    void Draw();

    /**
     * Puts every vertex back where its frame says it rests and stops it moving.
     */
    void Clear();

    /**
     * Runs one step of the cloth simulation against the frame, floor, bounds and wind.
     */
    void Step(int step);

    /**
     * Builds the packet that draws the cloth and returns its size in quadwords.
     */
    int CreateVUData(u_int *packet);

    /**
     * Gives the cloth its default grid size, stiffness and gravity.
     */
    void InitParam();

    /**
     * Constructs a cloth grid with the requested dimensions and spacing.
     */
    CCloth(int grid_i = 16, int grid_j = 16, float grid_pitch = 1.0f);

    /**
     * Takes the vertex packet storage out of an arena and records rest lengths.
     */
    virtual void Initialize(CDataAlloc2<1> *alloc);

    /**
     * Builds the grid from a model's mesh, then initialises from an arena.
     */
    virtual void Initialize(MDT_HEADER *header, CDataAlloc2<1> *alloc);

    /**
     * Rebuilds the cloth's packet for this frame and draws it through the vector unit.
     */
    virtual int DrawVu1(u_int *packet, float (*matrix)[4], RenderInfo *info, VU1_PROGRAM program, u_long128 *draw_state, int unknown1, int unknown2);

    /**
     * Draws the cloth into a VIF packet.
     */
    virtual int DrawVu1(sceVif1Packet *packet, float (*matrix)[4], RenderInfo *info, VU1_PROGRAM program, u_long128 *draw_state, int unknown1, int unknown2);
};

/**
 * The Options screen's Enhanced cloth pushes the cloth out of the body as well as the .clo files'
 * boxes. The body is the skeleton as capsules.
 */
namespace cloth_body {

constexpr int kTorsoSectors = 12; /**< How many directions round its axis a torso's shape is measured in. */
constexpr int kTorsoBands = 6;    /**< How many heights along its axis a torso's shape is measured at. */

/**
 * A capsule of the body, from a to b, radius thick.
 */
struct Capsule {
    std::array<float, 3> a;
    std::array<float, 3> b;
    float                radius;
    bool                 torso = false; /**< Keeps the cloth off itself, and stops the limbs pressing the cloth into it. */
    /**
     * A torso fitted to the body's mesh (FitTorsos): how far its surface lies from the axis, by direction
     * round it (measured from `side`, a direction square to the axis that turns with the body) and by
     * height from a to b. Unfitted, the torso is round, `radius` thick.
     */
    std::array<float, 3>                           side = {};
    std::array<float, kTorsoSectors * kTorsoBands> shape = {};
    bool                                           fitted = false;
};

/**
 * How far the capsule's surface lies from its axis in the direction of `p`.
 */
float Reach(const Capsule &capsule, const float *p);

/**
 * The body's mesh: its vertices, in world space, and its triangles, three vertices each.
 */
struct Mesh {
    std::vector<std::array<float, 3>>         vertices;
    std::vector<std::array<std::uint32_t, 3>> triangles;
};

/**
 * Fits the torsos among `capsules` to `body` as it stands: how far out from the axis each direction meets
 * the torso's faces. A vertex belongs to the capsule whose surface it lies nearest, `others` (the head,
 * the hands, the feet) among them, so arms and hands held against the torso don't fatten it. A round
 * capsule sized from the spine's length stood out behind Toan's back, his chest being deeper than it.
 */
void FitTorsos(std::span<Capsule> capsules, std::span<const Capsule> others, const Mesh &body);

/**
 * How near a limb's axis a vertex may come: the limb's thickness, or less where the cloth rests against
 * the limb, as near as `home` (where the cloth's rest shape puts the vertex when it settles on the body)
 * lies to it then. A vertex resting against an arm isn't pushed off it, and an arm raised through where
 * the cloth rests still pushes it.
 */
float Clearance(const Capsule &limb, const float *home);

/**
 * Pushes a free vertex out of `capsules`, each limb to its `clearances` entry (Clearance, taken when the
 * cloth settled), the torso to its surface, or as far out as `home` lies if that is inside it. A limb
 * pressing the vertex towards the torso stops it at the torso's surface, or where it rests if that is
 * nearer.
 */
void PushOutOfBody(std::span<const Capsule> capsules, std::span<const float> clearances, const float *home, float *vertex);

} // namespace cloth_body

} // namespace dc::game

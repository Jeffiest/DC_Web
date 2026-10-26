// What mesh.vert and mesh_prog0.vert share: gfx::MeshConstants records and the fragment inputs.

enable clip_distances; // @clip

// gfx::MeshConstants. Colours are modulations with 1.0 as GS 0x80. Each chunk's records form one array; the
// draw's record is at pc.mesh_index.
struct MeshConstantsData {
    mvp: mat4x4f,
    normal_matrix: mat3x3f,
    light_direction: array<vec4f, 4>,
    light_color: array<vec4f, 4>,
    ambient: vec4f,
    diffuse: vec4f,
    ambient_material: vec4f,
    specular: vec4f,
    fog: vec4f,
    clip_plane: array<vec4f, 2>,
    world_uv: array<vec4f, 2>,
    flags: u32,
    light_count: u32,
}

@group(1) @binding(0) var<storage, read> meshes: array<MeshConstantsData>;

// A byte colour (0..255 as 0..1) read as a modulation with 0x80 as 1.0.
const kModulate: f32 = 255.0 / 128.0;

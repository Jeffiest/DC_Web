// mesh.vert in WGSL.

struct In {
    @location(0) position: vec3f,
    @location(1) normal: vec3f,
    @location(2) uv: vec2f,
    @location(3) color: vec4f,
}

struct Out {
    @builtin(position) position: vec4f,
    @builtin(clip_distances) clip_distances: array<f32, 2>, // @clip
    @location(4) clip_low: vec4f, // @noclip
    @location(5) clip_high: vec4f, // @noclip
    @location(0) color: vec4f,
    @location(1) uv: vec2f,
    @location(2) fog: vec4f,
    @location(3) @interpolate(flat) fog_mode: vec4f,
}

const kLit: u32 = 1u;
const kVertexColor: u32 = 2u;
const kFog: u32 = 4u;
const kShadow: u32 = 8u;
const kClip0: u32 = 16u;
const kClip1: u32 = 32u;
const kScreenUv: u32 = 64u;
const kWorldUv: u32 = 128u;

@vertex
fn main(v: In) -> Out {
    let mc = &meshes[pc.mesh_index];
    let flags = (*mc).flags;
    let position = vec4f(v.position, 1.0);
    var clip = (*mc).mvp * position;
    // The GS rasterises vertices at 1/16-pixel precision: quantise in logical-pixel units before viewport
    // conversion, so separate cells reaching one screen edge through different transforms meet there.
    let logical_pixel_ndc = pc.xform.xy;
    // At the eye plane keep the homogeneous position: perspective division is undefined.
    if (clip.w != 0.0) {
        var ndc = clip.xy / clip.w;
        ndc = round(ndc / (logical_pixel_ndc / 16.0)) * (logical_pixel_ndc / 16.0);
        clip = vec4f(ndc * clip.w, clip.z, clip.w);
    }
    var o: Out;
    o.position = ToWebGpu(clip);

    let clip0 = select(1.0, dot((*mc).clip_plane[0], position), (flags & kClip0) != 0u);
    let clip1 = select(1.0, dot((*mc).clip_plane[1], position), (flags & kClip1) != 0u);
    o.clip_distances = array<f32, 2>(clip0, clip1); // @clip
    o.clip_low = vec4f(clip0, clip1, 1.0, 1.0); // @noclip
    o.clip_high = vec4f(1.0); // @noclip

    var color = (*mc).diffuse.rgb;
    var alpha = (*mc).diffuse.a;
    if ((flags & kShadow) == 0u) {
        if ((flags & kLit) != 0u) {
            let normal = normalize((*mc).normal_matrix * v.normal);
            var light = vec3f(0.0);
            for (var i = 0u; i < min((*mc).light_count, 4u); i++) {
                light += max(dot(normal, -(*mc).light_direction[i].xyz), 0.0) * (*mc).light_color[i].rgb;
            }
            color = (*mc).ambient.rgb * (*mc).ambient_material.rgb + light * (*mc).diffuse.rgb;
        }
        if ((flags & kVertexColor) != 0u) {
            color *= v.color.rgb * kModulate;
            alpha *= v.color.a * kModulate;
        }
    }

    // Back to GS bytes, saturated as VU1 saturates its colour output.
    o.color = clamp(vec4f(color, alpha) / kModulate, vec4f(0.0), vec4f(1.0));
    if ((flags & kWorldUv) != 0u) {
        o.uv = vec2f(dot((*mc).world_uv[0], position), dot((*mc).world_uv[1], position));
    } else if ((flags & kScreenUv) != 0u) {
        o.uv = clip.xy / clip.w * 0.5 + 0.5 + v.uv;
    } else {
        o.uv = v.uv;
    }
    let mfog = (*mc).fog;
    o.fog_mode = vec4f(mfog.w, mfog.z, 0.0, 1.0);
    if ((flags & kFog) == 0u) {
        o.fog = vec4f(1.0, 0.0, 0.0, 0.0);
        o.fog_mode.z = -2.0;
    } else if (mfog.y == 0.0) {
        o.fog = vec4f(0.0);
        o.fog_mode.w = min(max(mfog.x, mfog.z), mfog.w) / 255.0;
        o.fog_mode.z = -2.0;
    } else {
        // Reverse-Z: only near/eye crossings need fresh fog; far-plane vertices retain their F. The full
        // homogeneous norm keeps the weight continuous at z=w=0 with nonzero x/y.
        let clip_scale = max(max(abs(clip.x), abs(clip.y)), max(abs(clip.z), abs(clip.w)));
        let marker = select(0.0, min((clip.z - clip.w) / clip_scale, 1.0), clip.z > clip.w);
        let fog = select(min(max(mfog.x + mfog.y / clip.w, mfog.z), mfog.w), 0.0, clip.w <= 0.0);
        let weight = max(clip.w, 0.0) * (1.0 - marker);
        o.fog = vec4f(fog * weight, mfog.x * clip.w + mfog.y, marker, weight);
    }
    return o;
}

// draw_body.glsl with KZ_BOUND_TEXTURES in WGSL: draw_bound.frag (dual source, the lines marked @dual) and
// draw_bound_single.frag (KZ_SINGLE_SOURCE, the lines marked @single). embed_wgsl.py makes the two.
//
// The GS pixel pipeline for both 2D and mesh draws: texture function (MODULATE), TEXA, CLUT, fog, alpha
// test, and the source side of the alpha blend. The blend itself is fixed-function.

enable dual_source_blending; // @dual

@id(0) override kTextureMode: i32 = 0; // 0 none, 1 RGBA, 2 index + palette
@id(1) override kAlphaTest: bool = false;
@id(2) override kColorFeatures: i32 = -1; // fog | TEXA | AEM flags, or -1 for pc.flags
@id(3) override kAlphaFunction: i32 = -1; // fixed alpha test function, or -1 for pc.alpha
@id(4) override kSimpleColor: bool = false;
@id(5) override kAlphaReference: i32 = -1;
@id(6) override kPlainMeshFog: bool = false;
@id(10) override kNearSafeMeshFog: bool = false;
@id(11) override kNearSafeReciprocal: bool = false;

@group(0) @binding(0) var u_texture: texture_2d<f32>;
@group(0) @binding(1) var u_palette: texture_2d<f32>;
@group(0) @binding(2) var u_sampler: sampler;
@group(0) @binding(3) var u_palette_sampler: sampler;

struct In {
    @builtin(position) frag_coord: vec4f,
    @location(0) color: vec4f,
    @location(1) uv: vec2f,
    // xw: weighted vertex F*w and weight*w; y: unlimited F*w; z: near/eye blend weight.
    @location(2) fog: vec4f,
    // xy: near/far limits; z: -2 constant, -1 direct 2D, 0 DC1 mesh, 1 DC2 prog0; w: constant F.
    @location(3) @interpolate(flat) fog_mode: vec4f,
    // Without WebGPU's clip distances, the vertex shader's, which clip where any is negative.
    @location(4) clip_low: vec4f, // @noclip
    @location(5) clip_high: vec4f, // @noclip
}

struct Out {
    @location(0) @blend_src(0) color: vec4f, // @dual
    @location(0) @blend_src(1) factor: vec4f, // @dual
    @location(0) color: vec4f, // @single
}

const kFog: u32 = 1u;
const kTexa: u32 = 2u;
const kAem: u32 = 4u;
const kOpaqueTarget: u32 = 8u;
const kTransformShift: u32 = 4u;
const kFactorFix: u32 = 64u;
const kLinear: u32 = 128u;

const kModulate: f32 = 255.0 / 128.0;

// textureGather returns texels (0,1), (1,1), (1,0), (0,0) of the 2x2 footprint.
fn Bilerp(gathered: vec4f, f: vec2f) -> f32 {
    return mix(mix(gathered.w, gathered.z, f.x), mix(gathered.x, gathered.y, f.x), f.y);
}

fn Footprint(uv: vec2f) -> vec2f {
    return fract(uv * vec2f(textureDimensions(u_texture, 0)) - 0.5);
}

// The footprint's shared corner, so the hardware's pick of the 2x2 agrees with Footprint().
fn FootprintCorner(uv: vec2f) -> vec2f {
    let size = vec2f(textureDimensions(u_texture, 0));
    return (floor(uv * size - 0.5) + 1.0) / size;
}

fn PaletteEntry(index: f32) -> vec4f {
    return textureLoad(u_palette, vec2i(i32(index * 255.0 + 0.5), 0), 0);
}

// The GS looks the CLUT up before filtering, so a filtered palette texture blends four entries.
fn SamplePalette(uv: vec2f) -> vec4f {
    // Both samples are taken in uniform control flow, as WGSL requires; the flag picks one.
    let nearest = PaletteEntry(textureSample(u_texture, u_sampler, uv).r);
    let index = textureGather(0, u_texture, u_sampler, FootprintCorner(uv));
    let f = Footprint(uv);
    let filtered = mix(mix(PaletteEntry(index.w), PaletteEntry(index.z), f.x),
                       mix(PaletteEntry(index.x), PaletteEntry(index.y), f.x), f.y);
    return select(filtered, nearest, (pc.flags & kLinear) == 0u);
}

fn ColorFlags() -> u32 {
    return select(u32(kColorFeatures), pc.flags, kColorFeatures < 0);
}

// TEXA expands alpha per texel before filtering too: with AEM a filtered edge between black and coloured
// texels fades out instead of taking TA0 throughout.
fn SampleRgba(uv: vec2f) -> vec4f {
    var t = textureSample(u_texture, u_sampler, uv);
    let corner = FootprintCorner(uv);
    let sum = textureGather(0, u_texture, u_sampler, corner) + textureGather(1, u_texture, u_sampler, corner) +
              textureGather(2, u_texture, u_sampler, corner);
    let flags = ColorFlags();
    if ((flags & kTexa) != 0u) {
        let ta0 = f32(pc.alpha >> 24u) / 128.0;
        let aem = (flags & kAem) != 0u;
        if (aem && (pc.flags & kLinear) != 0u) {
            t.a = Bilerp(vec4f(sum > vec4f(0.0)) * ta0, Footprint(uv));
        } else {
            t.a = select(ta0, 0.0, aem && t.r + t.g + t.b == 0.0);
        }
    }
    return t;
}

fn AlphaPasses(alpha: f32) -> bool {
    let ref_value = select(i32(pc.alpha & 0xFFu), kAlphaReference, kAlphaReference >= 0);
    let value = i32(alpha * 128.0 + 0.5);
    let func = select((pc.alpha >> 8u) & 7u, u32(kAlphaFunction), kAlphaFunction >= 0);
    switch (func) {
        case 0u: { return false; }
        case 1u: { return true; }
        case 2u: { return value < ref_value; }
        case 3u: { return value <= ref_value; }
        case 4u: { return value == ref_value; }
        case 5u: { return value >= ref_value; }
        case 6u: { return value > ref_value; }
        default: { return value != ref_value; }
    }
}

@fragment
fn main(v: In) -> Out {
    if (any(min(v.clip_low, v.clip_high) < vec4f(0.0))) { discard; } // @noclip
    // Alpha is carried in GS units over 128 (1.0 = 0x80) so the alpha test and blend factor see values above
    // 0x80 before the attachment saturates them.
    var color: vec4f;
    if (kTextureMode == 0) {
        color = vec4f(v.color.rgb, v.color.a * kModulate);
    } else {
        var t: vec4f;
        if (kTextureMode == 1) {
            t = SampleRgba(v.uv);
        } else {
            t = SamplePalette(v.uv);
        }
        color = vec4f(t.rgb * v.color.rgb * kModulate, t.a * v.color.a * kModulate);
    }

    if (kAlphaTest && !AlphaPasses(color.a)) {
        discard;
    }

    if ((ColorFlags() & kFog) != 0u) {
        var fog: f32;
        if (kNearSafeMeshFog) {
            fog = select(v.fog.x / v.fog.w, v.fog.x * v.frag_coord.w, kNearSafeReciprocal) / 255.0;
        } else {
            fog = select(v.fog.x, v.fog_mode.w, v.fog_mode.z < -1.5);
            if (v.fog_mode.z >= 0.0) {
                // Each edge depends only on its own endpoints, including after homogeneous clipping.
                var fresh = v.fog.y * v.frag_coord.w;
                let floor_mode = !kPlainMeshFog && v.fog_mode.z > 0.5;
                fresh = select(min(max(fresh, v.fog_mode.y), v.fog_mode.x),
                               max(max(min(fresh, v.fog_mode.x), v.fog_mode.y), 0.0), floor_mode);
                if (floor_mode) {
                    fresh = floor(fresh);
                }
                fog = select(fresh, mix(v.fog.x / v.fog.w, fresh, clamp(v.fog.z, 0.0, 1.0)), v.fog.w > 0.0);
                fog /= 255.0;
            }
        }
        color = vec4f(mix(unpack4x8unorm(pc.fog_color).rgb, color.rgb, fog), color.a);
    }

    // Keep the original coverage, alpha test and blending. Only presentation RGB is tinted.
    let status = (pc.flags >> 8u) & 7u;
    if (!kSimpleColor && status != 0u) {
        var tint = vec3f(0.1, 1.0, 1.0);
        if (status == 1u) {
            tint = vec3f(0.1, 1.0, 0.1);
        } else if (status == 2u) {
            tint = vec3f(1.0, 0.1, 0.1);
        } else if (status == 3u) {
            tint = vec3f(0.1, 0.2, 1.0);
        } else if (status == 4u) {
            tint = vec3f(1.0, 1.0, 0.1);
        }
        color = vec4f(mix(color.rgb, tint, 0.8), color.a);
    }

    let factor = select(color.a, f32((pc.alpha >> 16u) & 0xFFu) / 128.0, (pc.flags & kFactorFix) != 0u);
    let transform = select((pc.flags >> kTransformShift) & 3u, 0u, kSimpleColor);
    var rgb = color.rgb;
    if (transform == 1u) {
        rgb = color.rgb * (1.0 + factor);
    } else if (transform == 2u) {
        rgb = vec3f(factor);
    }
    var o: Out;
    o.color = vec4f(rgb, select(color.a, 1.0, (pc.flags & kOpaqueTarget) != 0u));
    o.factor = vec4f(factor); // @dual
    return o;
}

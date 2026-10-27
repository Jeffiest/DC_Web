// Vulkan on WebGL2 (layer.hpp): the renderer's shaders in GLSL ES 3.00 and the programs linked from them.
//
// The ports follow the renderer's GLSL (gfx/shaders) as Vulkan on WebGPU's WGSL does (port/web/vulkan/shaders),
// with what GLSL ES 3.00 lacks stood in for:
// - Push constants and the storage buffers' records are uniform blocks: each draw's records are copied into them
//   (commands.cpp), so the record a draw reads is the block itself rather than an element of an array.
// - Specialisation constants are #defines put before the source.
// - Clip distances are carried to the fragment shader, which discards where any is negative, as WGSL's _noclip
//   variants do.
// - Separate textures and samplers are one sampler2D per texture unit; textureGather is four nearest samples of the
//   base level, from a unit holding the texture with its sampler's nearest twin.
// - Vulkan's clip-space depth runs 0 to w and GL's -w to w: each vertex shader ends by moving it.
// - Dual-source blending is drawn from one source (objects.cpp), so both of draw_bound's variants are the
//   single-source one.

#include "layer.hpp"

#include <cstdio>
#include <map>
#include <string>
#include <tuple>

namespace vkgl {

namespace {

constexpr const char *kPrelude = R"glsl(#version 300 es
precision highp float;
precision highp int;
precision highp sampler2D;
)glsl";

// gfx::detail::PushConstants, which every shader reads.
constexpr const char *kPush = R"glsl(
layout(std140) uniform Push {
    vec4 xform;
    vec4 uv_xform;
    uint texture_slot;
    uint palette_slot;
    uint sampler_slot;
    uint flags;
    uint alpha; // ref 0-7, func 8-10, fix 16-23, ta0 24-31
    uint fog_color;
    uint face;
    uint mesh_index;
    uint prog0_index;
    uint palette_index;
} pc;

// Vulkan's clip-space depth, 0 to w, as GL's, -w to w.
vec4 ToGl(vec4 clip) {
    return vec4(clip.xy, 2.0 * clip.z - clip.w, clip.w);
}
)glsl";

// What the vertex shaders hand the fragment shader.
constexpr const char *kVertexOut = R"glsl(
out vec4 v_color;
out vec2 v_uv;
out vec4 v_fog;
flat out vec4 v_fog_mode;
out vec4 v_clip_low;
out vec4 v_clip_high;
)glsl";

constexpr const char *kDraw2d = R"glsl(
// draw2d.vert. Logical 2D: x, y in the target's logical space, z the depth (1 near, 0 far).
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec2 a_uv;
layout(location = 2) in vec4 a_color;
layout(location = 3) in float a_fog;

void main() {
    gl_Position = ToGl(vec4(a_position.xy * pc.xform.xy + pc.xform.zw, a_position.z, 1.0));
    v_color = a_color;
    v_uv = a_uv * pc.uv_xform.xy + pc.uv_xform.zw;
    v_fog = vec4(a_fog, 0.0, 0.0, 0.0);
    v_fog_mode = vec4(0.0, 0.0, -1.0, 0.0);
    v_clip_low = vec4(1.0);
    v_clip_high = vec4(1.0);
}
)glsl";

// gfx::MeshConstants, which mesh.vert and mesh_prog0.vert both read. Colours are modulations with 1.0 as GS 0x80.
constexpr const char *kMeshCommon = R"glsl(
struct MeshConstantsData {
    mat4 mvp;
    mat3 normal_matrix;
    vec4 light_direction[4];
    vec4 light_color[4];
    vec4 ambient;
    vec4 diffuse;
    vec4 ambient_material;
    vec4 specular;
    vec4 fog;
    vec4 clip_plane[2];
    vec4 world_uv[2];
    uint flags;
    uint light_count;
};

layout(std140) uniform MeshBlock {
    MeshConstantsData mc;
};
)glsl";

constexpr const char *kMesh = R"glsl(
// mesh.vert.
layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec4 a_color;

const uint kLit         = 1u;
const uint kVertexColor = 2u;
const uint kFog         = 4u;
const uint kShadow      = 8u;
const uint kClip0       = 16u;
const uint kClip1       = 32u;
const uint kScreenUv    = 64u;
const uint kWorldUv     = 128u;

// A byte colour (0..255 as 0..1) read as a modulation with 0x80 as 1.0.
const float kModulate = 255.0 / 128.0;

void main() {
    uint flags = mc.flags;
    vec4 position = vec4(a_position, 1.0);
    vec4 clip     = mc.mvp * position;
    // The GS rasterises vertices at 1/16-pixel precision: quantise in logical-pixel units before viewport
    // conversion, so separate cells reaching one screen edge through different transforms meet there.
    vec2 logical_pixel_ndc = pc.xform.xy;
    // At the eye plane keep the homogeneous position: perspective division is undefined.
    if (clip.w != 0.0) {
        vec2 ndc = clip.xy / clip.w;
        ndc = round(ndc / (logical_pixel_ndc / 16.0)) * (logical_pixel_ndc / 16.0);
        clip.xy = ndc * clip.w;
    }
    gl_Position = ToGl(clip);

    float clip0 = (flags & kClip0) != 0u ? dot(mc.clip_plane[0], position) : 1.0;
    float clip1 = (flags & kClip1) != 0u ? dot(mc.clip_plane[1], position) : 1.0;
    v_clip_low = vec4(clip0, clip1, 1.0, 1.0);
    v_clip_high = vec4(1.0);

    vec3  color = mc.diffuse.rgb;
    float alpha = mc.diffuse.a;
    if ((flags & kShadow) == 0u) {
        if ((flags & kLit) != 0u) {
            vec3 normal = normalize(mc.normal_matrix * a_normal);
            vec3 light  = vec3(0.0);
            uint count = min(mc.light_count, 4u);
            for (uint i = 0u; i < count; i++) {
                light += max(dot(normal, -mc.light_direction[i].xyz), 0.0) * mc.light_color[i].rgb;
            }
            color = mc.ambient.rgb * mc.ambient_material.rgb + light * mc.diffuse.rgb;
        }
        if ((flags & kVertexColor) != 0u) {
            color *= a_color.rgb * kModulate;
            alpha *= a_color.a * kModulate;
        }
    }

    // Back to GS bytes, saturated as VU1 saturates its colour output.
    v_color = clamp(vec4(color, alpha) / kModulate, 0.0, 1.0);
    v_uv    = (flags & kWorldUv) != 0u ? vec2(dot(mc.world_uv[0], position), dot(mc.world_uv[1], position)) :
              (flags & kScreenUv) != 0u ? clip.xy / clip.w * 0.5 + 0.5 + a_uv : a_uv;
    v_fog_mode = vec4(mc.fog.w, mc.fog.z, 0.0, 1.0);
    if ((flags & kFog) == 0u) {
        v_fog = vec4(1.0, 0.0, 0.0, 0.0);
        v_fog_mode.z = -2.0;
    } else if (mc.fog.y == 0.0) {
        v_fog = vec4(0.0);
        v_fog_mode.w = min(max(mc.fog.x, mc.fog.z), mc.fog.w) / 255.0;
        v_fog_mode.z = -2.0;
    } else {
        // Reverse-Z: only near/eye crossings need fresh fog; far-plane vertices retain their F.
        // The full homogeneous norm keeps the weight continuous at z=w=0 with nonzero x/y.
        float clip_scale = max(max(abs(clip.x), abs(clip.y)), max(abs(clip.z), abs(clip.w)));
        float marker = clip.z > clip.w ? min((clip.z - clip.w) / clip_scale, 1.0) : 0.0;
        float fog = clip.w <= 0.0 ? 0.0 : min(max(mc.fog.x + mc.fog.y / clip.w, mc.fog.z), mc.fog.w);
        float weight = max(clip.w, 0.0) * (1.0 - marker);
        v_fog = vec4(fog * weight, mc.fog.x * clip.w + mc.fog.y, marker, weight);
    }
}
)glsl";

constexpr const char *kMeshProg0 = R"glsl(
// mesh_prog0.vert: Dark Chronicle's Vu_prog0, for DrawMeshProg0 draws. Spec: docs/vu1/prog0.md and progmain.md in
// the game's port; the O-n notes below are that spec's open items, kept as retail does them.
#ifndef SC_0
#define SC_0 0
#endif
const bool kSkinned = SC_0 != 0;

layout(location = 0) in vec3 a_position;
layout(location = 1) in vec3 a_normal;
layout(location = 2) in vec2 a_uv;
layout(location = 3) in vec4 a_color;
layout(location = 4) in vec4 a_skin_weight;
layout(location = 5) in uvec4 a_skin_bone;

// gfx::Prog0Constants in the VU's units (128 is 1.0).
struct Prog0Data {
    mat4 model_world;
    vec4 light_direction[3];
    vec4 light_color[4];
    vec4 ambient;
    vec4 object_color;
    vec4 eye;
    mat4 clip_or_view;
    vec4 diffuse;
    vec4 specular;
    vec4 point_light[4];
    vec4 point_color[4];
    vec4 fog;
    uint flags;
    uint pad0;
    uint specular_power;
    uint pad;
};

layout(std140) uniform Prog0Block {
    Prog0Data p0;
};

// gfx::Prog0Palette of a skinned draw.
layout(std140) uniform PaletteBlock {
    vec4 bones[96];
} palette;

const uint kScissor      = 0x002u;
const uint kReal         = 0x008u;
const uint kPointLights  = 0x010u;
const uint kUnlit        = 0x020u;
const uint kBlend        = 0x040u;
const uint kColorAdd     = 0x080u;
const uint kRefSphere    = 0x100u;

const uint kFaceColor    = 0x100u;
const uint kFaceNoNormal = 0x200u;

// A vertex colour byte of 0x80 is the VU's 1.0.
const float kModulate = 255.0 / 128.0;
const float kVuMax    = 3.40282347e38;

// VU rsqrt(0) is a finite maximum; anything scaled by it is zero.
float vu_rsqrt(float s) {
    return s > 0.0 ? inversesqrt(s) : 0.0;
}

vec3 vu_unit(vec3 v) {
    return v * vu_rsqrt(dot(v, v));
}

void blend(inout vec3 position, inout vec3 normal) {
    vec3 P = vec3(0.0);
    vec3 N = vec3(0.0);
    for (int i = 0; i < 4; i++) {
        float w = a_skin_weight[i];
        uint b = min(a_skin_bone[i], 31u) * 3u;
        vec4 r0 = palette.bones[b];
        vec4 r1 = palette.bones[b + 1u];
        vec4 r2 = palette.bones[b + 2u];

        vec4 pw = vec4(position * w, w);
        P += vec3(dot(r0, pw), dot(r1, pw), dot(r2, pw));

        vec3 nw = normal * w;
        vec3 c0 = vec3(r0.x, r1.x, r2.x);
        vec3 c1 = vec3(r0.y, r1.y, r2.y);
        vec3 c2 = vec3(r0.z, r1.z, r2.z);
        // O-8: retail reads column 1 for the z term of influence 3 (0x013E).
        N += c0 * nw.x + c1 * nw.y + (i == 3 ? c1 : c2) * nw.z;
    }
    position = P;
    normal = N;
}

void main() {
    uint F = p0.flags;
    uint T = pc.face;

    vec3 position = a_position;
    vec3 normal = a_normal;
    vec2 uv = a_uv;

    if ((F & kRefSphere) != 0u) {
        // O-9: env UV is taken from the unblended normal, before VERTEX_BLEND, as retail.
        vec3 nh = vu_unit(mat3(p0.clip_or_view) * a_normal);
        uv = (nh.xy + 1.0) * 0.5;
    }

    if (kSkinned && (F & kBlend) != 0u) {
        blend(position, normal);
    }

    mat3 W = mat3(p0.model_world);
    vec3 L[4];
    // progmain's MSCAL 0 setup: world directions into model space, each normalised.
    for (int c = 0; c < 4; c++) {
        vec3 lw = vec3(p0.light_direction[0][c], p0.light_direction[1][c], p0.light_direction[2][c]);
        L[c] = vu_unit(vec3(dot(lw, W[0]), dot(lw, W[1]), dot(lw, W[2])));
    }

    vec4 D = p0.diffuse;
    vec4 color;
    if ((F & kUnlit) != 0u || (T & kFaceNoNormal) != 0u) {
        color = min(p0.object_color * D, vec4(255.0));
    } else {
        color = p0.ambient * D;
        float s[4];
        for (int c = 0; c < 4; c++) {
            s[c] = max(0.0, dot(L[c], normal));
            color += p0.light_color[c] * D * s[c];
        }

        if ((F & kPointLights) != 0u) {
            vec3 pw = W * position;
            vec3 nw = W * normal;
            float inv_n = vu_rsqrt(dot(nw, nw));
            for (int k = 0; k < 4; k++) {
                vec3 d = p0.point_light[k].xyz - pw;
                float inv_d = vu_rsqrt(dot(d, d));
                float cosine = max(0.0, dot(d, nw)) * inv_d * inv_n;
                float a = p0.point_light[k].w * inv_d;
                // O-4: power enters squared, so a negative power acts as its magnitude.
                float att = min(1.0, a * a);
                color += p0.point_color[k] * D * (cosine * att);
            }
        } else if ((F & kReal) != 0u) {
            vec3 V = p0.eye.xyz - position;
            float inv_v = vu_rsqrt(dot(V, V));
            float vn = dot(V, normal);
            // Power 0 is the VU's 65536-pass loop; 32 passes reach the same saturated result.
            uint n = p0.specular_power == 0u ? 32u : min(p0.specular_power, 32u);
            for (int c = 0; c < 4; c++) {
                float lit = min(1.0, 1.0e4 * s[c]);
                float r = max(0.0, 2.0 * vn * dot(L[c], normal) - dot(L[c], V)) * inv_v;
                float x = r * r;
                for (uint j = 0u; j < n; j++) {
                    // Saturates at the VU's largest float; keeps lit * x finite.
                    x = min(x * x, kVuMax);
                }
                color += p0.light_color[c] * p0.specular * (lit * x);
            }
        }
        color = min(color, vec4(255.0));
    }

    if ((T & kFaceColor) != 0u) {
        vec4 vc = a_color * kModulate;
        if ((F & kColorAdd) != 0u) {
            // O-10: retail squares vertex alpha and drops the lit alpha.
            color.rgb = min(color.rgb + 128.0 * vc.rgb, vec3(255.0));
            color.a = min(128.0 * vc.a * vc.a, 255.0);
        } else {
            color = min(color * vc, vec4(255.0));
        }
    }

    vec4 clip = mc.mvp * vec4(position, 1.0);
    gl_Position = ToGl(clip);

    if ((F & kScissor) != 0u) {
        vec4 c = p0.clip_or_view * vec4(position, 1.0);
        v_clip_low = vec4(c.w - c.x, c.w + c.x, c.w - c.y, c.w + c.y);
        v_clip_high = vec4(c.w - c.z, c.w + c.z, 1.0, 1.0);
    } else {
        v_clip_low = vec4(1.0);
        v_clip_high = vec4(1.0);
    }

    // ftoi0 truncates the VU's colour channels.
    v_color = floor(clamp(color, 0.0, 255.0)) / 255.0;
    v_uv = uv;
    v_fog_mode = vec4(p0.fog.y, p0.fog.z, 1.0, 0.0);
    if (p0.fog.w == 0.0) {
        // A constant factor needs no perspective correction; clamp before truncating as VU1 does.
        float fog = floor(max(max(min(p0.fog.x, p0.fog.y), p0.fog.z), 0.0)) / 255.0;
        v_fog = vec4(fog, 0.0, 0.0, 0.0);
        v_fog_mode.z = -2.0;
        v_fog_mode.w = fog;
    } else {
        // Reverse-Z: only near/eye crossings need fresh fog; far-plane vertices retain their F.
        // The full homogeneous norm keeps the weight continuous at z=w=0 with nonzero x/y.
        float clip_scale = max(max(abs(clip.x), abs(clip.y)), max(abs(clip.z), abs(clip.w)));
        float marker = clip.z > clip.w ? min((clip.z - clip.w) / clip_scale, 1.0) : 0.0;
        float fog = clip.w <= 0.0 ? 0.0 : p0.fog.x + p0.fog.w / clip.w;
        fog = floor(max(max(min(fog, p0.fog.y), p0.fog.z), 0.0));
        // Retain weighted vertex F and its weight alongside the unlimited homogeneous numerator.
        float weight = max(clip.w, 0.0) * (1.0 - marker);
        v_fog = vec4(fog * weight, p0.fog.x * clip.w + p0.fog.w, marker, weight);
    }
}
)glsl";

constexpr const char *kDraw = R"glsl(
// draw_body.glsl with KZ_BOUND_TEXTURES and KZ_SINGLE_SOURCE: the GS pixel pipeline for both 2D and mesh draws,
// texture function (MODULATE), TEXA, CLUT, fog, alpha test, and the source side of the alpha blend. The blend
// itself is fixed-function.
#ifndef SC_0
#define SC_0 0
#endif
#ifndef SC_1
#define SC_1 0
#endif
#ifndef SC_2
#define SC_2 -1
#endif
#ifndef SC_3
#define SC_3 -1
#endif
#ifndef SC_4
#define SC_4 0
#endif
#ifndef SC_5
#define SC_5 -1
#endif
#ifndef SC_6
#define SC_6 0
#endif
#ifndef SC_10
#define SC_10 0
#endif
#ifndef SC_11
#define SC_11 0
#endif
const int  kTextureMode = SC_0;         // 0 none, 1 RGBA, 2 index + palette
const bool kAlphaTest = SC_1 != 0;
const int  kColorFeatures = SC_2;       // fog | TEXA | AEM flags, or -1 for pc.flags
const int  kAlphaFunction = SC_3;       // fixed alpha test function, or -1 for pc.alpha
const bool kSimpleColor = SC_4 != 0;
const int  kAlphaReference = SC_5;
const bool kPlainMeshFog = SC_6 != 0;
const bool kNearSafeMeshFog = SC_10 != 0;
const bool kNearSafeReciprocal = SC_11 != 0;

uniform sampler2D u_texture;
uniform sampler2D u_palette;
// u_texture's image with its sampler's nearest twin, for the footprint's single texels.
uniform sampler2D u_texture_nearest;

in vec4 v_color;
in vec2 v_uv;
// xw: weighted vertex F*w and weight*w; y: unlimited F*w; z: near/eye blend weight.
in vec4 v_fog;
// xy: near/far limits; z: -2 constant, -1 direct 2D, 0 DC1 mesh, 1 DC2 prog0; w: constant F.
flat in vec4 v_fog_mode;
// The vertex shader's clip distances, which clip where any is negative.
in vec4 v_clip_low;
in vec4 v_clip_high;

out vec4 o_color;

const uint kFog = 1u;
const uint kTexa = 2u;
const uint kAem = 4u;
const uint kOpaqueTarget = 8u;
const uint kTransformShift = 4u;
const uint kFactorFix = 64u;
const uint kLinear = 128u;

const float kModulate = 255.0 / 128.0;

// The 2x2 footprint a bilinear sample at uv reads, as textureGather returns it: texels (0,1), (1,1), (1,0), (0,0).
struct Footprint4 {
    vec4 t01;
    vec4 t11;
    vec4 t10;
    vec4 t00;
};

Footprint4 Gather(vec2 uv) {
    vec2 size = vec2(textureSize(u_texture, 0));
    vec2 base = floor(uv * size - 0.5) + 0.5;
    Footprint4 f;
    f.t00 = textureLod(u_texture_nearest, base / size, 0.0);
    f.t10 = textureLod(u_texture_nearest, (base + vec2(1.0, 0.0)) / size, 0.0);
    f.t01 = textureLod(u_texture_nearest, (base + vec2(0.0, 1.0)) / size, 0.0);
    f.t11 = textureLod(u_texture_nearest, (base + vec2(1.0, 1.0)) / size, 0.0);
    return f;
}

float Bilerp(vec4 gathered, vec2 f) {
    return mix(mix(gathered.w, gathered.z, f.x), mix(gathered.x, gathered.y, f.x), f.y);
}

vec2 Fraction(vec2 uv) {
    return fract(uv * vec2(textureSize(u_texture, 0)) - 0.5);
}

vec4 PaletteEntry(float index) {
    return texelFetch(u_palette, ivec2(int(index * 255.0 + 0.5), 0), 0);
}

// The GS looks the CLUT up before filtering, so a filtered palette texture blends four entries.
vec4 SamplePalette(vec2 uv) {
    vec4 nearest = PaletteEntry(texture(u_texture, uv).r);
    Footprint4 g = Gather(uv);
    vec2 f = Fraction(uv);
    vec4 filtered = mix(mix(PaletteEntry(g.t00.r), PaletteEntry(g.t10.r), f.x),
                        mix(PaletteEntry(g.t01.r), PaletteEntry(g.t11.r), f.x), f.y);
    return (pc.flags & kLinear) == 0u ? nearest : filtered;
}

// NaN and infinities as 0, read from the bits: what a GPU writes for them into an 8-bit target is its own.
vec4 Finite(vec4 v) {
    uvec4 exponent = floatBitsToUint(v) & uvec4(0x7f800000u);
    return mix(v, vec4(0.0), equal(exponent, uvec4(0x7f800000u)));
}

uint ColorFlags() {
    return kColorFeatures < 0 ? pc.flags : uint(kColorFeatures);
}

// TEXA expands alpha per texel before filtering too: with AEM a filtered edge between black and coloured
// texels fades out instead of taking TA0 throughout.
vec4 SampleRgba(vec2 uv) {
    vec4 t = texture(u_texture, uv);
    uint flags = ColorFlags();
    if ((flags & kTexa) != 0u) {
        float ta0 = float(pc.alpha >> 24u) / 128.0;
        bool aem = (flags & kAem) != 0u;
        if (aem && (pc.flags & kLinear) != 0u) {
            Footprint4 g = Gather(uv);
            vec4 sum = vec4(dot(g.t01.rgb, vec3(1.0)), dot(g.t11.rgb, vec3(1.0)), dot(g.t10.rgb, vec3(1.0)),
                            dot(g.t00.rgb, vec3(1.0)));
            t.a = Bilerp(vec4(greaterThan(sum, vec4(0.0))) * ta0, Fraction(uv));
        } else {
            t.a = aem && t.r + t.g + t.b == 0.0 ? 0.0 : ta0;
        }
    }
    return t;
}

bool AlphaPasses(float alpha) {
    int ref_value = kAlphaReference >= 0 ? kAlphaReference : int(pc.alpha & 0xFFu);
    int value = int(alpha * 128.0 + 0.5);
    uint func = kAlphaFunction >= 0 ? uint(kAlphaFunction) : (pc.alpha >> 8u) & 7u;
    switch (func) {
        case 0u: return false;
        case 1u: return true;
        case 2u: return value < ref_value;
        case 3u: return value <= ref_value;
        case 4u: return value == ref_value;
        case 5u: return value >= ref_value;
        case 6u: return value > ref_value;
        default: return value != ref_value;
    }
}

vec4 UnpackUnorm4x8(uint value) {
    return vec4(uvec4(value, value >> 8u, value >> 16u, value >> 24u) & uvec4(0xFFu)) / 255.0;
}

void main() {
    if (any(lessThan(min(v_clip_low, v_clip_high), vec4(0.0)))) {
        discard;
    }
    // Alpha is carried in GS units over 128 (1.0 = 0x80) so the alpha test and blend factor see values above
    // 0x80 before the attachment saturates them.
    vec4 color;
    if (kTextureMode == 0) {
        color = vec4(v_color.rgb, v_color.a * kModulate);
    } else {
        vec4 t = kTextureMode == 1 ? SampleRgba(v_uv) : SamplePalette(v_uv);
        color = vec4(t.rgb * v_color.rgb * kModulate, t.a * v_color.a * kModulate);
    }

    if (kAlphaTest && !AlphaPasses(color.a)) {
        discard;
    }

    if ((ColorFlags() & kFog) != 0u) {
        float fog;
        if (kNearSafeMeshFog) {
            fog = (kNearSafeReciprocal ? v_fog.x * gl_FragCoord.w : v_fog.x / v_fog.w) / 255.0;
        } else {
            fog = v_fog_mode.z < -1.5 ? v_fog_mode.w : v_fog.x;
            if (v_fog_mode.z >= 0.0) {
                // Each edge depends only on its own endpoints, including after homogeneous clipping.
                float fresh = v_fog.y * gl_FragCoord.w;
                bool floor_mode = !kPlainMeshFog && v_fog_mode.z > 0.5;
                fresh = floor_mode ? max(max(min(fresh, v_fog_mode.x), v_fog_mode.y), 0.0)
                                   : min(max(fresh, v_fog_mode.y), v_fog_mode.x);
                if (floor_mode) {
                    fresh = floor(fresh);
                }
                fog = v_fog.w > 0.0 ? mix(v_fog.x / v_fog.w, fresh, clamp(v_fog.z, 0.0, 1.0)) : fresh;
                fog /= 255.0;
            }
        }
        color = vec4(mix(UnpackUnorm4x8(pc.fog_color).rgb, color.rgb, fog), color.a);
    }

    // Keep the original coverage, alpha test and blending. Only presentation RGB is tinted.
    uint status = (pc.flags >> 8u) & 7u;
    if (!kSimpleColor && status != 0u) {
        vec3 tint = vec3(0.1, 1.0, 1.0);
        if (status == 1u) {
            tint = vec3(0.1, 1.0, 0.1);
        } else if (status == 2u) {
            tint = vec3(1.0, 0.1, 0.1);
        } else if (status == 3u) {
            tint = vec3(0.1, 0.2, 1.0);
        } else if (status == 4u) {
            tint = vec3(1.0, 1.0, 0.1);
        }
        color = vec4(mix(color.rgb, tint, 0.8), color.a);
    }

    float factor = (pc.flags & kFactorFix) != 0u ? float((pc.alpha >> 16u) & 0xFFu) / 128.0 : color.a;
    uint transform = kSimpleColor ? 0u : (pc.flags >> kTransformShift) & 3u;
    vec3 rgb = color.rgb;
    if (transform == 1u) {
        rgb = color.rgb * (1.0 + factor);
    } else if (transform == 2u) {
        rgb = vec3(factor);
    }
    o_color = Finite(vec4(rgb, (pc.flags & kOpaqueTarget) != 0u ? 1.0 : color.a));
}
)glsl";

// A triangle over the target.
constexpr const char *kFullscreenVertex = R"glsl(
void main() {
    vec2 corner = vec2(float((gl_VertexID << 1) & 2), float(gl_VertexID & 2));
    gl_Position = vec4(corner * 2.0 - 1.0, 0.0, 1.0);
}
)glsl";

// An image's depth as the bytes of the words Vulkan copies it out as: a float's bits for D32, 24-bit unorm for D24.
constexpr const char *kDepthAsColor = R"glsl(
uniform sampler2D u_depth;
uniform ivec2 u_origin;
uniform int u_unorm;
out vec4 o_color;

void main() {
    float depth = texelFetch(u_depth, ivec2(gl_FragCoord.xy) + u_origin, 0).r;
    uint word = u_unorm != 0 ? uint(round(clamp(depth, 0.0, 1.0) * 16777215.0)) : floatBitsToUint(depth);
    o_color = vec4(uvec4(word, word >> 8u, word >> 16u, word >> 24u) & uvec4(0xFFu)) / 255.0;
}
)glsl";

std::string Defines(const std::vector<std::pair<uint32_t, int32_t>> &constants) {
    std::string text;
    for (const auto &[id, value] : constants) {
        text += "#define SC_" + std::to_string(id) + " " + std::to_string(value) + "\n";
    }
    return text;
}

GLuint Compile(GLenum stage, const std::string &source, const char *name) {
    GLuint      shader = glCreateShader(stage);
    const char *text = source.c_str();
    glShaderSource(shader, 1, &text, nullptr);
    glCompileShader(shader);
    GLint ok = 0;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char  log[4096] = {};
        GLint length = 0;
        glGetShaderInfoLog(shader, sizeof log, &length, log);
        Warn("%s does not compile: %s", name, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

GLuint Link(const std::string &vertex, const std::string &fragment, const char *name) {
    GLuint vs = Compile(GL_VERTEX_SHADER, vertex, name);
    GLuint fs = Compile(GL_FRAGMENT_SHADER, fragment, name);
    if (vs == 0 || fs == 0) {
        glDeleteShader(vs);
        glDeleteShader(fs);
        return 0;
    }
    GLuint program = glCreateProgram();
    glAttachShader(program, vs);
    glAttachShader(program, fs);
    glLinkProgram(program);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = 0;
    glGetProgramiv(program, GL_LINK_STATUS, &ok);
    if (!ok) {
        char  log[4096] = {};
        GLint length = 0;
        glGetProgramInfoLog(program, sizeof log, &length, log);
        Warn("%s does not link: %s", name, log);
        glDeleteProgram(program);
        return 0;
    }
    return program;
}

void BindSampler(GLuint program, const char *name, GLint unit) {
    const GLint location = glGetUniformLocation(program, name);
    if (location >= 0) {
        glUniform1i(location, unit);
    }
}

std::map<std::string, Program *> g_programs;

} // namespace

Program *ProgramFor(const ShaderModule *vertex, const std::vector<std::pair<uint32_t, int32_t>> &vertex_constants,
                    const ShaderModule *fragment, const std::vector<std::pair<uint32_t, int32_t>> &fragment_constants) {
    const std::string vertex_defines = Defines(vertex_constants);
    const std::string fragment_defines = Defines(fragment_constants);
    const std::string key = std::string(vertex->name) + "|" + vertex_defines + "|" +
                            (fragment != nullptr ? fragment->name : "none") + "|" + fragment_defines;
    auto it = g_programs.find(key);
    if (it != g_programs.end()) {
        return it->second;
    }
    const std::string name = vertex->name;
    std::string       vertex_source = std::string(kPrelude) + vertex_defines + kPush + kVertexOut;
    if (name == "draw2d_vert") {
        vertex_source += kDraw2d;
    } else if (name == "mesh_vert") {
        vertex_source += std::string(kMeshCommon) + kMesh;
    } else if (name == "mesh_prog0_vert") {
        vertex_source += std::string(kMeshCommon) + kMeshProg0;
    } else {
        Fatal("%s is not a vertex shader", vertex->name);
    }
    // A pipeline without a fragment stage writes no colour; the fragment shader stands in all the same, as GL
    // needs one, and the pipeline's write mask keeps it out.
    std::string fragment_source = std::string(kPrelude) + fragment_defines + kPush + kDraw;
    auto *program = new Program;
    program->program = Link(vertex_source, fragment_source, key.c_str());
    if (program->program != 0) {
        static const char *const kNames[kBlockCount] = {"Push", "MeshBlock", "Prog0Block", "PaletteBlock"};
        for (uint32_t block = 0; block < kBlockCount; block++) {
            const GLuint index = glGetUniformBlockIndex(program->program, kNames[block]);
            if (index != GL_INVALID_INDEX) {
                glUniformBlockBinding(program->program, index, block);
                program->reads[block] = true;
            }
        }
        glUseProgram(program->program);
        BindSampler(program->program, "u_texture", 0);
        BindSampler(program->program, "u_palette", 1);
        BindSampler(program->program, "u_texture_nearest", 2);
    }
    g_programs.emplace(key, program);
    return program;
}

void DrawDepthAsColor(Image *depth, int32_t x, int32_t y, uint32_t width, uint32_t height) {
    static GLuint program = 0;
    static GLint  origin = -1;
    static GLint  unorm = -1;
    if (program == 0) {
        program = Link(std::string(kPrelude) + kFullscreenVertex, std::string(kPrelude) + kDepthAsColor,
                       "depth as colour");
        if (program == 0) {
            Fatal("the depth readback's program does not link");
        }
        origin = glGetUniformLocation(program, "u_origin");
        unorm = glGetUniformLocation(program, "u_unorm");
        glUseProgram(program);
        BindSampler(program, "u_depth", 0);
    }
    glUseProgram(program);
    glUniform2i(origin, x, y);
    glUniform1i(unorm, depth->vk_format == VK_FORMAT_D24_UNORM_S8_UINT ? 1 : 0);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, depth->texture);
    glBindSampler(0, 0);
    glViewport(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height));
    glDrawArrays(GL_TRIANGLES, 0, 3);
}

} // namespace vkgl

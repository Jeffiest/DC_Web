// mesh_prog0.vert in WGSL: Dark Chronicle's Vu_prog0, for DrawMeshProg0 draws. The O-n notes are the
// spec's open items (docs/vu1/prog0.md in the game's port), kept as retail does them.

@id(0) override kSkinned: bool = false;

struct In {
    @location(0) position: vec3f,
    @location(1) normal: vec3f,
    @location(2) uv: vec2f,
    @location(3) color: vec4f,
    @location(4) skin_weight: vec4f,
    @location(5) skin_bone: vec4u,
}

struct Out {
    @builtin(position) position: vec4f,
    @builtin(clip_distances) clip_distances: array<f32, 6>,
    @location(0) color: vec4f,
    @location(1) uv: vec2f,
    @location(2) fog: vec4f,
    @location(3) @interpolate(flat) fog_mode: vec4f,
}

// gfx::Prog0Constants in the VU's units (128 is 1.0). The draw's record is at pc.prog0_index.
struct Prog0Data {
    model_world: mat4x4f,
    light_direction: array<vec4f, 3>,
    light_color: array<vec4f, 4>,
    ambient: vec4f,
    object_color: vec4f,
    eye: vec4f,
    clip_or_view: mat4x4f,
    diffuse: vec4f,
    specular: vec4f,
    point_light: array<vec4f, 4>,
    point_color: array<vec4f, 4>,
    fog: vec4f,
    flags: u32,
    pad0: u32,
    specular_power: u32,
    pad: u32,
}

@group(1) @binding(1) var<storage, read> prog0_blocks: array<Prog0Data>;

// gfx::Prog0Palette of a skinned draw, at pc.palette_index.
struct Prog0PaletteData {
    bones: array<vec4f, 96>,
}

@group(1) @binding(2) var<storage, read> palettes: array<Prog0PaletteData>;

const kScissor: u32 = 0x002u;
const kReal: u32 = 0x008u;
const kPointLights: u32 = 0x010u;
const kUnlit: u32 = 0x020u;
const kBlend: u32 = 0x040u;
const kColorAdd: u32 = 0x080u;
const kRefSphere: u32 = 0x100u;

const kFaceColor: u32 = 0x100u;
const kFaceNoNormal: u32 = 0x200u;

const kVuMax: f32 = 0x1.fffffep+127f; // FLT_MAX

// VU rsqrt(0) is a finite maximum; anything scaled by it is zero.
fn vu_rsqrt(s: f32) -> f32 {
    return select(0.0, inverseSqrt(s), s > 0.0);
}

fn vu_unit(v: vec3f) -> vec3f {
    return v * vu_rsqrt(dot(v, v));
}

fn Upper3(m: mat4x4f) -> mat3x3f {
    return mat3x3f(m[0].xyz, m[1].xyz, m[2].xyz);
}

struct Blended {
    position: vec3f,
    normal: vec3f,
}

fn blend(v: In, position: vec3f, normal: vec3f) -> Blended {
    let palette = &palettes[pc.palette_index];
    var P = vec3f(0.0);
    var N = vec3f(0.0);
    for (var i = 0; i < 4; i++) {
        let w = v.skin_weight[i];
        let b = min(v.skin_bone[i], 31u) * 3u;
        let r0 = (*palette).bones[b];
        let r1 = (*palette).bones[b + 1u];
        let r2 = (*palette).bones[b + 2u];

        let pw = vec4f(position * w, w);
        P += vec3f(dot(r0, pw), dot(r1, pw), dot(r2, pw));

        let nw = normal * w;
        let c0 = vec3f(r0.x, r1.x, r2.x);
        let c1 = vec3f(r0.y, r1.y, r2.y);
        let c2 = vec3f(r0.z, r1.z, r2.z);
        // O-8: retail reads column 1 for the z term of influence 3 (0x013E).
        N += c0 * nw.x + c1 * nw.y + select(c2, c1, i == 3) * nw.z;
    }
    return Blended(P, N);
}

@vertex
fn main(v: In) -> Out {
    let p0 = &prog0_blocks[pc.prog0_index];
    let F = (*p0).flags;
    let T = pc.face;

    var position = v.position;
    var normal = v.normal;
    var uv = v.uv;

    if ((F & kRefSphere) != 0u) {
        // O-9: env UV is taken from the unblended normal, before VERTEX_BLEND, as retail.
        let nh = vu_unit(Upper3((*p0).clip_or_view) * v.normal);
        uv = (nh.xy + 1.0) * 0.5;
    }

    if (kSkinned && (F & kBlend) != 0u) {
        let blended = blend(v, position, normal);
        position = blended.position;
        normal = blended.normal;
    }

    let W = Upper3((*p0).model_world);
    var L: array<vec3f, 4>;
    // progmain's MSCAL 0 setup: world directions into model space, each normalised.
    for (var c = 0; c < 4; c++) {
        let lw = vec3f((*p0).light_direction[0][c], (*p0).light_direction[1][c], (*p0).light_direction[2][c]);
        L[c] = vu_unit(vec3f(dot(lw, W[0]), dot(lw, W[1]), dot(lw, W[2])));
    }

    let D = (*p0).diffuse;
    var color: vec4f;
    if ((F & kUnlit) != 0u || (T & kFaceNoNormal) != 0u) {
        color = min((*p0).object_color * D, vec4f(255.0));
    } else {
        color = (*p0).ambient * D;
        var s: array<f32, 4>;
        for (var c = 0; c < 4; c++) {
            s[c] = max(0.0, dot(L[c], normal));
            color += (*p0).light_color[c] * D * s[c];
        }

        if ((F & kPointLights) != 0u) {
            let pw = W * position;
            let nw = W * normal;
            let inv_n = vu_rsqrt(dot(nw, nw));
            for (var k = 0; k < 4; k++) {
                let d = (*p0).point_light[k].xyz - pw;
                let inv_d = vu_rsqrt(dot(d, d));
                let cosine = max(0.0, dot(d, nw)) * inv_d * inv_n;
                let a = (*p0).point_light[k].w * inv_d;
                // O-4: power enters squared, so a negative power acts as its magnitude.
                let att = min(1.0, a * a);
                color += (*p0).point_color[k] * D * (cosine * att);
            }
        } else if ((F & kReal) != 0u) {
            let V = (*p0).eye.xyz - position;
            let inv_v = vu_rsqrt(dot(V, V));
            let vn = dot(V, normal);
            // Power 0 is the VU's 65536-pass loop; 32 passes reach the same saturated result.
            let n = select(min((*p0).specular_power, 32u), 32u, (*p0).specular_power == 0u);
            for (var c = 0; c < 4; c++) {
                let lit = min(1.0, 1.0e4 * s[c]);
                let r = max(0.0, 2.0 * vn * dot(L[c], normal) - dot(L[c], V)) * inv_v;
                var x = r * r;
                for (var j = 0u; j < n; j++) {
                    // Saturates at the VU's largest float; keeps lit * x finite.
                    x = min(x * x, kVuMax);
                }
                color += (*p0).light_color[c] * (*p0).specular * (lit * x);
            }
        }
        color = min(color, vec4f(255.0));
    }

    if ((T & kFaceColor) != 0u) {
        let vc = v.color * kModulate;
        if ((F & kColorAdd) != 0u) {
            // O-10: retail squares vertex alpha and drops the lit alpha.
            color = vec4f(min(color.rgb + 128.0 * vc.rgb, vec3f(255.0)), min(128.0 * vc.a * vc.a, 255.0));
        } else {
            color = min(color * vc, vec4f(255.0));
        }
    }

    let clip = meshes[pc.mesh_index].mvp * vec4f(position, 1.0);
    var o: Out;
    o.position = ToWebGpu(clip);

    if ((F & kScissor) != 0u) {
        let c = (*p0).clip_or_view * vec4f(position, 1.0);
        o.clip_distances[0] = c.w - c.x;
        o.clip_distances[1] = c.w + c.x;
        o.clip_distances[2] = c.w - c.y;
        o.clip_distances[3] = c.w + c.y;
        o.clip_distances[4] = c.w - c.z;
        o.clip_distances[5] = c.w + c.z;
    } else {
        for (var i = 0; i < 6; i++) {
            o.clip_distances[i] = 1.0;
        }
    }

    // ftoi0 truncates the VU's colour channels.
    o.color = floor(clamp(color, vec4f(0.0), vec4f(255.0))) / 255.0;
    o.uv = uv;
    let pfog = (*p0).fog;
    o.fog_mode = vec4f(pfog.y, pfog.z, 1.0, 0.0);
    if (pfog.w == 0.0) {
        // A constant factor needs no perspective correction; clamp before truncating as VU1 does.
        let fog = floor(max(max(min(pfog.x, pfog.y), pfog.z), 0.0)) / 255.0;
        o.fog = vec4f(fog, 0.0, 0.0, 0.0);
        o.fog_mode.z = -2.0;
        o.fog_mode.w = fog;
    } else {
        // Reverse-Z: only near/eye crossings need fresh fog; far-plane vertices retain their F. The full
        // homogeneous norm keeps the weight continuous at z=w=0 with nonzero x/y.
        let clip_scale = max(max(abs(clip.x), abs(clip.y)), max(abs(clip.z), abs(clip.w)));
        let marker = select(0.0, min((clip.z - clip.w) / clip_scale, 1.0), clip.z > clip.w);
        var fog = select(pfog.x + pfog.w / clip.w, 0.0, clip.w <= 0.0);
        fog = floor(max(max(min(fog, pfog.y), pfog.z), 0.0));
        // Retain weighted vertex F and its weight alongside the unlimited homogeneous numerator.
        let weight = max(clip.w, 0.0) * (1.0 - marker);
        o.fog = vec4f(fog * weight, pfog.x * clip.w + pfog.w, marker, weight);
    }
    return o;
}

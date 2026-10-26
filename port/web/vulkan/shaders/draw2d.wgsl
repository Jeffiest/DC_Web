// draw2d.vert in WGSL. Logical 2D: x, y in the target's logical space, z the depth (1 near, 0 far).

struct In {
    @location(0) position: vec3f,
    @location(1) uv: vec2f,
    @location(2) color: vec4f,
    @location(3) fog: f32,
}

struct Out {
    @builtin(position) position: vec4f,
    @location(0) color: vec4f,
    @location(1) uv: vec2f,
    @location(2) fog: vec4f,
    @location(3) @interpolate(flat) fog_mode: vec4f,
}

@vertex
fn main(v: In) -> Out {
    var o: Out;
    o.position = ToWebGpu(vec4f(v.position.xy * pc.xform.xy + pc.xform.zw, v.position.z, 1.0));
    o.color = v.color;
    o.uv = v.uv * pc.uv_xform.xy + pc.uv_xform.zw;
    o.fog = vec4f(v.fog, 0.0, 0.0, 0.0);
    o.fog_mode = vec4f(0.0, 0.0, -1.0, 0.0);
    return o;
}

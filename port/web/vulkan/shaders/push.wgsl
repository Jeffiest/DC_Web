// The renderer's push constants (gfx::detail::PushConstants), which every shader reads. WebGPU has no push
// constants: each draw's block sits in a storage buffer at a dynamic offset (port/web/vulkan/commands.cpp).
// Storage layout is std430's, so the block is laid out as the GLSL declares it.

struct Push {
    xform: vec4f,
    uv_xform: vec4f,
    texture_slot: u32,
    palette_slot: u32,
    sampler_slot: u32,
    flags: u32,
    alpha: u32, // ref 0-7, func 8-10, fix 16-23, ta0 24-31
    fog_color: u32,
    face: u32,
    mesh_index: u32,
    prog0_index: u32,
    palette_index: u32,
}

@group(2) @binding(0) var<storage, read> pc: Push;

// Vulkan's clip space has y down, WebGPU's y up: the same framebuffer position either way.
fn ToWebGpu(clip: vec4f) -> vec4f {
    return vec4f(clip.x, -clip.y, clip.z, clip.w);
}

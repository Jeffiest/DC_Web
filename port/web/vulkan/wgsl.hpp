// The WGSL that stands for each of the renderer's SPIR-V modules (shaders/, embedded by embed_wgsl.py).
#pragma once

#include <cstddef>

namespace vkwebgpu {

struct WgslShader {
    const char *name; // the module's symbol in its .spv.hpp, e.g. draw2d_vert
    const char *code;
};

extern const WgslShader kWgslShaders[];
extern const size_t     kWgslShaderCount;

} // namespace vkwebgpu

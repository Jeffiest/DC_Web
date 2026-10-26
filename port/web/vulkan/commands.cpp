// Vulkan on WebGPU (layer.hpp): command pools and buffers, and every vkCmd the renderer records.
//
// A command buffer encodes into a WebGPU command encoder as it records. Dynamic rendering is a render pass.
// Vulkan's dynamic state is tracked here and, with the pass's formats, picks the WebGPU pipeline variant
// each draw uses; push constants are copied per draw into a storage buffer read at a dynamic offset. What
// WebGPU's copies cannot take directly is staged: texture uploads whose rows are not 256-byte multiples
// go through an upload buffer with padded rows, readbacks through map-read buffers. Blits that scale or
// flip, attachment clears and colour image clears are drawn by small pipelines of their own.

#include "layer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace vkwebgpu {

namespace {

constexpr uint32_t kPushSlot = 256;            // minStorageBufferOffsetAlignment
constexpr uint32_t kPushBufferBytes = 256 * 1024;
constexpr uint64_t kUploadChunkBytes = 4ull << 20;
constexpr size_t   kFlagsOffset = 44;          // PushConstants::flags
constexpr uint32_t kOpaqueTargetFlag = 8;

uint32_t AlignUp(uint32_t value, uint32_t alignment) { return (value + alignment - 1) / alignment * alignment; }

// A storage buffer of push constant blocks, one per 256-byte slot, and its bind group.
struct PushBuffer {
    WGPUBuffer           gpu = nullptr;
    WGPUBindGroup        group = nullptr;
    std::vector<uint8_t> data;
};

// Rows staged for copies into textures.
struct UploadChunk {
    WGPUBuffer           gpu = nullptr;
    uint64_t             capacity = 0;
    std::vector<uint8_t> data;
};

} // namespace

struct CommandState {
    WGPUCommandEncoder    encoder = nullptr;
    WGPUCommandBuffer     finished = nullptr;
    WGPURenderPassEncoder pass = nullptr;
    WGPUTextureFormat     pass_color = WGPUTextureFormat_Undefined;
    WGPUTextureFormat     pass_depth = WGPUTextureFormat_Undefined;
    uint32_t              pass_width = 0;
    uint32_t              pass_height = 0;

    // Vulkan's state.
    Pipeline                    *pipeline = nullptr;
    DynamicState                 state;
    Buffer                      *vertex_buffer = nullptr;
    VkDeviceSize                 vertex_offset = 0;
    Buffer                      *index_buffer = nullptr;
    VkDeviceSize                 index_offset = 0;
    VkIndexType                  index_type = VK_INDEX_TYPE_UINT32;
    std::array<uint8_t, 256>     push = {};
    std::array<DescriptorSet *, 4> sets = {};
    std::array<std::vector<DescriptorEntry>, 4> pushed;
    std::array<bool, 4>          pushed_valid = {};

    // What the current pass has been given, so unchanged state is not set again.
    WGPURenderPipeline            applied_pipeline = nullptr;
    std::array<WGPUBindGroup, 4>  applied_groups = {};
    PushBuffer                   *applied_push_buffer = nullptr;
    uint32_t                      applied_push_offset = UINT32_MAX;
    Buffer                       *applied_vertex = nullptr;
    VkDeviceSize                  applied_vertex_offset = 0;
    Buffer                       *applied_index = nullptr;
    VkDeviceSize                  applied_index_offset = 0;
    bool                          viewport_dirty = true;
    bool                          scissor_dirty = true;
    bool                          blend_dirty = true;
    bool                          stencil_dirty = true;
    bool                          applied_constant_alpha = false;

    // The last push block written, so a draw that changes nothing reuses its slot.
    std::array<uint8_t, 256> written_push = {};
    bool                     push_written = false;
    PushBuffer              *push_slot_buffer = nullptr;
    uint32_t                 push_slot_offset = 0;
    uint32_t                 push_size = 0;

    std::vector<PushBuffer *>  push_buffers;
    size_t                     push_buffer_index = 0;
    std::vector<UploadChunk *> uploads;
    size_t                     upload_index = 0;
    std::vector<Readback>      readbacks;
    // Views and bind groups made for one command, released once it is submitted.
    std::vector<WGPUTextureView> temporary_views;
    std::vector<WGPUBindGroup>   temporary_groups;
};

namespace {

struct CommandPool {
    std::vector<VkCommandBuffer> buffers;
};

void ReleaseTemporaries(CommandState &c) {
    for (WGPUTextureView view : c.temporary_views) {
        wgpuTextureViewRelease(view);
    }
    c.temporary_views.clear();
    for (WGPUBindGroup group : c.temporary_groups) {
        wgpuBindGroupRelease(group);
    }
    c.temporary_groups.clear();
}

void ResetState(CommandState &c) {
    if (c.pass != nullptr) {
        wgpuRenderPassEncoderRelease(c.pass);
        c.pass = nullptr;
    }
    if (c.encoder != nullptr) {
        wgpuCommandEncoderRelease(c.encoder);
        c.encoder = nullptr;
    }
    if (c.finished != nullptr) {
        wgpuCommandBufferRelease(c.finished);
        c.finished = nullptr;
    }
    ReleaseTemporaries(c);
    c.pipeline = nullptr;
    c.state = DynamicState{};
    c.vertex_buffer = c.index_buffer = nullptr;
    c.sets = {};
    c.pushed_valid = {};
    c.push_written = false;
    c.push_buffer_index = 0;
    for (PushBuffer *buffer : c.push_buffers) {
        buffer->data.clear();
    }
    c.upload_index = 0;
    for (UploadChunk *chunk : c.uploads) {
        chunk->data.clear();
    }
    c.readbacks.clear();
}

// The pass starts with nothing set.
void ForgetApplied(CommandState &c) {
    c.applied_pipeline = nullptr;
    c.applied_groups = {};
    c.applied_push_buffer = nullptr;
    c.applied_push_offset = UINT32_MAX;
    c.applied_vertex = nullptr;
    c.applied_index = nullptr;
    c.viewport_dirty = c.scissor_dirty = c.blend_dirty = c.stencil_dirty = true;
}

// A slot for a push block of size bytes, in the command buffer's push buffers.
std::pair<PushBuffer *, uint32_t> PushSlot(CommandState &c, const void *block, uint32_t size) {
    PushBuffer *buffer = c.push_buffer_index < c.push_buffers.size() ? c.push_buffers[c.push_buffer_index] : nullptr;
    if (buffer == nullptr || buffer->data.size() + kPushSlot > kPushBufferBytes) {
        if (buffer != nullptr) {
            c.push_buffer_index++;
        }
        if (c.push_buffer_index == c.push_buffers.size()) {
            auto *created = new PushBuffer;
            WGPUBufferDescriptor descriptor = {};
            descriptor.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_CopyDst;
            descriptor.size = kPushBufferBytes;
            created->gpu = wgpuDeviceCreateBuffer(g.device, &descriptor);
            WGPUBindGroupEntry entry = {};
            entry.binding = 0;
            entry.buffer = created->gpu;
            entry.size = kPushSlot;
            WGPUBindGroupDescriptor group = {};
            group.layout = PushGroupLayout();
            group.entryCount = 1;
            group.entries = &entry;
            created->group = wgpuDeviceCreateBindGroup(g.device, &group);
            created->data.reserve(kPushBufferBytes);
            c.push_buffers.push_back(created);
        }
        buffer = c.push_buffers[c.push_buffer_index];
    }
    const auto offset = static_cast<uint32_t>(buffer->data.size());
    buffer->data.resize(offset + kPushSlot);
    std::memcpy(buffer->data.data() + offset, block, std::min(size, kPushSlot));
    return {buffer, offset};
}

// Room for size bytes of rows bound for a texture, 256-aligned in an upload chunk.
std::pair<UploadChunk *, uint64_t> UploadSpace(CommandState &c, uint64_t size) {
    while (c.upload_index < c.uploads.size()) {
        UploadChunk   *chunk = c.uploads[c.upload_index];
        const uint64_t offset = (chunk->data.size() + 255) & ~uint64_t{255};
        if (offset + size <= chunk->capacity) {
            chunk->data.resize(offset + size);
            return {chunk, offset};
        }
        c.upload_index++;
    }
    auto *chunk = new UploadChunk;
    chunk->capacity = std::max<uint64_t>(kUploadChunkBytes, (size + 255) & ~uint64_t{255});
    WGPUBufferDescriptor descriptor = {};
    descriptor.usage = WGPUBufferUsage_CopySrc | WGPUBufferUsage_CopyDst;
    descriptor.size = chunk->capacity;
    chunk->gpu = wgpuDeviceCreateBuffer(g.device, &descriptor);
    chunk->data.reserve(chunk->capacity);
    chunk->data.resize(size);
    c.uploads.push_back(chunk);
    c.upload_index = c.uploads.size() - 1;
    return {chunk, 0};
}

void EndPass(CommandState &c) {
    if (c.pass != nullptr) {
        wgpuRenderPassEncoderEnd(c.pass);
        wgpuRenderPassEncoderRelease(c.pass);
        c.pass = nullptr;
    }
}

// --- Vulkan enums as WebGPU's ------------------------------------------------------------------------------

WGPUCompareFunction Compare(VkCompareOp op) {
    switch (op) {
        case VK_COMPARE_OP_NEVER:
            return WGPUCompareFunction_Never;
        case VK_COMPARE_OP_LESS:
            return WGPUCompareFunction_Less;
        case VK_COMPARE_OP_EQUAL:
            return WGPUCompareFunction_Equal;
        case VK_COMPARE_OP_LESS_OR_EQUAL:
            return WGPUCompareFunction_LessEqual;
        case VK_COMPARE_OP_GREATER:
            return WGPUCompareFunction_Greater;
        case VK_COMPARE_OP_NOT_EQUAL:
            return WGPUCompareFunction_NotEqual;
        case VK_COMPARE_OP_GREATER_OR_EQUAL:
            return WGPUCompareFunction_GreaterEqual;
        default:
            return WGPUCompareFunction_Always;
    }
}

WGPUStencilOperation StencilOperation(VkStencilOp op) {
    switch (op) {
        case VK_STENCIL_OP_ZERO:
            return WGPUStencilOperation_Zero;
        case VK_STENCIL_OP_REPLACE:
            return WGPUStencilOperation_Replace;
        case VK_STENCIL_OP_INCREMENT_AND_CLAMP:
            return WGPUStencilOperation_IncrementClamp;
        case VK_STENCIL_OP_DECREMENT_AND_CLAMP:
            return WGPUStencilOperation_DecrementClamp;
        case VK_STENCIL_OP_INVERT:
            return WGPUStencilOperation_Invert;
        case VK_STENCIL_OP_INCREMENT_AND_WRAP:
            return WGPUStencilOperation_IncrementWrap;
        case VK_STENCIL_OP_DECREMENT_AND_WRAP:
            return WGPUStencilOperation_DecrementWrap;
        default:
            return WGPUStencilOperation_Keep;
    }
}

WGPUBlendFactor BlendFactor(VkBlendFactor factor) {
    switch (factor) {
        case VK_BLEND_FACTOR_ZERO:
            return WGPUBlendFactor_Zero;
        case VK_BLEND_FACTOR_ONE:
            return WGPUBlendFactor_One;
        case VK_BLEND_FACTOR_SRC_COLOR:
            return WGPUBlendFactor_Src;
        case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
            return WGPUBlendFactor_OneMinusSrc;
        case VK_BLEND_FACTOR_DST_COLOR:
            return WGPUBlendFactor_Dst;
        case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
            return WGPUBlendFactor_OneMinusDst;
        case VK_BLEND_FACTOR_SRC_ALPHA:
            return WGPUBlendFactor_SrcAlpha;
        case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
            return WGPUBlendFactor_OneMinusSrcAlpha;
        case VK_BLEND_FACTOR_DST_ALPHA:
            return WGPUBlendFactor_DstAlpha;
        case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
            return WGPUBlendFactor_OneMinusDstAlpha;
        case VK_BLEND_FACTOR_CONSTANT_COLOR:
        case VK_BLEND_FACTOR_CONSTANT_ALPHA:
            return WGPUBlendFactor_Constant;
        case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
        case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
            return WGPUBlendFactor_OneMinusConstant;
        case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
            return WGPUBlendFactor_SrcAlphaSaturated;
        case VK_BLEND_FACTOR_SRC1_COLOR:
            return WGPUBlendFactor_Src1;
        case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
            return WGPUBlendFactor_OneMinusSrc1;
        case VK_BLEND_FACTOR_SRC1_ALPHA:
            return WGPUBlendFactor_Src1Alpha;
        case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
            return WGPUBlendFactor_OneMinusSrc1Alpha;
        default:
            return WGPUBlendFactor_One;
    }
}

WGPUBlendOperation BlendOperation(VkBlendOp op) {
    switch (op) {
        case VK_BLEND_OP_SUBTRACT:
            return WGPUBlendOperation_Subtract;
        case VK_BLEND_OP_REVERSE_SUBTRACT:
            return WGPUBlendOperation_ReverseSubtract;
        case VK_BLEND_OP_MIN:
            return WGPUBlendOperation_Min;
        case VK_BLEND_OP_MAX:
            return WGPUBlendOperation_Max;
        default:
            return WGPUBlendOperation_Add;
    }
}

WGPUVertexFormat VertexFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return WGPUVertexFormat_Float32x4;
        case VK_FORMAT_R32G32B32_SFLOAT:
            return WGPUVertexFormat_Float32x3;
        case VK_FORMAT_R32G32_SFLOAT:
            return WGPUVertexFormat_Float32x2;
        case VK_FORMAT_R32_SFLOAT:
            return WGPUVertexFormat_Float32;
        case VK_FORMAT_R8G8B8A8_UNORM:
            return WGPUVertexFormat_Unorm8x4;
        case VK_FORMAT_R8_UNORM:
            return WGPUVertexFormat_Unorm8;
        case VK_FORMAT_R8G8B8A8_UINT:
            return WGPUVertexFormat_Uint8x4;
        default:
            Fatal("no WebGPU vertex format for VkFormat %d", static_cast<int>(format));
    }
}

WGPUPrimitiveTopology Topology(VkPrimitiveTopology topology) {
    switch (topology) {
        case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
            return WGPUPrimitiveTopology_PointList;
        case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
            return WGPUPrimitiveTopology_LineList;
        case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
            return WGPUPrimitiveTopology_LineStrip;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
            return WGPUPrimitiveTopology_TriangleStrip;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST:
            return WGPUPrimitiveTopology_TriangleList;
        default:
            // Triangle fans are not offered (portability subset).
            Fatal("primitive topology %d has no WebGPU equivalent", static_cast<int>(topology));
    }
}

bool IsStrip(VkPrimitiveTopology topology) {
    return topology == VK_PRIMITIVE_TOPOLOGY_LINE_STRIP || topology == VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
}

bool IsSecondSource(VkBlendFactor factor) {
    return factor == VK_BLEND_FACTOR_SRC1_ALPHA || factor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA ||
           factor == VK_BLEND_FACTOR_SRC1_COLOR || factor == VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
}

VkBlendFactor FirstSource(VkBlendFactor factor) {
    switch (factor) {
        case VK_BLEND_FACTOR_SRC1_ALPHA:
            return VK_BLEND_FACTOR_SRC_ALPHA;
        case VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case VK_BLEND_FACTOR_SRC1_COLOR:
            return VK_BLEND_FACTOR_SRC_COLOR;
        case VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR:
            return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        default:
            return factor;
    }
}

bool IsConstantAlpha(VkBlendFactor factor) {
    return factor == VK_BLEND_FACTOR_CONSTANT_ALPHA || factor == VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
}

// The single-source shader that stands in for draw_bound_frag where WebGPU has no dual-source blending.
ShaderModule *SingleSourceStandIn() {
    static ShaderModule module;
    if (module.wgsl == nullptr) {
        module.wgsl = FindWgsl("draw_bound_single_frag");
        for (const char *at = std::strstr(module.wgsl->code, "@id("); at != nullptr; at = std::strstr(at + 4, "@id(")) {
            module.override_ids.push_back(static_cast<uint32_t>(std::strtoul(at + 4, nullptr, 10)));
        }
    }
    return &module;
}

std::vector<WGPUConstantEntry> Constants(const std::vector<std::pair<uint32_t, double>> &values,
                                         std::vector<std::string> &keys) {
    keys.clear();
    keys.reserve(values.size());
    std::vector<WGPUConstantEntry> entries;
    for (const auto &[id, value] : values) {
        keys.push_back(std::to_string(id));
        WGPUConstantEntry entry = {};
        entry.key = View(keys.back().c_str());
        entry.value = value;
        entries.push_back(entry);
    }
    return entries;
}

// --- Pipeline variants --------------------------------------------------------------------------------------

struct VariantKey {
    uint8_t           topology;
    uint8_t           strip_index;
    uint8_t           cull;
    uint8_t           front_face;
    uint8_t           depth_test;
    uint8_t           depth_write;
    uint8_t           depth_compare;
    uint8_t           stencil_test;
    uint8_t           stencil_ops[2][4];
    uint8_t           stencil_read;
    uint8_t           stencil_write;
    WGPUTextureFormat color;
    WGPUTextureFormat depth;
};

VariantKey KeyOf(const CommandState &c, bool indexed) {
    VariantKey key = {};
    const DynamicState &s = c.state;
    key.topology = static_cast<uint8_t>(s.topology);
    key.strip_index = indexed && IsStrip(s.topology) ? 1 : 0;
    key.cull = static_cast<uint8_t>(s.cull);
    key.front_face = static_cast<uint8_t>(s.front_face);
    key.depth_test = s.depth_test;
    key.depth_write = s.depth_test && s.depth_write;
    key.depth_compare = s.depth_test ? static_cast<uint8_t>(s.depth_compare) : 0;
    key.stencil_test = s.stencil_test;
    if (s.stencil_test) {
        for (int face = 0; face < 2; face++) {
            key.stencil_ops[face][0] = static_cast<uint8_t>(s.stencil[face].fail);
            key.stencil_ops[face][1] = static_cast<uint8_t>(s.stencil[face].pass);
            key.stencil_ops[face][2] = static_cast<uint8_t>(s.stencil[face].depth_fail);
            key.stencil_ops[face][3] = static_cast<uint8_t>(s.stencil[face].compare);
        }
        key.stencil_read = static_cast<uint8_t>(s.stencil[0].compare_mask);
        key.stencil_write = static_cast<uint8_t>(s.stencil[0].write_mask);
    }
    key.color = c.pass_color;
    key.depth = c.pass_depth;
    return key;
}

const PipelineVariant &VariantOf(Pipeline *p, const VariantKey &key) {
    std::string text(reinterpret_cast<const char *>(&key), sizeof key);
    auto        it = p->variants.find(text);
    if (it != p->variants.end()) {
        return it->second;
    }
    PipelineVariant variant;

    std::vector<std::string>    vertex_keys;
    std::vector<WGPUConstantEntry> vertex_constants = Constants(p->vertex_constants, vertex_keys);
    std::vector<WGPUVertexAttribute> attributes;
    for (const VertexAttribute &attribute : p->attributes) {
        WGPUVertexAttribute a = {};
        a.format = VertexFormat(attribute.format);
        a.offset = attribute.offset;
        a.shaderLocation = attribute.location;
        attributes.push_back(a);
    }
    WGPUVertexBufferLayout buffer = {};
    buffer.stepMode = WGPUVertexStepMode_Vertex;
    buffer.arrayStride = p->stride;
    buffer.attributeCount = attributes.size();
    buffer.attributes = attributes.data();

    WGPURenderPipelineDescriptor descriptor = {};
    descriptor.layout = PipelineLayoutOf(p->layout);
    descriptor.vertex.module = ModuleOf(p->vertex);
    descriptor.vertex.entryPoint = View("main");
    descriptor.vertex.constantCount = vertex_constants.size();
    descriptor.vertex.constants = vertex_constants.data();
    descriptor.vertex.bufferCount = attributes.empty() ? 0 : 1;
    descriptor.vertex.buffers = &buffer;

    const auto topology = static_cast<VkPrimitiveTopology>(key.topology);
    descriptor.primitive.topology = Topology(topology);
    descriptor.primitive.stripIndexFormat = key.strip_index ? WGPUIndexFormat_Uint32 : WGPUIndexFormat_Undefined;
    descriptor.primitive.frontFace =
        key.front_face == VK_FRONT_FACE_CLOCKWISE ? WGPUFrontFace_CW : WGPUFrontFace_CCW;
    descriptor.primitive.cullMode = key.cull == VK_CULL_MODE_FRONT_BIT  ? WGPUCullMode_Front
                                    : key.cull == VK_CULL_MODE_BACK_BIT ? WGPUCullMode_Back
                                    : key.cull == VK_CULL_MODE_NONE     ? WGPUCullMode_None
                                                                        : WGPUCullMode_None;

    WGPUDepthStencilState depth = {};
    if (key.depth != WGPUTextureFormat_Undefined) {
        depth.format = key.depth;
        depth.depthWriteEnabled = key.depth_write ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        depth.depthCompare =
            key.depth_test ? Compare(static_cast<VkCompareOp>(key.depth_compare)) : WGPUCompareFunction_Always;
        const bool has_stencil = key.depth != WGPUTextureFormat_Depth32Float;
        for (int face = 0; face < 2; face++) {
            WGPUStencilFaceState &state = face == 0 ? depth.stencilFront : depth.stencilBack;
            if (key.stencil_test && has_stencil) {
                state.failOp = StencilOperation(static_cast<VkStencilOp>(key.stencil_ops[face][0]));
                state.passOp = StencilOperation(static_cast<VkStencilOp>(key.stencil_ops[face][1]));
                state.depthFailOp = StencilOperation(static_cast<VkStencilOp>(key.stencil_ops[face][2]));
                state.compare = Compare(static_cast<VkCompareOp>(key.stencil_ops[face][3]));
            } else {
                state.failOp = state.passOp = state.depthFailOp = WGPUStencilOperation_Keep;
                state.compare = WGPUCompareFunction_Always;
            }
        }
        depth.stencilReadMask = key.stencil_test && has_stencil ? key.stencil_read : 0xFF;
        depth.stencilWriteMask = key.stencil_test && has_stencil ? key.stencil_write : 0;
        descriptor.depthStencil = &depth;
    }
    descriptor.multisample.count = 1;
    descriptor.multisample.mask = 0xFFFFFFFF;

    // The colour target, its blend and the fragment stage.
    VkBlendFactor src_color = p->src_color;
    VkBlendFactor dst_color = p->dst_color;
    VkBlendFactor src_alpha = p->src_alpha;
    VkBlendFactor dst_alpha = p->dst_alpha;
    ShaderModule *fragment = p->fragment;
    WGPUColorWriteMask mask = WGPUColorWriteMask_None;
    if (p->write_mask & VK_COLOR_COMPONENT_R_BIT) {
        mask |= WGPUColorWriteMask_Red;
    }
    if (p->write_mask & VK_COLOR_COMPONENT_G_BIT) {
        mask |= WGPUColorWriteMask_Green;
    }
    if (p->write_mask & VK_COLOR_COMPONENT_B_BIT) {
        mask |= WGPUColorWriteMask_Blue;
    }
    if (p->write_mask & VK_COLOR_COMPONENT_A_BIT) {
        mask |= WGPUColorWriteMask_Alpha;
    }
    const bool second_source = p->blend && (IsSecondSource(src_color) || IsSecondSource(dst_color) ||
                                            IsSecondSource(src_alpha) || IsSecondSource(dst_alpha));
    if (second_source && !g.dual_source) {
        // The renderer only blends from a second source on targets without alpha, with the factor in the
        // second output and alpha forced to 1 in the first: here the factor is the first output's alpha,
        // which the target then does not take.
        src_color = FirstSource(src_color);
        dst_color = FirstSource(dst_color);
        src_alpha = FirstSource(src_alpha);
        dst_alpha = FirstSource(dst_alpha);
        fragment = SingleSourceStandIn();
        mask &= ~WGPUColorWriteMask_Alpha;
        variant.factor_in_alpha = true;
    }
    variant.constant_alpha = p->blend && (IsConstantAlpha(src_color) || IsConstantAlpha(dst_color) ||
                                          IsConstantAlpha(src_alpha) || IsConstantAlpha(dst_alpha));
    WGPUBlendState blend = {};
    blend.color = {BlendOperation(p->color_op), BlendFactor(src_color), BlendFactor(dst_color)};
    blend.alpha = {BlendOperation(p->alpha_op), BlendFactor(src_alpha), BlendFactor(dst_alpha)};
    WGPUColorTargetState target = {};
    target.format = key.color;
    target.blend = p->blend ? &blend : nullptr;
    target.writeMask = mask;

    std::vector<std::string>       fragment_keys;
    std::vector<WGPUConstantEntry> fragment_constants;
    WGPUFragmentState              fragment_state = {};
    if (fragment != nullptr) {
        fragment_constants = Constants(p->fragment_constants, fragment_keys);
        fragment_state.module = ModuleOf(fragment);
        fragment_state.constantCount = fragment_constants.size();
        fragment_state.constants = fragment_constants.data();
    } else {
        // WebGPU needs a fragment stage for the pass's colour target; this one writes nothing.
        fragment_state.module = EmptyFragmentModule();
        target.writeMask = WGPUColorWriteMask_None;
        target.blend = nullptr;
    }
    fragment_state.entryPoint = View("main");
    fragment_state.targetCount = key.color != WGPUTextureFormat_Undefined ? 1 : 0;
    fragment_state.targets = &target;
    descriptor.fragment = &fragment_state;

    variant.pipeline = wgpuDeviceCreateRenderPipeline(g.device, &descriptor);
    return p->variants.emplace(std::move(text), variant).first->second;
}

// Sets what a draw needs that the pass does not have yet. False when the draw cannot be made.
bool PrepareDraw(CommandState &c, bool indexed) {
    if (c.pass == nullptr || c.pipeline == nullptr) {
        return false;
    }
    Pipeline              *p = c.pipeline;
    const PipelineVariant &variant = VariantOf(p, KeyOf(c, indexed));
    if (variant.pipeline == nullptr) {
        return false;
    }
    if (variant.pipeline != c.applied_pipeline) {
        wgpuRenderPassEncoderSetPipeline(c.pass, variant.pipeline);
        c.applied_pipeline = variant.pipeline;
    }

    PipelineLayout *layout = p->layout;
    for (uint32_t i = 0; i < layout->sets.size(); i++) {
        WGPUBindGroup group = c.sets[i] != nullptr  ? BindGroupOf(c.sets[i])
                              : c.pushed_valid[i]   ? PushedBindGroup(layout->sets[i], c.pushed[i])
                                                    : FillerBindGroup(layout->sets[i]);
        if (group != c.applied_groups[i]) {
            wgpuRenderPassEncoderSetBindGroup(c.pass, i, group, 0, nullptr);
            c.applied_groups[i] = group;
        }
    }

    // The push constants, in a new slot only when they changed.
    std::array<uint8_t, 256> block = c.push;
    if (variant.factor_in_alpha) {
        uint32_t flags = 0;
        std::memcpy(&flags, block.data() + kFlagsOffset, sizeof flags);
        flags &= ~kOpaqueTargetFlag;
        std::memcpy(block.data() + kFlagsOffset, &flags, sizeof flags);
    }
    const uint32_t size = std::max<uint32_t>(layout->push_size, 16);
    if (!c.push_written || c.push_size != size || std::memcmp(block.data(), c.written_push.data(), size) != 0) {
        auto [buffer, offset] = PushSlot(c, block.data(), size);
        c.push_slot_buffer = buffer;
        c.push_slot_offset = offset;
        c.written_push = block;
        c.push_size = size;
        c.push_written = true;
    }
    const auto push_group = static_cast<uint32_t>(layout->sets.size());
    if (c.applied_push_buffer != c.push_slot_buffer || c.applied_push_offset != c.push_slot_offset) {
        wgpuRenderPassEncoderSetBindGroup(c.pass, push_group, c.push_slot_buffer->group, 1, &c.push_slot_offset);
        c.applied_push_buffer = c.push_slot_buffer;
        c.applied_push_offset = c.push_slot_offset;
        c.applied_groups[push_group] = nullptr;
    }

    if (!p->attributes.empty() && c.vertex_buffer != nullptr &&
        (c.applied_vertex != c.vertex_buffer || c.applied_vertex_offset != c.vertex_offset)) {
        wgpuRenderPassEncoderSetVertexBuffer(c.pass, 0, c.vertex_buffer->gpu, c.vertex_offset, WGPU_WHOLE_SIZE);
        c.applied_vertex = c.vertex_buffer;
        c.applied_vertex_offset = c.vertex_offset;
    }
    if (indexed && c.index_buffer != nullptr &&
        (c.applied_index != c.index_buffer || c.applied_index_offset != c.index_offset)) {
        wgpuRenderPassEncoderSetIndexBuffer(
            c.pass, c.index_buffer->gpu,
            c.index_type == VK_INDEX_TYPE_UINT16 ? WGPUIndexFormat_Uint16 : WGPUIndexFormat_Uint32, c.index_offset,
            WGPU_WHOLE_SIZE);
        c.applied_index = c.index_buffer;
        c.applied_index_offset = c.index_offset;
    }

    if (c.viewport_dirty) {
        const VkViewport &v = c.state.viewport;
        wgpuRenderPassEncoderSetViewport(c.pass, v.x, v.y, std::max(v.width, 0.0f), std::max(v.height, 0.0f),
                                         std::clamp(v.minDepth, 0.0f, 1.0f), std::clamp(v.maxDepth, 0.0f, 1.0f));
        c.viewport_dirty = false;
    }
    if (c.scissor_dirty) {
        const VkRect2D &r = c.state.scissor;
        const int64_t   x0 = std::clamp<int64_t>(r.offset.x, 0, c.pass_width);
        const int64_t   y0 = std::clamp<int64_t>(r.offset.y, 0, c.pass_height);
        const int64_t   x1 = std::clamp<int64_t>(int64_t{r.offset.x} + r.extent.width, x0, c.pass_width);
        const int64_t   y1 = std::clamp<int64_t>(int64_t{r.offset.y} + r.extent.height, y0, c.pass_height);
        wgpuRenderPassEncoderSetScissorRect(c.pass, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                                            static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0));
        c.scissor_dirty = false;
    }
    if (c.blend_dirty || c.applied_constant_alpha != variant.constant_alpha) {
        const float *k = c.state.blend_constants;
        WGPUColor    color = variant.constant_alpha ? WGPUColor{k[3], k[3], k[3], k[3]} : WGPUColor{k[0], k[1], k[2], k[3]};
        wgpuRenderPassEncoderSetBlendConstant(c.pass, &color);
        c.applied_constant_alpha = variant.constant_alpha;
        c.blend_dirty = false;
    }
    if (c.stencil_dirty) {
        wgpuRenderPassEncoderSetStencilReference(c.pass, c.state.stencil[0].reference & 0xFF);
        c.stencil_dirty = false;
    }
    return true;
}

// --- The layer's own pipelines: blits that scale or flip, and clears inside a pass --------------------------

constexpr const char *kBlitWgsl = R"wgsl(
struct Blit { uv: vec4f, }
@group(0) @binding(0) var source: texture_2d<f32>;
@group(0) @binding(1) var source_sampler: sampler;
@group(1) @binding(0) var<storage, read> blit: Blit;

struct Out {
    @builtin(position) position: vec4f,
    @location(0) uv: vec2f,
}

@vertex
fn vs(@builtin(vertex_index) index: u32) -> Out {
    let corner = vec2f(f32(index & 1u), f32(index >> 1u));
    var o: Out;
    o.position = vec4f(corner.x * 2.0 - 1.0, 1.0 - corner.y * 2.0, 0.0, 1.0);
    o.uv = mix(blit.uv.xy, blit.uv.zw, corner);
    return o;
}

@fragment
fn fs(v: Out) -> @location(0) vec4f {
    return textureSampleLevel(source, source_sampler, v.uv, 0.0);
}
)wgsl";

constexpr const char *kClearWgsl = R"wgsl(
struct Clear { color: vec4f, depth: vec4f, }
@group(0) @binding(0) var<storage, read> clear: Clear;

@vertex
fn vs(@builtin(vertex_index) index: u32) -> @builtin(position) vec4f {
    let corner = vec2f(f32((index << 1u) & 2u), f32(index & 2u));
    return vec4f(corner * 2.0 - 1.0, clear.depth.x, 1.0);
}

@fragment
fn fs() -> @location(0) vec4f {
    return clear.color;
}
)wgsl";

WGPUShaderModule HelperModule(const char *label, const char *code) {
    WGPUShaderSourceWGSL source = {};
    source.chain.sType = WGPUSType_ShaderSourceWGSL;
    source.code = View(code);
    WGPUShaderModuleDescriptor descriptor = {};
    descriptor.nextInChain = &source.chain;
    descriptor.label = View(label);
    return wgpuDeviceCreateShaderModule(g.device, &descriptor);
}

struct BlitPipelines {
    WGPUBindGroupLayout textures = nullptr;
    WGPUPipelineLayout  layout = nullptr;
    WGPUShaderModule    module = nullptr;
    WGPUSampler         nearest = nullptr;
    WGPUSampler         linear = nullptr;
    std::unordered_map<int, WGPURenderPipeline> by_format;
};

BlitPipelines &Blits() {
    static BlitPipelines blits;
    if (blits.module == nullptr) {
        WGPUBindGroupLayoutEntry entries[2] = {};
        entries[0].binding = 0;
        entries[0].visibility = WGPUShaderStage_Fragment;
        entries[0].texture.sampleType = WGPUTextureSampleType_Float;
        entries[0].texture.viewDimension = WGPUTextureViewDimension_2D;
        entries[1].binding = 1;
        entries[1].visibility = WGPUShaderStage_Fragment;
        entries[1].sampler.type = WGPUSamplerBindingType_Filtering;
        WGPUBindGroupLayoutDescriptor layout = {};
        layout.entryCount = 2;
        layout.entries = entries;
        blits.textures = wgpuDeviceCreateBindGroupLayout(g.device, &layout);
        WGPUBindGroupLayout groups[2] = {blits.textures, PushGroupLayout()};
        WGPUPipelineLayoutDescriptor pipeline_layout = {};
        pipeline_layout.bindGroupLayoutCount = 2;
        pipeline_layout.bindGroupLayouts = groups;
        blits.layout = wgpuDeviceCreatePipelineLayout(g.device, &pipeline_layout);
        blits.module = HelperModule("blit", kBlitWgsl);
        for (bool linear : {false, true}) {
            WGPUSamplerDescriptor sampler = {};
            sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = WGPUAddressMode_ClampToEdge;
            sampler.magFilter = sampler.minFilter = linear ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
            sampler.mipmapFilter = WGPUMipmapFilterMode_Nearest;
            sampler.lodMaxClamp = 32.0f;
            sampler.maxAnisotropy = 1;
            (linear ? blits.linear : blits.nearest) = wgpuDeviceCreateSampler(g.device, &sampler);
        }
    }
    return blits;
}

WGPURenderPipeline BlitPipeline(WGPUTextureFormat format) {
    BlitPipelines &blits = Blits();
    auto           it = blits.by_format.find(static_cast<int>(format));
    if (it != blits.by_format.end()) {
        return it->second;
    }
    WGPUColorTargetState target = {};
    target.format = format;
    target.writeMask = WGPUColorWriteMask_All;
    WGPUFragmentState fragment = {};
    fragment.module = blits.module;
    fragment.entryPoint = View("fs");
    fragment.targetCount = 1;
    fragment.targets = &target;
    WGPURenderPipelineDescriptor descriptor = {};
    descriptor.layout = blits.layout;
    descriptor.vertex.module = blits.module;
    descriptor.vertex.entryPoint = View("vs");
    descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleStrip;
    descriptor.primitive.cullMode = WGPUCullMode_None;
    descriptor.multisample.count = 1;
    descriptor.multisample.mask = 0xFFFFFFFF;
    descriptor.fragment = &fragment;
    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(g.device, &descriptor);
    blits.by_format.emplace(static_cast<int>(format), pipeline);
    return pipeline;
}

WGPURenderPipeline ClearPipeline(WGPUTextureFormat color, WGPUTextureFormat depth, bool clear_color, bool clear_depth,
                                 bool clear_stencil) {
    static WGPUShaderModule   module = nullptr;
    static WGPUPipelineLayout layout = nullptr;
    static std::unordered_map<std::string, WGPURenderPipeline> pipelines;
    if (module == nullptr) {
        module = HelperModule("clear", kClearWgsl);
        WGPUBindGroupLayout          group = PushGroupLayout();
        WGPUPipelineLayoutDescriptor descriptor = {};
        descriptor.bindGroupLayoutCount = 1;
        descriptor.bindGroupLayouts = &group;
        layout = wgpuDeviceCreatePipelineLayout(g.device, &descriptor);
    }
    std::string key;
    key.append(reinterpret_cast<const char *>(&color), sizeof color);
    key.append(reinterpret_cast<const char *>(&depth), sizeof depth);
    key.push_back(static_cast<char>(clear_color | clear_depth << 1 | clear_stencil << 2));
    auto it = pipelines.find(key);
    if (it != pipelines.end()) {
        return it->second;
    }
    WGPUColorTargetState target = {};
    target.format = color;
    target.writeMask = clear_color ? WGPUColorWriteMask_All : WGPUColorWriteMask_None;
    WGPUFragmentState fragment = {};
    fragment.module = module;
    fragment.entryPoint = View("fs");
    fragment.targetCount = color != WGPUTextureFormat_Undefined ? 1 : 0;
    fragment.targets = &target;
    WGPUDepthStencilState depth_state = {};
    WGPURenderPipelineDescriptor descriptor = {};
    if (depth != WGPUTextureFormat_Undefined) {
        depth_state.format = depth;
        depth_state.depthWriteEnabled = clear_depth ? WGPUOptionalBool_True : WGPUOptionalBool_False;
        depth_state.depthCompare = WGPUCompareFunction_Always;
        const bool has_stencil = depth != WGPUTextureFormat_Depth32Float;
        for (WGPUStencilFaceState *face : {&depth_state.stencilFront, &depth_state.stencilBack}) {
            face->compare = WGPUCompareFunction_Always;
            face->failOp = face->depthFailOp = WGPUStencilOperation_Keep;
            face->passOp = clear_stencil && has_stencil ? WGPUStencilOperation_Replace : WGPUStencilOperation_Keep;
        }
        depth_state.stencilReadMask = 0xFF;
        depth_state.stencilWriteMask = clear_stencil && has_stencil ? 0xFF : 0;
        descriptor.depthStencil = &depth_state;
    }
    descriptor.layout = layout;
    descriptor.vertex.module = module;
    descriptor.vertex.entryPoint = View("vs");
    descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    descriptor.primitive.cullMode = WGPUCullMode_None;
    descriptor.multisample.count = 1;
    descriptor.multisample.mask = 0xFFFFFFFF;
    descriptor.fragment = &fragment;
    WGPURenderPipeline pipeline = wgpuDeviceCreateRenderPipeline(g.device, &descriptor);
    pipelines.emplace(std::move(key), pipeline);
    return pipeline;
}

uint32_t MipSize(uint32_t size, uint32_t mip) { return std::max(size >> mip, 1u); }

} // namespace

void TakeSubmission(VkCommandBuffer cmd, std::vector<WGPUCommandBuffer> &commands, std::vector<Readback> &readbacks) {
    CommandState &c = *cmd->state;
    if (c.finished == nullptr) {
        Warn("a command buffer was submitted without vkEndCommandBuffer");
        return;
    }
    for (PushBuffer *buffer : c.push_buffers) {
        if (!buffer->data.empty()) {
            wgpuQueueWriteBuffer(g.queue, buffer->gpu, 0, buffer->data.data(), buffer->data.size());
        }
    }
    for (UploadChunk *chunk : c.uploads) {
        if (!chunk->data.empty()) {
            chunk->data.resize((chunk->data.size() + 3) & ~size_t{3});
            wgpuQueueWriteBuffer(g.queue, chunk->gpu, 0, chunk->data.data(), chunk->data.size());
        }
    }
    commands.push_back(c.finished);
    c.finished = nullptr;
    readbacks.insert(readbacks.end(), c.readbacks.begin(), c.readbacks.end());
    c.readbacks.clear();
    ReleaseTemporaries(c);
}

} // namespace vkwebgpu

using namespace vkwebgpu;

extern "C" {

// --- Pools and buffers --------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo *,
                                                   const VkAllocationCallbacks *, VkCommandPool *pool) {
    *pool = Handle<VkCommandPool>(new CommandPool);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo *info,
                                                        VkCommandBuffer *buffers) {
    auto *pool = Get<CommandPool>(info->commandPool);
    for (uint32_t i = 0; i < info->commandBufferCount; i++) {
        auto *cmd = new VkCommandBuffer_T;
        cmd->state = new CommandState;
        pool->buffers.push_back(cmd);
        buffers[i] = cmd;
    }
    return VK_SUCCESS;
}

namespace {

void DestroyCommandBuffer(VkCommandBuffer cmd) {
    CommandState &c = *cmd->state;
    ResetState(c);
    for (PushBuffer *buffer : c.push_buffers) {
        wgpuBindGroupRelease(buffer->group);
        wgpuBufferDestroy(buffer->gpu);
        wgpuBufferRelease(buffer->gpu);
        delete buffer;
    }
    for (UploadChunk *chunk : c.uploads) {
        wgpuBufferDestroy(chunk->gpu);
        wgpuBufferRelease(chunk->gpu);
        delete chunk;
    }
    delete cmd->state;
    delete cmd;
}

} // namespace

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice, VkCommandPool pool, uint32_t count,
                                                const VkCommandBuffer *buffers) {
    auto *object = Get<CommandPool>(pool);
    for (uint32_t i = 0; i < count; i++) {
        if (buffers[i] == nullptr) {
            continue;
        }
        std::erase(object->buffers, buffers[i]);
        DestroyCommandBuffer(buffers[i]);
    }
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice, VkCommandPool pool, const VkAllocationCallbacks *) {
    auto *object = Get<CommandPool>(pool);
    if (object == nullptr) {
        return;
    }
    for (VkCommandBuffer cmd : object->buffers) {
        DestroyCommandBuffer(cmd);
    }
    delete object;
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandPool(VkDevice, VkCommandPool pool, VkCommandPoolResetFlags) {
    for (VkCommandBuffer cmd : Get<CommandPool>(pool)->buffers) {
        ResetState(*cmd->state);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer cmd, const VkCommandBufferBeginInfo *) {
    AssertDeviceThread(__func__);
    CommandState &c = *cmd->state;
    ResetState(c);
    c.encoder = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cmd) {
    CommandState &c = *cmd->state;
    EndPass(c);
    c.finished = wgpuCommandEncoderFinish(c.encoder, nullptr);
    wgpuCommandEncoderRelease(c.encoder);
    c.encoder = nullptr;
    return VK_SUCCESS;
}

// --- Rendering ----------------------------------------------------------------------------------------------

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo *info) {
    CommandState &c = *cmd->state;
    EndPass(c);
    WGPURenderPassColorAttachment color = {};
    WGPURenderPassDepthStencilAttachment depth = {};
    WGPURenderPassDescriptor             pass = {};
    c.pass_color = WGPUTextureFormat_Undefined;
    c.pass_depth = WGPUTextureFormat_Undefined;
    c.pass_width = c.pass_height = 0;

    auto attachment_view = [&](VkImageView handle) -> std::pair<WGPUTextureView, ImageView *> {
        ImageView *view = Get<ImageView>(handle);
        if (view == nullptr) {
            return {nullptr, nullptr};
        }
        WGPUTextureView texture_view = nullptr;
        if (view->mip_count == 1) {
            texture_view = ViewOf(view);
        } else {
            texture_view = MipView(view->image, view->base_mip);
            c.temporary_views.push_back(texture_view);
        }
        c.pass_width = MipSize(view->image->width, view->base_mip);
        c.pass_height = MipSize(view->image->height, view->base_mip);
        return {texture_view, view};
    };

    if (info->colorAttachmentCount > 0 && info->pColorAttachments[0].imageView != VK_NULL_HANDLE) {
        const VkRenderingAttachmentInfo &a = info->pColorAttachments[0];
        auto [texture_view, view] = attachment_view(a.imageView);
        color.view = texture_view;
        color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        color.loadOp = a.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? WGPULoadOp_Clear : WGPULoadOp_Load;
        color.storeOp = WGPUStoreOp_Store;
        const VkClearColorValue &clear = a.clearValue.color;
        color.clearValue = {clear.float32[0], clear.float32[1], clear.float32[2], clear.float32[3]};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments = &color;
        c.pass_color = view->image->format;
    }
    const VkRenderingAttachmentInfo *d = info->pDepthAttachment;
    const VkRenderingAttachmentInfo *s = info->pStencilAttachment;
    const VkRenderingAttachmentInfo *ds = d != nullptr && d->imageView != VK_NULL_HANDLE   ? d
                                          : s != nullptr && s->imageView != VK_NULL_HANDLE ? s
                                                                                           : nullptr;
    if (ds != nullptr) {
        auto [texture_view, view] = attachment_view(ds->imageView);
        depth.view = texture_view;
        c.pass_depth = view->image->format;
        if (d != nullptr && d->imageView != VK_NULL_HANDLE) {
            depth.depthLoadOp = d->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? WGPULoadOp_Clear : WGPULoadOp_Load;
            depth.depthClearValue = d->clearValue.depthStencil.depth;
        } else {
            depth.depthLoadOp = WGPULoadOp_Load;
        }
        depth.depthStoreOp = WGPUStoreOp_Store;
        if (c.pass_depth != WGPUTextureFormat_Depth32Float) {
            if (s != nullptr && s->imageView != VK_NULL_HANDLE) {
                depth.stencilLoadOp = s->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? WGPULoadOp_Clear : WGPULoadOp_Load;
                depth.stencilClearValue = s->clearValue.depthStencil.stencil;
            } else {
                depth.stencilLoadOp = WGPULoadOp_Load;
            }
            depth.stencilStoreOp = WGPUStoreOp_Store;
        }
        pass.depthStencilAttachment = &depth;
    }
    c.pass = wgpuCommandEncoderBeginRenderPass(c.encoder, &pass);
    ForgetApplied(c);
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer cmd) { EndPass(*cmd->state); }

// --- State --------------------------------------------------------------------------------------------------

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint, VkPipeline pipeline) {
    CommandState &c = *cmd->state;
    Pipeline     *p = Get<Pipeline>(pipeline);
    c.pipeline = p;
    // What the pipeline fixes replaces the dynamic state set before.
    auto fixed = [&](VkDynamicState state) { return !p->dynamic.contains(state); };
    DynamicState &s = c.state;
    if (fixed(VK_DYNAMIC_STATE_PRIMITIVE_TOPOLOGY)) {
        s.topology = p->fixed.topology;
    }
    if (fixed(VK_DYNAMIC_STATE_CULL_MODE)) {
        s.cull = p->fixed.cull;
    }
    if (fixed(VK_DYNAMIC_STATE_FRONT_FACE)) {
        s.front_face = p->fixed.front_face;
    }
    if (fixed(VK_DYNAMIC_STATE_DEPTH_TEST_ENABLE)) {
        s.depth_test = p->fixed.depth_test;
    }
    if (fixed(VK_DYNAMIC_STATE_DEPTH_WRITE_ENABLE)) {
        s.depth_write = p->fixed.depth_write;
    }
    if (fixed(VK_DYNAMIC_STATE_DEPTH_COMPARE_OP)) {
        s.depth_compare = p->fixed.depth_compare;
    }
    if (fixed(VK_DYNAMIC_STATE_STENCIL_TEST_ENABLE)) {
        s.stencil_test = p->fixed.stencil_test;
    }
    for (int face = 0; face < 2; face++) {
        if (fixed(VK_DYNAMIC_STATE_STENCIL_OP)) {
            s.stencil[face].fail = p->fixed.stencil[face].fail;
            s.stencil[face].pass = p->fixed.stencil[face].pass;
            s.stencil[face].depth_fail = p->fixed.stencil[face].depth_fail;
            s.stencil[face].compare = p->fixed.stencil[face].compare;
        }
        if (fixed(VK_DYNAMIC_STATE_STENCIL_COMPARE_MASK)) {
            s.stencil[face].compare_mask = p->fixed.stencil[face].compare_mask;
        }
        if (fixed(VK_DYNAMIC_STATE_STENCIL_WRITE_MASK)) {
            s.stencil[face].write_mask = p->fixed.stencil[face].write_mask;
        }
        if (fixed(VK_DYNAMIC_STATE_STENCIL_REFERENCE)) {
            s.stencil[face].reference = p->fixed.stencil[face].reference;
            c.stencil_dirty = true;
        }
    }
    if (fixed(VK_DYNAMIC_STATE_BLEND_CONSTANTS)) {
        std::memcpy(s.blend_constants, p->fixed.blend_constants, sizeof s.blend_constants);
        c.blend_dirty = true;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer cmd, uint32_t, uint32_t count, const VkViewport *viewports) {
    if (count > 0) {
        cmd->state->state.viewport = viewports[0];
        cmd->state->viewport_dirty = true;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer cmd, uint32_t, uint32_t count, const VkRect2D *scissors) {
    if (count > 0) {
        cmd->state->state.scissor = scissors[0];
        cmd->state->scissor_dirty = true;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer cmd, VkCullModeFlags mode) { cmd->state->state.cull = mode; }

VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer cmd, VkFrontFace face) {
    cmd->state->state.front_face = face;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer cmd, VkPrimitiveTopology topology) {
    cmd->state->state.topology = topology;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer cmd, VkBool32 enable) {
    cmd->state->state.depth_test = enable;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer cmd, VkBool32 enable) {
    cmd->state->state.depth_write = enable;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer cmd, VkCompareOp op) {
    cmd->state->state.depth_compare = op;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer, float, float) {}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer cmd, VkBool32 enable) {
    cmd->state->state.stencil_test = enable;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilOp(VkCommandBuffer cmd, VkStencilFaceFlags faces, VkStencilOp fail,
                                             VkStencilOp pass, VkStencilOp depth_fail, VkCompareOp compare) {
    for (int face = 0; face < 2; face++) {
        if (faces & (face == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT)) {
            StencilState &s = cmd->state->state.stencil[face];
            s.fail = fail;
            s.pass = pass;
            s.depth_fail = depth_fail;
            s.compare = compare;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer cmd, VkStencilFaceFlags faces, uint32_t mask) {
    for (int face = 0; face < 2; face++) {
        if (faces & (face == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT)) {
            cmd->state->state.stencil[face].compare_mask = mask;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer cmd, VkStencilFaceFlags faces, uint32_t mask) {
    for (int face = 0; face < 2; face++) {
        if (faces & (face == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT)) {
            cmd->state->state.stencil[face].write_mask = mask;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer cmd, VkStencilFaceFlags faces, uint32_t reference) {
    for (int face = 0; face < 2; face++) {
        if (faces & (face == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT)) {
            cmd->state->state.stencil[face].reference = reference;
        }
    }
    cmd->state->stencil_dirty = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer cmd, const float constants[4]) {
    std::memcpy(cmd->state->state.blend_constants, constants, sizeof(float) * 4);
    cmd->state->blend_dirty = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout, VkShaderStageFlags,
                                              uint32_t offset, uint32_t size, const void *values) {
    if (offset + size <= 256) {
        std::memcpy(cmd->state->push.data() + offset, values, size);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint, VkPipelineLayout,
                                                   uint32_t first, uint32_t count, const VkDescriptorSet *sets,
                                                   uint32_t, const uint32_t *) {
    CommandState &c = *cmd->state;
    for (uint32_t i = 0; i < count && first + i < c.sets.size(); i++) {
        c.sets[first + i] = Get<DescriptorSet>(sets[i]);
        c.pushed_valid[first + i] = false;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetKHR(VkCommandBuffer cmd, VkPipelineBindPoint, VkPipelineLayout,
                                                     uint32_t set, uint32_t count, const VkWriteDescriptorSet *writes) {
    CommandState &c = *cmd->state;
    if (set >= c.sets.size()) {
        return;
    }
    std::vector<DescriptorEntry> &entries = c.pushed[set];
    entries.clear();
    for (uint32_t i = 0; i < count; i++) {
        const VkWriteDescriptorSet &write = writes[i];
        DescriptorEntry             entry;
        entry.binding = write.dstBinding;
        if (write.pBufferInfo != nullptr) {
            entry.buffer = Get<Buffer>(write.pBufferInfo->buffer);
            entry.offset = write.pBufferInfo->offset;
            entry.range = write.pBufferInfo->range;
        } else if (write.pImageInfo != nullptr) {
            entry.view = Get<ImageView>(write.pImageInfo->imageView);
            entry.sampler = Get<Sampler>(write.pImageInfo->sampler);
        }
        entries.push_back(entry);
    }
    c.sets[set] = nullptr;
    c.pushed_valid[set] = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindVertexBuffers(VkCommandBuffer cmd, uint32_t first, uint32_t count,
                                                  const VkBuffer *buffers, const VkDeviceSize *offsets) {
    if (first == 0 && count > 0) {
        cmd->state->vertex_buffer = Get<Buffer>(buffers[0]);
        cmd->state->vertex_offset = offsets[0];
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindIndexBuffer(VkCommandBuffer cmd, VkBuffer buffer, VkDeviceSize offset,
                                                VkIndexType type) {
    cmd->state->index_buffer = Get<Buffer>(buffer);
    cmd->state->index_offset = offset;
    cmd->state->index_type = type;
}

// --- Draws --------------------------------------------------------------------------------------------------

VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                                     uint32_t first_instance) {
    CommandState &c = *cmd->state;
    if (PrepareDraw(c, false)) {
        wgpuRenderPassEncoderDraw(c.pass, vertices, instances, first_vertex, first_instance);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer cmd, uint32_t indices, uint32_t instances,
                                            uint32_t first_index, int32_t vertex_offset, uint32_t first_instance) {
    CommandState &c = *cmd->state;
    if (PrepareDraw(c, true)) {
        wgpuRenderPassEncoderDrawIndexed(c.pass, indices, instances, first_index, vertex_offset, first_instance);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer cmd, uint32_t count, const VkClearAttachment *attachments,
                                                 uint32_t rect_count, const VkClearRect *rects) {
    CommandState &c = *cmd->state;
    if (c.pass == nullptr) {
        return;
    }
    bool  clear_color = false;
    bool  clear_depth = false;
    bool  clear_stencil = false;
    float values[8] = {};
    uint32_t stencil = 0;
    for (uint32_t i = 0; i < count; i++) {
        const VkClearAttachment &a = attachments[i];
        if (a.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
            clear_color = c.pass_color != WGPUTextureFormat_Undefined;
            std::memcpy(values, a.clearValue.color.float32, sizeof(float) * 4);
        }
        if (a.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
            clear_depth = c.pass_depth != WGPUTextureFormat_Undefined;
            values[4] = a.clearValue.depthStencil.depth;
        }
        if (a.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
            clear_stencil = c.pass_depth != WGPUTextureFormat_Undefined;
            stencil = a.clearValue.depthStencil.stencil;
        }
    }
    if (!clear_color && !clear_depth && !clear_stencil) {
        return;
    }
    WGPURenderPipeline pipeline = ClearPipeline(c.pass_color, c.pass_depth, clear_color, clear_depth, clear_stencil);
    auto [buffer, offset] = PushSlot(c, values, sizeof values);
    wgpuRenderPassEncoderSetPipeline(c.pass, pipeline);
    wgpuRenderPassEncoderSetBindGroup(c.pass, 0, buffer->group, 1, &offset);
    wgpuRenderPassEncoderSetViewport(c.pass, 0.0f, 0.0f, static_cast<float>(c.pass_width),
                                     static_cast<float>(c.pass_height), 0.0f, 1.0f);
    wgpuRenderPassEncoderSetStencilReference(c.pass, stencil & 0xFF);
    for (uint32_t r = 0; r < rect_count; r++) {
        const VkRect2D &rect = rects[r].rect;
        const int64_t   x0 = std::clamp<int64_t>(rect.offset.x, 0, c.pass_width);
        const int64_t   y0 = std::clamp<int64_t>(rect.offset.y, 0, c.pass_height);
        const int64_t   x1 = std::clamp<int64_t>(int64_t{rect.offset.x} + rect.extent.width, x0, c.pass_width);
        const int64_t   y1 = std::clamp<int64_t>(int64_t{rect.offset.y} + rect.extent.height, y0, c.pass_height);
        if (x1 > x0 && y1 > y0) {
            wgpuRenderPassEncoderSetScissorRect(c.pass, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                                                static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0));
            wgpuRenderPassEncoderDraw(c.pass, 3, 1, 0, 0);
        }
    }
    // The renderer's own state is set again at its next draw.
    ForgetApplied(c);
}

// --- Copies, blits and clears outside rendering -------------------------------------------------------------

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo *) {}

VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t) {}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2(VkCommandBuffer, VkPipelineStageFlags2, VkQueryPool, uint32_t) {}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, uint32_t count,
                                           const VkBufferCopy *regions) {
    CommandState &c = *cmd->state;
    EndPass(c);
    Buffer *source = Get<Buffer>(src);
    Buffer *destination = Get<Buffer>(dst);
    for (uint32_t i = 0; i < count; i++) {
        const VkBufferCopy &r = regions[i];
        if (source->gpu != nullptr && destination->gpu != nullptr && r.srcOffset % 4 == 0 && r.dstOffset % 4 == 0 &&
            r.size % 4 == 0) {
            wgpuCommandEncoderCopyBufferToBuffer(c.encoder, source->gpu, r.srcOffset, destination->gpu, r.dstOffset,
                                                 r.size);
        } else if (source->shadow != nullptr && destination->gpu != nullptr && r.dstOffset % 4 == 0) {
            // Through an upload chunk, which takes any size once padded to 4 bytes; the destination's WebGPU
            // buffer is padded to 4 bytes as well.
            const uint64_t padded = (r.size + 3) & ~uint64_t{3};
            auto [chunk, offset] = UploadSpace(c, padded);
            std::memcpy(chunk->data.data() + offset, source->shadow + r.srcOffset, r.size);
            wgpuCommandEncoderCopyBufferToBuffer(c.encoder, chunk->gpu, offset, destination->gpu, r.dstOffset, padded);
        } else {
            Warn("a buffer copy WebGPU cannot make was dropped");
        }
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer src, VkImage dst, VkImageLayout,
                                                  uint32_t count, const VkBufferImageCopy *regions) {
    CommandState &c = *cmd->state;
    EndPass(c);
    Buffer *source = Get<Buffer>(src);
    Image  *image = Get<Image>(dst);
    const uint32_t texel = TexelBytes(image->vk_format);
    for (uint32_t i = 0; i < count; i++) {
        const VkBufferImageCopy &r = regions[i];
        const uint32_t width = r.imageExtent.width;
        const uint32_t height = r.imageExtent.height;
        const uint32_t row = (r.bufferRowLength != 0 ? r.bufferRowLength : width) * texel;
        const uint32_t rows_per_image = r.bufferImageHeight != 0 ? r.bufferImageHeight : height;
        WGPUTexelCopyTextureInfo destination = {};
        destination.texture = image->texture;
        destination.mipLevel = r.imageSubresource.mipLevel;
        destination.origin = {static_cast<uint32_t>(r.imageOffset.x), static_cast<uint32_t>(r.imageOffset.y), 0};
        destination.aspect = WGPUTextureAspect_All;
        WGPUExtent3D            size = {width, height, 1};
        WGPUTexelCopyBufferInfo from = {};
        if (row % 256 == 0 && r.bufferOffset % texel == 0 && source->gpu != nullptr) {
            from.buffer = source->gpu;
            from.layout.offset = r.bufferOffset;
            from.layout.bytesPerRow = row;
            from.layout.rowsPerImage = rows_per_image;
        } else if (source->shadow != nullptr) {
            const uint32_t padded = AlignUp(width * texel, 256);
            auto [chunk, offset] = UploadSpace(c, uint64_t{padded} * height);
            for (uint32_t y = 0; y < height; y++) {
                std::memcpy(chunk->data.data() + offset + uint64_t{y} * padded,
                            source->shadow + r.bufferOffset + uint64_t{y} * row, uint64_t{width} * texel);
            }
            from.buffer = chunk->gpu;
            from.layout.offset = offset;
            from.layout.bytesPerRow = padded;
            from.layout.rowsPerImage = height;
        } else {
            Warn("a texture upload WebGPU cannot make was dropped");
            continue;
        }
        wgpuCommandEncoderCopyBufferToTexture(c.encoder, &from, &destination, &size);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage src, VkImageLayout, VkBuffer dst,
                                                  uint32_t count, const VkBufferImageCopy *regions) {
    CommandState &c = *cmd->state;
    EndPass(c);
    Image  *image = Get<Image>(src);
    Buffer *buffer = Get<Buffer>(dst);
    for (uint32_t i = 0; i < count; i++) {
        const VkBufferImageCopy &r = regions[i];
        const bool     depth = (r.imageSubresource.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        const uint32_t texel = depth ? 4 : TexelBytes(image->vk_format);
        const uint32_t width = r.imageExtent.width;
        const uint32_t height = r.imageExtent.height;
        Readback       readback;
        readback.row = (r.bufferRowLength != 0 ? r.bufferRowLength : width) * texel;
        readback.padded_row = AlignUp(width * texel, 256);
        readback.rows = height;
        readback.dst = buffer;
        readback.dst_offset = r.bufferOffset;
        WGPUBufferDescriptor descriptor = {};
        descriptor.usage = WGPUBufferUsage_MapRead | WGPUBufferUsage_CopyDst;
        descriptor.size = uint64_t{readback.padded_row} * height;
        readback.staging = wgpuDeviceCreateBuffer(g.device, &descriptor);
        WGPUTexelCopyTextureInfo source = {};
        source.texture = image->texture;
        source.mipLevel = r.imageSubresource.mipLevel;
        source.origin = {static_cast<uint32_t>(r.imageOffset.x), static_cast<uint32_t>(r.imageOffset.y), 0};
        source.aspect = depth ? WGPUTextureAspect_DepthOnly : WGPUTextureAspect_All;
        WGPUTexelCopyBufferInfo destination = {};
        destination.buffer = readback.staging;
        destination.layout.bytesPerRow = readback.padded_row;
        destination.layout.rowsPerImage = height;
        WGPUExtent3D size = {width, height, 1};
        wgpuCommandEncoderCopyTextureToBuffer(c.encoder, &source, &destination, &size);
        // Only the bytes of the row the image covers reach the buffer, as Vulkan copies them.
        readback.row = std::min(readback.row, width * texel);
        c.readbacks.push_back(readback);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer cmd, VkImage src, VkImageLayout, VkImage dst, VkImageLayout,
                                          uint32_t count, const VkImageCopy *regions) {
    CommandState &c = *cmd->state;
    EndPass(c);
    Image *source = Get<Image>(src);
    Image *destination = Get<Image>(dst);
    for (uint32_t i = 0; i < count; i++) {
        const VkImageCopy       &r = regions[i];
        WGPUTexelCopyTextureInfo from = {};
        from.texture = source->texture;
        from.mipLevel = r.srcSubresource.mipLevel;
        from.origin = {static_cast<uint32_t>(r.srcOffset.x), static_cast<uint32_t>(r.srcOffset.y), 0};
        from.aspect = WGPUTextureAspect_All;
        WGPUTexelCopyTextureInfo to = {};
        to.texture = destination->texture;
        to.mipLevel = r.dstSubresource.mipLevel;
        to.origin = {static_cast<uint32_t>(r.dstOffset.x), static_cast<uint32_t>(r.dstOffset.y), 0};
        to.aspect = WGPUTextureAspect_All;
        WGPUExtent3D size = {r.extent.width, r.extent.height, 1};
        wgpuCommandEncoderCopyTextureToTexture(c.encoder, &from, &to, &size);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer cmd, VkImage src, VkImageLayout, VkImage dst, VkImageLayout,
                                          uint32_t count, const VkImageBlit *regions, VkFilter filter) {
    CommandState &c = *cmd->state;
    EndPass(c);
    Image *source = Get<Image>(src);
    Image *destination = Get<Image>(dst);
    if (source->texture == nullptr || destination->texture == nullptr || IsDepthFormat(source->vk_format)) {
        Warn("a blit WebGPU cannot make was dropped");
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        const VkImageBlit &r = regions[i];
        const int32_t sx0 = r.srcOffsets[0].x, sy0 = r.srcOffsets[0].y, sx1 = r.srcOffsets[1].x, sy1 = r.srcOffsets[1].y;
        const int32_t dx0 = r.dstOffsets[0].x, dy0 = r.dstOffsets[0].y, dx1 = r.dstOffsets[1].x, dy1 = r.dstOffsets[1].y;
        const uint32_t src_mip = r.srcSubresource.mipLevel;
        const uint32_t dst_mip = r.dstSubresource.mipLevel;
        // A copy where nothing is scaled or flipped and the formats agree.
        if (sx1 - sx0 == dx1 - dx0 && sy1 - sy0 == dy1 - dy0 && sx1 > sx0 && sy1 > sy0 &&
            source->format == destination->format) {
            WGPUTexelCopyTextureInfo from = {};
            from.texture = source->texture;
            from.mipLevel = src_mip;
            from.origin = {static_cast<uint32_t>(sx0), static_cast<uint32_t>(sy0), 0};
            from.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyTextureInfo to = {};
            to.texture = destination->texture;
            to.mipLevel = dst_mip;
            to.origin = {static_cast<uint32_t>(dx0), static_cast<uint32_t>(dy0), 0};
            to.aspect = WGPUTextureAspect_All;
            WGPUExtent3D size = {static_cast<uint32_t>(sx1 - sx0), static_cast<uint32_t>(sy1 - sy0), 1};
            wgpuCommandEncoderCopyTextureToTexture(c.encoder, &from, &to, &size);
            continue;
        }
        // Drawn: the destination rect as the viewport, the source rect as texture coordinates, where a flip on
        // either side swaps the coordinates' ends.
        const float src_w = static_cast<float>(MipSize(source->width, src_mip));
        const float src_h = static_cast<float>(MipSize(source->height, src_mip));
        float       u0 = sx0 / src_w, u1 = sx1 / src_w, v0 = sy0 / src_h, v1 = sy1 / src_h;
        if (dx1 < dx0) {
            std::swap(u0, u1);
        }
        if (dy1 < dy0) {
            std::swap(v0, v1);
        }
        const int32_t x = std::min(dx0, dx1), y = std::min(dy0, dy1);
        const int32_t w = std::abs(dx1 - dx0), h = std::abs(dy1 - dy0);
        if (w == 0 || h == 0) {
            continue;
        }
        BlitPipelines  &blits = Blits();
        WGPUTextureView source_view = MipView(source, src_mip);
        WGPUTextureView target_view = MipView(destination, dst_mip);
        c.temporary_views.push_back(source_view);
        c.temporary_views.push_back(target_view);
        WGPUBindGroupEntry entries[2] = {};
        entries[0].binding = 0;
        entries[0].textureView = source_view;
        entries[1].binding = 1;
        entries[1].sampler = filter == VK_FILTER_LINEAR ? blits.linear : blits.nearest;
        WGPUBindGroupDescriptor group = {};
        group.layout = blits.textures;
        group.entryCount = 2;
        group.entries = entries;
        WGPUBindGroup textures = wgpuDeviceCreateBindGroup(g.device, &group);
        c.temporary_groups.push_back(textures);
        const float uv[4] = {u0, v0, u1, v1};
        auto [buffer, offset] = PushSlot(c, uv, sizeof uv);

        WGPURenderPassColorAttachment color = {};
        color.view = target_view;
        color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
        color.loadOp = WGPULoadOp_Load;
        color.storeOp = WGPUStoreOp_Store;
        WGPURenderPassDescriptor pass = {};
        pass.colorAttachmentCount = 1;
        pass.colorAttachments = &color;
        WGPURenderPassEncoder encoder = wgpuCommandEncoderBeginRenderPass(c.encoder, &pass);
        wgpuRenderPassEncoderSetPipeline(encoder, BlitPipeline(destination->format));
        wgpuRenderPassEncoderSetBindGroup(encoder, 0, textures, 0, nullptr);
        wgpuRenderPassEncoderSetBindGroup(encoder, 1, buffer->group, 1, &offset);
        const int64_t dst_w = MipSize(destination->width, dst_mip);
        const int64_t dst_h = MipSize(destination->height, dst_mip);
        const int64_t x0 = std::clamp<int64_t>(x, 0, dst_w);
        const int64_t y0 = std::clamp<int64_t>(y, 0, dst_h);
        const int64_t x1 = std::clamp<int64_t>(int64_t{x} + w, x0, dst_w);
        const int64_t y1 = std::clamp<int64_t>(int64_t{y} + h, y0, dst_h);
        wgpuRenderPassEncoderSetViewport(encoder, static_cast<float>(x), static_cast<float>(y), static_cast<float>(w),
                                         static_cast<float>(h), 0.0f, 1.0f);
        wgpuRenderPassEncoderSetScissorRect(encoder, static_cast<uint32_t>(x0), static_cast<uint32_t>(y0),
                                            static_cast<uint32_t>(x1 - x0), static_cast<uint32_t>(y1 - y0));
        wgpuRenderPassEncoderDraw(encoder, 4, 1, 0, 0);
        wgpuRenderPassEncoderEnd(encoder);
        wgpuRenderPassEncoderRelease(encoder);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer cmd, VkImage image, VkImageLayout,
                                                const VkClearColorValue *value, uint32_t count,
                                                const VkImageSubresourceRange *ranges) {
    CommandState &c = *cmd->state;
    EndPass(c);
    Image *object = Get<Image>(image);
    if (object->texture == nullptr) {
        return;
    }
    for (uint32_t i = 0; i < count; i++) {
        const VkImageSubresourceRange &range = ranges[i];
        const uint32_t levels = range.levelCount == VK_REMAINING_MIP_LEVELS ? object->mips - range.baseMipLevel
                                                                            : range.levelCount;
        for (uint32_t mip = range.baseMipLevel; mip < range.baseMipLevel + levels; mip++) {
            WGPUTextureView view = MipView(object, mip);
            c.temporary_views.push_back(view);
            WGPURenderPassColorAttachment color = {};
            color.view = view;
            color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            color.loadOp = WGPULoadOp_Clear;
            color.storeOp = WGPUStoreOp_Store;
            color.clearValue = {value->float32[0], value->float32[1], value->float32[2], value->float32[3]};
            WGPURenderPassDescriptor pass = {};
            pass.colorAttachmentCount = 1;
            pass.colorAttachments = &color;
            WGPURenderPassEncoder encoder = wgpuCommandEncoderBeginRenderPass(c.encoder, &pass);
            wgpuRenderPassEncoderEnd(encoder);
            wgpuRenderPassEncoderRelease(encoder);
        }
    }
}

} // extern "C"

// The transient allocator's spans (gfx/memory.cpp): what the CPU writes there is uploaded at the next submit.
extern "C" void vkWebGpuMarkWritten(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
    if (Buffer *object = Get<Buffer>(buffer)) {
        MarkDirty(object, offset, size);
    }
}

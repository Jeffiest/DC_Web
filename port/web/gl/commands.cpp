// Vulkan on WebGL2 (layer.hpp): command pools and buffers, every vkCmd the renderer records, and their replay.
//
// A command buffer records its commands with the state each draw needs; a submission replays them. The replay
// runs twice over the commands: first what happens in wasm memory (buffer copies) and each draw's uniform blocks,
// its push constants and the storage buffers' records it reads, gathered into one uniform buffer; then the GL
// calls, with that buffer given to WebGL once. GL state is set only where it differs from what was set last.
//
// Rendering is a framebuffer of the attachments; copies and blits are framebuffer blits, image clears
// glClearBuffer. Depth, which WebGL does not read back, is drawn into a colour target as its bytes first.

#include "layer.hpp"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <tuple>

namespace vkgl {

namespace {

constexpr size_t   kFlagsOffset = 44; // PushConstants::flags
constexpr uint32_t kOpaqueTargetFlag = 8;
// PushConstants::mesh_index, prog0_index and palette_index: the records of storage set 1's bindings 0-2.
constexpr size_t   kRecordIndexOffset[kBlockCount] = {0, 60, 64, 68};
constexpr uint32_t kPushBytes = kBlockBytes[kPushBlock];
// The storage buffers' set, as the renderer's shaders declare it.
constexpr uint32_t kRecordSet = 1;

enum class OpType : uint8_t {
    BeginRendering,
    EndRendering,
    Draw,
    ClearAttachments,
    CopyBuffer,
    CopyBufferToImage,
    CopyImageToBuffer,
    CopyImage,
    BlitImage,
    ClearColorImage,
};

struct Op {
    OpType   type;
    uint32_t index; // into the vector of its type's operations
};

struct Attachment {
    Image             *image = nullptr;
    uint32_t           mip = 0;
    bool               clear = false;
    bool               discard = false; // not stored
    VkClearValue       value = {};
};

struct BeginOp {
    Attachment color;
    Attachment depth;
    bool       clear_stencil = false;
    uint32_t   stencil = 0;
    VkRect2D   area = {};
};

// A set as a draw sees it: a bound set, or pushed entries in the command buffer's pool.
struct SetRef {
    DescriptorSet *set = nullptr;
    uint32_t       begin = 0;
    uint32_t       count = 0;
};

struct DrawOp {
    Pipeline              *pipeline = nullptr;
    uint32_t               state = 0; // into states
    uint32_t               push = 0;  // into pushes
    std::array<SetRef, 4>  sets = {};
    Buffer                *vertex = nullptr;
    VkDeviceSize           vertex_offset = 0;
    Buffer                *index = nullptr;
    VkDeviceSize           index_offset = 0;
    VkIndexType            index_type = VK_INDEX_TYPE_UINT32;
    bool                   indexed = false;
    uint32_t               count = 0;
    uint32_t               instances = 1;
    uint32_t               first = 0;
    int32_t                base_vertex = 0;
    // Where the replay put the draw's uniform blocks.
    std::array<uint32_t, kBlockCount> block_offset = {};
};

struct ClearOp {
    bool     color = false;
    bool     depth = false;
    bool     stencil = false;
    float    color_value[4] = {};
    float    depth_value = 0.0f;
    uint32_t stencil_value = 0;
    uint32_t rect_begin = 0;
    uint32_t rect_count = 0;
};

template <typename Region> struct CopyOp {
    void    *src = nullptr;
    void    *dst = nullptr;
    uint32_t begin = 0;
    uint32_t count = 0;
    VkFilter filter = VK_FILTER_NEAREST;
};

struct ClearColorOp {
    Image            *image = nullptr;
    VkClearColorValue value = {};
    uint32_t          begin = 0;
    uint32_t          count = 0;
};

using PushBlock = std::array<uint8_t, kPushBytes>;

} // namespace

struct CommandState {
    // Vulkan's state while recording.
    Pipeline                *pipeline = nullptr;
    DynamicState             state;
    bool                     state_changed = true;
    Buffer                  *vertex_buffer = nullptr;
    VkDeviceSize             vertex_offset = 0;
    Buffer                  *index_buffer = nullptr;
    VkDeviceSize             index_offset = 0;
    VkIndexType              index_type = VK_INDEX_TYPE_UINT32;
    std::array<uint8_t, 256> push = {};
    bool                     push_changed = true;
    std::array<SetRef, 4>    sets = {};
    // The pass's attachments' stores, for EndRendering.
    BeginOp                  *open_pass = nullptr;

    // What was recorded.
    std::vector<Op>                        ops;
    std::vector<BeginOp>                   begins;
    std::vector<DrawOp>                    draws;
    std::vector<DynamicState>              states;
    std::vector<PushBlock>                 pushes;
    std::vector<DescriptorEntry>           pushed_entries;
    std::vector<ClearOp>                   clears;
    std::vector<VkClearRect>               clear_rects;
    std::vector<CopyOp<VkBufferCopy>>      buffer_copies;
    std::vector<VkBufferCopy>              buffer_regions;
    std::vector<CopyOp<VkBufferImageCopy>> uploads;
    std::vector<CopyOp<VkBufferImageCopy>> readbacks;
    std::vector<VkBufferImageCopy>         buffer_image_regions;
    std::vector<CopyOp<VkImageCopy>>       image_copies;
    std::vector<VkImageCopy>               image_regions;
    std::vector<CopyOp<VkImageBlit>>       blits;
    std::vector<VkImageBlit>               blit_regions;
    std::vector<ClearColorOp>              color_clears;
    std::vector<VkImageSubresourceRange>   clear_ranges;
};

namespace {

struct CommandPool {
    std::vector<VkCommandBuffer> buffers;
};

void ResetState(CommandState &c) {
    c.pipeline = nullptr;
    c.state = DynamicState{};
    c.state_changed = true;
    c.vertex_buffer = c.index_buffer = nullptr;
    c.push = {};
    c.push_changed = true;
    c.sets = {};
    c.open_pass = nullptr;
    c.ops.clear();
    c.begins.clear();
    c.draws.clear();
    c.states.clear();
    c.pushes.clear();
    c.pushed_entries.clear();
    c.clears.clear();
    c.clear_rects.clear();
    c.buffer_copies.clear();
    c.buffer_regions.clear();
    c.uploads.clear();
    c.readbacks.clear();
    c.buffer_image_regions.clear();
    c.image_copies.clear();
    c.image_regions.clear();
    c.blits.clear();
    c.blit_regions.clear();
    c.color_clears.clear();
    c.clear_ranges.clear();
}

template <typename T> uint32_t Append(std::vector<T> &items, const T &item) {
    items.push_back(item);
    return static_cast<uint32_t>(items.size() - 1);
}

template <typename T> uint32_t AppendAll(std::vector<T> &items, const T *first, uint32_t count) {
    const auto begin = static_cast<uint32_t>(items.size());
    items.insert(items.end(), first, first + count);
    return begin;
}

// --- Vulkan enums as GL's -----------------------------------------------------------------------------------

GLenum Compare(VkCompareOp op) {
    switch (op) {
        case VK_COMPARE_OP_NEVER:
            return GL_NEVER;
        case VK_COMPARE_OP_LESS:
            return GL_LESS;
        case VK_COMPARE_OP_EQUAL:
            return GL_EQUAL;
        case VK_COMPARE_OP_LESS_OR_EQUAL:
            return GL_LEQUAL;
        case VK_COMPARE_OP_GREATER:
            return GL_GREATER;
        case VK_COMPARE_OP_NOT_EQUAL:
            return GL_NOTEQUAL;
        case VK_COMPARE_OP_GREATER_OR_EQUAL:
            return GL_GEQUAL;
        default:
            return GL_ALWAYS;
    }
}

GLenum StencilOperation(VkStencilOp op) {
    switch (op) {
        case VK_STENCIL_OP_ZERO:
            return GL_ZERO;
        case VK_STENCIL_OP_REPLACE:
            return GL_REPLACE;
        case VK_STENCIL_OP_INCREMENT_AND_CLAMP:
            return GL_INCR;
        case VK_STENCIL_OP_DECREMENT_AND_CLAMP:
            return GL_DECR;
        case VK_STENCIL_OP_INVERT:
            return GL_INVERT;
        case VK_STENCIL_OP_INCREMENT_AND_WRAP:
            return GL_INCR_WRAP;
        case VK_STENCIL_OP_DECREMENT_AND_WRAP:
            return GL_DECR_WRAP;
        default:
            return GL_KEEP;
    }
}

GLenum BlendFactor(VkBlendFactor factor) {
    switch (factor) {
        case VK_BLEND_FACTOR_ZERO:
            return GL_ZERO;
        case VK_BLEND_FACTOR_ONE:
            return GL_ONE;
        case VK_BLEND_FACTOR_SRC_COLOR:
            return GL_SRC_COLOR;
        case VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR:
            return GL_ONE_MINUS_SRC_COLOR;
        case VK_BLEND_FACTOR_DST_COLOR:
            return GL_DST_COLOR;
        case VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR:
            return GL_ONE_MINUS_DST_COLOR;
        case VK_BLEND_FACTOR_SRC_ALPHA:
            return GL_SRC_ALPHA;
        case VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA:
            return GL_ONE_MINUS_SRC_ALPHA;
        case VK_BLEND_FACTOR_DST_ALPHA:
            return GL_DST_ALPHA;
        case VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA:
            return GL_ONE_MINUS_DST_ALPHA;
        case VK_BLEND_FACTOR_CONSTANT_COLOR:
            return GL_CONSTANT_COLOR;
        case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR:
            return GL_ONE_MINUS_CONSTANT_COLOR;
        case VK_BLEND_FACTOR_CONSTANT_ALPHA:
            return GL_CONSTANT_ALPHA;
        case VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA:
            return GL_ONE_MINUS_CONSTANT_ALPHA;
        case VK_BLEND_FACTOR_SRC_ALPHA_SATURATE:
            return GL_SRC_ALPHA_SATURATE;
        default:
            return GL_ONE;
    }
}

GLenum BlendOperation(VkBlendOp op) {
    switch (op) {
        case VK_BLEND_OP_SUBTRACT:
            return GL_FUNC_SUBTRACT;
        case VK_BLEND_OP_REVERSE_SUBTRACT:
            return GL_FUNC_REVERSE_SUBTRACT;
        case VK_BLEND_OP_MIN:
            return GL_MIN;
        case VK_BLEND_OP_MAX:
            return GL_MAX;
        default:
            return GL_FUNC_ADD;
    }
}

GLenum Topology(VkPrimitiveTopology topology) {
    switch (topology) {
        case VK_PRIMITIVE_TOPOLOGY_POINT_LIST:
            return GL_POINTS;
        case VK_PRIMITIVE_TOPOLOGY_LINE_LIST:
            return GL_LINES;
        case VK_PRIMITIVE_TOPOLOGY_LINE_STRIP:
            return GL_LINE_STRIP;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP:
            return GL_TRIANGLE_STRIP;
        case VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN:
            return GL_TRIANGLE_FAN;
        default:
            return GL_TRIANGLES;
    }
}

// A vertex attribute's components, type, whether it is normalised and whether the shader reads it as integers.
struct AttributeFormat {
    GLint     size;
    GLenum    type;
    GLboolean normalized;
    bool      integer;
};

AttributeFormat FormatOfAttribute(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R32G32B32A32_SFLOAT:
            return {4, GL_FLOAT, GL_FALSE, false};
        case VK_FORMAT_R32G32B32_SFLOAT:
            return {3, GL_FLOAT, GL_FALSE, false};
        case VK_FORMAT_R32G32_SFLOAT:
            return {2, GL_FLOAT, GL_FALSE, false};
        case VK_FORMAT_R32_SFLOAT:
            return {1, GL_FLOAT, GL_FALSE, false};
        case VK_FORMAT_R8G8B8A8_UNORM:
            return {4, GL_UNSIGNED_BYTE, GL_TRUE, false};
        case VK_FORMAT_R8_UNORM:
            return {1, GL_UNSIGNED_BYTE, GL_TRUE, false};
        case VK_FORMAT_R8G8B8A8_UINT:
            return {4, GL_UNSIGNED_BYTE, GL_FALSE, true};
        default:
            Fatal("no WebGL vertex format for VkFormat %d", static_cast<int>(format));
    }
}

// --- GL state, as last set ----------------------------------------------------------------------------------

struct GlState {
    bool known = false;
    // Objects.
    GLuint program = 0;
    GLuint framebuffer = 0;
    GLuint textures[3] = {};
    GLuint samplers[3] = {};
    GLint  block_offset[kBlockCount] = {};
    GLuint vertex_buffer = 0;
    GLintptr vertex_base = 0;
    const Pipeline *vertex_layout = nullptr;
    uint32_t enabled_attributes = 0;
    GLuint index_buffer = 0;
    // Fixed functions.
    GLint     viewport[4] = {};
    float     depth_range[2] = {};
    GLint     scissor[4] = {};
    bool      cull = false;
    GLenum    cull_face = 0;
    GLenum    front_face = 0;
    bool      depth_test = false;
    GLenum    depth_func = 0;
    bool      depth_mask = false;
    bool      stencil_test = false;
    GLenum    stencil[2][4] = {}; // func, fail, depth fail, pass per face
    GLuint    stencil_ref = 0;
    GLuint    stencil_read = 0;
    GLuint    stencil_write = 0;
    bool      blend = false;
    GLenum    blend_func[4] = {};
    GLenum    blend_equation[2] = {};
    float     blend_color[4] = {};
    GLboolean color_mask[4] = {};
};

GlState g_gl;
GLuint  g_uniforms = 0;
GLuint  g_vertex_array = 0;
GLuint  g_empty_vertex_array = 0;
GLuint  g_filler_texture = 0;

void EnsureObjects() {
    if (g_uniforms != 0) {
        return;
    }
    glGenBuffers(1, &g_uniforms);
    glGenVertexArrays(1, &g_vertex_array);
    glGenVertexArrays(1, &g_empty_vertex_array);
    glGenTextures(1, &g_filler_texture);
    glBindTexture(GL_TEXTURE_2D, g_filler_texture);
    glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, 1, 1);
    const uint8_t black[4] = {};
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, 1, 1, GL_RGBA, GL_UNSIGNED_BYTE, black);
    glBindTexture(GL_TEXTURE_2D, 0);
    // A shader's integer attribute that no array feeds reads the generic value, which must be integer too.
    glVertexAttribI4ui(5, 0, 0, 0, 0);
}

// Everything is set afresh at the next draw: what set it last is unknown.
void ForgetGl() {
    g_gl = GlState{};
}

template <typename T, size_t N> bool Differs(T (&have)[N], std::initializer_list<T> want) {
    if (g_gl.known && std::equal(want.begin(), want.end(), have)) {
        return false;
    }
    std::copy(want.begin(), want.end(), have);
    return true;
}

void SetEnabled(GLenum capability, bool &have, bool want) {
    if (g_gl.known && have == want) {
        return;
    }
    have = want;
    (want ? glEnable : glDisable)(capability);
}

void BindDrawFramebuffer(GLuint framebuffer) {
    if (!g_gl.known || g_gl.framebuffer != framebuffer) {
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
        g_gl.framebuffer = framebuffer;
    }
}

void SetScissor(GLint x, GLint y, GLint width, GLint height) {
    if (Differs(g_gl.scissor, {x, y, width, height})) {
        glScissor(x, y, width, height);
    }
}

void SetColorMask(bool r, bool gr, bool b, bool a) {
    if (Differs(g_gl.color_mask, {GLboolean(r), GLboolean(gr), GLboolean(b), GLboolean(a)})) {
        glColorMask(r, gr, b, a);
    }
}

void SetDepthMask(bool mask) {
    if (!g_gl.known || g_gl.depth_mask != mask) {
        glDepthMask(mask);
        g_gl.depth_mask = mask;
    }
}

void SetStencilWrite(GLuint mask) {
    if (!g_gl.known || g_gl.stencil_write != mask) {
        glStencilMask(mask);
        g_gl.stencil_write = mask;
    }
}

// --- The replay ---------------------------------------------------------------------------------------------

// The pass the replay is in.
struct Pass {
    uint32_t       width = 0;
    uint32_t       height = 0;
    Image         *depth = nullptr;
    const BeginOp *begin = nullptr;
};

const DescriptorEntry *FindEntry(const CommandState &c, const SetRef &ref, uint32_t binding) {
    if (ref.set != nullptr) {
        for (const DescriptorEntry &entry : ref.set->entries) {
            if (entry.binding == binding) {
                return &entry;
            }
        }
        return nullptr;
    }
    for (uint32_t i = ref.begin; i < ref.begin + ref.count; i++) {
        if (c.pushed_entries[i].binding == binding) {
            return &c.pushed_entries[i];
        }
    }
    return nullptr;
}

Sampler *SamplerAt(const CommandState &c, const DrawOp &d, uint32_t binding) {
    if (!d.pipeline->layout->sets.empty()) {
        for (const LayoutBinding &b : d.pipeline->layout->sets[0]->bindings) {
            if (b.binding == binding && b.immutable_sampler != nullptr) {
                return b.immutable_sampler;
            }
        }
    }
    const DescriptorEntry *entry = FindEntry(c, d.sets[0], binding);
    return entry != nullptr ? entry->sampler : nullptr;
}

// The uniform blocks of a submission: zeros at the start for what a draw has no record of, then each draw's.
struct Uniforms {
    std::vector<uint8_t> data;
    uint32_t             alignment = 256;
    // Records already placed, by where they come from, and the last push block placed.
    std::map<std::tuple<const uint8_t *, uint32_t>, uint32_t> records;
    const PushBlock *last_push = nullptr;
    bool             last_push_factor = false;
    uint32_t         last_push_offset = 0;

    uint32_t Place(const void *bytes, uint32_t size) {
        const auto offset = static_cast<uint32_t>((data.size() + alignment - 1) / alignment * alignment);
        data.resize(offset + size);
        std::memcpy(data.data() + offset, bytes, size);
        return offset;
    }
};

void PrepareDraw(CommandState &c, DrawOp &d, Uniforms &uniforms) {
    Pipeline *p = d.pipeline;
    if (p->program == nullptr) {
        p->program = ProgramFor(p->vertex, p->vertex_constants, p->fragment, p->fragment_constants);
    }
    const Program  *program = p->program;
    const PushBlock &push = c.pushes[d.push];
    d.block_offset = {};
    if (program->reads[kPushBlock]) {
        if (uniforms.last_push != &push || uniforms.last_push_factor != p->factor_in_alpha) {
            PushBlock block = push;
            if (p->factor_in_alpha) {
                uint32_t flags = 0;
                std::memcpy(&flags, block.data() + kFlagsOffset, sizeof flags);
                flags &= ~kOpaqueTargetFlag;
                std::memcpy(block.data() + kFlagsOffset, &flags, sizeof flags);
            }
            uniforms.last_push_offset = uniforms.Place(block.data(), kPushBytes);
            uniforms.last_push = &push;
            uniforms.last_push_factor = p->factor_in_alpha;
        }
        d.block_offset[kPushBlock] = uniforms.last_push_offset;
    }
    for (uint32_t block = kMeshBlock; block < kBlockCount; block++) {
        if (!program->reads[block]) {
            continue;
        }
        const DescriptorEntry *entry = FindEntry(c, d.sets[kRecordSet], block - kMeshBlock);
        if (entry == nullptr || entry->buffer == nullptr || entry->buffer->shadow == nullptr) {
            continue; // the zeros at the start
        }
        uint32_t index = 0;
        std::memcpy(&index, push.data() + kRecordIndexOffset[block], sizeof index);
        const VkDeviceSize at = entry->offset + VkDeviceSize{index} * kBlockBytes[block];
        if (at + kBlockBytes[block] > entry->buffer->size) {
            continue;
        }
        const uint8_t *source = entry->buffer->shadow + at;
        const auto     key = std::make_tuple(source, block);
        auto           it = uniforms.records.find(key);
        if (it == uniforms.records.end()) {
            it = uniforms.records.emplace(key, uniforms.Place(source, kBlockBytes[block])).first;
        }
        d.block_offset[block] = it->second;
    }
}

// A buffer copy in wasm memory, where the replay's first run over the commands reads records as they stand at
// each draw. The destination's GL buffers take the copy in the second run, at its place among the draws.
void CopyBuffers(const CommandState &c, const CopyOp<VkBufferCopy> &op) {
    auto *source = static_cast<Buffer *>(op.src);
    auto *destination = static_cast<Buffer *>(op.dst);
    if (source->shadow == nullptr || destination->shadow == nullptr) {
        Warn("a buffer copy without memory was dropped");
        return;
    }
    for (uint32_t i = op.begin; i < op.begin + op.count; i++) {
        const VkBufferCopy &r = c.buffer_regions[i];
        if (r.srcOffset + r.size > source->size || r.dstOffset + r.size > destination->size) {
            continue;
        }
        std::memmove(destination->shadow + r.dstOffset, source->shadow + r.srcOffset, r.size);
    }
}

// The copy into the destination's GL buffers, from the source's bytes, which the submission does not change.
void CopyIntoGlBuffers(const CommandState &c, const CopyOp<VkBufferCopy> &op) {
    auto *source = static_cast<Buffer *>(op.src);
    auto *destination = static_cast<Buffer *>(op.dst);
    if (source->shadow == nullptr || destination->shadow == nullptr) {
        return;
    }
    for (uint32_t i = op.begin; i < op.begin + op.count; i++) {
        const VkBufferCopy &r = c.buffer_regions[i];
        if (r.srcOffset + r.size > source->size || r.dstOffset + r.size > destination->size) {
            Warn("a buffer copy out of bounds was dropped");
            continue;
        }
        for (GLenum target : {GL_ARRAY_BUFFER, GL_ELEMENT_ARRAY_BUFFER}) {
            const GlCopy &copy = target == GL_ARRAY_BUFFER ? destination->vertex : destination->index;
            if (copy.name == 0) {
                continue;
            }
            glBindBuffer(target, copy.name);
            glBufferSubData(target, static_cast<GLintptr>(r.dstOffset), static_cast<GLsizeiptr>(r.size),
                            source->shadow + r.srcOffset);
            if (target == GL_ELEMENT_ARRAY_BUFFER) {
                g_gl.index_buffer = copy.name; // the vertex array's
            }
        }
    }
}

void BindTexture(GLuint unit, GLuint texture, GLuint sampler) {
    if (!g_gl.known || g_gl.textures[unit] != texture) {
        glActiveTexture(GL_TEXTURE0 + unit);
        glBindTexture(GL_TEXTURE_2D, texture);
        g_gl.textures[unit] = texture;
    }
    if (!g_gl.known || g_gl.samplers[unit] != sampler) {
        glBindSampler(unit, sampler);
        g_gl.samplers[unit] = sampler;
    }
}

// The view's mips as the texture's sampled range, which GL keeps on the texture.
void UseMips(GLuint unit, const ImageView *view) {
    Image        *image = view->image;
    const int32_t base = static_cast<int32_t>(view->base_mip);
    const int32_t max = base + static_cast<int32_t>(view->mip_count) - 1;
    if (image->base_level == base && image->max_level == max) {
        return;
    }
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, image->texture);
    g_gl.textures[unit] = image->texture;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, base);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, max);
    image->base_level = base;
    image->max_level = max;
}

void ExecuteDraw(const CommandState &c, const DrawOp &d, const Pass &pass) {
    const Pipeline *p = d.pipeline;
    const Program  *program = p->program;
    if (program == nullptr || program->program == 0) {
        return;
    }
    const DynamicState &s = c.states[d.state];
    if (!g_gl.known || g_gl.program != program->program) {
        glUseProgram(program->program);
        g_gl.program = program->program;
    }

    // Rasterisation. GL's window y runs the other way from Vulkan's framebuffer y over the same rows, so the
    // winding that faces the front is the other one.
    const bool cull = s.cull != VK_CULL_MODE_NONE;
    SetEnabled(GL_CULL_FACE, g_gl.cull, cull);
    if (cull) {
        const GLenum face = s.cull == VK_CULL_MODE_FRONT_AND_BACK ? GL_FRONT_AND_BACK
                            : s.cull == VK_CULL_MODE_FRONT_BIT    ? GL_FRONT
                                                                  : GL_BACK;
        if (!g_gl.known || g_gl.cull_face != face) {
            glCullFace(face);
            g_gl.cull_face = face;
        }
    }
    const GLenum front = s.front_face == VK_FRONT_FACE_COUNTER_CLOCKWISE ? GL_CW : GL_CCW;
    if (!g_gl.known || g_gl.front_face != front) {
        glFrontFace(front);
        g_gl.front_face = front;
    }

    // Depth and stencil.
    const bool depth_test = pass.depth != nullptr && s.depth_test;
    SetEnabled(GL_DEPTH_TEST, g_gl.depth_test, depth_test);
    if (depth_test) {
        const GLenum func = Compare(s.depth_compare);
        if (!g_gl.known || g_gl.depth_func != func) {
            glDepthFunc(func);
            g_gl.depth_func = func;
        }
        SetDepthMask(s.depth_write);
    }
    const bool stencil_test = pass.depth != nullptr && HasStencil(pass.depth->vk_format) && s.stencil_test;
    SetEnabled(GL_STENCIL_TEST, g_gl.stencil_test, stencil_test);
    if (stencil_test) {
        // WebGL takes the front face's reference and masks for both faces (separateStencilMaskRef is false).
        const GLuint ref = s.stencil[0].reference & 0xFF;
        const GLuint read = s.stencil[0].compare_mask & 0xFF;
        const bool   refs_changed = !g_gl.known || g_gl.stencil_ref != ref || g_gl.stencil_read != read;
        for (int face = 0; face < 2; face++) {
            const StencilState &f = s.stencil[face];
            const GLenum        gl_face = face == 0 ? GL_FRONT : GL_BACK;
            const GLenum        func = Compare(f.compare);
            if (refs_changed || !g_gl.known || g_gl.stencil[face][0] != func) {
                glStencilFuncSeparate(gl_face, func, static_cast<GLint>(ref), read);
                g_gl.stencil[face][0] = func;
            }
            const GLenum fail = StencilOperation(f.fail);
            const GLenum depth_fail = StencilOperation(f.depth_fail);
            const GLenum pass_op = StencilOperation(f.pass);
            if (!g_gl.known || g_gl.stencil[face][1] != fail || g_gl.stencil[face][2] != depth_fail ||
                g_gl.stencil[face][3] != pass_op) {
                glStencilOpSeparate(gl_face, fail, depth_fail, pass_op);
                g_gl.stencil[face][1] = fail;
                g_gl.stencil[face][2] = depth_fail;
                g_gl.stencil[face][3] = pass_op;
            }
        }
        g_gl.stencil_ref = ref;
        g_gl.stencil_read = read;
        SetStencilWrite(s.stencil[0].write_mask & 0xFF);
    }

    // The colour target's blend and mask.
    SetEnabled(GL_BLEND, g_gl.blend, p->blend);
    if (p->blend) {
        if (Differs(g_gl.blend_func, {BlendFactor(p->src_color), BlendFactor(p->dst_color), BlendFactor(p->src_alpha),
                                      BlendFactor(p->dst_alpha)})) {
            glBlendFuncSeparate(g_gl.blend_func[0], g_gl.blend_func[1], g_gl.blend_func[2], g_gl.blend_func[3]);
        }
        if (Differs(g_gl.blend_equation, {BlendOperation(p->color_op), BlendOperation(p->alpha_op)})) {
            glBlendEquationSeparate(g_gl.blend_equation[0], g_gl.blend_equation[1]);
        }
        const float *k = s.blend_constants;
        if (Differs(g_gl.blend_color, {k[0], k[1], k[2], k[3]})) {
            glBlendColor(k[0], k[1], k[2], k[3]);
        }
    }
    const VkColorComponentFlags mask = p->fragment != nullptr ? p->write_mask : 0;
    SetColorMask(mask & VK_COLOR_COMPONENT_R_BIT, mask & VK_COLOR_COMPONENT_G_BIT, mask & VK_COLOR_COMPONENT_B_BIT,
                 mask & VK_COLOR_COMPONENT_A_BIT);

    // Viewport and scissor, in the framebuffer's rows as Vulkan numbers them.
    const VkViewport &v = s.viewport;
    if (Differs(g_gl.viewport, {static_cast<GLint>(std::lround(v.x)), static_cast<GLint>(std::lround(v.y)),
                                static_cast<GLint>(std::lround(std::max(v.width, 0.0f))),
                                static_cast<GLint>(std::lround(std::max(v.height, 0.0f)))})) {
        glViewport(g_gl.viewport[0], g_gl.viewport[1], g_gl.viewport[2], g_gl.viewport[3]);
    }
    if (Differs(g_gl.depth_range, {std::clamp(v.minDepth, 0.0f, 1.0f), std::clamp(v.maxDepth, 0.0f, 1.0f)})) {
        glDepthRangef(g_gl.depth_range[0], g_gl.depth_range[1]);
    }
    const VkRect2D &r = s.scissor;
    const int64_t   x0 = std::clamp<int64_t>(r.offset.x, 0, pass.width);
    const int64_t   y0 = std::clamp<int64_t>(r.offset.y, 0, pass.height);
    const int64_t   x1 = std::clamp<int64_t>(int64_t{r.offset.x} + r.extent.width, x0, pass.width);
    const int64_t   y1 = std::clamp<int64_t>(int64_t{r.offset.y} + r.extent.height, y0, pass.height);
    SetScissor(static_cast<GLint>(x0), static_cast<GLint>(y0), static_cast<GLint>(x1 - x0),
               static_cast<GLint>(y1 - y0));

    // Textures: set 0's image, palette and their samplers (draw_body.glsl), and the image's nearest twin.
    if (!p->layout->sets.empty()) {
        const DescriptorEntry *image = FindEntry(c, d.sets[0], 0);
        const DescriptorEntry *palette = FindEntry(c, d.sets[0], 1);
        Sampler               *sampler = SamplerAt(c, d, 2);
        Sampler               *palette_sampler = SamplerAt(c, d, 3);
        if (sampler != nullptr) {
            SamplerOf(sampler);
        }
        if (palette_sampler != nullptr) {
            SamplerOf(palette_sampler);
        }
        const ImageView *image_view = image != nullptr ? image->view : nullptr;
        const ImageView *palette_view = palette != nullptr ? palette->view : nullptr;
        if (image_view != nullptr) {
            UseMips(0, image_view);
        }
        if (palette_view != nullptr) {
            UseMips(1, palette_view);
        }
        const GLuint image_texture = image_view != nullptr ? image_view->image->texture : g_filler_texture;
        const GLuint palette_texture = palette_view != nullptr ? palette_view->image->texture : g_filler_texture;
        BindTexture(0, image_texture, sampler != nullptr ? sampler->sampler : 0);
        BindTexture(1, palette_texture, palette_sampler != nullptr ? palette_sampler->nearest : 0);
        BindTexture(2, image_texture, sampler != nullptr ? sampler->nearest : 0);
    }

    // Uniform blocks.
    for (uint32_t block = 0; block < kBlockCount; block++) {
        if (!program->reads[block]) {
            continue;
        }
        const auto offset = static_cast<GLint>(d.block_offset[block]);
        if (!g_gl.known || g_gl.block_offset[block] != offset) {
            glBindBufferRange(GL_UNIFORM_BUFFER, block, g_uniforms, offset, kBlockBytes[block]);
            g_gl.block_offset[block] = offset;
        }
    }

    // Vertices: WebGL2 has no base vertex, so the attributes start at it.
    if (!p->attributes.empty() && d.vertex != nullptr) {
        const GLuint   buffer = GlBufferFor(d.vertex, GL_ARRAY_BUFFER);
        const GLintptr base = static_cast<GLintptr>(d.vertex_offset) + GLintptr{d.base_vertex} * p->stride;
        if (!g_gl.known || g_gl.vertex_buffer != buffer || g_gl.vertex_base != base || g_gl.vertex_layout != p) {
            glBindBuffer(GL_ARRAY_BUFFER, buffer);
            uint32_t enabled = 0;
            for (const VertexAttribute &attribute : p->attributes) {
                const AttributeFormat format = FormatOfAttribute(attribute.format);
                const auto           *at = reinterpret_cast<const void *>(base + attribute.offset);
                if (format.integer) {
                    glVertexAttribIPointer(attribute.location, format.size, format.type, static_cast<GLsizei>(p->stride),
                                           at);
                } else {
                    glVertexAttribPointer(attribute.location, format.size, format.type, format.normalized,
                                          static_cast<GLsizei>(p->stride), at);
                }
                enabled |= 1u << attribute.location;
            }
            for (uint32_t location = 0; location < 16; location++) {
                const uint32_t bit = 1u << location;
                if ((enabled & bit) && (!g_gl.known || !(g_gl.enabled_attributes & bit))) {
                    glEnableVertexAttribArray(location);
                } else if (!(enabled & bit) && (!g_gl.known || (g_gl.enabled_attributes & bit))) {
                    glDisableVertexAttribArray(location);
                }
            }
            g_gl.enabled_attributes = enabled;
            g_gl.vertex_buffer = buffer;
            g_gl.vertex_base = base;
            g_gl.vertex_layout = p;
        }
    }

    const GLenum mode = Topology(s.topology);
    const auto   instances = static_cast<GLsizei>(std::max(d.instances, 1u));
    if (d.indexed) {
        if (d.index == nullptr) {
            return;
        }
        const GLuint buffer = GlBufferFor(d.index, GL_ELEMENT_ARRAY_BUFFER);
        if (!g_gl.known || g_gl.index_buffer != buffer) {
            glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, buffer);
            g_gl.index_buffer = buffer;
        }
        const bool  shorts = d.index_type == VK_INDEX_TYPE_UINT16;
        const auto *at = reinterpret_cast<const void *>(
            static_cast<uintptr_t>(d.index_offset + VkDeviceSize{d.first} * (shorts ? 2 : 4)));
        glDrawElementsInstanced(mode, static_cast<GLsizei>(d.count), shorts ? GL_UNSIGNED_SHORT : GL_UNSIGNED_INT, at,
                                instances);
    } else {
        glDrawArraysInstanced(mode, static_cast<GLint>(d.first), static_cast<GLsizei>(d.count), instances);
    }
}

// Clears the bound framebuffer's attachments inside a rectangle, whatever the masks were.
void ClearBound(bool color, const float *color_value, bool depth, float depth_value, bool stencil,
                uint32_t stencil_value, GLint x, GLint y, GLint width, GLint height) {
    SetScissor(x, y, width, height);
    if (color) {
        SetColorMask(true, true, true, true);
        glClearBufferfv(GL_COLOR, 0, color_value);
    }
    if (depth) {
        SetDepthMask(true);
    }
    if (stencil) {
        SetStencilWrite(0xFF);
    }
    const auto s = static_cast<GLint>(stencil_value & 0xFF);
    if (depth && stencil) {
        glClearBufferfi(GL_DEPTH_STENCIL, 0, depth_value, s);
    } else if (depth) {
        glClearBufferfv(GL_DEPTH, 0, &depth_value);
    } else if (stencil) {
        glClearBufferiv(GL_STENCIL, 0, &s);
    }
}

void BeginPass(const BeginOp &b, Pass &pass) {
    const GLuint framebuffer = FramebufferFor(b.color.image, b.color.mip, b.depth.image, b.depth.mip);
    BindDrawFramebuffer(framebuffer);
    const Image   *any = b.color.image != nullptr ? b.color.image : b.depth.image;
    const uint32_t mip = b.color.image != nullptr ? b.color.mip : b.depth.mip;
    pass.width = any != nullptr ? MipSize(any->width, mip) : 0;
    pass.height = any != nullptr ? MipSize(any->height, mip) : 0;
    pass.depth = b.depth.image;
    pass.begin = &b;
    const bool clear_color = b.color.image != nullptr && b.color.clear;
    const bool clear_depth = b.depth.image != nullptr && b.depth.clear;
    const bool clear_stencil = b.depth.image != nullptr && HasStencil(b.depth.image->vk_format) && b.clear_stencil;
    if (!clear_color && !clear_depth && !clear_stencil) {
        return;
    }
    const int64_t x0 = std::clamp<int64_t>(b.area.offset.x, 0, pass.width);
    const int64_t y0 = std::clamp<int64_t>(b.area.offset.y, 0, pass.height);
    const int64_t x1 = std::clamp<int64_t>(int64_t{b.area.offset.x} + b.area.extent.width, x0, pass.width);
    const int64_t y1 = std::clamp<int64_t>(int64_t{b.area.offset.y} + b.area.extent.height, y0, pass.height);
    ClearBound(clear_color, b.color.value.color.float32, clear_depth, b.depth.value.depthStencil.depth, clear_stencil,
               b.stencil, static_cast<GLint>(x0), static_cast<GLint>(y0), static_cast<GLint>(x1 - x0),
               static_cast<GLint>(y1 - y0));
}

void EndPass(Pass &pass) {
    // A depth the pass does not store need not leave the GPU's tile memory.
    if (pass.begin != nullptr && pass.begin->depth.image != nullptr && pass.begin->depth.discard) {
        const GLenum attachment =
            HasStencil(pass.begin->depth.image->vk_format) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT;
        glInvalidateFramebuffer(GL_DRAW_FRAMEBUFFER, 1, &attachment);
    }
    pass = Pass{};
}

// The framebuffer that reads or writes an image's mip, as its colour or its depth.
GLuint FramebufferOf(Image *image, uint32_t mip) {
    return IsDepthFormat(image->vk_format) ? FramebufferFor(nullptr, 0, image, mip) : FramebufferFor(image, mip, nullptr, 0);
}

GLbitfield BlitMask(const Image *image) {
    if (!IsDepthFormat(image->vk_format)) {
        return GL_COLOR_BUFFER_BIT;
    }
    return HasStencil(image->vk_format) ? GL_DEPTH_BUFFER_BIT | GL_STENCIL_BUFFER_BIT : GL_DEPTH_BUFFER_BIT;
}

// Blits copy only what the scissor and masks let through.
void PrepareBlit() {
    glDisable(GL_SCISSOR_TEST);
    SetColorMask(true, true, true, true);
    SetDepthMask(true);
    SetStencilWrite(0xFF);
}

void FinishBlit() {
    glEnable(GL_SCISSOR_TEST);
    // The draw framebuffer was replaced.
    g_gl.framebuffer = UINT32_MAX;
}

void Blit(Image *source, uint32_t source_mip, const int32_t (&s)[4], Image *destination, uint32_t destination_mip,
          const int32_t (&d)[4], GLenum filter) {
    glBindFramebuffer(GL_READ_FRAMEBUFFER, FramebufferOf(source, source_mip));
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, FramebufferOf(destination, destination_mip));
    glBlitFramebuffer(s[0], s[1], s[2], s[3], d[0], d[1], d[2], d[3], BlitMask(source), filter);
}

void Upload(const CommandState &c, const CopyOp<VkBufferImageCopy> &op) {
    auto *buffer = static_cast<Buffer *>(op.src);
    auto *image = static_cast<Image *>(op.dst);
    if (buffer->shadow == nullptr || IsDepthFormat(image->vk_format)) {
        Warn("a texture upload WebGL cannot make was dropped");
        return;
    }
    const GlFormat format = FormatOf(image->vk_format);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, image->texture);
    g_gl.textures[0] = image->texture;
    for (uint32_t i = op.begin; i < op.begin + op.count; i++) {
        const VkBufferImageCopy &r = c.buffer_image_regions[i];
        glPixelStorei(GL_UNPACK_ROW_LENGTH, static_cast<GLint>(r.bufferRowLength));
        glTexSubImage2D(GL_TEXTURE_2D, static_cast<GLint>(r.imageSubresource.mipLevel), r.imageOffset.x,
                        r.imageOffset.y, static_cast<GLsizei>(r.imageExtent.width),
                        static_cast<GLsizei>(r.imageExtent.height), format.format, format.type,
                        buffer->shadow + r.bufferOffset);
    }
    glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
}

// A colour target the size of a depth readback, for its words drawn as colour.
GLuint DepthWordTarget(uint32_t width, uint32_t height) {
    static Image   target;
    if (target.texture == 0 || target.width < width || target.height < height) {
        if (target.texture != 0) {
            ForgetFramebuffersOf(&target);
            glDeleteTextures(1, &target.texture);
        }
        target.vk_format = VK_FORMAT_R8G8B8A8_UNORM;
        target.width = std::max(width, target.width);
        target.height = std::max(height, target.height);
        glGenTextures(1, &target.texture);
        glBindTexture(GL_TEXTURE_2D, target.texture);
        glTexStorage2D(GL_TEXTURE_2D, 1, GL_RGBA8, static_cast<GLsizei>(target.width),
                       static_cast<GLsizei>(target.height));
    }
    return FramebufferFor(&target, 0, nullptr, 0);
}

void Readback(const CommandState &c, const CopyOp<VkBufferImageCopy> &op) {
    auto *image = static_cast<Image *>(op.src);
    auto *buffer = static_cast<Buffer *>(op.dst);
    if (buffer->shadow == nullptr) {
        return;
    }
    for (uint32_t i = op.begin; i < op.begin + op.count; i++) {
        const VkBufferImageCopy &r = c.buffer_image_regions[i];
        const bool     depth = (r.imageSubresource.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) != 0;
        const uint32_t texel = depth ? 4 : TexelBytes(image->vk_format);
        const uint32_t width = r.imageExtent.width;
        const uint32_t height = r.imageExtent.height;
        const uint32_t row_texels = r.bufferRowLength != 0 ? r.bufferRowLength : width;
        if (r.bufferOffset + (VkDeviceSize{height - 1} * row_texels + width) * texel > buffer->size || width == 0 ||
            height == 0) {
            Warn("a readback out of bounds was dropped");
            continue;
        }
        uint8_t *out = buffer->shadow + r.bufferOffset;
        GLint    x = r.imageOffset.x;
        GLint    y = r.imageOffset.y;
        if (depth) {
            // Drawn as colour, which WebGL reads back.
            const GLuint framebuffer = DepthWordTarget(width, height);
            glBindFramebuffer(GL_DRAW_FRAMEBUFFER, framebuffer);
            glBindVertexArray(g_empty_vertex_array);
            glDisable(GL_SCISSOR_TEST);
            glDisable(GL_BLEND);
            glDisable(GL_DEPTH_TEST);
            glDisable(GL_STENCIL_TEST);
            glDisable(GL_CULL_FACE);
            glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
            DrawDepthAsColor(image, x, y, width, height);
            glBindVertexArray(g_vertex_array);
            glEnable(GL_SCISSOR_TEST);
            ForgetGl();
            glBindFramebuffer(GL_READ_FRAMEBUFFER, framebuffer);
            x = 0;
            y = 0;
        } else {
            glBindFramebuffer(GL_READ_FRAMEBUFFER, FramebufferOf(image, r.imageSubresource.mipLevel));
        }
        if (texel == 4) {
            glPixelStorei(GL_PACK_ROW_LENGTH, static_cast<GLint>(row_texels));
            glReadPixels(x, y, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE,
                         out);
            glPixelStorei(GL_PACK_ROW_LENGTH, 0);
        } else {
            // One channel: WebGL reads RGBA for certain.
            std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);
            glReadPixels(x, y, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE,
                         rgba.data());
            for (uint32_t row = 0; row < height; row++) {
                for (uint32_t column = 0; column < width; column++) {
                    out[static_cast<size_t>(row) * row_texels + column] =
                        rgba[(static_cast<size_t>(row) * width + column) * 4];
                }
            }
        }
    }
}

void CopyImages(const CommandState &c, const CopyOp<VkImageCopy> &op) {
    auto *source = static_cast<Image *>(op.src);
    auto *destination = static_cast<Image *>(op.dst);
    PrepareBlit();
    for (uint32_t i = op.begin; i < op.begin + op.count; i++) {
        const VkImageCopy &r = c.image_regions[i];
        const auto         w = static_cast<int32_t>(r.extent.width);
        const auto         h = static_cast<int32_t>(r.extent.height);
        Blit(source, r.srcSubresource.mipLevel, {r.srcOffset.x, r.srcOffset.y, r.srcOffset.x + w, r.srcOffset.y + h},
             destination, r.dstSubresource.mipLevel, {r.dstOffset.x, r.dstOffset.y, r.dstOffset.x + w, r.dstOffset.y + h},
             GL_NEAREST);
    }
    FinishBlit();
}

void BlitImages(const CommandState &c, const CopyOp<VkImageBlit> &op) {
    auto *source = static_cast<Image *>(op.src);
    auto *destination = static_cast<Image *>(op.dst);
    if (IsDepthFormat(source->vk_format)) {
        Warn("a depth blit was dropped");
        return;
    }
    PrepareBlit();
    for (uint32_t i = op.begin; i < op.begin + op.count; i++) {
        const VkImageBlit &r = c.blit_regions[i];
        // GL's blit flips where either rectangle's ends are swapped, as Vulkan's does.
        Blit(source, r.srcSubresource.mipLevel, {r.srcOffsets[0].x, r.srcOffsets[0].y, r.srcOffsets[1].x, r.srcOffsets[1].y},
             destination, r.dstSubresource.mipLevel,
             {r.dstOffsets[0].x, r.dstOffsets[0].y, r.dstOffsets[1].x, r.dstOffsets[1].y},
             op.filter == VK_FILTER_LINEAR ? GL_LINEAR : GL_NEAREST);
    }
    FinishBlit();
}

void ClearColorImage(const CommandState &c, const ClearColorOp &op) {
    for (uint32_t i = op.begin; i < op.begin + op.count; i++) {
        const VkImageSubresourceRange &range = c.clear_ranges[i];
        const uint32_t levels = range.levelCount == VK_REMAINING_MIP_LEVELS ? op.image->mips - range.baseMipLevel
                                                                            : range.levelCount;
        for (uint32_t mip = range.baseMipLevel; mip < range.baseMipLevel + levels; mip++) {
            BindDrawFramebuffer(FramebufferFor(op.image, mip, nullptr, 0));
            ClearBound(true, op.value.float32, false, 0.0f, false, 0, 0, 0,
                       static_cast<GLint>(MipSize(op.image->width, mip)),
                       static_cast<GLint>(MipSize(op.image->height, mip)));
        }
    }
}

} // namespace

void Execute(VkCommandBuffer cmd) {
    CommandState &c = *cmd->state;
    EnsureObjects();

    // The GL buffers copies write to, as they stand before the submission: the copies reach them in order.
    for (const Op &op : c.ops) {
        if (op.type == OpType::CopyBuffer) {
            auto *destination = static_cast<Buffer *>(c.buffer_copies[op.index].dst);
            if (destination->shadow == nullptr) {
                continue;
            }
            if (destination->usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) {
                GlBufferFor(destination, GL_ARRAY_BUFFER);
            }
            if (destination->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) {
                GlBufferFor(destination, GL_ELEMENT_ARRAY_BUFFER);
            }
        }
    }

    // In wasm memory, in order: buffer copies, and the uniform blocks each draw reads as they stand then.
    Uniforms uniforms;
    uniforms.alignment = static_cast<uint32_t>(g.ubo_alignment);
    uniforms.data.assign(kBlockBytes[kPaletteBlock], 0);
    for (const Op &op : c.ops) {
        if (op.type == OpType::CopyBuffer) {
            CopyBuffers(c, c.buffer_copies[op.index]);
            uniforms.records.clear(); // they may have changed
        } else if (op.type == OpType::Draw) {
            PrepareDraw(c, c.draws[op.index], uniforms);
        }
    }
    glBindBuffer(GL_UNIFORM_BUFFER, g_uniforms);
    glBufferData(GL_UNIFORM_BUFFER, static_cast<GLsizeiptr>(uniforms.data.size()), uniforms.data.data(), GL_STREAM_DRAW);

    // GL.
    ForgetGl();
    // The scissor is always on, as Vulkan's always is; what else GL has is set at the first draw.
    glBindVertexArray(g_vertex_array);
    glEnable(GL_SCISSOR_TEST);
    Pass pass;
    for (const Op &op : c.ops) {
        switch (op.type) {
            case OpType::BeginRendering:
                BeginPass(c.begins[op.index], pass);
                break;
            case OpType::EndRendering:
                EndPass(pass);
                break;
            case OpType::Draw:
                ExecuteDraw(c, c.draws[op.index], pass);
                g_gl.known = true;
                break;
            case OpType::ClearAttachments: {
                const ClearOp &clear = c.clears[op.index];
                for (uint32_t i = clear.rect_begin; i < clear.rect_begin + clear.rect_count; i++) {
                    const VkRect2D &rect = c.clear_rects[i].rect;
                    const int64_t   x0 = std::clamp<int64_t>(rect.offset.x, 0, pass.width);
                    const int64_t   y0 = std::clamp<int64_t>(rect.offset.y, 0, pass.height);
                    const int64_t   x1 = std::clamp<int64_t>(int64_t{rect.offset.x} + rect.extent.width, x0, pass.width);
                    const int64_t   y1 = std::clamp<int64_t>(int64_t{rect.offset.y} + rect.extent.height, y0, pass.height);
                    ClearBound(clear.color, clear.color_value, clear.depth && pass.depth != nullptr, clear.depth_value,
                               clear.stencil && pass.depth != nullptr && HasStencil(pass.depth->vk_format),
                               clear.stencil_value, static_cast<GLint>(x0), static_cast<GLint>(y0),
                               static_cast<GLint>(x1 - x0), static_cast<GLint>(y1 - y0));
                }
                break;
            }
            case OpType::CopyBuffer:
                CopyIntoGlBuffers(c, c.buffer_copies[op.index]);
                break;
            case OpType::CopyBufferToImage:
                Upload(c, c.uploads[op.index]);
                break;
            case OpType::CopyImageToBuffer:
                Readback(c, c.readbacks[op.index]);
                break;
            case OpType::CopyImage:
                CopyImages(c, c.image_copies[op.index]);
                break;
            case OpType::BlitImage:
                BlitImages(c, c.blits[op.index]);
                break;
            case OpType::ClearColorImage:
                ClearColorImage(c, c.color_clears[op.index]);
                break;
        }
    }
}

} // namespace vkgl

using namespace vkgl;

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

VKAPI_ATTR void VKAPI_CALL vkFreeCommandBuffers(VkDevice, VkCommandPool pool, uint32_t count,
                                                const VkCommandBuffer *buffers) {
    auto *object = Get<CommandPool>(pool);
    for (uint32_t i = 0; i < count; i++) {
        if (buffers[i] == nullptr) {
            continue;
        }
        std::erase(object->buffers, buffers[i]);
        delete buffers[i]->state;
        delete buffers[i];
    }
}

VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice, VkCommandPool pool, const VkAllocationCallbacks *) {
    auto *object = Get<CommandPool>(pool);
    if (object == nullptr) {
        return;
    }
    for (VkCommandBuffer cmd : object->buffers) {
        delete cmd->state;
        delete cmd;
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
    ResetState(*cmd->state);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer cmd) {
    cmd->state->open_pass = nullptr;
    return VK_SUCCESS;
}

// --- Rendering ----------------------------------------------------------------------------------------------

VKAPI_ATTR void VKAPI_CALL vkCmdBeginRendering(VkCommandBuffer cmd, const VkRenderingInfo *info) {
    CommandState &c = *cmd->state;
    BeginOp       b;
    b.area = info->renderArea;
    auto attachment = [](const VkRenderingAttachmentInfo &a, Attachment &out) {
        ImageView *view = Get<ImageView>(a.imageView);
        out.image = view->image;
        out.mip = view->base_mip;
        out.clear = a.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
        out.discard = a.storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE || a.storeOp == VK_ATTACHMENT_STORE_OP_NONE;
        out.value = a.clearValue;
    };
    if (info->colorAttachmentCount > 0 && info->pColorAttachments[0].imageView != VK_NULL_HANDLE) {
        attachment(info->pColorAttachments[0], b.color);
    }
    const VkRenderingAttachmentInfo *d = info->pDepthAttachment;
    const VkRenderingAttachmentInfo *s = info->pStencilAttachment;
    if (d != nullptr && d->imageView != VK_NULL_HANDLE) {
        attachment(*d, b.depth);
    } else if (s != nullptr && s->imageView != VK_NULL_HANDLE) {
        attachment(*s, b.depth);
        b.depth.clear = false;
    }
    if (s != nullptr && s->imageView != VK_NULL_HANDLE) {
        b.clear_stencil = s->loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR;
        b.stencil = s->clearValue.depthStencil.stencil;
        // Kept if either aspect is stored.
        b.depth.discard = b.depth.discard && (s->storeOp == VK_ATTACHMENT_STORE_OP_DONT_CARE ||
                                              s->storeOp == VK_ATTACHMENT_STORE_OP_NONE);
    }
    c.ops.push_back({OpType::BeginRendering, Append(c.begins, b)});
}

VKAPI_ATTR void VKAPI_CALL vkCmdEndRendering(VkCommandBuffer cmd) {
    cmd->state->ops.push_back({OpType::EndRendering, 0});
}

// --- State --------------------------------------------------------------------------------------------------

VKAPI_ATTR void VKAPI_CALL vkCmdBindPipeline(VkCommandBuffer cmd, VkPipelineBindPoint, VkPipeline pipeline) {
    CommandState &c = *cmd->state;
    Pipeline     *p = Get<Pipeline>(pipeline);
    c.pipeline = p;
    c.state_changed = true;
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
        }
    }
    if (fixed(VK_DYNAMIC_STATE_BLEND_CONSTANTS)) {
        std::memcpy(s.blend_constants, p->fixed.blend_constants, sizeof s.blend_constants);
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetViewport(VkCommandBuffer cmd, uint32_t, uint32_t count, const VkViewport *viewports) {
    if (count > 0) {
        cmd->state->state.viewport = viewports[0];
        cmd->state->state_changed = true;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetScissor(VkCommandBuffer cmd, uint32_t, uint32_t count, const VkRect2D *scissors) {
    if (count > 0) {
        cmd->state->state.scissor = scissors[0];
        cmd->state->state_changed = true;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetCullMode(VkCommandBuffer cmd, VkCullModeFlags mode) {
    cmd->state->state.cull = mode;
    cmd->state->state_changed = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetFrontFace(VkCommandBuffer cmd, VkFrontFace face) {
    cmd->state->state.front_face = face;
    cmd->state->state_changed = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetPrimitiveTopology(VkCommandBuffer cmd, VkPrimitiveTopology topology) {
    cmd->state->state.topology = topology;
    cmd->state->state_changed = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthTestEnable(VkCommandBuffer cmd, VkBool32 enable) {
    cmd->state->state.depth_test = enable;
    cmd->state->state_changed = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthWriteEnable(VkCommandBuffer cmd, VkBool32 enable) {
    cmd->state->state.depth_write = enable;
    cmd->state->state_changed = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthCompareOp(VkCommandBuffer cmd, VkCompareOp op) {
    cmd->state->state.depth_compare = op;
    cmd->state->state_changed = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetDepthBounds(VkCommandBuffer, float, float) {}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilTestEnable(VkCommandBuffer cmd, VkBool32 enable) {
    cmd->state->state.stencil_test = enable;
    cmd->state->state_changed = true;
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
    cmd->state->state_changed = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilCompareMask(VkCommandBuffer cmd, VkStencilFaceFlags faces, uint32_t mask) {
    for (int face = 0; face < 2; face++) {
        if (faces & (face == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT)) {
            cmd->state->state.stencil[face].compare_mask = mask;
        }
    }
    cmd->state->state_changed = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilWriteMask(VkCommandBuffer cmd, VkStencilFaceFlags faces, uint32_t mask) {
    for (int face = 0; face < 2; face++) {
        if (faces & (face == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT)) {
            cmd->state->state.stencil[face].write_mask = mask;
        }
    }
    cmd->state->state_changed = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetStencilReference(VkCommandBuffer cmd, VkStencilFaceFlags faces, uint32_t reference) {
    for (int face = 0; face < 2; face++) {
        if (faces & (face == 0 ? VK_STENCIL_FACE_FRONT_BIT : VK_STENCIL_FACE_BACK_BIT)) {
            cmd->state->state.stencil[face].reference = reference;
        }
    }
    cmd->state->state_changed = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdSetBlendConstants(VkCommandBuffer cmd, const float constants[4]) {
    std::memcpy(cmd->state->state.blend_constants, constants, sizeof(float) * 4);
    cmd->state->state_changed = true;
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushConstants(VkCommandBuffer cmd, VkPipelineLayout, VkShaderStageFlags,
                                              uint32_t offset, uint32_t size, const void *values) {
    if (offset + size <= 256) {
        std::memcpy(cmd->state->push.data() + offset, values, size);
        cmd->state->push_changed = true;
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdBindDescriptorSets(VkCommandBuffer cmd, VkPipelineBindPoint, VkPipelineLayout,
                                                   uint32_t first, uint32_t count, const VkDescriptorSet *sets,
                                                   uint32_t, const uint32_t *) {
    CommandState &c = *cmd->state;
    for (uint32_t i = 0; i < count && first + i < c.sets.size(); i++) {
        c.sets[first + i] = {Get<DescriptorSet>(sets[i]), 0, 0};
    }
}

VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetKHR(VkCommandBuffer cmd, VkPipelineBindPoint, VkPipelineLayout,
                                                     uint32_t set, uint32_t count, const VkWriteDescriptorSet *writes) {
    CommandState &c = *cmd->state;
    if (set >= c.sets.size()) {
        return;
    }
    const auto begin = static_cast<uint32_t>(c.pushed_entries.size());
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
        c.pushed_entries.push_back(entry);
    }
    c.sets[set] = {nullptr, begin, count};
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

static void RecordDraw(VkCommandBuffer cmd, bool indexed, uint32_t count, uint32_t instances, uint32_t first,
                       int32_t base_vertex) {
    CommandState &c = *cmd->state;
    if (c.pipeline == nullptr || count == 0) {
        return;
    }
    if (c.state_changed || c.states.empty()) {
        c.states.push_back(c.state);
        c.state_changed = false;
    }
    if (c.push_changed || c.pushes.empty()) {
        PushBlock block = {};
        std::memcpy(block.data(), c.push.data(), block.size());
        c.pushes.push_back(block);
        c.push_changed = false;
    }
    DrawOp d;
    d.pipeline = c.pipeline;
    d.state = static_cast<uint32_t>(c.states.size() - 1);
    d.push = static_cast<uint32_t>(c.pushes.size() - 1);
    d.sets = c.sets;
    d.vertex = c.vertex_buffer;
    d.vertex_offset = c.vertex_offset;
    d.index = c.index_buffer;
    d.index_offset = c.index_offset;
    d.index_type = c.index_type;
    d.indexed = indexed;
    d.count = count;
    d.instances = instances;
    d.first = first;
    d.base_vertex = base_vertex;
    c.ops.push_back({OpType::Draw, Append(c.draws, d)});
}

VKAPI_ATTR void VKAPI_CALL vkCmdDraw(VkCommandBuffer cmd, uint32_t vertices, uint32_t instances, uint32_t first_vertex,
                                     uint32_t) {
    RecordDraw(cmd, false, vertices, instances, first_vertex, 0);
}

VKAPI_ATTR void VKAPI_CALL vkCmdDrawIndexed(VkCommandBuffer cmd, uint32_t indices, uint32_t instances,
                                            uint32_t first_index, int32_t vertex_offset, uint32_t) {
    RecordDraw(cmd, true, indices, instances, first_index, vertex_offset);
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer cmd, uint32_t count, const VkClearAttachment *attachments,
                                                 uint32_t rect_count, const VkClearRect *rects) {
    CommandState &c = *cmd->state;
    ClearOp       clear;
    for (uint32_t i = 0; i < count; i++) {
        const VkClearAttachment &a = attachments[i];
        if (a.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) {
            clear.color = true;
            std::memcpy(clear.color_value, a.clearValue.color.float32, sizeof clear.color_value);
        }
        if (a.aspectMask & VK_IMAGE_ASPECT_DEPTH_BIT) {
            clear.depth = true;
            clear.depth_value = a.clearValue.depthStencil.depth;
        }
        if (a.aspectMask & VK_IMAGE_ASPECT_STENCIL_BIT) {
            clear.stencil = true;
            clear.stencil_value = a.clearValue.depthStencil.stencil;
        }
    }
    clear.rect_begin = AppendAll(c.clear_rects, rects, rect_count);
    clear.rect_count = rect_count;
    c.ops.push_back({OpType::ClearAttachments, Append(c.clears, clear)});
}

// --- Copies, blits and clears outside rendering -------------------------------------------------------------

VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier2(VkCommandBuffer, const VkDependencyInfo *) {}

VKAPI_ATTR void VKAPI_CALL vkCmdResetQueryPool(VkCommandBuffer, VkQueryPool, uint32_t, uint32_t) {}

VKAPI_ATTR void VKAPI_CALL vkCmdWriteTimestamp2(VkCommandBuffer, VkPipelineStageFlags2, VkQueryPool, uint32_t) {}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBuffer(VkCommandBuffer cmd, VkBuffer src, VkBuffer dst, uint32_t count,
                                           const VkBufferCopy *regions) {
    CommandState          &c = *cmd->state;
    CopyOp<VkBufferCopy> op;
    op.src = Get<Buffer>(src);
    op.dst = Get<Buffer>(dst);
    op.begin = AppendAll(c.buffer_regions, regions, count);
    op.count = count;
    c.ops.push_back({OpType::CopyBuffer, Append(c.buffer_copies, op)});
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyBufferToImage(VkCommandBuffer cmd, VkBuffer src, VkImage dst, VkImageLayout,
                                                  uint32_t count, const VkBufferImageCopy *regions) {
    CommandState               &c = *cmd->state;
    CopyOp<VkBufferImageCopy> op;
    op.src = Get<Buffer>(src);
    op.dst = Get<Image>(dst);
    op.begin = AppendAll(c.buffer_image_regions, regions, count);
    op.count = count;
    c.ops.push_back({OpType::CopyBufferToImage, Append(c.uploads, op)});
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImageToBuffer(VkCommandBuffer cmd, VkImage src, VkImageLayout, VkBuffer dst,
                                                  uint32_t count, const VkBufferImageCopy *regions) {
    CommandState               &c = *cmd->state;
    CopyOp<VkBufferImageCopy> op;
    op.src = Get<Image>(src);
    op.dst = Get<Buffer>(dst);
    op.begin = AppendAll(c.buffer_image_regions, regions, count);
    op.count = count;
    c.ops.push_back({OpType::CopyImageToBuffer, Append(c.readbacks, op)});
}

VKAPI_ATTR void VKAPI_CALL vkCmdCopyImage(VkCommandBuffer cmd, VkImage src, VkImageLayout, VkImage dst, VkImageLayout,
                                          uint32_t count, const VkImageCopy *regions) {
    CommandState         &c = *cmd->state;
    CopyOp<VkImageCopy> op;
    op.src = Get<Image>(src);
    op.dst = Get<Image>(dst);
    op.begin = AppendAll(c.image_regions, regions, count);
    op.count = count;
    c.ops.push_back({OpType::CopyImage, Append(c.image_copies, op)});
}

VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer cmd, VkImage src, VkImageLayout, VkImage dst, VkImageLayout,
                                          uint32_t count, const VkImageBlit *regions, VkFilter filter) {
    CommandState         &c = *cmd->state;
    CopyOp<VkImageBlit> op;
    op.src = Get<Image>(src);
    op.dst = Get<Image>(dst);
    op.begin = AppendAll(c.blit_regions, regions, count);
    op.count = count;
    op.filter = filter;
    c.ops.push_back({OpType::BlitImage, Append(c.blits, op)});
}

VKAPI_ATTR void VKAPI_CALL vkCmdClearColorImage(VkCommandBuffer cmd, VkImage image, VkImageLayout,
                                                const VkClearColorValue *value, uint32_t count,
                                                const VkImageSubresourceRange *ranges) {
    CommandState &c = *cmd->state;
    ClearColorOp  op;
    op.image = Get<Image>(image);
    op.value = *value;
    op.begin = AppendAll(c.clear_ranges, ranges, count);
    op.count = count;
    c.ops.push_back({OpType::ClearColorImage, Append(c.color_clears, op)});
}

} // extern "C"

// The transient allocator's spans (gfx/memory.cpp): what the CPU writes there reaches the GL buffers before a
// draw next reads them.
extern "C" void vkWebGpuMarkWritten(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size) {
    if (Buffer *object = Get<Buffer>(buffer)) {
        MarkDirty(object, offset, size);
    }
}

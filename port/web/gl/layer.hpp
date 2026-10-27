// Vulkan on WebGL2 (docs/WEB.md): the commands Kazarov's renderer calls, implemented on WebGL2 (OpenGL ES 3.0)
// so the renderer runs unchanged in a browser whose WebGPU draws it wrong or slowly (Android's Mali GPUs). It is the
// same Vulkan 1.3 portability-subset device as Vulkan on WebGPU (port/web/vulkan), reporting what that one reports:
// push descriptors, no bindless array, no triangle fans, one stencil reference and mask pair, no timestamps, no
// depth blits.
//
// - Command buffers record what they are given; a submission replays it as GL calls (commands.cpp), so a fence is
//   signalled once vkQueueSubmit2 returns and a readback has landed by then. WebGL orders what it is given itself:
//   nothing is waited for.
// - Every buffer has its bytes in wasm memory: host-visible memory's own, a device-local buffer's copy. Buffers the
//   GPU reads as vertices or indices have GL buffers too, updated from those bytes before a draw reads them. The
//   renderer writes host-visible memory through its transient allocator, which marks what it hands out
//   (vkWebGpuMarkWritten).
// - What the shaders read from storage buffers and push constants goes to uniform blocks: each draw's records are
//   copied from the buffers' bytes into one uniform buffer per submission.
// - GL draws into framebuffers row 0 first, as Vulkan does, so images keep Vulkan's layout and clip space its y;
//   only the winding that faces the front and the depth range differ (shaders.cpp). The canvas, whose row 0 is at
//   the bottom, is drawn flipped at present.
// - GL objects belong to the context, current on the thread that made the device, the game's (render thread off):
//   pipelines made elsewhere are descriptions until a draw first needs them.
#pragma once

// The portability subset's structures, as the renderer includes them (gfx/requirements.hpp).
#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif
#include <vulkan/vulkan.h>
#include <GLES3/gl3.h>
#include <emscripten/html5_webgl.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace vkgl {
struct CommandState;
}

struct VkInstance_T {};
struct VkPhysicalDevice_T {};
struct VkDevice_T {};
struct VkQueue_T {};

namespace vkgl {

// Non-dispatchable handles are 64-bit integers on wasm32; they carry the object's address.
template <typename T> T *Get(uint64_t handle) { return reinterpret_cast<T *>(static_cast<uintptr_t>(handle)); }
template <typename H, typename T> H Handle(T *object) { return static_cast<H>(reinterpret_cast<uintptr_t>(object)); }

[[noreturn]] void Fatal(const char *format, ...) __attribute__((format(printf, 1, 2)));
void Warn(const char *format, ...) __attribute__((format(printf, 1, 2)));
void AssertDeviceThread(const char *what);

// Memory types: 0 device-local, 1 host-visible and coherent.
constexpr uint32_t kDeviceLocalType = 0;
constexpr uint32_t kHostVisibleType = 1;

struct Memory {
    VkDeviceSize size = 0;
    uint32_t     type = 0;
    uint8_t     *shadow = nullptr; // host-visible memory's bytes
};

// A GL buffer holding a buffer's bytes for one binding target: WebGL keeps a buffer that has been an index buffer
// from being bound as anything else. What changed since it was last updated is [dirty_begin, dirty_end).
struct GlCopy {
    GLuint       name = 0;
    VkDeviceSize dirty_begin = 0;
    VkDeviceSize dirty_end = 0;
};

struct Buffer {
    VkDeviceSize       size = 0;
    VkBufferUsageFlags usage = 0;
    uint8_t           *shadow = nullptr; // host-visible: into the memory's bytes; device-local: its own
    bool               owns_shadow = false;
    GlCopy             vertex;
    GlCopy             index;
};

struct Image {
    GLuint   texture = 0;
    GLenum   internal_format = 0;
    VkFormat vk_format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0;
    uint32_t height = 0;
    uint32_t mips = 1;
    bool     swapchain = false;
    // The mip range sampling last set, as GL keeps it on the texture rather than the view.
    int32_t base_level = 0;
    int32_t max_level = 1000;
};

struct ImageView {
    Image             *image = nullptr;
    uint32_t           base_mip = 0;
    uint32_t           mip_count = 1;
    VkImageAspectFlags aspect = 0;
};

// A sampler and its nearest twin, which stands in for textureGather (shaders.cpp). Samplers are made on the
// renderer's worker threads too, where there is no context: the GL objects are made at first use (SamplerOf).
struct Sampler {
    VkSamplerCreateInfo info = {};
    GLuint              sampler = 0;
    GLuint              nearest = 0;
};

void SamplerOf(Sampler *sampler);

// One of the renderer's modules, known by its SPIR-V.
struct ShaderModule {
    const char *name = nullptr;
};

struct LayoutBinding {
    uint32_t           binding = 0;
    VkDescriptorType   type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    VkShaderStageFlags stages = 0;
    Sampler           *immutable_sampler = nullptr;
};

struct DescriptorSetLayout {
    std::vector<LayoutBinding> bindings;
    bool                       push = false;
};

struct PipelineLayout {
    std::vector<DescriptorSetLayout *> sets;
    uint32_t                           push_size = 0;
};

// What a descriptor set holds at one binding.
struct DescriptorEntry {
    uint32_t     binding = 0;
    Buffer      *buffer = nullptr;
    VkDeviceSize offset = 0;
    VkDeviceSize range = 0;
    ImageView   *view = nullptr;
    Sampler     *sampler = nullptr;
};

struct DescriptorPool;

struct DescriptorSet {
    DescriptorSetLayout         *layout = nullptr;
    DescriptorPool              *pool = nullptr;
    std::vector<DescriptorEntry> entries;
};

struct DescriptorPool {
    std::unordered_set<DescriptorSet *> sets;
};

struct VertexAttribute {
    uint32_t location = 0;
    uint32_t offset = 0;
    VkFormat format = VK_FORMAT_UNDEFINED;
};

struct StencilState {
    VkStencilOp fail = VK_STENCIL_OP_KEEP;
    VkStencilOp pass = VK_STENCIL_OP_KEEP;
    VkStencilOp depth_fail = VK_STENCIL_OP_KEEP;
    VkCompareOp compare = VK_COMPARE_OP_ALWAYS;
    uint32_t    compare_mask = 0xFF;
    uint32_t    write_mask = 0xFF;
    uint32_t    reference = 0;
};

// Vulkan's state a pipeline fixes or leaves dynamic, as the command buffer holds it.
struct DynamicState {
    VkPrimitiveTopology topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkCullModeFlags     cull = VK_CULL_MODE_NONE;
    VkFrontFace         front_face = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    bool                depth_test = false;
    bool                depth_write = false;
    VkCompareOp         depth_compare = VK_COMPARE_OP_ALWAYS;
    bool                stencil_test = false;
    StencilState        stencil[2]; // front, back
    float               blend_constants[4] = {};
    VkViewport          viewport = {};
    VkRect2D            scissor = {};
};

// The uniform blocks a program reads, by binding point (shaders.cpp).
enum Block : uint32_t {
    kPushBlock = 0,
    kMeshBlock = 1,
    kProg0Block = 2,
    kPaletteBlock = 3,
    kBlockCount = 4,
};

// Each block's size as std140 lays it out: gfx::detail::PushConstants, MeshConstants, Prog0Constants, Prog0Palette.
constexpr uint32_t kBlockBytes[kBlockCount] = {80, 400, 480, 1536};

// A linked program and what it reads.
struct Program {
    GLuint program = 0;
    bool   reads[kBlockCount] = {};
};

// The program for a pair of the renderer's modules with their specialisation constants, linked on first use and kept.
Program *ProgramFor(const ShaderModule *vertex, const std::vector<std::pair<uint32_t, int32_t>> &vertex_constants,
                    const ShaderModule *fragment, const std::vector<std::pair<uint32_t, int32_t>> &fragment_constants);
// Writes an image's depth as 4-byte words, a float's bits or 24-bit unorm, into an RGBA8 target the size of the
// region: WebGL reads no depth back.
void DrawDepthAsColor(Image *depth, int32_t x, int32_t y, uint32_t width, uint32_t height);

struct Pipeline {
    PipelineLayout                          *layout = nullptr;
    ShaderModule                            *vertex = nullptr;
    ShaderModule                            *fragment = nullptr;
    std::vector<std::pair<uint32_t, int32_t>> vertex_constants;
    std::vector<std::pair<uint32_t, int32_t>> fragment_constants;
    uint32_t                     stride = 0;
    std::vector<VertexAttribute> attributes;
    // The colour attachment's blend.
    bool                  blend = false;
    VkBlendFactor         src_color = VK_BLEND_FACTOR_ONE;
    VkBlendFactor         dst_color = VK_BLEND_FACTOR_ZERO;
    VkBlendOp             color_op = VK_BLEND_OP_ADD;
    VkBlendFactor         src_alpha = VK_BLEND_FACTOR_ONE;
    VkBlendFactor         dst_alpha = VK_BLEND_FACTOR_ZERO;
    VkBlendOp             alpha_op = VK_BLEND_OP_ADD;
    VkColorComponentFlags write_mask = 0xF;
    // Without dual-source blending: the fragment shader writes the blend factor as alpha, which the target then
    // does not take (commands.cpp).
    bool factor_in_alpha = false;
    // The state the pipeline fixes, applied when it is bound, and which of it is dynamic instead.
    DynamicState                       fixed;
    std::unordered_set<VkDynamicState> dynamic;
    Program                           *program = nullptr; // linked at its first draw
};

struct Fence {
    bool signaled = false;
};

// Replays a command buffer's commands as GL calls (commands.cpp).
void Execute(VkCommandBuffer cmd);

// A framebuffer with an image's mip as the colour attachment and another's as the depth, made once and kept until
// either image goes. Either may be null.
GLuint FramebufferFor(Image *color, uint32_t color_mip, Image *depth, uint32_t depth_mip);
void   ForgetFramebuffersOf(Image *image);

// Marks a buffer's bytes as changed, for its GL buffers to be updated before they are next read.
void MarkDirty(Buffer *buffer, VkDeviceSize offset, VkDeviceSize size);
// The GL buffer for a buffer's bytes as target reads them, brought up to date.
GLuint GlBufferFor(Buffer *buffer, GLenum target);
void   DeleteGlBuffers(Buffer *buffer);

struct Swapchain {
    std::vector<Image *> images;
    uint32_t             width = 0;
    uint32_t             height = 0;
    uint32_t             next = 0;
};

struct Context {
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = 0;
    GLint                           ubo_alignment = 256;
    GLint                           max_texture_size = 4096;
    GLint                           max_uniform_block_size = 16384;
    char                            renderer_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
};

extern Context g;

// The GL formats an image of a Vulkan format takes.
struct GlFormat {
    GLenum internal_format;
    GLenum format;
    GLenum type;
};

GlFormat FormatOf(VkFormat format);
bool     IsDepthFormat(VkFormat format);
bool     HasStencil(VkFormat format);
uint32_t TexelBytes(VkFormat format);
uint32_t MipSize(uint32_t size, uint32_t mip);

} // namespace vkgl

// A command buffer: what it recorded and the Vulkan state it tracks while recording.
struct VkCommandBuffer_T {
    vkgl::CommandState *state = nullptr;
};

// Called by the renderer's transient allocator (gfx/memory.cpp) for each span it hands out.
extern "C" void vkWebGpuMarkWritten(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size);
// The page's canvas as a surface, in place of SDL_Vulkan_CreateSurface (gfx/renderer.cpp).
extern "C" VkResult vkCreateCanvasSurfaceWEB(VkInstance instance, VkSurfaceKHR *surface);

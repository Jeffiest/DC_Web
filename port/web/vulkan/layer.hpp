// Vulkan on WebGPU (docs/WEB.md): the commands Kazarov's renderer calls, implemented on Emscripten's webgpu.h
// so the renderer runs unchanged in a browser. Only what that renderer uses is here. It is a Vulkan 1.3
// portability-subset device with push descriptors: no bindless array, no triangle fans, one stencil
// reference and mask pair, no depth bounds, no timestamps, no depth blits.
//
// - Command buffers encode into a WebGPU command encoder as they record. Vulkan's dynamic state, which
//   WebGPU lacks, is tracked per command buffer and picks a WebGPU pipeline per draw from the pipeline's
//   variants (commands.cpp). Push constants go to a storage buffer at a dynamic offset (group 2).
// - Host-visible memory is a copy in wasm memory. The renderer writes it through its transient allocator,
//   which marks what it hands out (vkWebGpuMarkWritten); the marked ranges are uploaded at the next submit.
// - Readbacks copy into map-read buffers that a fence wait maps and unpacks into the copy in wasm memory.
// - WebGPU objects are made and used on the thread that made the device, the game's (render thread off):
//   pipelines, layouts and samplers made elsewhere are descriptions until a draw first needs them.
#pragma once

// The portability subset's structures, as the renderer includes them (gfx/requirements.hpp).
#ifndef VK_ENABLE_BETA_EXTENSIONS
#define VK_ENABLE_BETA_EXTENSIONS
#endif
#include <vulkan/vulkan.h>
#include <webgpu/webgpu.h>

#include <array>
#include <cstdint>
#include <cstring>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "wgsl.hpp"

namespace vkwebgpu {
struct CommandState;
}

struct VkInstance_T {};
struct VkPhysicalDevice_T {};
struct VkDevice_T {};
struct VkQueue_T {};

namespace vkwebgpu {

// Non-dispatchable handles are 64-bit integers on wasm32; they carry the object's address.
template <typename T> T *Get(uint64_t handle) { return reinterpret_cast<T *>(static_cast<uintptr_t>(handle)); }
template <typename H, typename T> H Handle(T *object) { return static_cast<H>(reinterpret_cast<uintptr_t>(object)); }

[[noreturn]] void Fatal(const char *format, ...) __attribute__((format(printf, 1, 2)));
void Warn(const char *format, ...) __attribute__((format(printf, 1, 2)));
void AssertDeviceThread(const char *what);

// Waits for done, handing the thread back to the browser between checks so WebGPU's callbacks can run.
void WaitUntil(const std::function<bool()> &done);

WGPUStringView View(const char *text);

// Memory types: 0 device-local, 1 host-visible and coherent.
constexpr uint32_t kDeviceLocalType = 0;
constexpr uint32_t kHostVisibleType = 1;

struct Memory {
    VkDeviceSize size = 0;
    uint32_t     type = 0;
    uint8_t     *shadow = nullptr; // host-visible memory's bytes
};

struct Buffer {
    VkDeviceSize       size = 0;
    VkBufferUsageFlags usage = 0;
    uint8_t           *shadow = nullptr; // host-visible: into the memory's bytes
    // Device-local and host-visible buffers the GPU reads have a WebGPU buffer. A host-visible buffer the
    // GPU only copies into (a readback) has none: its copies land in map-read buffers (Readback).
    WGPUBuffer gpu = nullptr;
    // What the CPU wrote since the last submit, uploaded then: [dirty_begin, dirty_end).
    VkDeviceSize dirty_begin = 0;
    VkDeviceSize dirty_end = 0;
};

struct Image {
    WGPUTexture       texture = nullptr; // a swapchain image's is the canvas's current texture
    WGPUTextureFormat format = WGPUTextureFormat_Undefined;
    VkFormat          vk_format = VK_FORMAT_UNDEFINED;
    uint32_t          width = 0;
    uint32_t          height = 0;
    uint32_t          mips = 1;
    bool              swapchain = false;
    uint32_t          generation = 0; // bumped when a swapchain image takes a new texture
};

struct ImageView {
    Image             *image = nullptr;
    uint32_t           base_mip = 0;
    uint32_t           mip_count = 1;
    VkImageAspectFlags aspect = 0;
    uint64_t           id = 0; // unique for the process's life: keys bind group caches
    WGPUTextureView    view = nullptr;
    uint32_t           generation = 0;
};

WGPUTextureView ViewOf(ImageView *view);
// A view of one mip for an attachment or a copy, made per use.
WGPUTextureView MipView(Image *image, uint32_t mip, WGPUTextureAspect aspect = WGPUTextureAspect_All);

struct Sampler {
    WGPUSamplerDescriptor descriptor = {};
    WGPUSampler           sampler = nullptr;
};

WGPUSampler SamplerOf(Sampler *sampler);

struct ShaderModule {
    const WgslShader     *wgsl = nullptr;
    std::vector<uint32_t> override_ids; // the WGSL's @id(n) overrides
    WGPUShaderModule      module = nullptr;
};

WGPUShaderModule ModuleOf(ShaderModule *shader);
// The empty fragment shader for pipelines that write no colour and have no fragment stage.
WGPUShaderModule EmptyFragmentModule();

struct LayoutBinding {
    uint32_t           binding = 0;
    VkDescriptorType   type = VK_DESCRIPTOR_TYPE_MAX_ENUM;
    VkShaderStageFlags stages = 0;
    Sampler           *immutable_sampler = nullptr;
};

struct DescriptorSetLayout {
    std::vector<LayoutBinding> bindings;
    bool                       push = false;
    WGPUBindGroupLayout        layout = nullptr;
};

WGPUBindGroupLayout BindGroupLayoutOf(DescriptorSetLayout *layout);
// The push constants' group: one read-only storage binding at a dynamic offset.
WGPUBindGroupLayout PushGroupLayout();

struct PipelineLayout {
    std::vector<DescriptorSetLayout *> sets;
    uint32_t                           push_size = 0;
    WGPUPipelineLayout                 layout = nullptr;
};

WGPUPipelineLayout PipelineLayoutOf(PipelineLayout *layout);

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
    WGPUBindGroup                group = nullptr;
};

struct DescriptorPool {
    std::unordered_set<DescriptorSet *> sets;
};

WGPUBindGroup BindGroupOf(DescriptorSet *set);
// A bind group for a push descriptor set: the layout's bindings from the entries pushed, cached.
WGPUBindGroup PushedBindGroup(DescriptorSetLayout *layout, const std::vector<DescriptorEntry> &entries);
// A bind group for a set the draw's pipeline layout has but nothing bound: harmless stand-ins.
WGPUBindGroup FillerBindGroup(DescriptorSetLayout *layout);

void ReleaseBindGroupsOf(ImageView *view);

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

// A WebGPU pipeline for one combination of the state WebGPU bakes in.
struct PipelineVariant {
    WGPURenderPipeline pipeline = nullptr;
    // Without dual-source blending: the single-source shader writes the blend factor as alpha, which the
    // pipeline then leaves out of the target (commands.cpp).
    bool factor_in_alpha = false;
    // WebGPU's constant factor is per channel: Vulkan's CONSTANT_ALPHA takes the constant's alpha for all.
    bool constant_alpha = false;
};

struct Pipeline {
    PipelineLayout              *layout = nullptr;
    ShaderModule                *vertex = nullptr;
    ShaderModule                *fragment = nullptr;
    std::vector<std::pair<uint32_t, double>> vertex_constants;
    std::vector<std::pair<uint32_t, double>> fragment_constants;
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
    // The state the pipeline fixes, applied when it is bound, and which of it is dynamic instead.
    DynamicState              fixed;
    std::unordered_set<VkDynamicState> dynamic;
    // WebGPU pipelines by the state they bake in (commands.cpp).
    std::unordered_map<std::string, PipelineVariant> variants;
};

void DestroyPipelineVariants(Pipeline *pipeline);

// A map-read buffer a copy lands in, unpacked into dst's bytes once its submission is done.
struct Readback {
    WGPUBuffer   staging = nullptr;
    uint32_t     padded_row = 0; // bytes per row in staging, a multiple of 256
    uint32_t     row = 0;        // bytes per row in dst
    uint32_t     rows = 0;
    Buffer      *dst = nullptr;
    VkDeviceSize dst_offset = 0;
};

struct Fence {
    bool                  signaled = false;
    uint64_t              serial = 0; // the submission it waits for
    std::vector<Readback> readbacks;
};

// Submissions are numbered; the queue reports each one done in order.
uint64_t SubmitSerial();
uint64_t CompletedSerial();
void     WaitForSerial(uint64_t serial);
void     FinishReadbacks(std::vector<Readback> &readbacks);

// Ends a command buffer's encoding and hands over its commands and readbacks, after uploading what its
// commands read from wasm memory (commands.cpp).
void TakeSubmission(VkCommandBuffer cmd, std::vector<WGPUCommandBuffer> &commands, std::vector<Readback> &readbacks);

// Buffers with bytes written since the last submit.
void MarkDirty(Buffer *buffer, VkDeviceSize offset, VkDeviceSize size);
void ForgetDirty(Buffer *buffer);
void UploadDirty();

struct Swapchain {
    std::vector<Image *> images;
    uint32_t             width = 0;
    uint32_t             height = 0;
    uint32_t             next = 0;
    int32_t              acquired = -1;
};

struct Context {
    WGPUInstance      instance = nullptr;
    WGPUAdapter       adapter = nullptr;
    WGPUDevice        device = nullptr;
    WGPUQueue         queue = nullptr;
    WGPUSurface       surface = nullptr; // the page's canvas
    WGPUTextureFormat surface_format = WGPUTextureFormat_BGRA8Unorm;
    bool              dual_source = false;
    bool              depth32_stencil8 = false;
    WGPULimits        limits = {};
    char              adapter_name[VK_MAX_PHYSICAL_DEVICE_NAME_SIZE] = {};
};

extern Context g;

WGPUTextureFormat TextureFormat(VkFormat format);
bool              IsDepthFormat(VkFormat format);
uint32_t          TexelBytes(VkFormat format);

} // namespace vkwebgpu

// A command buffer: its WebGPU encoder and the Vulkan state it tracks while recording.
struct VkCommandBuffer_T {
    vkwebgpu::CommandState *state = nullptr;
};

// Called by the renderer's transient allocator (gfx/memory.cpp) for each span it hands out.
extern "C" void vkWebGpuMarkWritten(VkBuffer buffer, VkDeviceSize offset, VkDeviceSize size);
// The page's canvas as a surface, in place of SDL_Vulkan_CreateSurface (gfx/renderer.cpp).
extern "C" VkResult vkCreateCanvasSurfaceWEB(VkInstance instance, VkSurfaceKHR *surface);

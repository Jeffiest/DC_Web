// Vulkan on WebGPU (layer.hpp): memory, buffers, images and views, samplers, shader modules, descriptor set
// layouts, pools and sets, pipeline layouts and pipelines (as descriptions), pipeline caches and query pools.

#include "layer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <string>

#include "shaders/depth_planes.frag.spv.hpp"
#include "shaders/draw.frag.spv.hpp"
#include "shaders/draw2d.vert.spv.hpp"
#include "shaders/draw_bound.frag.spv.hpp"
#include "shaders/draw_bound_single.frag.spv.hpp"
#include "shaders/draw_single.frag.spv.hpp"
#include "shaders/mesh.vert.spv.hpp"
#include "shaders/mesh_prog0.vert.spv.hpp"

namespace vkwebgpu {

namespace {

uint64_t g_next_view_id = 1;

// Bind groups made for pushed descriptor sets, by what they hold.
std::unordered_map<std::string, WGPUBindGroup> g_pushed_groups;
std::unordered_map<DescriptorSetLayout *, WGPUBindGroup> g_filler_groups;

WGPUTexture     g_filler_texture = nullptr;
WGPUTextureView g_filler_view = nullptr;
WGPUSampler     g_filler_sampler = nullptr;
WGPUBuffer      g_filler_buffer = nullptr;
constexpr uint64_t kFillerBufferSize = 64 * 1024;

WGPUBindGroupLayout g_push_layout = nullptr;
WGPUShaderModule    g_empty_fragment = nullptr;

void EnsureFillers() {
    if (g_filler_view != nullptr) {
        return;
    }
    WGPUTextureDescriptor texture = {};
    texture.usage = WGPUTextureUsage_TextureBinding;
    texture.dimension = WGPUTextureDimension_2D;
    texture.size = {1, 1, 1};
    texture.format = WGPUTextureFormat_RGBA8Unorm;
    texture.mipLevelCount = 1;
    texture.sampleCount = 1;
    g_filler_texture = wgpuDeviceCreateTexture(g.device, &texture);
    g_filler_view = wgpuTextureCreateView(g_filler_texture, nullptr);
    WGPUSamplerDescriptor sampler = {};
    sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = WGPUAddressMode_ClampToEdge;
    sampler.magFilter = sampler.minFilter = WGPUFilterMode_Nearest;
    sampler.mipmapFilter = WGPUMipmapFilterMode_Nearest;
    sampler.lodMaxClamp = 32.0f;
    sampler.maxAnisotropy = 1;
    g_filler_sampler = wgpuDeviceCreateSampler(g.device, &sampler);
    WGPUBufferDescriptor buffer = {};
    buffer.usage = WGPUBufferUsage_Storage | WGPUBufferUsage_Uniform;
    buffer.size = kFillerBufferSize;
    g_filler_buffer = wgpuDeviceCreateBuffer(g.device, &buffer);
}

WGPUShaderStage Stages(VkShaderStageFlags stages) {
    WGPUShaderStage result = WGPUShaderStage_None;
    if (stages & VK_SHADER_STAGE_VERTEX_BIT) {
        result |= WGPUShaderStage_Vertex;
    }
    if (stages & VK_SHADER_STAGE_FRAGMENT_BIT) {
        result |= WGPUShaderStage_Fragment;
    }
    return result;
}

WGPUAddressMode AddressMode(VkSamplerAddressMode mode) {
    switch (mode) {
        case VK_SAMPLER_ADDRESS_MODE_REPEAT:
            return WGPUAddressMode_Repeat;
        case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
            return WGPUAddressMode_MirrorRepeat;
        default:
            return WGPUAddressMode_ClampToEdge;
    }
}

// The renderer's SPIR-V modules, matched by their words, and the WGSL that stands for each.
struct KnownModule {
    const char     *name;
    const uint32_t *code;
    size_t          bytes;
};

const KnownModule kKnownModules[] = {
    {"draw2d_vert",            draw2d_vert,            sizeof(draw2d_vert)           },
    {"mesh_vert",              mesh_vert,              sizeof(mesh_vert)             },
    {"mesh_prog0_vert",        mesh_prog0_vert,        sizeof(mesh_prog0_vert)       },
    {"draw_bound_frag",        draw_bound_frag,        sizeof(draw_bound_frag)       },
    {"draw_bound_single_frag", draw_bound_single_frag, sizeof(draw_bound_single_frag)},
    {"draw_frag",              draw_frag,              sizeof(draw_frag)             },
    {"draw_single_frag",       draw_single_frag,       sizeof(draw_single_frag)      },
    {"depth_planes_frag",      depth_planes_frag,      sizeof(depth_planes_frag)     },
};

const WgslShader *FindWgsl(const char *name) {
    for (size_t i = 0; i < kWgslShaderCount; i++) {
        if (std::strcmp(kWgslShaders[i].name, name) == 0) {
            return &kWgslShaders[i];
        }
    }
    return nullptr;
}

std::vector<uint32_t> OverrideIds(const char *code) {
    std::vector<uint32_t> ids;
    for (const char *at = std::strstr(code, "@id("); at != nullptr; at = std::strstr(at + 4, "@id(")) {
        ids.push_back(static_cast<uint32_t>(std::strtoul(at + 4, nullptr, 10)));
    }
    return ids;
}

WGPUShaderModule CompileWgsl(const char *label, const char *code) {
    WGPUShaderSourceWGSL source = {};
    source.chain.sType = WGPUSType_ShaderSourceWGSL;
    source.code = View(code);
    WGPUShaderModuleDescriptor descriptor = {};
    descriptor.nextInChain = &source.chain;
    descriptor.label = View(label);
    return wgpuDeviceCreateShaderModule(g.device, &descriptor);
}

void AppendKey(std::string &key, const void *data, size_t size) {
    key.append(static_cast<const char *>(data), size);
}

} // namespace

WGPUTextureView ViewOf(ImageView *view) {
    Image *image = view->image;
    if (view->view != nullptr && view->generation == image->generation) {
        return view->view;
    }
    if (view->view != nullptr) {
        wgpuTextureViewRelease(view->view);
        view->view = nullptr;
    }
    if (image->texture == nullptr) {
        return nullptr;
    }
    WGPUTextureViewDescriptor descriptor = {};
    descriptor.format = image->format;
    descriptor.dimension = WGPUTextureViewDimension_2D;
    descriptor.baseMipLevel = view->base_mip;
    descriptor.mipLevelCount = view->mip_count;
    descriptor.baseArrayLayer = 0;
    descriptor.arrayLayerCount = 1;
    descriptor.aspect = IsDepthFormat(image->vk_format) && !(view->aspect & VK_IMAGE_ASPECT_STENCIL_BIT)
                            ? WGPUTextureAspect_DepthOnly
                            : WGPUTextureAspect_All;
    descriptor.usage = WGPUTextureUsage_None;
    view->view = wgpuTextureCreateView(image->texture, &descriptor);
    view->generation = image->generation;
    return view->view;
}

WGPUTextureView MipView(Image *image, uint32_t mip, WGPUTextureAspect aspect) {
    WGPUTextureViewDescriptor descriptor = {};
    descriptor.format = image->format;
    descriptor.dimension = WGPUTextureViewDimension_2D;
    descriptor.baseMipLevel = mip;
    descriptor.mipLevelCount = 1;
    descriptor.baseArrayLayer = 0;
    descriptor.arrayLayerCount = 1;
    descriptor.aspect = aspect;
    return wgpuTextureCreateView(image->texture, &descriptor);
}

WGPUSampler SamplerOf(Sampler *sampler) {
    if (sampler->sampler == nullptr) {
        sampler->sampler = wgpuDeviceCreateSampler(g.device, &sampler->descriptor);
    }
    return sampler->sampler;
}

WGPUShaderModule ModuleOf(ShaderModule *shader) {
    if (shader->module == nullptr) {
        shader->module = CompileWgsl(shader->wgsl->name, shader->wgsl->code);
    }
    return shader->module;
}

WGPUShaderModule EmptyFragmentModule() {
    if (g_empty_fragment == nullptr) {
        g_empty_fragment = CompileWgsl("empty fragment", "@fragment fn main() {}\n");
    }
    return g_empty_fragment;
}

WGPUBindGroupLayout BindGroupLayoutOf(DescriptorSetLayout *layout) {
    if (layout->layout != nullptr) {
        return layout->layout;
    }
    std::vector<WGPUBindGroupLayoutEntry> entries;
    for (const LayoutBinding &binding : layout->bindings) {
        WGPUBindGroupLayoutEntry entry = {};
        entry.binding = binding.binding;
        entry.visibility = Stages(binding.stages);
        switch (binding.type) {
            case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE:
                entry.texture.sampleType = WGPUTextureSampleType_Float;
                entry.texture.viewDimension = WGPUTextureViewDimension_2D;
                break;
            case VK_DESCRIPTOR_TYPE_SAMPLER:
                entry.sampler.type = WGPUSamplerBindingType_Filtering;
                break;
            case VK_DESCRIPTOR_TYPE_STORAGE_BUFFER:
                entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
                break;
            case VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER:
                entry.buffer.type = WGPUBufferBindingType_Uniform;
                break;
            default:
                Fatal("descriptor type %d has no WebGPU binding here", static_cast<int>(binding.type));
        }
        entries.push_back(entry);
    }
    WGPUBindGroupLayoutDescriptor descriptor = {};
    descriptor.entryCount = entries.size();
    descriptor.entries = entries.data();
    layout->layout = wgpuDeviceCreateBindGroupLayout(g.device, &descriptor);
    return layout->layout;
}

WGPUBindGroupLayout PushGroupLayout() {
    if (g_push_layout == nullptr) {
        WGPUBindGroupLayoutEntry entry = {};
        entry.binding = 0;
        entry.visibility = WGPUShaderStage_Vertex | WGPUShaderStage_Fragment;
        entry.buffer.type = WGPUBufferBindingType_ReadOnlyStorage;
        entry.buffer.hasDynamicOffset = true;
        WGPUBindGroupLayoutDescriptor descriptor = {};
        descriptor.entryCount = 1;
        descriptor.entries = &entry;
        g_push_layout = wgpuDeviceCreateBindGroupLayout(g.device, &descriptor);
    }
    return g_push_layout;
}

WGPUPipelineLayout PipelineLayoutOf(PipelineLayout *layout) {
    if (layout->layout == nullptr) {
        std::vector<WGPUBindGroupLayout> groups;
        for (DescriptorSetLayout *set : layout->sets) {
            groups.push_back(BindGroupLayoutOf(set));
        }
        groups.push_back(PushGroupLayout());
        WGPUPipelineLayoutDescriptor descriptor = {};
        descriptor.bindGroupLayoutCount = groups.size();
        descriptor.bindGroupLayouts = groups.data();
        layout->layout = wgpuDeviceCreatePipelineLayout(g.device, &descriptor);
    }
    return layout->layout;
}

namespace {

WGPUBindGroupEntry EntryFor(const LayoutBinding &binding, const DescriptorEntry *entry) {
    WGPUBindGroupEntry out = {};
    out.binding = binding.binding;
    switch (binding.type) {
        case VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE: {
            WGPUTextureView view = entry != nullptr && entry->view != nullptr ? ViewOf(entry->view) : nullptr;
            out.textureView = view != nullptr ? view : g_filler_view;
            break;
        }
        case VK_DESCRIPTOR_TYPE_SAMPLER: {
            Sampler *sampler = binding.immutable_sampler != nullptr ? binding.immutable_sampler
                               : entry != nullptr                   ? entry->sampler
                                                                    : nullptr;
            out.sampler = sampler != nullptr ? SamplerOf(sampler) : g_filler_sampler;
            break;
        }
        default: {
            if (entry != nullptr && entry->buffer != nullptr && entry->buffer->gpu != nullptr) {
                out.buffer = entry->buffer->gpu;
                out.offset = entry->offset;
                out.size = entry->range == VK_WHOLE_SIZE ? entry->buffer->size - entry->offset : entry->range;
            } else {
                out.buffer = g_filler_buffer;
                out.offset = 0;
                out.size = kFillerBufferSize;
            }
            break;
        }
    }
    return out;
}

const DescriptorEntry *FindEntry(const std::vector<DescriptorEntry> &entries, uint32_t binding) {
    for (const DescriptorEntry &entry : entries) {
        if (entry.binding == binding) {
            return &entry;
        }
    }
    return nullptr;
}

WGPUBindGroup MakeBindGroup(DescriptorSetLayout *layout, const std::vector<DescriptorEntry> &entries) {
    EnsureFillers();
    std::vector<WGPUBindGroupEntry> group_entries;
    for (const LayoutBinding &binding : layout->bindings) {
        group_entries.push_back(EntryFor(binding, FindEntry(entries, binding.binding)));
    }
    WGPUBindGroupDescriptor descriptor = {};
    descriptor.layout = BindGroupLayoutOf(layout);
    descriptor.entryCount = group_entries.size();
    descriptor.entries = group_entries.data();
    return wgpuDeviceCreateBindGroup(g.device, &descriptor);
}

} // namespace

WGPUBindGroup BindGroupOf(DescriptorSet *set) {
    if (set->group == nullptr) {
        set->group = MakeBindGroup(set->layout, set->entries);
    }
    return set->group;
}

WGPUBindGroup PushedBindGroup(DescriptorSetLayout *layout, const std::vector<DescriptorEntry> &entries) {
    std::string key;
    AppendKey(key, &layout, sizeof layout);
    for (const LayoutBinding &binding : layout->bindings) {
        const DescriptorEntry *entry = FindEntry(entries, binding.binding);
        uint64_t               id = 0;
        if (entry != nullptr && entry->view != nullptr) {
            // A swapchain image's view changes texture each frame.
            id = entry->view->id << 16 | (entry->view->image->generation & 0xFFFF);
        } else if (entry != nullptr && entry->sampler != nullptr) {
            id = reinterpret_cast<uintptr_t>(entry->sampler);
        } else if (entry != nullptr && entry->buffer != nullptr) {
            id = reinterpret_cast<uintptr_t>(entry->buffer) ^ entry->offset << 32;
        }
        AppendKey(key, &id, sizeof id);
    }
    auto it = g_pushed_groups.find(key);
    if (it != g_pushed_groups.end()) {
        return it->second;
    }
    // Views come and go; the cache is emptied rather than ever pruned entry by entry.
    if (g_pushed_groups.size() > 4096) {
        for (auto &[_, group] : g_pushed_groups) {
            wgpuBindGroupRelease(group);
        }
        g_pushed_groups.clear();
    }
    WGPUBindGroup group = MakeBindGroup(layout, entries);
    g_pushed_groups.emplace(std::move(key), group);
    return group;
}

WGPUBindGroup FillerBindGroup(DescriptorSetLayout *layout) {
    auto it = g_filler_groups.find(layout);
    if (it != g_filler_groups.end()) {
        return it->second;
    }
    WGPUBindGroup group = MakeBindGroup(layout, {});
    g_filler_groups.emplace(layout, group);
    return group;
}

void ReleaseBindGroupsOf(ImageView *) {
    // Keys carry view ids that are never reused, so a dead view's groups are only unused, never wrong: they
    // go when the cache is next emptied. Their textures stay alive until then, which WebGPU allows.
}

void DestroyPipelineVariants(Pipeline *pipeline) {
    for (auto &[_, variant] : pipeline->variants) {
        wgpuRenderPipelineRelease(variant.pipeline);
    }
    pipeline->variants.clear();
}

} // namespace vkwebgpu

using namespace vkwebgpu;

extern "C" {

// --- Memory -------------------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *info,
                                                const VkAllocationCallbacks *, VkDeviceMemory *memory) {
    auto *object = new Memory;
    object->size = info->allocationSize;
    object->type = info->memoryTypeIndex;
    if (object->type == kHostVisibleType) {
        // Room past the end, as uploads round up to 4 bytes.
        object->shadow = static_cast<uint8_t *>(std::calloc(static_cast<size_t>(object->size) + 16, 1));
        if (object->shadow == nullptr) {
            delete object;
            return VK_ERROR_OUT_OF_HOST_MEMORY;
        }
    }
    *memory = Handle<VkDeviceMemory>(object);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkFreeMemory(VkDevice, VkDeviceMemory memory, const VkAllocationCallbacks *) {
    if (auto *object = Get<Memory>(memory)) {
        std::free(object->shadow);
        delete object;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkMapMemory(VkDevice, VkDeviceMemory memory, VkDeviceSize offset, VkDeviceSize,
                                           VkMemoryMapFlags, void **data) {
    Memory *object = Get<Memory>(memory);
    if (object->shadow == nullptr) {
        return VK_ERROR_MEMORY_MAP_FAILED;
    }
    *data = object->shadow + offset;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUnmapMemory(VkDevice, VkDeviceMemory) {}

// --- Buffers ------------------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkCreateBuffer(VkDevice, const VkBufferCreateInfo *info, const VkAllocationCallbacks *,
                                              VkBuffer *buffer) {
    auto *object = new Buffer;
    object->size = info->size;
    object->usage = info->usage;
    *buffer = Handle<VkBuffer>(object);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyBuffer(VkDevice, VkBuffer buffer, const VkAllocationCallbacks *) {
    auto *object = Get<Buffer>(buffer);
    if (object == nullptr) {
        return;
    }
    ForgetDirty(object);
    if (object->gpu != nullptr) {
        wgpuBufferDestroy(object->gpu);
        wgpuBufferRelease(object->gpu);
    }
    delete object;
}

VKAPI_ATTR void VKAPI_CALL vkGetBufferMemoryRequirements(VkDevice, VkBuffer buffer,
                                                         VkMemoryRequirements *requirements) {
    const Buffer *object = Get<Buffer>(buffer);
    requirements->size = (object->size + 255) & ~VkDeviceSize{255};
    requirements->alignment = 256;
    requirements->memoryTypeBits = (1u << kDeviceLocalType) | (1u << kHostVisibleType);
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindBufferMemory(VkDevice, VkBuffer buffer, VkDeviceMemory memory,
                                                  VkDeviceSize offset) {
    AssertDeviceThread(__func__);
    Buffer *object = Get<Buffer>(buffer);
    Memory *backing = Get<Memory>(memory);
    if (backing->shadow != nullptr) {
        object->shadow = backing->shadow + offset;
        // What the GPU only copies into is read back through map-read buffers instead.
        if (object->usage == VK_BUFFER_USAGE_TRANSFER_DST_BIT) {
            return VK_SUCCESS;
        }
    }
    WGPUBufferDescriptor descriptor = {};
    descriptor.size = (object->size + 3) & ~VkDeviceSize{3};
    descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_CopySrc;
    if (object->usage & VK_BUFFER_USAGE_VERTEX_BUFFER_BIT) {
        descriptor.usage |= WGPUBufferUsage_Vertex;
    }
    if (object->usage & VK_BUFFER_USAGE_INDEX_BUFFER_BIT) {
        descriptor.usage |= WGPUBufferUsage_Index;
    }
    if (object->usage & VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT) {
        descriptor.usage |= WGPUBufferUsage_Uniform;
    }
    if (object->usage & VK_BUFFER_USAGE_STORAGE_BUFFER_BIT) {
        descriptor.usage |= WGPUBufferUsage_Storage;
    }
    object->gpu = wgpuDeviceCreateBuffer(g.device, &descriptor);
    return object->gpu != nullptr ? VK_SUCCESS : VK_ERROR_OUT_OF_DEVICE_MEMORY;
}

// --- Images and views ---------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice, const VkImageCreateInfo *info, const VkAllocationCallbacks *,
                                             VkImage *image) {
    AssertDeviceThread(__func__);
    auto *object = new Image;
    object->vk_format = info->format;
    object->format = TextureFormat(info->format);
    object->width = info->extent.width;
    object->height = info->extent.height;
    object->mips = info->mipLevels;
    const bool  depth = IsDepthFormat(info->format);
    WGPUTextureDescriptor descriptor = {};
    descriptor.dimension = WGPUTextureDimension_2D;
    descriptor.size = {info->extent.width, info->extent.height, 1};
    descriptor.format = object->format;
    descriptor.mipLevelCount = info->mipLevels;
    descriptor.sampleCount = 1;
    descriptor.usage = WGPUTextureUsage_None;
    if (info->usage & VK_IMAGE_USAGE_SAMPLED_BIT) {
        descriptor.usage |= WGPUTextureUsage_TextureBinding;
    }
    if (info->usage & (VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)) {
        descriptor.usage |= WGPUTextureUsage_RenderAttachment;
    }
    if (info->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) {
        // A blit source is sampled by the blit's draw.
        descriptor.usage |= WGPUTextureUsage_CopySrc | (depth ? WGPUTextureUsage_None : WGPUTextureUsage_TextureBinding);
    }
    if (info->usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT) {
        // A blit or clear destination is drawn into.
        descriptor.usage |= depth ? WGPUTextureUsage_RenderAttachment
                                  : WGPUTextureUsage_CopyDst | WGPUTextureUsage_RenderAttachment;
    }
    object->texture = wgpuDeviceCreateTexture(g.device, &descriptor);
    if (object->texture == nullptr) {
        delete object;
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    *image = Handle<VkImage>(object);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImage(VkDevice, VkImage image, const VkAllocationCallbacks *) {
    auto *object = Get<Image>(image);
    if (object == nullptr) {
        return;
    }
    if (object->texture != nullptr) {
        wgpuTextureDestroy(object->texture);
        wgpuTextureRelease(object->texture);
    }
    delete object;
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements(VkDevice, VkImage image, VkMemoryRequirements *requirements) {
    const Image *object = Get<Image>(image);
    const VkDeviceSize base = VkDeviceSize{object->width} * object->height * TexelBytes(object->vk_format);
    requirements->size = ((object->mips > 1 ? base * 4 / 3 : base) + 255) & ~VkDeviceSize{255};
    requirements->alignment = 256;
    requirements->memoryTypeBits = 1u << kDeviceLocalType;
}

VKAPI_ATTR void VKAPI_CALL vkGetImageMemoryRequirements2(VkDevice device, const VkImageMemoryRequirementsInfo2 *info,
                                                         VkMemoryRequirements2 *requirements) {
    vkGetImageMemoryRequirements(device, info->image, &requirements->memoryRequirements);
    for (auto *next = static_cast<VkBaseOutStructure *>(requirements->pNext); next != nullptr; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_MEMORY_DEDICATED_REQUIREMENTS) {
            auto *dedicated = reinterpret_cast<VkMemoryDedicatedRequirements *>(next);
            dedicated->prefersDedicatedAllocation = VK_FALSE;
            dedicated->requiresDedicatedAllocation = VK_FALSE;
        }
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkBindImageMemory(VkDevice, VkImage, VkDeviceMemory, VkDeviceSize) {
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice, const VkImageViewCreateInfo *info,
                                                 const VkAllocationCallbacks *, VkImageView *view) {
    auto *object = new ImageView;
    object->image = Get<Image>(info->image);
    object->base_mip = info->subresourceRange.baseMipLevel;
    object->mip_count = info->subresourceRange.levelCount == VK_REMAINING_MIP_LEVELS
                            ? object->image->mips - object->base_mip
                            : info->subresourceRange.levelCount;
    object->aspect = info->subresourceRange.aspectMask;
    object->id = g_next_view_id++;
    *view = Handle<VkImageView>(object);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView view, const VkAllocationCallbacks *) {
    auto *object = Get<ImageView>(view);
    if (object == nullptr) {
        return;
    }
    ReleaseBindGroupsOf(object);
    if (object->view != nullptr) {
        wgpuTextureViewRelease(object->view);
    }
    delete object;
}

// --- Samplers -----------------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice, const VkSamplerCreateInfo *info,
                                               const VkAllocationCallbacks *, VkSampler *sampler) {
    auto                  *object = new Sampler;
    WGPUSamplerDescriptor &d = object->descriptor;
    d.addressModeU = AddressMode(info->addressModeU);
    d.addressModeV = AddressMode(info->addressModeV);
    d.addressModeW = AddressMode(info->addressModeW);
    d.magFilter = info->magFilter == VK_FILTER_LINEAR ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    d.minFilter = info->minFilter == VK_FILTER_LINEAR ? WGPUFilterMode_Linear : WGPUFilterMode_Nearest;
    d.mipmapFilter =
        info->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR ? WGPUMipmapFilterMode_Linear : WGPUMipmapFilterMode_Nearest;
    d.lodMinClamp = std::max(info->minLod, 0.0f);
    d.lodMaxClamp = std::clamp(info->maxLod, d.lodMinClamp, 32.0f);
    d.maxAnisotropy = 1;
    // WebGPU filters anisotropically only with every filter linear.
    if (info->anisotropyEnable && d.magFilter == WGPUFilterMode_Linear && d.minFilter == WGPUFilterMode_Linear &&
        d.mipmapFilter == WGPUMipmapFilterMode_Linear) {
        d.maxAnisotropy = static_cast<uint16_t>(std::clamp(info->maxAnisotropy, 1.0f, 16.0f));
    }
    *sampler = Handle<VkSampler>(object);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice, VkSampler sampler, const VkAllocationCallbacks *) {
    auto *object = Get<Sampler>(sampler);
    if (object == nullptr) {
        return;
    }
    if (object->sampler != nullptr) {
        wgpuSamplerRelease(object->sampler);
    }
    delete object;
}

// --- Shader modules -----------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *info,
                                                    const VkAllocationCallbacks *, VkShaderModule *module) {
    for (const KnownModule &known : kKnownModules) {
        if (known.bytes == info->codeSize && std::memcmp(known.code, info->pCode, known.bytes) == 0) {
            const WgslShader *wgsl = FindWgsl(known.name);
            if (wgsl == nullptr) {
                Warn("no WGSL for %s", known.name);
                return VK_ERROR_INVALID_SHADER_NV;
            }
            auto *object = new ShaderModule;
            object->wgsl = wgsl;
            object->override_ids = OverrideIds(wgsl->code);
            *module = Handle<VkShaderModule>(object);
            return VK_SUCCESS;
        }
    }
    Warn("a SPIR-V module of %zu bytes is not one of the renderer's", info->codeSize);
    return VK_ERROR_INVALID_SHADER_NV;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice, VkShaderModule module, const VkAllocationCallbacks *) {
    // Pipelines compile their variants later, so the module stays for them; its WGSL is static.
    (void)module;
}

// --- Descriptor set layouts, pools and sets -----------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorSetLayout(VkDevice, const VkDescriptorSetLayoutCreateInfo *info,
                                                           const VkAllocationCallbacks *,
                                                           VkDescriptorSetLayout *layout) {
    auto *object = new DescriptorSetLayout;
    object->push = (info->flags & VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR) != 0;
    for (uint32_t i = 0; i < info->bindingCount; i++) {
        const VkDescriptorSetLayoutBinding &binding = info->pBindings[i];
        if (binding.descriptorCount != 1) {
            Warn("descriptor arrays are not supported (binding %u has %u)", binding.binding, binding.descriptorCount);
            delete object;
            return VK_ERROR_FEATURE_NOT_PRESENT;
        }
        LayoutBinding entry;
        entry.binding = binding.binding;
        entry.type = binding.descriptorType;
        entry.stages = binding.stageFlags;
        if (binding.pImmutableSamplers != nullptr) {
            entry.immutable_sampler = Get<Sampler>(binding.pImmutableSamplers[0]);
        }
        object->bindings.push_back(entry);
    }
    *layout = Handle<VkDescriptorSetLayout>(object);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorSetLayout(VkDevice, VkDescriptorSetLayout layout,
                                                        const VkAllocationCallbacks *) {
    auto *object = Get<DescriptorSetLayout>(layout);
    if (object == nullptr) {
        return;
    }
    if (object->layout != nullptr) {
        wgpuBindGroupLayoutRelease(object->layout);
    }
    delete object;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDescriptorPool(VkDevice, const VkDescriptorPoolCreateInfo *,
                                                      const VkAllocationCallbacks *, VkDescriptorPool *pool) {
    *pool = Handle<VkDescriptorPool>(new DescriptorPool);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDescriptorPool(VkDevice, VkDescriptorPool pool, const VkAllocationCallbacks *) {
    auto *object = Get<DescriptorPool>(pool);
    if (object == nullptr) {
        return;
    }
    for (DescriptorSet *set : object->sets) {
        if (set->group != nullptr) {
            wgpuBindGroupRelease(set->group);
        }
        delete set;
    }
    delete object;
}

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateDescriptorSets(VkDevice, const VkDescriptorSetAllocateInfo *info,
                                                        VkDescriptorSet *sets) {
    auto *pool = Get<DescriptorPool>(info->descriptorPool);
    for (uint32_t i = 0; i < info->descriptorSetCount; i++) {
        auto *set = new DescriptorSet;
        set->layout = Get<DescriptorSetLayout>(info->pSetLayouts[i]);
        set->pool = pool;
        pool->sets.insert(set);
        sets[i] = Handle<VkDescriptorSet>(set);
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkFreeDescriptorSets(VkDevice, VkDescriptorPool, uint32_t count,
                                                    const VkDescriptorSet *sets) {
    for (uint32_t i = 0; i < count; i++) {
        auto *set = Get<DescriptorSet>(sets[i]);
        if (set == nullptr) {
            continue;
        }
        set->pool->sets.erase(set);
        if (set->group != nullptr) {
            wgpuBindGroupRelease(set->group);
        }
        delete set;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkUpdateDescriptorSets(VkDevice, uint32_t count, const VkWriteDescriptorSet *writes,
                                                  uint32_t copy_count, const VkCopyDescriptorSet *) {
    if (copy_count != 0) {
        Warn("descriptor copies are not supported");
    }
    for (uint32_t i = 0; i < count; i++) {
        const VkWriteDescriptorSet &write = writes[i];
        auto                       *set = Get<DescriptorSet>(write.dstSet);
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
        auto it = std::find_if(set->entries.begin(), set->entries.end(),
                               [&](const DescriptorEntry &e) { return e.binding == entry.binding; });
        if (it != set->entries.end()) {
            *it = entry;
        } else {
            set->entries.push_back(entry);
        }
        if (set->group != nullptr) {
            wgpuBindGroupRelease(set->group);
            set->group = nullptr;
        }
    }
}

// --- Pipeline layouts and pipelines -------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineLayout(VkDevice, const VkPipelineLayoutCreateInfo *info,
                                                      const VkAllocationCallbacks *, VkPipelineLayout *layout) {
    auto *object = new PipelineLayout;
    for (uint32_t i = 0; i < info->setLayoutCount; i++) {
        object->sets.push_back(Get<DescriptorSetLayout>(info->pSetLayouts[i]));
    }
    for (uint32_t i = 0; i < info->pushConstantRangeCount; i++) {
        const VkPushConstantRange &range = info->pPushConstantRanges[i];
        object->push_size = std::max(object->push_size, range.offset + range.size);
    }
    if (object->push_size > 256) {
        Warn("push constants of %u bytes exceed the 256 reported", object->push_size);
    }
    *layout = Handle<VkPipelineLayout>(object);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice, VkPipelineLayout layout, const VkAllocationCallbacks *) {
    auto *object = Get<PipelineLayout>(layout);
    if (object == nullptr) {
        return;
    }
    if (object->layout != nullptr) {
        wgpuPipelineLayoutRelease(object->layout);
    }
    delete object;
}

} // extern "C"

namespace {

std::vector<std::pair<uint32_t, double>> Specialize(const VkSpecializationInfo *info, const ShaderModule *module) {
    std::vector<std::pair<uint32_t, double>> constants;
    if (info == nullptr || module == nullptr) {
        return constants;
    }
    for (uint32_t i = 0; i < info->mapEntryCount; i++) {
        const VkSpecializationMapEntry &entry = info->pMapEntries[i];
        // WebGPU rejects constants the shader does not declare.
        if (std::find(module->override_ids.begin(), module->override_ids.end(), entry.constantID) ==
            module->override_ids.end()) {
            continue;
        }
        int32_t value = 0;
        std::memcpy(&value, static_cast<const uint8_t *>(info->pData) + entry.offset,
                    std::min<size_t>(entry.size, sizeof value));
        constants.emplace_back(entry.constantID, static_cast<double>(value));
    }
    return constants;
}

Pipeline *Describe(const VkGraphicsPipelineCreateInfo &info) {
    auto *pipeline = new Pipeline;
    pipeline->layout = Get<PipelineLayout>(info.layout);
    for (uint32_t i = 0; i < info.stageCount; i++) {
        const VkPipelineShaderStageCreateInfo &stage = info.pStages[i];
        ShaderModule                          *module = Get<ShaderModule>(stage.module);
        if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
            pipeline->vertex = module;
            pipeline->vertex_constants = Specialize(stage.pSpecializationInfo, module);
        } else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            pipeline->fragment = module;
            pipeline->fragment_constants = Specialize(stage.pSpecializationInfo, module);
        }
    }
    if (const VkPipelineVertexInputStateCreateInfo *input = info.pVertexInputState) {
        if (input->vertexBindingDescriptionCount > 0) {
            pipeline->stride = input->pVertexBindingDescriptions[0].stride;
        }
        for (uint32_t i = 0; i < input->vertexAttributeDescriptionCount; i++) {
            const VkVertexInputAttributeDescription &attribute = input->pVertexAttributeDescriptions[i];
            pipeline->attributes.push_back({attribute.location, attribute.offset, attribute.format});
        }
    }
    DynamicState &fixed = pipeline->fixed;
    if (info.pInputAssemblyState != nullptr) {
        fixed.topology = info.pInputAssemblyState->topology;
    }
    if (const VkPipelineRasterizationStateCreateInfo *raster = info.pRasterizationState) {
        fixed.cull = raster->cullMode;
        fixed.front_face = raster->frontFace;
    }
    if (const VkPipelineDepthStencilStateCreateInfo *depth = info.pDepthStencilState) {
        fixed.depth_test = depth->depthTestEnable;
        fixed.depth_write = depth->depthWriteEnable;
        fixed.depth_compare = depth->depthCompareOp;
        fixed.stencil_test = depth->stencilTestEnable;
        for (int face = 0; face < 2; face++) {
            const VkStencilOpState &op = face == 0 ? depth->front : depth->back;
            fixed.stencil[face] = {op.failOp, op.passOp, op.depthFailOp, op.compareOp,
                                   op.compareMask, op.writeMask, op.reference};
        }
    }
    if (const VkPipelineColorBlendStateCreateInfo *blend = info.pColorBlendState) {
        if (blend->attachmentCount > 0) {
            const VkPipelineColorBlendAttachmentState &a = blend->pAttachments[0];
            pipeline->blend = a.blendEnable;
            pipeline->src_color = a.srcColorBlendFactor;
            pipeline->dst_color = a.dstColorBlendFactor;
            pipeline->color_op = a.colorBlendOp;
            pipeline->src_alpha = a.srcAlphaBlendFactor;
            pipeline->dst_alpha = a.dstAlphaBlendFactor;
            pipeline->alpha_op = a.alphaBlendOp;
            pipeline->write_mask = a.colorWriteMask;
        }
        std::memcpy(fixed.blend_constants, blend->blendConstants, sizeof fixed.blend_constants);
    }
    if (const VkPipelineDynamicStateCreateInfo *dynamic = info.pDynamicState) {
        for (uint32_t i = 0; i < dynamic->dynamicStateCount; i++) {
            pipeline->dynamic.insert(dynamic->pDynamicStates[i]);
        }
    }
    return pipeline;
}

} // namespace

extern "C" {

// Descriptions only: pipeline creation runs on the renderer's worker threads, and WebGPU's pipelines are
// made per variant at the draws that need them (commands.cpp).
VKAPI_ATTR VkResult VKAPI_CALL vkCreateGraphicsPipelines(VkDevice, VkPipelineCache, uint32_t count,
                                                         const VkGraphicsPipelineCreateInfo *infos,
                                                         const VkAllocationCallbacks *, VkPipeline *pipelines) {
    for (uint32_t i = 0; i < count; i++) {
        for (auto *next = static_cast<const VkBaseInStructure *>(infos[i].pNext); next != nullptr; next = next->pNext) {
            if (next->sType == VK_STRUCTURE_TYPE_PIPELINE_CREATION_FEEDBACK_CREATE_INFO) {
                auto *feedback = reinterpret_cast<const VkPipelineCreationFeedbackCreateInfo *>(next);
                feedback->pPipelineCreationFeedback->flags = VK_PIPELINE_CREATION_FEEDBACK_VALID_BIT |
                    VK_PIPELINE_CREATION_FEEDBACK_APPLICATION_PIPELINE_CACHE_HIT_BIT;
            }
        }
        pipelines[i] = Handle<VkPipeline>(Describe(infos[i]));
    }
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipeline(VkDevice, VkPipeline pipeline, const VkAllocationCallbacks *) {
    auto *object = Get<Pipeline>(pipeline);
    if (object == nullptr) {
        return;
    }
    DestroyPipelineVariants(object);
    delete object;
}

// The browser caches compiled shaders itself; the renderer's cache holds only a header.
VKAPI_ATTR VkResult VKAPI_CALL vkCreatePipelineCache(VkDevice, const VkPipelineCacheCreateInfo *,
                                                     const VkAllocationCallbacks *, VkPipelineCache *cache) {
    static uint8_t caches;
    *cache = Handle<VkPipelineCache>(&caches);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineCache(VkDevice, VkPipelineCache, const VkAllocationCallbacks *) {}

// A cache of just its header, which matches the device, so the renderer keeps and accepts its cache file.
VKAPI_ATTR VkResult VKAPI_CALL vkGetPipelineCacheData(VkDevice, VkPipelineCache, size_t *size, void *data) {
    VkPipelineCacheHeaderVersionOne header = {};
    header.headerSize = sizeof header;
    header.headerVersion = VK_PIPELINE_CACHE_HEADER_VERSION_ONE;
    if (data == nullptr) {
        *size = sizeof header;
        return VK_SUCCESS;
    }
    if (*size < sizeof header) {
        *size = 0;
        return VK_INCOMPLETE;
    }
    std::memcpy(data, &header, sizeof header);
    *size = sizeof header;
    return VK_SUCCESS;
}

// No timestamps are reported, so no query pool is ever asked for; one that is gives no results.
VKAPI_ATTR VkResult VKAPI_CALL vkCreateQueryPool(VkDevice, const VkQueryPoolCreateInfo *,
                                                 const VkAllocationCallbacks *, VkQueryPool *) {
    return VK_ERROR_FEATURE_NOT_PRESENT;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyQueryPool(VkDevice, VkQueryPool, const VkAllocationCallbacks *) {}

VKAPI_ATTR VkResult VKAPI_CALL vkGetQueryPoolResults(VkDevice, VkQueryPool, uint32_t, uint32_t, size_t, void *,
                                                     VkDeviceSize, VkQueryResultFlags) {
    return VK_NOT_READY;
}

} // extern "C"

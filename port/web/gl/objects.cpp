// Vulkan on WebGL2 (layer.hpp): memory, buffers, images and views, samplers, shader modules, descriptor set
// layouts, pools and sets, pipeline layouts and pipelines (as descriptions), pipeline caches and query pools.

#include "layer.hpp"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <map>
#include <string>
#include <tuple>

#include "shaders/depth_planes.frag.spv.hpp"
#include "shaders/draw.frag.spv.hpp"
#include "shaders/draw2d.vert.spv.hpp"
#include "shaders/draw_bound.frag.spv.hpp"
#include "shaders/draw_bound_single.frag.spv.hpp"
#include "shaders/draw_single.frag.spv.hpp"
#include "shaders/mesh.vert.spv.hpp"
#include "shaders/mesh_prog0.vert.spv.hpp"

namespace vkgl {

namespace {

GLenum AddressMode(VkSamplerAddressMode mode) {
    switch (mode) {
        case VK_SAMPLER_ADDRESS_MODE_REPEAT:
            return GL_REPEAT;
        case VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT:
            return GL_MIRRORED_REPEAT;
        default:
            return GL_CLAMP_TO_EDGE;
    }
}

// The renderer's SPIR-V modules, matched by their words. The GLSL that stands for each is in shaders.cpp; the
// bindless ones and depth_planes have none, as Vulkan on WebGPU has none.
struct KnownModule {
    const char     *name;
    const uint32_t *code;
    size_t          bytes;
    bool            ported;
};

const KnownModule kKnownModules[] = {
    {"draw2d_vert",            draw2d_vert,            sizeof(draw2d_vert),            true },
    {"mesh_vert",              mesh_vert,              sizeof(mesh_vert),              true },
    {"mesh_prog0_vert",        mesh_prog0_vert,        sizeof(mesh_prog0_vert),        true },
    {"draw_bound_frag",        draw_bound_frag,        sizeof(draw_bound_frag),        true },
    {"draw_bound_single_frag", draw_bound_single_frag, sizeof(draw_bound_single_frag), true },
    {"draw_frag",              draw_frag,              sizeof(draw_frag),              false},
    {"draw_single_frag",       draw_single_frag,       sizeof(draw_single_frag),       false},
    {"depth_planes_frag",      depth_planes_frag,      sizeof(depth_planes_frag),      false},
};

// Framebuffers by their attachments, and which an image is in.
using FramebufferKey = std::tuple<Image *, uint32_t, Image *, uint32_t>;
std::map<FramebufferKey, GLuint>                       g_framebuffers;
std::unordered_map<Image *, std::vector<FramebufferKey>> g_framebuffers_of;

void Attach(GLenum attachment, Image *image, uint32_t mip) {
    glFramebufferTexture2D(GL_FRAMEBUFFER, attachment, GL_TEXTURE_2D, image->texture, static_cast<GLint>(mip));
}

} // namespace

GLuint FramebufferFor(Image *color, uint32_t color_mip, Image *depth, uint32_t depth_mip) {
    const FramebufferKey key{color, color_mip, depth, depth_mip};
    auto                 it = g_framebuffers.find(key);
    if (it != g_framebuffers.end()) {
        return it->second;
    }
    GLuint framebuffer = 0;
    glGenFramebuffers(1, &framebuffer);
    GLint previous = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &previous);
    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
    if (color != nullptr) {
        Attach(GL_COLOR_ATTACHMENT0, color, color_mip);
        const GLenum buffer = GL_COLOR_ATTACHMENT0;
        glDrawBuffers(1, &buffer);
        glReadBuffer(GL_COLOR_ATTACHMENT0);
    } else {
        const GLenum none = GL_NONE;
        glDrawBuffers(1, &none);
        glReadBuffer(GL_NONE);
    }
    if (depth != nullptr) {
        Attach(HasStencil(depth->vk_format) ? GL_DEPTH_STENCIL_ATTACHMENT : GL_DEPTH_ATTACHMENT, depth, depth_mip);
    }
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        Warn("a framebuffer is incomplete (0x%x)", status);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, static_cast<GLuint>(previous));
    g_framebuffers.emplace(key, framebuffer);
    if (color != nullptr) {
        g_framebuffers_of[color].push_back(key);
    }
    if (depth != nullptr && depth != color) {
        g_framebuffers_of[depth].push_back(key);
    }
    return framebuffer;
}

void ForgetFramebuffersOf(Image *image) {
    auto it = g_framebuffers_of.find(image);
    if (it == g_framebuffers_of.end()) {
        return;
    }
    for (const FramebufferKey &key : it->second) {
        auto found = g_framebuffers.find(key);
        if (found == g_framebuffers.end()) {
            continue;
        }
        glDeleteFramebuffers(1, &found->second);
        g_framebuffers.erase(found);
        // The other attachment's list keeps the key; it is gone from the map, so skipped there.
    }
    g_framebuffers_of.erase(it);
}

void MarkDirty(Buffer *buffer, VkDeviceSize offset, VkDeviceSize size) {
    if (size == 0) {
        return;
    }
    for (GlCopy *copy : {&buffer->vertex, &buffer->index}) {
        if (copy->name == 0) {
            continue;
        }
        if (copy->dirty_end == 0) {
            copy->dirty_begin = offset;
            copy->dirty_end = offset + size;
        } else {
            copy->dirty_begin = std::min(copy->dirty_begin, offset);
            copy->dirty_end = std::max(copy->dirty_end, offset + size);
        }
    }
}

GLuint GlBufferFor(Buffer *buffer, GLenum target) {
    GlCopy &copy = target == GL_ELEMENT_ARRAY_BUFFER ? buffer->index : buffer->vertex;
    if (copy.name == 0) {
        glGenBuffers(1, &copy.name);
        glBindBuffer(target, copy.name);
        glBufferData(target, static_cast<GLsizeiptr>(buffer->size), buffer->shadow, GL_DYNAMIC_DRAW);
        copy.dirty_begin = copy.dirty_end = 0;
        return copy.name;
    }
    if (copy.dirty_end > copy.dirty_begin) {
        const VkDeviceSize end = std::min(copy.dirty_end, buffer->size);
        glBindBuffer(target, copy.name);
        if (copy.dirty_begin < end) {
            glBufferSubData(target, static_cast<GLintptr>(copy.dirty_begin), static_cast<GLsizeiptr>(end - copy.dirty_begin),
                            buffer->shadow + copy.dirty_begin);
        }
        copy.dirty_begin = copy.dirty_end = 0;
    }
    return copy.name;
}

void DeleteGlBuffers(Buffer *buffer) {
    for (GlCopy *copy : {&buffer->vertex, &buffer->index}) {
        if (copy->name != 0) {
            glDeleteBuffers(1, &copy->name);
            copy->name = 0;
        }
    }
}

void SamplerOf(Sampler *object) {
    if (object->sampler != 0) {
        return;
    }
    const VkSamplerCreateInfo *info = &object->info;
    const GLenum wrap_s = AddressMode(info->addressModeU);
    const GLenum wrap_t = AddressMode(info->addressModeV);
    const GLenum wrap_r = AddressMode(info->addressModeW);
    const float  min_lod = std::max(info->minLod, 0.0f);
    const float  max_lod = std::clamp(info->maxLod, min_lod, 32.0f);
    const bool   linear_mip = info->mipmapMode == VK_SAMPLER_MIPMAP_MODE_LINEAR;
    GLenum       min_filter = 0;
    if (info->minFilter == VK_FILTER_LINEAR) {
        min_filter = linear_mip ? GL_LINEAR_MIPMAP_LINEAR : GL_LINEAR_MIPMAP_NEAREST;
    } else {
        min_filter = linear_mip ? GL_NEAREST_MIPMAP_LINEAR : GL_NEAREST_MIPMAP_NEAREST;
    }
    for (GLuint *name : {&object->sampler, &object->nearest}) {
        glGenSamplers(1, name);
        glSamplerParameteri(*name, GL_TEXTURE_WRAP_S, static_cast<GLint>(wrap_s));
        glSamplerParameteri(*name, GL_TEXTURE_WRAP_T, static_cast<GLint>(wrap_t));
        glSamplerParameteri(*name, GL_TEXTURE_WRAP_R, static_cast<GLint>(wrap_r));
        glSamplerParameterf(*name, GL_TEXTURE_MIN_LOD, min_lod);
        glSamplerParameterf(*name, GL_TEXTURE_MAX_LOD, max_lod);
    }
    glSamplerParameteri(object->sampler, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(min_filter));
    glSamplerParameteri(object->sampler, GL_TEXTURE_MAG_FILTER,
                        info->magFilter == VK_FILTER_LINEAR ? GL_LINEAR : GL_NEAREST);
    // The twin reads single texels of the base level, as textureGather does.
    glSamplerParameteri(object->nearest, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glSamplerParameteri(object->nearest, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    // EXT_texture_filter_anisotropic, where the browser has it and every filter is linear (as Vulkan on WebGPU).
    if (info->anisotropyEnable && info->magFilter == VK_FILTER_LINEAR && info->minFilter == VK_FILTER_LINEAR &&
        linear_mip) {
        static const bool anisotropic = emscripten_webgl_enable_extension(g.context, "EXT_texture_filter_anisotropic");
        if (anisotropic) {
            glSamplerParameterf(object->sampler, 0x84FE /* TEXTURE_MAX_ANISOTROPY_EXT */,
                                std::clamp(info->maxAnisotropy, 1.0f, 16.0f));
        }
    }
}

} // namespace vkgl

using namespace vkgl;

extern "C" {

// --- Memory -------------------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkAllocateMemory(VkDevice, const VkMemoryAllocateInfo *info,
                                                const VkAllocationCallbacks *, VkDeviceMemory *memory) {
    auto *object = new Memory;
    object->size = info->allocationSize;
    object->type = info->memoryTypeIndex;
    if (object->type == kHostVisibleType) {
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
    DeleteGlBuffers(object);
    if (object->owns_shadow) {
        std::free(object->shadow);
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
    Buffer *object = Get<Buffer>(buffer);
    Memory *backing = Get<Memory>(memory);
    if (backing->shadow != nullptr) {
        object->shadow = backing->shadow + offset;
        return VK_SUCCESS;
    }
    // Device-local: its bytes are the layer's own, and its GL buffers are made when a draw first reads it.
    object->shadow = static_cast<uint8_t *>(std::calloc(static_cast<size_t>(object->size) + 16, 1));
    if (object->shadow == nullptr) {
        return VK_ERROR_OUT_OF_DEVICE_MEMORY;
    }
    object->owns_shadow = true;
    return VK_SUCCESS;
}

// --- Images and views ---------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkCreateImage(VkDevice, const VkImageCreateInfo *info, const VkAllocationCallbacks *,
                                             VkImage *image) {
    AssertDeviceThread(__func__);
    auto *object = new Image;
    object->vk_format = info->format;
    object->internal_format = FormatOf(info->format).internal_format;
    object->width = info->extent.width;
    object->height = info->extent.height;
    object->mips = info->mipLevels;
    glGenTextures(1, &object->texture);
    glBindTexture(GL_TEXTURE_2D, object->texture);
    glTexStorage2D(GL_TEXTURE_2D, static_cast<GLsizei>(object->mips), object->internal_format,
                   static_cast<GLsizei>(object->width), static_cast<GLsizei>(object->height));
    // Depth is read by texelFetch alone (shaders.cpp), which a depth texture takes only unfiltered.
    const GLint filter = IsDepthFormat(info->format) ? GL_NEAREST : GL_LINEAR;
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, filter);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    if (glGetError() == GL_OUT_OF_MEMORY) {
        glDeleteTextures(1, &object->texture);
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
    ForgetFramebuffersOf(object);
    glDeleteTextures(1, &object->texture);
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
    *view = Handle<VkImageView>(object);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView view, const VkAllocationCallbacks *) {
    delete Get<ImageView>(view);
}

// --- Samplers -----------------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSampler(VkDevice, const VkSamplerCreateInfo *info,
                                               const VkAllocationCallbacks *, VkSampler *sampler) {
    auto *object = new Sampler;
    object->info = *info;
    object->info.pNext = nullptr;
    *sampler = Handle<VkSampler>(object);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySampler(VkDevice, VkSampler sampler, const VkAllocationCallbacks *) {
    auto *object = Get<Sampler>(sampler);
    if (object == nullptr) {
        return;
    }
    if (object->sampler != 0) {
        glDeleteSamplers(1, &object->sampler);
        glDeleteSamplers(1, &object->nearest);
    }
    delete object;
}

// --- Shader modules -----------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkCreateShaderModule(VkDevice, const VkShaderModuleCreateInfo *info,
                                                    const VkAllocationCallbacks *, VkShaderModule *module) {
    for (const KnownModule &known : kKnownModules) {
        if (known.bytes == info->codeSize && std::memcmp(known.code, info->pCode, known.bytes) == 0) {
            if (!known.ported) {
                Warn("no GLSL for %s", known.name);
                return VK_ERROR_INVALID_SHADER_NV;
            }
            auto *object = new ShaderModule;
            object->name = known.name;
            *module = Handle<VkShaderModule>(object);
            return VK_SUCCESS;
        }
    }
    Warn("a SPIR-V module of %zu bytes is not one of the renderer's", info->codeSize);
    return VK_ERROR_INVALID_SHADER_NV;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyShaderModule(VkDevice, VkShaderModule module, const VkAllocationCallbacks *) {
    // Pipelines link their programs later, so the module stays for them; it holds only a name.
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
    delete Get<DescriptorSetLayout>(layout);
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
    if (object->push_size > kBlockBytes[kPushBlock]) {
        Warn("push constants of %u bytes exceed the %u the shaders' block holds", object->push_size,
             kBlockBytes[kPushBlock]);
    }
    *layout = Handle<VkPipelineLayout>(object);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyPipelineLayout(VkDevice, VkPipelineLayout layout, const VkAllocationCallbacks *) {
    delete Get<PipelineLayout>(layout);
}

} // extern "C"

namespace {

std::vector<std::pair<uint32_t, int32_t>> Specialize(const VkSpecializationInfo *info) {
    std::vector<std::pair<uint32_t, int32_t>> constants;
    if (info == nullptr) {
        return constants;
    }
    for (uint32_t i = 0; i < info->mapEntryCount; i++) {
        const VkSpecializationMapEntry &entry = info->pMapEntries[i];
        int32_t                         value = 0;
        std::memcpy(&value, static_cast<const uint8_t *>(info->pData) + entry.offset,
                    std::min<size_t>(entry.size, sizeof value));
        constants.emplace_back(entry.constantID, value);
    }
    std::sort(constants.begin(), constants.end());
    return constants;
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

Pipeline *Describe(const VkGraphicsPipelineCreateInfo &info) {
    auto *pipeline = new Pipeline;
    pipeline->layout = Get<PipelineLayout>(info.layout);
    for (uint32_t i = 0; i < info.stageCount; i++) {
        const VkPipelineShaderStageCreateInfo &stage = info.pStages[i];
        ShaderModule                          *module = Get<ShaderModule>(stage.module);
        if (stage.stage == VK_SHADER_STAGE_VERTEX_BIT) {
            pipeline->vertex = module;
            pipeline->vertex_constants = Specialize(stage.pSpecializationInfo);
        } else if (stage.stage == VK_SHADER_STAGE_FRAGMENT_BIT) {
            pipeline->fragment = module;
            pipeline->fragment_constants = Specialize(stage.pSpecializationInfo);
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
    // WebGL has no second blend source. The renderer only blends from one on targets without alpha, with the
    // factor in the second output and alpha forced to 1 in the first: here the factor is the first output's
    // alpha, which the target then does not take.
    if (pipeline->blend && (IsSecondSource(pipeline->src_color) || IsSecondSource(pipeline->dst_color) ||
                            IsSecondSource(pipeline->src_alpha) || IsSecondSource(pipeline->dst_alpha))) {
        pipeline->src_color = FirstSource(pipeline->src_color);
        pipeline->dst_color = FirstSource(pipeline->dst_color);
        pipeline->src_alpha = FirstSource(pipeline->src_alpha);
        pipeline->dst_alpha = FirstSource(pipeline->dst_alpha);
        pipeline->write_mask &= ~VK_COLOR_COMPONENT_A_BIT;
        pipeline->factor_in_alpha = true;
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

// Descriptions only: pipeline creation runs on the renderer's worker threads, and GL programs are linked at the
// draws that first need them (commands.cpp).
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
    // Programs are shared by every pipeline with the same shaders and kept.
    delete Get<Pipeline>(pipeline);
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

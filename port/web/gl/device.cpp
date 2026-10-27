// Vulkan on WebGL2 (layer.hpp): the instance, the one physical device and what it reports, the device and its
// queue, the canvas surface and its swapchain, fences, semaphores and submission.

#include "layer.hpp"

#include <emscripten/em_asm.h>
#include <emscripten/emscripten.h>
#include <emscripten/html5.h>

#include <algorithm>
#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <thread>

namespace vkgl {

Context g;

namespace {

VkInstance_T       g_instance_object;
VkPhysicalDevice_T g_physical_device_object;
VkDevice_T         g_device_object;
VkQueue_T          g_queue_object;
std::thread::id    g_device_thread;
bool               g_have_device = false;

constexpr uint32_t kSwapchainImages = 3;

// The surface handle: there is only the page's canvas.
struct Surface {};
Surface g_surface_object;

// Zeroes a chained output structure, keeping its place in the chain.
template <typename T> void Reset(T *structure) {
    const VkStructureType type = structure->sType;
    void *const           next = structure->pNext;
    std::memset(static_cast<void *>(structure), 0, sizeof *structure);
    structure->sType = type;
    structure->pNext = next;
}

void Copy(char *dst, size_t size, const char *src) {
    std::snprintf(dst, size, "%s", src);
}

template <typename T> VkResult Enumerate(const std::vector<T> &items, uint32_t *count, T *out) {
    if (out == nullptr) {
        *count = static_cast<uint32_t>(items.size());
        return VK_SUCCESS;
    }
    const uint32_t n = std::min<uint32_t>(*count, static_cast<uint32_t>(items.size()));
    std::copy_n(items.begin(), n, out);
    *count = n;
    return n < items.size() ? VK_INCOMPLETE : VK_SUCCESS;
}

VkExtensionProperties Extension(const char *name, uint32_t version = 1) {
    VkExtensionProperties properties = {};
    Copy(properties.extensionName, sizeof properties.extensionName, name);
    properties.specVersion = version;
    return properties;
}

// The page's canvas, given to this thread (OFFSCREENCANVASES_TO_PTHREAD), or where there is none (the renderer
// tests run offscreen without one) a canvas of the worker's own.
EMSCRIPTEN_WEBGL_CONTEXT_HANDLE CreateContext() {
    EmscriptenWebGLContextAttributes attributes;
    emscripten_webgl_init_context_attributes(&attributes);
    attributes.majorVersion = 2;
    attributes.minorVersion = 0;
    // The frames are drawn into textures and copied to the canvas at present: it needs no depth, and opaque it
    // ignores the GS frame's alpha.
    attributes.alpha = false;
    attributes.depth = false;
    attributes.stencil = false;
    attributes.antialias = false;
    attributes.premultipliedAlpha = false;
    attributes.preserveDrawingBuffer = false;
    attributes.powerPreference = EM_WEBGL_POWER_PREFERENCE_HIGH_PERFORMANCE;
    attributes.enableExtensionsByDefault = true;
    EMSCRIPTEN_WEBGL_CONTEXT_HANDLE context = emscripten_webgl_create_context("#canvas", &attributes);
    if (context > 0) {
        return context;
    }
    const int registered = EM_ASM_INT({
        if (typeof OffscreenCanvas === 'undefined') {
            return 0;
        }
        const canvas = new OffscreenCanvas(16, 16);
        const gl = canvas.getContext('webgl2', {alpha: false, depth: false, stencil: false, antialias: false,
                                                powerPreference: 'high-performance'});
        if (!gl) {
            return 0;
        }
        return GL.registerContext(gl, {majorVersion: 2, minorVersion: 0, enableExtensionsByDefault: true});
    });
    return registered;
}

bool EnsureContext() {
    if (g.context > 0) {
        return true;
    }
    g.context = CreateContext();
    if (g.context <= 0 || emscripten_webgl_make_context_current(g.context) != EMSCRIPTEN_RESULT_SUCCESS) {
        Warn("this browser has no WebGL2");
        g.context = 0;
        return false;
    }
    glGetIntegerv(GL_UNIFORM_BUFFER_OFFSET_ALIGNMENT, &g.ubo_alignment);
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &g.max_texture_size);
    glGetIntegerv(GL_MAX_UNIFORM_BLOCK_SIZE, &g.max_uniform_block_size);
    g.ubo_alignment = std::max(g.ubo_alignment, 16);
    const char *renderer = nullptr;
    if (emscripten_webgl_enable_extension(g.context, "WEBGL_debug_renderer_info")) {
        renderer = reinterpret_cast<const char *>(glGetString(0x9246 /* UNMASKED_RENDERER_WEBGL */));
    }
    if (renderer == nullptr) {
        renderer = reinterpret_cast<const char *>(glGetString(GL_RENDERER));
    }
    std::string name = std::string("WebGL2 ") + (renderer != nullptr ? renderer : "");
    Copy(g.renderer_name, sizeof g.renderer_name, name.c_str());
    std::fprintf(stderr, "webgl: %s\n", g.renderer_name);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    return true;
}

VkFormatFeatureFlags FormatFeatures(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_R8_UNORM:
            return VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                   VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                   VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT:
            // No blits, as Vulkan on WebGPU offers none: the renderer copies depth only where both do.
            return VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        default:
            return 0;
    }
}

void FillLimits(VkPhysicalDeviceLimits &limits) {
    const auto texture = static_cast<uint32_t>(g.max_texture_size);
    limits = {};
    limits.maxImageDimension1D = texture;
    limits.maxImageDimension2D = texture;
    limits.maxImageDimension3D = 256;
    limits.maxImageArrayLayers = 256;
    limits.maxUniformBufferRange = static_cast<uint32_t>(g.max_uniform_block_size);
    // Storage buffers are read through the buffers' bytes in wasm memory (commands.cpp): any size.
    limits.maxStorageBufferRange = 1u << 30;
    limits.maxPushConstantsSize = 256;
    limits.maxMemoryAllocationCount = 1u << 20;
    limits.maxSamplerAllocationCount = 4000;
    limits.maxBoundDescriptorSets = 4;
    limits.maxPerStageDescriptorSamplers = 16;
    limits.maxPerStageDescriptorUniformBuffers = 12;
    limits.maxPerStageDescriptorStorageBuffers = 8;
    limits.maxPerStageDescriptorSampledImages = 16;
    limits.maxPerStageResources = 64;
    limits.maxDescriptorSetSamplers = 16;
    limits.maxDescriptorSetStorageBuffers = 8;
    limits.maxDescriptorSetSampledImages = 16;
    limits.maxVertexInputAttributes = 16;
    limits.maxVertexInputBindings = 8;
    limits.maxVertexInputAttributeOffset = 2047;
    limits.maxVertexInputBindingStride = 2048;
    limits.maxVertexOutputComponents = 64;
    limits.maxFragmentInputComponents = 60;
    limits.maxFragmentOutputAttachments = 4;
    limits.maxFragmentDualSrcAttachments = 1;
    limits.maxClipDistances = 8;
    limits.maxCombinedClipAndCullDistances = 8;
    limits.maxColorAttachments = 4;
    limits.maxViewports = 1;
    limits.maxViewportDimensions[0] = texture;
    limits.maxViewportDimensions[1] = texture;
    limits.viewportBoundsRange[0] = -2.0f * static_cast<float>(texture);
    limits.viewportBoundsRange[1] = 2.0f * static_cast<float>(texture);
    limits.maxFramebufferWidth = texture;
    limits.maxFramebufferHeight = texture;
    limits.maxFramebufferLayers = 1;
    limits.framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits.framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits.framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits.framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits.sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits.sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits.maxSamplerAnisotropy = 16.0f;
    limits.maxSamplerLodBias = 0.0f;
    limits.minUniformBufferOffsetAlignment = static_cast<VkDeviceSize>(g.ubo_alignment);
    limits.minStorageBufferOffsetAlignment = 256;
    limits.minMemoryMapAlignment = 64;
    limits.optimalBufferCopyOffsetAlignment = 4;
    limits.optimalBufferCopyRowPitchAlignment = 4;
    limits.nonCoherentAtomSize = 4;
    limits.bufferImageGranularity = 1;
    limits.timestampComputeAndGraphics = VK_FALSE;
    limits.timestampPeriod = 0.0f;
    limits.lineWidthRange[0] = 1.0f;
    limits.lineWidthRange[1] = 1.0f;
    limits.pointSizeRange[0] = 1.0f;
    limits.pointSizeRange[1] = 1.0f;
}

} // namespace

void Fatal(const char *format, ...) {
    va_list args;
    va_start(args, format);
    std::fprintf(stderr, "webgl: ");
    std::vfprintf(stderr, format, args);
    std::fprintf(stderr, "\n");
    va_end(args);
    std::abort();
}

void Warn(const char *format, ...) {
    va_list args;
    va_start(args, format);
    std::fprintf(stderr, "webgl: ");
    std::vfprintf(stderr, format, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

void AssertDeviceThread(const char *what) {
    if (g_have_device && std::this_thread::get_id() != g_device_thread) {
        Fatal("%s off the device's thread: the GL context is current on the thread that made the device", what);
    }
}

GlFormat FormatOf(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
            return {GL_RGBA8, GL_RGBA, GL_UNSIGNED_BYTE};
        case VK_FORMAT_R8_UNORM:
            return {GL_R8, GL_RED, GL_UNSIGNED_BYTE};
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return {GL_DEPTH32F_STENCIL8, GL_DEPTH_STENCIL, GL_FLOAT_32_UNSIGNED_INT_24_8_REV};
        case VK_FORMAT_D24_UNORM_S8_UINT:
            return {GL_DEPTH24_STENCIL8, GL_DEPTH_STENCIL, GL_UNSIGNED_INT_24_8};
        case VK_FORMAT_D32_SFLOAT:
            return {GL_DEPTH_COMPONENT32F, GL_DEPTH_COMPONENT, GL_FLOAT};
        default:
            Fatal("no WebGL format for VkFormat %d", static_cast<int>(format));
    }
}

bool IsDepthFormat(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D32_SFLOAT;
}

bool HasStencil(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT;
}

uint32_t TexelBytes(VkFormat format) { return format == VK_FORMAT_R8_UNORM ? 1 : 4; }

uint32_t MipSize(uint32_t size, uint32_t mip) { return std::max(size >> mip, 1u); }

} // namespace vkgl

using namespace vkgl;

extern "C" {

// --- The instance -------------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceVersion(uint32_t *version) {
    *version = VK_API_VERSION_1_3;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceExtensionProperties(const char *layer, uint32_t *count,
                                                                     VkExtensionProperties *properties) {
    if (layer != nullptr) {
        *count = 0;
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    return Enumerate(std::vector{Extension(VK_KHR_SURFACE_EXTENSION_NAME, 25),
                                 Extension(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME)},
                     count, properties);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateInstanceLayerProperties(uint32_t *count, VkLayerProperties *) {
    *count = 0;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateInstance(const VkInstanceCreateInfo *, const VkAllocationCallbacks *,
                                                VkInstance *instance) {
    *instance = &g_instance_object;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance, const VkAllocationCallbacks *) {}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance, uint32_t *count, VkPhysicalDevice *devices) {
    if (!EnsureContext()) {
        *count = 0;
        return VK_SUCCESS;
    }
    return Enumerate(std::vector<VkPhysicalDevice>{&g_physical_device_object}, count, devices);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties(VkPhysicalDevice, VkPhysicalDeviceProperties *properties) {
    *properties = {};
    properties->apiVersion = VK_API_VERSION_1_3;
    properties->driverVersion = 1;
    properties->deviceType = VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU;
    Copy(properties->deviceName, sizeof properties->deviceName, g.renderer_name);
    FillLimits(properties->limits);
    properties->sparseProperties = {};
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceProperties2(VkPhysicalDevice device,
                                                          VkPhysicalDeviceProperties2 *properties) {
    vkGetPhysicalDeviceProperties(device, &properties->properties);
    for (auto *next = static_cast<VkBaseOutStructure *>(properties->pNext); next != nullptr; next = next->pNext) {
        if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_PROPERTIES_KHR) {
            reinterpret_cast<VkPhysicalDevicePortabilitySubsetPropertiesKHR *>(next)
                ->minVertexInputBindingStrideAlignment = 4;
        } else if (next->sType == VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_PROPERTIES) {
            Reset(reinterpret_cast<VkPhysicalDeviceVulkan12Properties *>(next));
        }
    }
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures(VkPhysicalDevice, VkPhysicalDeviceFeatures *features) {
    *features = {};
    // The layer draws dual-source blends from one source and clips in the fragment shader (commands.cpp).
    features->dualSrcBlend = VK_TRUE;
    features->shaderClipDistance = VK_TRUE;
    features->samplerAnisotropy = VK_TRUE;
    features->independentBlend = VK_TRUE;
    features->fullDrawIndexUint32 = VK_TRUE;
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFeatures2(VkPhysicalDevice device, VkPhysicalDeviceFeatures2 *features) {
    vkGetPhysicalDeviceFeatures(device, &features->features);
    for (auto *next = static_cast<VkBaseOutStructure *>(features->pNext); next != nullptr; next = next->pNext) {
        switch (next->sType) {
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES: {
                auto *f = reinterpret_cast<VkPhysicalDeviceVulkan13Features *>(next);
                Reset(f);
                f->dynamicRendering = VK_TRUE;
                f->synchronization2 = VK_TRUE;
                f->shaderDemoteToHelperInvocation = VK_TRUE;
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES: {
                Reset(reinterpret_cast<VkPhysicalDeviceVulkan12Features *>(next));
                break;
            }
            case VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PORTABILITY_SUBSET_FEATURES_KHR: {
                auto *f = reinterpret_cast<VkPhysicalDevicePortabilitySubsetFeaturesKHR *>(next);
                Reset(f);
                f->constantAlphaColorBlendFactors = VK_TRUE;
                f->imageViewFormatSwizzle = VK_TRUE;
                f->multisampleArrayImage = VK_TRUE;
                f->mutableComparisonSamplers = VK_TRUE;
                f->samplerMipLodBias = VK_TRUE;
                // WebGL takes one stencil reference and mask pair for both faces; fans are left out as Vulkan on
                // WebGPU leaves them out.
                f->triangleFans = VK_FALSE;
                f->separateStencilMaskRef = VK_FALSE;
                break;
            }
            default:
                break;
        }
    }
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceMemoryProperties(VkPhysicalDevice,
                                                               VkPhysicalDeviceMemoryProperties *properties) {
    *properties = {};
    properties->memoryHeapCount = 2;
    properties->memoryHeaps[0] = {VkDeviceSize{2} << 30, VK_MEMORY_HEAP_DEVICE_LOCAL_BIT};
    properties->memoryHeaps[1] = {VkDeviceSize{1} << 30, 0};
    properties->memoryTypeCount = 2;
    properties->memoryTypes[kDeviceLocalType] = {VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT, 0};
    properties->memoryTypes[kHostVisibleType] = {
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT,
        1};
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceQueueFamilyProperties(VkPhysicalDevice, uint32_t *count,
                                                                    VkQueueFamilyProperties *properties) {
    VkQueueFamilyProperties family = {};
    family.queueFlags = VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_TRANSFER_BIT;
    family.queueCount = 1;
    family.timestampValidBits = 0; // no timestamps
    family.minImageTransferGranularity = {1, 1, 1};
    Enumerate(std::vector{family}, count, properties);
}

VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat format,
                                                               VkFormatProperties *properties) {
    *properties = {};
    properties->optimalTilingFeatures = FormatFeatures(format);
}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumerateDeviceExtensionProperties(VkPhysicalDevice, const char *layer,
                                                                   uint32_t *count, VkExtensionProperties *properties) {
    if (layer != nullptr) {
        *count = 0;
        return VK_ERROR_LAYER_NOT_PRESENT;
    }
    return Enumerate(std::vector{Extension(VK_KHR_SWAPCHAIN_EXTENSION_NAME, 70),
                                 Extension(VK_KHR_PORTABILITY_SUBSET_EXTENSION_NAME),
                                 Extension(VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, 2)},
                     count, properties);
}

// --- The device ---------------------------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkCreateDevice(VkPhysicalDevice, const VkDeviceCreateInfo *,
                                              const VkAllocationCallbacks *, VkDevice *device) {
    if (!EnsureContext()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (!g_have_device) {
        g_device_thread = std::this_thread::get_id();
        g_have_device = true;
    }
    *device = &g_device_object;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice, const VkAllocationCallbacks *) {}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *queue) { *queue = &g_queue_object; }

// What was submitted has been given to WebGL, which orders it before anything after: nothing to wait for.
VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice) { return VK_SUCCESS; }

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue) { return VK_SUCCESS; }

// --- The canvas surface and its swapchain -------------------------------------------------------------------

VkResult vkCreateCanvasSurfaceWEB(VkInstance, VkSurfaceKHR *surface) {
    if (!EnsureContext()) {
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    *surface = Handle<VkSurfaceKHR>(&g_surface_object);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySurfaceKHR(VkInstance, VkSurfaceKHR, const VkAllocationCallbacks *) {}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceSupportKHR(VkPhysicalDevice, uint32_t, VkSurfaceKHR,
                                                                    VkBool32 *supported) {
    *supported = VK_TRUE;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceCapabilitiesKHR(VkPhysicalDevice, VkSurfaceKHR,
                                                                         VkSurfaceCapabilitiesKHR *capabilities) {
    const auto texture = static_cast<uint32_t>(g.max_texture_size);
    *capabilities = {};
    capabilities->minImageCount = 2;
    capabilities->maxImageCount = kSwapchainImages;
    // The canvas follows the window: the renderer takes SDL's pixel size.
    capabilities->currentExtent = {UINT32_MAX, UINT32_MAX};
    capabilities->minImageExtent = {1, 1};
    capabilities->maxImageExtent = {texture, texture};
    capabilities->maxImageArrayLayers = 1;
    capabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    capabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    capabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    capabilities->supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *count,
                                                                    VkSurfaceFormatKHR *formats) {
    return Enumerate(std::vector{VkSurfaceFormatKHR{VK_FORMAT_R8G8B8A8_UNORM, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}}, count,
                     formats);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR,
                                                                         uint32_t *count, VkPresentModeKHR *modes) {
    return Enumerate(std::vector{VK_PRESENT_MODE_FIFO_KHR}, count, modes);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR *info,
                                                    const VkAllocationCallbacks *, VkSwapchainKHR *swapchain) {
    AssertDeviceThread(__func__);
    auto *chain = new Swapchain;
    chain->width = info->imageExtent.width;
    chain->height = info->imageExtent.height;
    // The frames are drawn into textures of the layer's own and copied to the canvas when presented: the canvas
    // is shown whenever this thread yields, and its row 0 is at the bottom.
    for (uint32_t i = 0; i < kSwapchainImages; i++) {
        VkImageCreateInfo image = {};
        image.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        image.imageType = VK_IMAGE_TYPE_2D;
        image.format = VK_FORMAT_R8G8B8A8_UNORM;
        image.extent = {chain->width, chain->height, 1};
        image.mipLevels = 1;
        image.arrayLayers = 1;
        image.samples = VK_SAMPLE_COUNT_1_BIT;
        image.usage = info->imageUsage;
        VkImage handle = VK_NULL_HANDLE;
        if (vkCreateImage(device, &image, nullptr, &handle) != VK_SUCCESS) {
            Fatal("cannot make a swapchain image of %ux%u", chain->width, chain->height);
        }
        Get<Image>(handle)->swapchain = true;
        chain->images.push_back(Get<Image>(handle));
    }
    *swapchain = Handle<VkSwapchainKHR>(chain);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR handle, const VkAllocationCallbacks *) {
    auto *chain = Get<Swapchain>(handle);
    if (chain == nullptr) {
        return;
    }
    for (Image *image : chain->images) {
        vkDestroyImage(device, Handle<VkImage>(image), nullptr);
    }
    delete chain;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetSwapchainImagesKHR(VkDevice, VkSwapchainKHR handle, uint32_t *count,
                                                       VkImage *images) {
    auto                *chain = Get<Swapchain>(handle);
    std::vector<VkImage> handles;
    for (Image *image : chain->images) {
        handles.push_back(Handle<VkImage>(image));
    }
    return Enumerate(handles, count, images);
}

VKAPI_ATTR VkResult VKAPI_CALL vkAcquireNextImageKHR(VkDevice, VkSwapchainKHR handle, uint64_t, VkSemaphore,
                                                     VkFence fence, uint32_t *index) {
    AssertDeviceThread(__func__);
    auto          *chain = Get<Swapchain>(handle);
    const uint32_t slot = chain->next;
    chain->next = (chain->next + 1) % kSwapchainImages;
    if (fence != VK_NULL_HANDLE) {
        Get<Fence>(fence)->signaled = true;
    }
    *index = slot;
    return VK_SUCCESS;
}

// The page's Capture button (port/web/site/shell.js) asks for the next frames as the layer presents them.
std::atomic<int> g_capture_frames{0};

// Reads the frame back and hands it to the page as RGBA, opaque.
static void CaptureFrame(Image *image, uint32_t width, uint32_t height) {
    std::vector<uint8_t> pixels(static_cast<size_t>(width) * height * 4);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, FramebufferFor(image, 0, nullptr, 0));
    glReadPixels(0, 0, static_cast<GLsizei>(width), static_cast<GLsizei>(height), GL_RGBA, GL_UNSIGNED_BYTE,
                 pixels.data());
    for (size_t i = 3; i < pixels.size(); i += 4) {
        pixels[i] = 255;
    }
    const int remaining = g_capture_frames.fetch_sub(1) - 1;
    MAIN_THREAD_EM_ASM({
        // Memory may have grown since the main thread's views were made.
        const heap = HEAPU8.buffer === wasmMemory.buffer ? HEAPU8 : new Uint8Array(wasmMemory.buffer);
        if (Module.dcCaptured) {
            Module.dcCaptured(heap.slice($0, $0 + $1 * $2 * 4), $1, $2, $3);
        }
    }, pixels.data(), width, height, remaining);
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR *info) {
    AssertDeviceThread(__func__);
    VkResult overall = VK_SUCCESS;
    for (uint32_t i = 0; i < info->swapchainCount; i++) {
        auto    *chain = Get<Swapchain>(info->pSwapchains[i]);
        Image   *image = chain->images[info->pImageIndices[i]];
        VkResult result = VK_SUCCESS;
        if (g_capture_frames.load() > 0) {
            CaptureFrame(image, chain->width, chain->height);
        }
        // The canvas resizes with the page; a drawing buffer of another size means the swapchain is stale. What
        // overlaps is still shown, so the frame is not blank.
        int width = 0;
        int height = 0;
        emscripten_webgl_get_drawing_buffer_size(g.context, &width, &height);
        if (static_cast<uint32_t>(width) != chain->width || static_cast<uint32_t>(height) != chain->height) {
            result = VK_ERROR_OUT_OF_DATE_KHR;
        }
        const auto w = static_cast<GLint>(std::min<uint32_t>(width, chain->width));
        const auto h = static_cast<GLint>(std::min<uint32_t>(height, chain->height));
        glBindFramebuffer(GL_READ_FRAMEBUFFER, FramebufferFor(image, 0, nullptr, 0));
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
        glDisable(GL_SCISSOR_TEST);
        // The image's row 0 is the frame's top; the canvas's is its bottom.
        glBlitFramebuffer(0, 0, w, h, 0, height, w, height - h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
        glEnable(GL_SCISSOR_TEST);
        if (info->pResults != nullptr) {
            info->pResults[i] = result;
        }
        if (result != VK_SUCCESS) {
            overall = result;
        }
    }
    // The browser shows the canvas once this thread yields to it; there is no present call.
    emscripten_sleep(0);
    return overall;
}

// --- Synchronisation and submission -------------------------------------------------------------------------

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSemaphore(VkDevice, const VkSemaphoreCreateInfo *,
                                                 const VkAllocationCallbacks *, VkSemaphore *semaphore) {
    // One queue, in order: a semaphore orders nothing WebGL does not already.
    static uint8_t semaphores;
    *semaphore = Handle<VkSemaphore>(&semaphores);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySemaphore(VkDevice, VkSemaphore, const VkAllocationCallbacks *) {}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice, const VkFenceCreateInfo *info, const VkAllocationCallbacks *,
                                             VkFence *fence) {
    auto *object = new Fence;
    object->signaled = (info->flags & VK_FENCE_CREATE_SIGNALED_BIT) != 0;
    *fence = Handle<VkFence>(object);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice, VkFence fence, const VkAllocationCallbacks *) {
    delete Get<Fence>(fence);
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice, uint32_t count, const VkFence *fences) {
    for (uint32_t i = 0; i < count; i++) {
        Get<Fence>(fences[i])->signaled = false;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice, VkFence fence) {
    return Get<Fence>(fence)->signaled ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice, uint32_t count, const VkFence *fences, VkBool32 wait_all,
                                               uint64_t) {
    // A fence is signalled at its submission, or by an acquire; one never submitted is never signalled.
    bool all = true;
    bool any = false;
    for (uint32_t i = 0; i < count; i++) {
        const bool signaled = Get<Fence>(fences[i])->signaled;
        all = all && signaled;
        any = any || signaled;
    }
    return (wait_all ? all : any) ? VK_SUCCESS : VK_TIMEOUT;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence) {
    AssertDeviceThread(__func__);
    for (uint32_t s = 0; s < count; s++) {
        for (uint32_t c = 0; c < submits[s].commandBufferInfoCount; c++) {
            Execute(submits[s].pCommandBufferInfos[c].commandBuffer);
        }
    }
    if (fence != VK_NULL_HANDLE) {
        Get<Fence>(fence)->signaled = true;
    }
    return VK_SUCCESS;
}

// --- Entry points by name: the extension commands the renderer looks up ------------------------------------

VKAPI_ATTR void VKAPI_CALL vkCmdPushDescriptorSetKHR(VkCommandBuffer, VkPipelineBindPoint, VkPipelineLayout, uint32_t,
                                                     uint32_t, const VkWriteDescriptorSet *);

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetDeviceProcAddr(VkDevice, const char *name) {
    if (std::strcmp(name, "vkCmdPushDescriptorSetKHR") == 0) {
        return reinterpret_cast<PFN_vkVoidFunction>(&vkCmdPushDescriptorSetKHR);
    }
    return nullptr;
}

VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL vkGetInstanceProcAddr(VkInstance, const char *name) {
    return vkGetDeviceProcAddr(&g_device_object, name);
}

} // extern "C"

// The page's Capture button: the next frames presented are read back and handed to it (CaptureFrame).
extern "C" EMSCRIPTEN_KEEPALIVE void dc_web_capture(int frames) {
    g_capture_frames.store(frames);
}

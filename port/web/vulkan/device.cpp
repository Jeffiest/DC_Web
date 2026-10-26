// Vulkan on WebGPU (layer.hpp): the instance, the one physical device and what it reports, the device and its
// queue, the canvas surface and its swapchain, fences, semaphores and submission.

#include "layer.hpp"

#include <emscripten/em_asm.h>
#include <emscripten/emscripten.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>

namespace vkwebgpu {

Context g;

namespace {

VkInstance_T       g_instance_object;
VkPhysicalDevice_T g_physical_device_object;
VkDevice_T         g_device_object;
VkQueue_T          g_queue_object;
std::thread::id    g_device_thread;
bool               g_have_device = false;

uint64_t g_submitted = 0;
uint64_t g_completed = 0;

std::unordered_set<Buffer *> g_dirty;

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

void OnDeviceError(WGPUDevice const *, WGPUErrorType type, WGPUStringView message, void *, void *) {
    static int reported = 0;
    if (reported < 50) {
        reported++;
        std::fprintf(stderr, "webgpu: error %d: %.*s\n", static_cast<int>(type), static_cast<int>(message.length),
                     message.data);
    }
}

void OnDeviceLost(WGPUDevice const *, WGPUDeviceLostReason reason, WGPUStringView message, void *, void *) {
    if (reason != WGPUDeviceLostReason_Destroyed) {
        std::fprintf(stderr, "webgpu: device lost (%d): %.*s\n", static_cast<int>(reason),
                     static_cast<int>(message.length), message.data);
    }
}

bool RequestAdapter() {
    if (g.adapter != nullptr) {
        return true;
    }
    bool                           done = false;
    WGPURequestAdapterOptions      options = {};
    options.powerPreference = WGPUPowerPreference_HighPerformance;
    WGPURequestAdapterCallbackInfo callback = {};
    callback.mode = WGPUCallbackMode_AllowProcessEvents;
    callback.callback = [](WGPURequestAdapterStatus status, WGPUAdapter adapter, WGPUStringView message, void *out,
                           void *) {
        if (status == WGPURequestAdapterStatus_Success) {
            g.adapter = adapter;
        } else {
            std::fprintf(stderr, "webgpu: no adapter: %.*s\n", static_cast<int>(message.length), message.data);
        }
        *static_cast<bool *>(out) = true;
    };
    callback.userdata1 = &done;
    wgpuInstanceRequestAdapter(g.instance, &options, callback);
    WaitUntil([&] { return done; });
    if (g.adapter == nullptr) {
        return false;
    }
    // DC_WEBGPU_NO_<feature>=1 takes the fallback where the adapter has the feature: the web workflow tests the
    // clip distances' that way, and a player can tell a driver's fault in one from the layer's.
    auto feature = [](WGPUFeatureName name, const char *off) {
        const char *value = std::getenv(off);
        if (value != nullptr && std::strcmp(value, "1") == 0) {
            std::fprintf(stderr, "webgpu: %s: left unused\n", off);
            return false;
        }
        return static_cast<bool>(wgpuAdapterHasFeature(g.adapter, name));
    };
    g.dual_source = feature(WGPUFeatureName_DualSourceBlending, "DC_WEBGPU_NO_DUAL_SOURCE");
    g.depth32_stencil8 = feature(WGPUFeatureName_Depth32FloatStencil8, "DC_WEBGPU_NO_DEPTH32_STENCIL8");
    g.clip_distances = feature(WGPUFeatureName_ClipDistances, "DC_WEBGPU_NO_CLIP_DISTANCES");
    wgpuAdapterGetLimits(g.adapter, &g.limits);
    WGPUAdapterInfo info = {};
    wgpuAdapterGetInfo(g.adapter, &info);
    auto text = [](WGPUStringView view) {
        return std::string(view.data != nullptr ? view.data : "", view.data != nullptr ? view.length : 0);
    };
    std::string name = "WebGPU";
    for (const std::string &part : {text(info.vendor), text(info.architecture), text(info.device)}) {
        if (!part.empty()) {
            name += " " + part;
        }
    }
    Copy(g.adapter_name, sizeof g.adapter_name, name.c_str());
    wgpuAdapterInfoFreeMembers(info);
    return true;
}

VkFormatFeatureFlags FormatFeatures(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_R8_UNORM:
            return VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
                   VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BLEND_BIT |
                   VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_SRC_BIT | VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            if (!g.depth32_stencil8) {
                return 0;
            }
            [[fallthrough]];
        case VK_FORMAT_D24_UNORM_S8_UINT:
        case VK_FORMAT_D32_SFLOAT:
            // No blits: WebGPU cannot draw into or scale depth. ReadDepth copies it out.
            return VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_TRANSFER_SRC_BIT |
                   VK_FORMAT_FEATURE_TRANSFER_DST_BIT;
        default:
            return 0;
    }
}

void FillLimits(VkPhysicalDeviceLimits &limits) {
    limits = {};
    limits.maxImageDimension1D = g.limits.maxTextureDimension1D;
    limits.maxImageDimension2D = g.limits.maxTextureDimension2D;
    limits.maxImageDimension3D = g.limits.maxTextureDimension3D;
    limits.maxImageArrayLayers = g.limits.maxTextureArrayLayers;
    limits.maxUniformBufferRange = static_cast<uint32_t>(std::min<uint64_t>(g.limits.maxUniformBufferBindingSize, UINT32_MAX));
    limits.maxStorageBufferRange = static_cast<uint32_t>(std::min<uint64_t>(g.limits.maxStorageBufferBindingSize, UINT32_MAX));
    limits.maxPushConstantsSize = 256;
    limits.maxMemoryAllocationCount = 1u << 20;
    limits.maxSamplerAllocationCount = 4000;
    limits.maxBoundDescriptorSets = g.limits.maxBindGroups - 1; // one group holds the push constants
    limits.maxPerStageDescriptorSamplers = g.limits.maxSamplersPerShaderStage;
    limits.maxPerStageDescriptorUniformBuffers = g.limits.maxUniformBuffersPerShaderStage;
    limits.maxPerStageDescriptorStorageBuffers = g.limits.maxStorageBuffersPerShaderStage - 1;
    limits.maxPerStageDescriptorSampledImages = g.limits.maxSampledTexturesPerShaderStage;
    limits.maxPerStageResources = 64;
    limits.maxDescriptorSetSamplers = g.limits.maxSamplersPerShaderStage;
    limits.maxDescriptorSetStorageBuffers = g.limits.maxStorageBuffersPerShaderStage;
    limits.maxDescriptorSetSampledImages = g.limits.maxSampledTexturesPerShaderStage;
    limits.maxVertexInputAttributes = g.limits.maxVertexAttributes;
    limits.maxVertexInputBindings = g.limits.maxVertexBuffers;
    limits.maxVertexInputAttributeOffset = 2047;
    limits.maxVertexInputBindingStride = g.limits.maxVertexBufferArrayStride;
    limits.maxVertexOutputComponents = g.limits.maxInterStageShaderVariables * 4;
    limits.maxFragmentInputComponents = g.limits.maxInterStageShaderVariables * 4;
    limits.maxFragmentOutputAttachments = g.limits.maxColorAttachments;
    limits.maxFragmentDualSrcAttachments = 1;
    limits.maxClipDistances = 8;
    limits.maxCombinedClipAndCullDistances = 8;
    limits.maxColorAttachments = g.limits.maxColorAttachments;
    limits.maxViewports = 1;
    limits.maxViewportDimensions[0] = g.limits.maxTextureDimension2D;
    limits.maxViewportDimensions[1] = g.limits.maxTextureDimension2D;
    limits.viewportBoundsRange[0] = -2.0f * static_cast<float>(g.limits.maxTextureDimension2D);
    limits.viewportBoundsRange[1] = 2.0f * static_cast<float>(g.limits.maxTextureDimension2D);
    limits.maxFramebufferWidth = g.limits.maxTextureDimension2D;
    limits.maxFramebufferHeight = g.limits.maxTextureDimension2D;
    limits.maxFramebufferLayers = 1;
    limits.framebufferColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits.framebufferDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits.framebufferStencilSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits.framebufferNoAttachmentsSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits.sampledImageColorSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits.sampledImageDepthSampleCounts = VK_SAMPLE_COUNT_1_BIT;
    limits.maxSamplerAnisotropy = 16.0f;
    limits.maxSamplerLodBias = 0.0f;
    limits.minUniformBufferOffsetAlignment = g.limits.minUniformBufferOffsetAlignment;
    limits.minStorageBufferOffsetAlignment = g.limits.minStorageBufferOffsetAlignment;
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
    std::fprintf(stderr, "webgpu: ");
    std::vfprintf(stderr, format, args);
    std::fprintf(stderr, "\n");
    va_end(args);
    std::abort();
}

void Warn(const char *format, ...) {
    va_list args;
    va_start(args, format);
    std::fprintf(stderr, "webgpu: ");
    std::vfprintf(stderr, format, args);
    std::fprintf(stderr, "\n");
    va_end(args);
}

void AssertDeviceThread(const char *what) {
    if (g_have_device && std::this_thread::get_id() != g_device_thread) {
        Fatal("%s off the device's thread: WebGPU objects belong to the thread that made the device", what);
    }
}

WGPUStringView View(const char *text) { return WGPUStringView{text, WGPU_STRLEN}; }

void WaitUntil(const std::function<bool()> &done) {
    while (!done()) {
        wgpuInstanceProcessEvents(g.instance);
        if (!done()) {
            emscripten_sleep(1);
        }
    }
}

WGPUTextureFormat TextureFormat(VkFormat format) {
    switch (format) {
        case VK_FORMAT_R8G8B8A8_UNORM:
            return WGPUTextureFormat_RGBA8Unorm;
        case VK_FORMAT_B8G8R8A8_UNORM:
            return WGPUTextureFormat_BGRA8Unorm;
        case VK_FORMAT_R8_UNORM:
            return WGPUTextureFormat_R8Unorm;
        case VK_FORMAT_D32_SFLOAT_S8_UINT:
            return WGPUTextureFormat_Depth32FloatStencil8;
        case VK_FORMAT_D24_UNORM_S8_UINT:
            return WGPUTextureFormat_Depth24PlusStencil8;
        case VK_FORMAT_D32_SFLOAT:
            return WGPUTextureFormat_Depth32Float;
        default:
            Fatal("no WebGPU format for VkFormat %d", static_cast<int>(format));
    }
}

bool IsDepthFormat(VkFormat format) {
    return format == VK_FORMAT_D32_SFLOAT_S8_UINT || format == VK_FORMAT_D24_UNORM_S8_UINT ||
           format == VK_FORMAT_D32_SFLOAT;
}

uint32_t TexelBytes(VkFormat format) { return format == VK_FORMAT_R8_UNORM ? 1 : 4; }

uint64_t SubmitSerial() { return g_submitted; }

uint64_t CompletedSerial() { return g_completed; }

void WaitForSerial(uint64_t serial) {
    WaitUntil([serial] { return g_completed >= serial; });
}

void MarkDirty(Buffer *buffer, VkDeviceSize offset, VkDeviceSize size) {
    if (buffer->gpu == nullptr || size == 0) {
        return;
    }
    if (buffer->dirty_end == 0) {
        buffer->dirty_begin = offset;
        buffer->dirty_end = offset + size;
        g_dirty.insert(buffer);
        return;
    }
    buffer->dirty_begin = std::min(buffer->dirty_begin, offset);
    buffer->dirty_end = std::max(buffer->dirty_end, offset + size);
}

void ForgetDirty(Buffer *buffer) {
    g_dirty.erase(buffer);
    buffer->dirty_begin = buffer->dirty_end = 0;
}

// writeBuffer takes multiples of 4 bytes; the copy in wasm memory has room past the buffer's end.
void UploadDirty() {
    for (Buffer *buffer : g_dirty) {
        const VkDeviceSize begin = buffer->dirty_begin & ~VkDeviceSize{3};
        const VkDeviceSize end = std::min<VkDeviceSize>((buffer->dirty_end + 3) & ~VkDeviceSize{3},
                                                        (buffer->size + 3) & ~VkDeviceSize{3});
        if (end > begin) {
            wgpuQueueWriteBuffer(g.queue, buffer->gpu, begin, buffer->shadow + begin, end - begin);
        }
        buffer->dirty_begin = buffer->dirty_end = 0;
    }
    g_dirty.clear();
}

// Maps every readback's buffer, then copies each one's rows into its destination's bytes.
void FinishReadbacks(std::vector<Readback> &readbacks) {
    if (readbacks.empty()) {
        return;
    }
    struct Pending {
        bool done = false;
        bool ok = false;
    };
    std::vector<Pending> pending(readbacks.size());
    for (size_t i = 0; i < readbacks.size(); i++) {
        WGPUBufferMapCallbackInfo callback = {};
        callback.mode = WGPUCallbackMode_AllowProcessEvents;
        callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void *out, void *) {
            auto *p = static_cast<Pending *>(out);
            p->ok = status == WGPUMapAsyncStatus_Success;
            if (!p->ok) {
                std::fprintf(stderr, "webgpu: readback map failed (%d): %.*s\n", static_cast<int>(status),
                             static_cast<int>(message.length), message.data);
            }
            p->done = true;
        };
        callback.userdata1 = &pending[i];
        wgpuBufferMapAsync(readbacks[i].staging, WGPUMapMode_Read, 0,
                           static_cast<size_t>(readbacks[i].padded_row) * readbacks[i].rows, callback);
    }
    WaitUntil([&] { return std::all_of(pending.begin(), pending.end(), [](const Pending &p) { return p.done; }); });
    for (size_t i = 0; i < readbacks.size(); i++) {
        Readback &readback = readbacks[i];
        if (pending[i].ok && readback.dst != nullptr && readback.dst->shadow != nullptr) {
            const size_t size = static_cast<size_t>(readback.padded_row) * readback.rows;
            const auto  *src = static_cast<const uint8_t *>(wgpuBufferGetConstMappedRange(readback.staging, 0, size));
            for (uint32_t row = 0; row < readback.rows; row++) {
                std::memcpy(readback.dst->shadow + readback.dst_offset + static_cast<size_t>(row) * readback.row,
                            src + static_cast<size_t>(row) * readback.padded_row, readback.row);
            }
            wgpuBufferUnmap(readback.staging);
        }
        wgpuBufferRelease(readback.staging);
    }
    readbacks.clear();
}

} // namespace vkwebgpu

using namespace vkwebgpu;

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
    if (g.instance == nullptr) {
        // The browser has no navigator.gpu without WebGPU; say so plainly rather than failing later.
        const bool has_gpu = EM_ASM_INT({ return typeof navigator !== 'undefined' && !!navigator.gpu; });
        if (!has_gpu) {
            Warn("this browser has no WebGPU");
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        g.instance = wgpuCreateInstance(nullptr);
        if (g.instance == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
    }
    *instance = &g_instance_object;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyInstance(VkInstance, const VkAllocationCallbacks *) {}

VKAPI_ATTR VkResult VKAPI_CALL vkEnumeratePhysicalDevices(VkInstance, uint32_t *count, VkPhysicalDevice *devices) {
    if (!RequestAdapter()) {
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
    Copy(properties->deviceName, sizeof properties->deviceName, g.adapter_name);
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
    // Without WebGPU's dual-source blending the layer draws those blends from one source (commands.cpp).
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
                // WebGPU has no triangle fans and one stencil reference and mask pair for both faces.
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
    if (g.device == nullptr) {
        if (!RequestAdapter()) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        std::vector<WGPUFeatureName> features;
        if (g.clip_distances) {
            features.push_back(WGPUFeatureName_ClipDistances);
        } else {
            Warn("no clip distances: the fragment shader clips instead");
        }
        if (g.dual_source) {
            features.push_back(WGPUFeatureName_DualSourceBlending);
        } else {
            Warn("no dual-source blending: those blends are drawn from one source");
        }
        if (g.depth32_stencil8) {
            features.push_back(WGPUFeatureName_Depth32FloatStencil8);
        }
        for (WGPUFeatureName feature : features) {
            if (!wgpuAdapterHasFeature(g.adapter, feature)) {
                Warn("the adapter lacks WebGPU feature %d", static_cast<int>(feature));
                return VK_ERROR_FEATURE_NOT_PRESENT;
            }
        }
        WGPULimits limits = g.limits; // all the adapter offers
        bool       done = false;
        WGPUDeviceDescriptor descriptor = {};
        descriptor.label = View("Kazarov");
        descriptor.requiredFeatureCount = features.size();
        descriptor.requiredFeatures = features.data();
        descriptor.requiredLimits = &limits;
        descriptor.uncapturedErrorCallbackInfo.callback = OnDeviceError;
        descriptor.deviceLostCallbackInfo.mode = WGPUCallbackMode_AllowSpontaneous;
        descriptor.deviceLostCallbackInfo.callback = OnDeviceLost;
        WGPURequestDeviceCallbackInfo callback = {};
        callback.mode = WGPUCallbackMode_AllowProcessEvents;
        callback.callback = [](WGPURequestDeviceStatus status, WGPUDevice device, WGPUStringView message, void *out,
                               void *) {
            if (status == WGPURequestDeviceStatus_Success) {
                g.device = device;
            } else {
                std::fprintf(stderr, "webgpu: no device: %.*s\n", static_cast<int>(message.length), message.data);
            }
            *static_cast<bool *>(out) = true;
        };
        callback.userdata1 = &done;
        wgpuAdapterRequestDevice(g.adapter, &descriptor, callback);
        WaitUntil([&] { return done; });
        if (g.device == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
        wgpuDeviceGetLimits(g.device, &g.limits);
        g.queue = wgpuDeviceGetQueue(g.device);
        g_device_thread = std::this_thread::get_id();
        g_have_device = true;
    }
    *device = &g_device_object;
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroyDevice(VkDevice, const VkAllocationCallbacks *) {}

VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice, uint32_t, uint32_t, VkQueue *queue) { *queue = &g_queue_object; }

VKAPI_ATTR VkResult VKAPI_CALL vkDeviceWaitIdle(VkDevice) {
    AssertDeviceThread(__func__);
    WaitForSerial(g_submitted);
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueWaitIdle(VkQueue) { return vkDeviceWaitIdle(&g_device_object); }

// --- The canvas surface and its swapchain -------------------------------------------------------------------

VkResult vkCreateCanvasSurfaceWEB(VkInstance, VkSurfaceKHR *surface) {
    if (g.surface == nullptr) {
        WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas = {};
        canvas.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
        canvas.selector = View("#canvas");
        WGPUSurfaceDescriptor descriptor = {};
        descriptor.nextInChain = &canvas.chain;
        g.surface = wgpuInstanceCreateSurface(g.instance, &descriptor);
        if (g.surface == nullptr) {
            return VK_ERROR_INITIALIZATION_FAILED;
        }
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
    *capabilities = {};
    capabilities->minImageCount = 2;
    capabilities->maxImageCount = kSwapchainImages;
    // The canvas follows the window: the renderer takes SDL's pixel size.
    capabilities->currentExtent = {UINT32_MAX, UINT32_MAX};
    capabilities->minImageExtent = {1, 1};
    capabilities->maxImageExtent = {g.limits.maxTextureDimension2D, g.limits.maxTextureDimension2D};
    capabilities->maxImageArrayLayers = 1;
    capabilities->supportedTransforms = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    capabilities->currentTransform = VK_SURFACE_TRANSFORM_IDENTITY_BIT_KHR;
    capabilities->supportedCompositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    capabilities->supportedUsageFlags = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfaceFormatsKHR(VkPhysicalDevice, VkSurfaceKHR, uint32_t *count,
                                                                    VkSurfaceFormatKHR *formats) {
    WGPUSurfaceCapabilities capabilities = {};
    if (g.surface != nullptr && wgpuSurfaceGetCapabilities(g.surface, g.adapter, &capabilities) == WGPUStatus_Success &&
        capabilities.formatCount > 0) {
        for (size_t i = 0; i < capabilities.formatCount; i++) {
            if (capabilities.formats[i] == WGPUTextureFormat_BGRA8Unorm ||
                capabilities.formats[i] == WGPUTextureFormat_RGBA8Unorm) {
                g.surface_format = capabilities.formats[i];
                break;
            }
        }
        wgpuSurfaceCapabilitiesFreeMembers(capabilities);
    }
    const VkFormat format =
        g.surface_format == WGPUTextureFormat_RGBA8Unorm ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_B8G8R8A8_UNORM;
    return Enumerate(std::vector{VkSurfaceFormatKHR{format, VK_COLOR_SPACE_SRGB_NONLINEAR_KHR}}, count, formats);
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetPhysicalDeviceSurfacePresentModesKHR(VkPhysicalDevice, VkSurfaceKHR,
                                                                         uint32_t *count, VkPresentModeKHR *modes) {
    return Enumerate(std::vector{VK_PRESENT_MODE_FIFO_KHR}, count, modes);
}

VKAPI_ATTR VkResult VKAPI_CALL vkCreateSwapchainKHR(VkDevice, const VkSwapchainCreateInfoKHR *info,
                                                    const VkAllocationCallbacks *, VkSwapchainKHR *swapchain) {
    AssertDeviceThread(__func__);
    WGPUSurfaceConfiguration configuration = {};
    configuration.device = g.device;
    configuration.format = TextureFormat(info->imageFormat);
    configuration.usage = WGPUTextureUsage_CopyDst | WGPUTextureUsage_RenderAttachment;
    configuration.width = info->imageExtent.width;
    configuration.height = info->imageExtent.height;
    configuration.alphaMode = WGPUCompositeAlphaMode_Opaque;
    configuration.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(g.surface, &configuration);

    auto *chain = new Swapchain;
    chain->width = info->imageExtent.width;
    chain->height = info->imageExtent.height;
    // The frames are drawn into textures of the layer's own and copied to the canvas when presented. The canvas's
    // texture is shown as soon as this thread yields, which the renderer's waits do mid-frame on a slow GPU: drawn
    // into directly, it flashed half-drawn or empty frames.
    WGPUTextureDescriptor descriptor = {};
    descriptor.label = View("swapchain image");
    descriptor.dimension = WGPUTextureDimension_2D;
    descriptor.size = {chain->width, chain->height, 1};
    descriptor.format = configuration.format;
    descriptor.mipLevelCount = 1;
    descriptor.sampleCount = 1;
    descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopyDst | WGPUTextureUsage_CopySrc |
                       WGPUTextureUsage_TextureBinding;
    for (uint32_t i = 0; i < kSwapchainImages; i++) {
        auto *image = new Image;
        image->format = configuration.format;
        image->vk_format = info->imageFormat;
        image->width = chain->width;
        image->height = chain->height;
        image->swapchain = true;
        image->texture = wgpuDeviceCreateTexture(g.device, &descriptor);
        chain->images.push_back(image);
    }
    *swapchain = Handle<VkSwapchainKHR>(chain);
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL vkDestroySwapchainKHR(VkDevice, VkSwapchainKHR handle, const VkAllocationCallbacks *) {
    auto *chain = Get<Swapchain>(handle);
    if (chain == nullptr) {
        return;
    }
    for (Image *image : chain->images) {
        if (image->texture != nullptr) {
            wgpuTextureRelease(image->texture);
        }
        delete image;
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
    chain->acquired = static_cast<int32_t>(slot);
    if (fence != VK_NULL_HANDLE) {
        Get<Fence>(fence)->signaled = true;
    }
    *index = slot;
    return VK_SUCCESS;
}

// Draws alpha 1 over the canvas's texture, leaving its colour. The frame's alpha is the GS frame buffer's, often
// below 1; the canvas is configured opaque, which should make the browser ignore it. Android (on Mali at least)
// flickers to frames as much brighter as a colour divided by that alpha would be, and only there.
static WGPURenderPipeline OpaquePipeline(WGPUTextureFormat format) {
    static WGPURenderPipeline pipeline = nullptr;
    static WGPUTextureFormat  made_for = WGPUTextureFormat_Undefined;
    if (pipeline != nullptr && made_for == format) {
        return pipeline;
    }
    if (pipeline != nullptr) {
        wgpuRenderPipelineRelease(pipeline);
    }
    static const char *const kCode = R"wgsl(
@vertex fn vs(@builtin(vertex_index) i: u32) -> @builtin(position) vec4f {
    let corner = vec2f(f32((i << 1u) & 2u), f32(i & 2u));
    return vec4f(corner * 2.0 - 1.0, 0.0, 1.0);
}
@fragment fn fs() -> @location(0) vec4f { return vec4f(0.0, 0.0, 0.0, 1.0); }
)wgsl";
    WGPUShaderSourceWGSL source = {};
    source.chain.sType = WGPUSType_ShaderSourceWGSL;
    source.code = View(kCode);
    WGPUShaderModuleDescriptor module_descriptor = {};
    module_descriptor.nextInChain = &source.chain;
    module_descriptor.label = View("opaque canvas");
    WGPUShaderModule module = wgpuDeviceCreateShaderModule(g.device, &module_descriptor);
    WGPUColorTargetState target = {};
    target.format = format;
    target.writeMask = WGPUColorWriteMask_Alpha;
    WGPUFragmentState fragment = {};
    fragment.module = module;
    fragment.entryPoint = View("fs");
    fragment.targetCount = 1;
    fragment.targets = &target;
    WGPURenderPipelineDescriptor descriptor = {};
    descriptor.label = View("opaque canvas");
    descriptor.vertex.module = module;
    descriptor.vertex.entryPoint = View("vs");
    descriptor.primitive.topology = WGPUPrimitiveTopology_TriangleList;
    descriptor.primitive.cullMode = WGPUCullMode_None;
    descriptor.multisample.count = 1;
    descriptor.multisample.mask = 0xFFFFFFFF;
    descriptor.fragment = &fragment;
    pipeline = wgpuDeviceCreateRenderPipeline(g.device, &descriptor);
    wgpuShaderModuleRelease(module);
    made_for = format;
    return pipeline;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueuePresentKHR(VkQueue, const VkPresentInfoKHR *info) {
    AssertDeviceThread(__func__);
    VkResult overall = VK_SUCCESS;
    for (uint32_t i = 0; i < info->swapchainCount; i++) {
        auto              *chain = Get<Swapchain>(info->pSwapchains[i]);
        Image             *image = chain->images[info->pImageIndices[i]];
        VkResult           result = VK_SUCCESS;
        WGPUSurfaceTexture current = {};
        wgpuSurfaceGetCurrentTexture(g.surface, &current);
        if (current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
            current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            result = VK_ERROR_OUT_OF_DATE_KHR;
        } else {
            // The canvas resizes with the page; a texture of another size means the swapchain is stale. What
            // overlaps is still shown, so the frame is not blank.
            const uint32_t width = wgpuTextureGetWidth(current.texture);
            const uint32_t height = wgpuTextureGetHeight(current.texture);
            if (width != chain->width || height != chain->height) {
                result = VK_ERROR_OUT_OF_DATE_KHR;
            }
            WGPUCommandEncoder       encoder = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
            WGPUTexelCopyTextureInfo from = {};
            from.texture = image->texture;
            from.aspect = WGPUTextureAspect_All;
            WGPUTexelCopyTextureInfo to = {};
            to.texture = current.texture;
            to.aspect = WGPUTextureAspect_All;
            const WGPUExtent3D extent = {std::min(width, chain->width), std::min(height, chain->height), 1};
            wgpuCommandEncoderCopyTextureToTexture(encoder, &from, &to, &extent);
            WGPUCommandBuffer copy = wgpuCommandEncoderFinish(encoder, nullptr);
            wgpuQueueSubmit(g.queue, 1, &copy);
            wgpuCommandBufferRelease(copy);
            wgpuCommandEncoderRelease(encoder);
            // Its own submission, so a browser that refused it would still show the frame.
            encoder = wgpuDeviceCreateCommandEncoder(g.device, nullptr);
            WGPUTextureView                view = wgpuTextureCreateView(current.texture, nullptr);
            WGPURenderPassColorAttachment attachment = {};
            attachment.view = view;
            attachment.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
            attachment.loadOp = WGPULoadOp_Load;
            attachment.storeOp = WGPUStoreOp_Store;
            WGPURenderPassDescriptor pass_descriptor = {};
            pass_descriptor.colorAttachmentCount = 1;
            pass_descriptor.colorAttachments = &attachment;
            WGPURenderPassEncoder pass = wgpuCommandEncoderBeginRenderPass(encoder, &pass_descriptor);
            wgpuRenderPassEncoderSetPipeline(pass, OpaquePipeline(wgpuTextureGetFormat(current.texture)));
            wgpuRenderPassEncoderDraw(pass, 3, 1, 0, 0);
            wgpuRenderPassEncoderEnd(pass);
            wgpuRenderPassEncoderRelease(pass);
            wgpuTextureViewRelease(view);
            WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
            wgpuQueueSubmit(g.queue, 1, &commands);
            wgpuCommandBufferRelease(commands);
            wgpuCommandEncoderRelease(encoder);
        }
        if (current.texture != nullptr) {
            wgpuTextureRelease(current.texture);
        }
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
    // One queue, in order: a semaphore orders nothing WebGPU does not already.
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
    if (auto *object = Get<Fence>(fence)) {
        if (!object->readbacks.empty()) {
            WaitForSerial(object->serial);
            FinishReadbacks(object->readbacks);
        }
        delete object;
    }
}

VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice, uint32_t count, const VkFence *fences) {
    for (uint32_t i = 0; i < count; i++) {
        Get<Fence>(fences[i])->signaled = false;
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice, VkFence fence) {
    Fence *object = Get<Fence>(fence);
    if (!object->signaled && object->serial != 0 && g_completed >= object->serial) {
        FinishReadbacks(object->readbacks);
        object->signaled = true;
    }
    return object->signaled ? VK_SUCCESS : VK_NOT_READY;
}

VKAPI_ATTR VkResult VKAPI_CALL vkWaitForFences(VkDevice, uint32_t count, const VkFence *fences, VkBool32 wait_all,
                                               uint64_t timeout) {
    AssertDeviceThread(__func__);
    auto ready = [&](uint32_t i) {
        Fence *fence = Get<Fence>(fences[i]);
        return fence->signaled || (fence->serial != 0 && g_completed >= fence->serial);
    };
    auto satisfied = [&] {
        for (uint32_t i = 0; i < count; i++) {
            if (ready(i) != static_cast<bool>(wait_all)) {
                return !wait_all;
            }
        }
        return static_cast<bool>(wait_all);
    };
    if (timeout == 0) {
        wgpuInstanceProcessEvents(g.instance);
        if (!satisfied()) {
            return VK_TIMEOUT;
        }
    } else {
        WaitUntil(satisfied);
    }
    for (uint32_t i = 0; i < count; i++) {
        Fence *fence = Get<Fence>(fences[i]);
        if (!fence->signaled && ready(i)) {
            FinishReadbacks(fence->readbacks);
            fence->signaled = true;
        }
    }
    return VK_SUCCESS;
}

VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit2(VkQueue, uint32_t count, const VkSubmitInfo2 *submits, VkFence fence) {
    AssertDeviceThread(__func__);
    std::vector<WGPUCommandBuffer> commands;
    std::vector<Readback>          readbacks;
    for (uint32_t s = 0; s < count; s++) {
        for (uint32_t c = 0; c < submits[s].commandBufferInfoCount; c++) {
            TakeSubmission(submits[s].pCommandBufferInfos[c].commandBuffer, commands, readbacks);
        }
    }
    // What the CPU wrote reaches the GPU before the commands that read it.
    UploadDirty();
    wgpuQueueSubmit(g.queue, commands.size(), commands.data());
    for (WGPUCommandBuffer command : commands) {
        wgpuCommandBufferRelease(command);
    }
    const uint64_t                    serial = ++g_submitted;
    WGPUQueueWorkDoneCallbackInfo callback = {};
    callback.mode = WGPUCallbackMode_AllowProcessEvents;
    callback.callback = [](WGPUQueueWorkDoneStatus, WGPUStringView, void *done, void *) {
        const auto value = reinterpret_cast<uintptr_t>(done);
        g_completed = std::max<uint64_t>(g_completed, value);
    };
    callback.userdata1 = reinterpret_cast<void *>(static_cast<uintptr_t>(serial));
    wgpuQueueOnSubmittedWorkDone(g.queue, callback);
    if (fence != VK_NULL_HANDLE) {
        Fence *object = Get<Fence>(fence);
        object->signaled = false;
        object->serial = serial;
        object->readbacks.insert(object->readbacks.end(), readbacks.begin(), readbacks.end());
    } else if (!readbacks.empty()) {
        WaitForSerial(serial);
        FinishReadbacks(readbacks);
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

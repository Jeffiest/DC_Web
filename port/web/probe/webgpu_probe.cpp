// The WebGPU probe (docs/WEB.md): what the renderer's WebGPU backend will rely on, tried with the game's own
// link flags before the backend is written. It runs on the proxied main pthread as the game does, creates the
// device there, waits for it the way the backend will (process events, sleep through Asyncify), and draws
// from inside a fiber, as the game's PS2 threads draw. Each check prints "probe: <name> ok|FAIL ...".
//
//   webgpu_probe.js [frames]

#include <emscripten/emscripten.h>
#include <emscripten/fiber.h>
#include <webgpu/webgpu.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace {

WGPUInstance instance = nullptr;
WGPUAdapter  adapter = nullptr;
WGPUDevice   device = nullptr;
WGPUQueue    queue = nullptr;
WGPUSurface  surface = nullptr;
int          failures = 0;

void Report(const char *name, bool ok, const char *detail = "") {
    std::printf("probe: %s %s %s\n", name, ok ? "ok" : "FAIL", detail);
    std::fflush(stdout);
    failures += ok ? 0 : 1;
}

WGPUStringView View(const char *text) { return WGPUStringView{text, WGPU_STRLEN}; }

// The backend's wait: callbacks fire from ProcessEvents, and the sleep hands the worker back to the browser.
template <typename Done> void WaitFor(Done done) {
    while (!done()) {
        wgpuInstanceProcessEvents(instance);
        if (!done()) {
            emscripten_sleep(1);
        }
    }
}

bool CreateDevice() {
    instance = wgpuCreateInstance(nullptr);
    if (instance == nullptr) {
        Report("instance", false);
        return false;
    }
    bool adapter_done = false;
    WGPURequestAdapterCallbackInfo adapter_callback{};
    adapter_callback.mode = WGPUCallbackMode_AllowProcessEvents;
    adapter_callback.callback = [](WGPURequestAdapterStatus status, WGPUAdapter result, WGPUStringView message,
                                   void *done, void *) {
        if (status == WGPURequestAdapterStatus_Success) {
            adapter = result;
        } else {
            std::printf("probe: adapter error %.*s\n", static_cast<int>(message.length), message.data);
        }
        *static_cast<bool *>(done) = true;
    };
    adapter_callback.userdata1 = &adapter_done;
    wgpuInstanceRequestAdapter(instance, nullptr, adapter_callback);
    WaitFor([&] { return adapter_done; });
    Report("adapter", adapter != nullptr);
    if (adapter == nullptr) {
        return false;
    }

    // What the renderer asks of Vulkan, as WebGPU features: report which the adapter has.
    const struct {
        WGPUFeatureName name;
        const char     *label;
    } wanted[] = {
        {WGPUFeatureName_DualSourceBlending, "dual-source-blending"},
        {WGPUFeatureName_ClipDistances, "clip-distances"},
        {WGPUFeatureName_Depth32FloatStencil8, "depth32float-stencil8"},
        {WGPUFeatureName_Float32Filterable, "float32-filterable"},
    };
    WGPUFeatureName features[4];
    size_t          feature_count = 0;
    for (const auto &feature : wanted) {
        const bool has = wgpuAdapterHasFeature(adapter, feature.name);
        std::printf("probe: feature %s %s\n", feature.label, has ? "yes" : "no");
        if (has) {
            features[feature_count++] = feature.name;
        }
    }

    bool                 device_done = false;
    WGPUDeviceDescriptor descriptor{};
    descriptor.requiredFeatureCount = feature_count;
    descriptor.requiredFeatures = features;
    descriptor.uncapturedErrorCallbackInfo.callback = [](WGPUDevice const *, WGPUErrorType type,
                                                         WGPUStringView message, void *, void *) {
        std::printf("probe: device error %d %.*s\n", static_cast<int>(type), static_cast<int>(message.length),
                    message.data);
        failures++;
    };
    WGPURequestDeviceCallbackInfo device_callback{};
    device_callback.mode = WGPUCallbackMode_AllowProcessEvents;
    device_callback.callback = [](WGPURequestDeviceStatus status, WGPUDevice result, WGPUStringView message,
                                  void *done, void *) {
        if (status == WGPURequestDeviceStatus_Success) {
            device = result;
        } else {
            std::printf("probe: device request error %.*s\n", static_cast<int>(message.length), message.data);
        }
        *static_cast<bool *>(done) = true;
    };
    device_callback.userdata1 = &device_done;
    wgpuAdapterRequestDevice(adapter, &descriptor, device_callback);
    WaitFor([&] { return device_done; });
    Report("device", device != nullptr);
    if (device == nullptr) {
        return false;
    }
    queue = wgpuDeviceGetQueue(device);
    return true;
}

// The canvas, transferred to this pthread as an OffscreenCanvas (-sOFFSCREENCANVASES_TO_PTHREAD).
void ConfigureSurface() {
    WGPUEmscriptenSurfaceSourceCanvasHTMLSelector canvas{};
    canvas.chain.sType = WGPUSType_EmscriptenSurfaceSourceCanvasHTMLSelector;
    canvas.selector = View("#canvas");
    WGPUSurfaceDescriptor descriptor{};
    descriptor.nextInChain = &canvas.chain;
    surface = wgpuInstanceCreateSurface(instance, &descriptor);
    Report("surface", surface != nullptr);
    if (surface == nullptr) {
        return;
    }
    WGPUSurfaceConfiguration configuration{};
    configuration.device = device;
    configuration.format = WGPUTextureFormat_RGBA8Unorm;
    configuration.usage = WGPUTextureUsage_RenderAttachment;
    configuration.width = 640;
    configuration.height = 480;
    configuration.alphaMode = WGPUCompositeAlphaMode_Opaque;
    configuration.presentMode = WGPUPresentMode_Fifo;
    wgpuSurfaceConfigure(surface, &configuration);
}

void ClearTo(WGPUTextureView view, double r, double g, double b) {
    WGPUCommandEncoder            encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
    WGPURenderPassColorAttachment color{};
    color.view = view;
    color.depthSlice = WGPU_DEPTH_SLICE_UNDEFINED;
    color.loadOp = WGPULoadOp_Clear;
    color.storeOp = WGPUStoreOp_Store;
    color.clearValue = WGPUColor{r, g, b, 1.0};
    WGPURenderPassDescriptor pass{};
    pass.colorAttachmentCount = 1;
    pass.colorAttachments = &color;
    WGPURenderPassEncoder render = wgpuCommandEncoderBeginRenderPass(encoder, &pass);
    wgpuRenderPassEncoderEnd(render);
    wgpuRenderPassEncoderRelease(render);
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(queue, 1, &commands);
    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
}

// A synchronous readback, as ReadbackFrame and ReadDepth need: copy, map, wait.
void Readback(const char *name) {
    WGPUTextureDescriptor texture_descriptor{};
    texture_descriptor.usage = WGPUTextureUsage_RenderAttachment | WGPUTextureUsage_CopySrc;
    texture_descriptor.dimension = WGPUTextureDimension_2D;
    texture_descriptor.size = WGPUExtent3D{64, 64, 1};
    texture_descriptor.format = WGPUTextureFormat_RGBA8Unorm;
    texture_descriptor.mipLevelCount = 1;
    texture_descriptor.sampleCount = 1;
    WGPUTexture     texture = wgpuDeviceCreateTexture(device, &texture_descriptor);
    WGPUTextureView view = wgpuTextureCreateView(texture, nullptr);
    ClearTo(view, 1.0, 0.5, 0.0);

    WGPUBufferDescriptor buffer_descriptor{};
    buffer_descriptor.usage = WGPUBufferUsage_CopyDst | WGPUBufferUsage_MapRead;
    buffer_descriptor.size = 256 * 64;
    WGPUBuffer         buffer = wgpuDeviceCreateBuffer(device, &buffer_descriptor);
    WGPUCommandEncoder encoder = wgpuDeviceCreateCommandEncoder(device, nullptr);
    WGPUTexelCopyTextureInfo source{};
    source.texture = texture;
    source.aspect = WGPUTextureAspect_All;
    WGPUTexelCopyBufferInfo destination{};
    destination.buffer = buffer;
    destination.layout.bytesPerRow = 256;
    destination.layout.rowsPerImage = 64;
    WGPUExtent3D size{64, 64, 1};
    wgpuCommandEncoderCopyTextureToBuffer(encoder, &source, &destination, &size);
    WGPUCommandBuffer commands = wgpuCommandEncoderFinish(encoder, nullptr);
    wgpuQueueSubmit(queue, 1, &commands);

    struct MapResult {
        bool done = false;
        int  status = -1;
        char message[128] = {};
    } result;
    WGPUBufferMapCallbackInfo map_callback{};
    map_callback.mode = WGPUCallbackMode_AllowProcessEvents;
    map_callback.callback = [](WGPUMapAsyncStatus status, WGPUStringView message, void *out, void *) {
        auto *r = static_cast<MapResult *>(out);
        r->status = static_cast<int>(status);
        std::snprintf(r->message, sizeof(r->message), "%.*s", static_cast<int>(message.length),
                      message.data != nullptr ? message.data : "");
        r->done = true;
    };
    map_callback.userdata1 = &result;
    wgpuBufferMapAsync(buffer, WGPUMapMode_Read, 0, buffer_descriptor.size, map_callback);
    WaitFor([&] { return result.done; });
    const bool map_ok = result.status == WGPUMapAsyncStatus_Success;
    uint8_t    pixel[4] = {};
    if (map_ok) {
        std::memcpy(pixel, wgpuBufferGetConstMappedRange(buffer, 0, 4), 4);
        wgpuBufferUnmap(buffer);
    }
    char detail[192];
    std::snprintf(detail, sizeof(detail), "%u %u %u %u (map status %d %s)", pixel[0], pixel[1], pixel[2], pixel[3],
                  result.status, result.message);
    Report(name, map_ok && pixel[0] == 255 && pixel[1] >= 127 && pixel[1] <= 128 && pixel[2] == 0, detail);

    wgpuCommandBufferRelease(commands);
    wgpuCommandEncoderRelease(encoder);
    wgpuBufferRelease(buffer);
    wgpuTextureViewRelease(view);
    wgpuTextureRelease(texture);
}

// Frames as the game draws them: from its own loop, yielding once a frame so the browser can show the canvas.
int frames_drawn = 0;

void DrawFrames(int count) {
    for (int i = 0; i < count; i++) {
        WGPUSurfaceTexture current{};
        wgpuSurfaceGetCurrentTexture(surface, &current);
        if (current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessOptimal &&
            current.status != WGPUSurfaceGetCurrentTextureStatus_SuccessSuboptimal) {
            char detail[32];
            std::snprintf(detail, sizeof(detail), "status %d", static_cast<int>(current.status));
            Report("frame", false, detail);
            return;
        }
        WGPUTextureView view = wgpuTextureCreateView(current.texture, nullptr);
        // A slow sweep from dark blue to teal, so a screenshot shows which frame it caught.
        ClearTo(view, 0.0, 0.25 + 0.5 * i / count, 0.5);
        wgpuTextureViewRelease(view);
        // No wgpuSurfacePresent on the web: the browser shows the canvas once the worker yields.
        wgpuTextureRelease(current.texture);
        emscripten_sleep(16);
        frames_drawn++;
    }
}

struct FiberJob {
    int  frames;
    bool done;
};

emscripten_fiber_t main_fiber;
emscripten_fiber_t draw_fiber;

void FiberEntry(void *arg) {
    auto *job = static_cast<FiberJob *>(arg);
    Readback("readback in a fiber");
    DrawFrames(job->frames);
    job->done = true;
    emscripten_fiber_swap(&draw_fiber, &main_fiber);
}

} // namespace

int main(int argc, char **argv) {
    const int frames = argc > 1 ? std::atoi(argv[1]) : 120;
    std::printf("probe: start, %d frames\n", frames);
    if (!CreateDevice()) {
        std::printf("probe: done, %d failed\n", failures + 1);
        return 1;
    }
    // Readbacks before the canvas: headless Chromium on SwiftShader loses the device once a canvas presents.
    Readback("readback");
    ConfigureSurface();

    // The rest from a fiber, as the game's PS2 threads run.
    static char main_asyncify[256 * 1024];
    static char fiber_asyncify[256 * 1024];
    static char fiber_stack[1024 * 1024];
    emscripten_fiber_init_from_current_context(&main_fiber, main_asyncify, sizeof(main_asyncify));
    FiberJob job{surface != nullptr ? frames - frames / 2 : 0, false};
    emscripten_fiber_init(&draw_fiber, FiberEntry, &job, fiber_stack, sizeof(fiber_stack), fiber_asyncify,
                          sizeof(fiber_asyncify));
    emscripten_fiber_swap(&main_fiber, &draw_fiber);
    Report("fiber", job.done);
    if (surface != nullptr) {
        DrawFrames(frames / 2);
    }

    char detail[32];
    std::snprintf(detail, sizeof(detail), "%d drawn", frames_drawn);
    Report("frames", surface == nullptr || frames_drawn == frames, detail);
    std::printf("probe: done, %d failed\n", failures);
    return failures == 0 ? 0 : 1;
}

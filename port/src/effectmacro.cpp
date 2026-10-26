#include "effectmacro.hpp"

#include <libgraph.h>
#include <libvu0.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <vector>

#include "draw2d_port.hpp"
#include "draw3d.hpp"
#include "mglib.hpp"
#include "mglib_port.hpp"
#include "rect.hpp"
#include "screen.hpp"
#include "texture.hpp"

namespace {

// The jitter table. Retail reads rd[i][j + 1] for j up to 14, which runs one entry past the end of
// the last row; the spare entry stands in for that word and stays zero.
float g_jitter[21 * 15 + 1];

float &Jitter(int i, int j) { return g_jitter[i * 15 + j]; }

// Keep the haze's focus-derived front behind the edit camera's near clip.
constexpr float kHazeNear = 10.0f;

// The columns of the logical frame the effect covers (retail's screen was the frame; a target that
// shows past it is covered to its edges) and the texel width of the blurred image laid over them.
struct Span {
    float left;
    float right;
    float sample_left;
    float texels;
    float row_scale;
    float sample_top = 0.0f;
};

// A band vertex: x in 12.4 GS units of the logical frame, stretched over the span; across is how
// far over the blurred image it samples, v its row there.
gfx::Vertex2D StripVertex(const Span &span, int x, int y, unsigned z, float across, int v, int alpha) {
    const float place = MGPortLogicalX(x) / kScreenWidth;
    return draw2d::Vertex(span.left + place * (span.right - span.left), MGPortLogicalY(y) * draw2d::RowScale(),
                          MGPortDepth(z & 0xFFFFFF), span.sample_left + across * span.texels,
                          static_cast<float>(v) / 16.0f * span.row_scale + span.sample_top, 0x80,
                          0x80, 0x80, static_cast<u_char>(alpha));
}

std::array<gfx::Vertex2D, 4> Quad(float x0, float y0, float x1, float y1, float z, float u0, float v0, float u1,
                                  float v1) {
    return {
        draw2d::Vertex(x0, y0, z, u0, v0, 0x80, 0x80, 0x80, 0x80),
        draw2d::Vertex(x1, y0, z, u1, v0, 0x80, 0x80, 0x80, 0x80),
        draw2d::Vertex(x1, y1, z, u1, v1, 0x80, 0x80, 0x80, 0x80),
        draw2d::Vertex(x0, y1, z, u0, v1, 0x80, 0x80, 0x80, 0x80),
    };
}

// Accumulate the frame beyond several nearby depth planes into a premultiplied image. With heat
// haze, the planes are spread from `front` to `back` so the effect gains coverage gradually from
// the nearest depth it reaches to the farthest; the depths are spaced in 1/z, which is what the
// screen's rows are linear in, so the haze ramps at an even rate down the frame instead of fading
// out in a band of a few rows. Each plane contributes an equal fraction.
// Where a coverage image lies: its target's logical size, the rectangle of the frame it samples and whether the
// target first takes the main depth over that rectangle (a target with a depth of its own, smaller than the frame).
struct BeyondArea {
    float            width = kScreenWidth;
    float            height = kScreenHeight;
    gfx::LogicalRect source = {0.0f, 0.0f, kScreenWidth, kScreenHeight};
    bool             copy_depth = false;
};

// False only when the depth copy was refused; nothing is drawn then.
bool DrawBeyond(gfx::TextureHandle beyond, gfx::TextureHandle snapshot, float front, float back, bool feather,
                const BeyondArea &area = {}) {
    const uint8_t clear[4] = {};
    gfx::SetRenderTarget(beyond);
    // Colour only, after the copy: the target's depth is the main depth's, not a cleared one.
    if (area.copy_depth && !gfx::CopyMainDepthToTarget(area.source)) {
        return false;
    }
    gfx::Clear(true, clear, false, 0.0f);
    gfx::DrawState state;
    state.depth_test = gfx::DepthTest::GEqual;
    state.texa_aem = false;
    state.texa_ta0 = 0x80;
    state.blend = feather;
    state.alpha = {0, 2, 2, 1, 0x80};
    gfx::TextureBinding binding;
    binding.texture = snapshot;
    binding.filter = gfx::Filter::Nearest;
    const int  samples = feather ? 8 : 1;
    const auto plane_depth = [&](int i) {
        float         t = static_cast<float>(i) / 7.0f;
        float         plane = feather ? 1.0f / (1.0f / front + (1.0f / back - 1.0f / front) * t) : front;
        sceVu0FVECTOR point = {0.0f, 0.0f, plane, 1.0f};
        sceVu0ApplyMatrix(point, mgRenderInfo.screen, point);
        point[2] /= point[3];
        return MGPortDepth(static_cast<unsigned>(static_cast<int>(point[2])) & 0xFFFFFF);
    };
    // Where the renderer has it (KZ_DEPTH_PLANES=1), the eight planes are one call that shades each pixel once.
    // It draws them one by one itself whenever it cannot take them in one pass, to the same image.
    if (feather && gfx::DepthPlanesAvailable()) {
        gfx::DepthPlanes2D planes;
        planes.count = static_cast<uint32_t>(samples);
        for (int i = 0; i < samples; i++) {
            planes.depth[i] = plane_depth(i);
            planes.alpha[i] = static_cast<u_char>((i + 1) * 0x10);
        }
        std::array<gfx::Vertex2D, 4> quad =
            Quad(0.0f, 0.0f, area.width, area.height, 0.0f, area.source.x, area.source.y, area.source.x + area.source.w,
                 area.source.y + area.source.h);
        for (gfx::Vertex2D &vertex : quad) {
            vertex.color[0] = vertex.color[1] = vertex.color[2] = 0x10;
        }
        gfx::DrawDepthPlanes2D(planes, quad, binding, state);
        return true;
    }
    for (int i = 0; i < samples; i++) {
        float                        depth = plane_depth(i);
        std::array<gfx::Vertex2D, 4> quad =
            Quad(0.0f, 0.0f, area.width, area.height, depth, area.source.x, area.source.y,
                 area.source.x + area.source.w, area.source.y + area.source.h);
        if (feather) {
            for (gfx::Vertex2D &vertex : quad) {
                vertex.color[0] = vertex.color[1] = vertex.color[2] = 0x10;
                vertex.color[3] = static_cast<u_char>((i + 1) * 0x10);
            }
        }
        gfx::Draw2D(gfx::Primitive::Quads, quad, binding, state);
    }
    return true;
}

// src's texel rect resized over all of a width by height target.
void Resize(gfx::TextureHandle src, float u0, float v0, float u1, float v1, gfx::TextureHandle dst, uint32_t width,
            uint32_t height) {
    gfx::SetRenderTarget(dst);
    gfx::TextureBinding binding;
    binding.texture = src;
    binding.filter = gfx::Filter::Linear;
    std::array<gfx::Vertex2D, 4> quad =
        Quad(0.0f, 0.0f, static_cast<float>(width), static_cast<float>(height), 0.0f, u0, v0, u1, v1);
    gfx::Draw2D(gfx::Primitive::Quads, quad, binding, gfx::DrawState{});
}

// The bands over the frame from a premultiplied image at the vertices' alpha: the frame is first
// scaled down by the image's coverage, then the image added, so where the image is transparent
// (beside something nearer than the plane) the frame keeps its own colour.
void DrawBands(std::vector<gfx::Vertex2D> &triangles, gfx::TextureHandle image, gfx::DrawState state) {
    if (triangles.empty()) {
        return;
    }
    gfx::TextureBinding binding;
    binding.texture = image;
    binding.filter = gfx::Filter::Linear;
    state.blend = true;
    state.fog = false;
    state.cull = gfx::CullMode::None;
    state.alpha = {2, 1, 0, 1, 0x80};
    gfx::Draw2D(gfx::Primitive::Triangles, triangles, binding, state);
    for (gfx::Vertex2D &vertex : triangles) {
        vertex.color[0] = vertex.color[1] = vertex.color[2] = vertex.color[3];
        vertex.color[3] = 0x80;
    }
    state.alpha = {0, 2, 2, 1, 0x80};
    gfx::Draw2D(gfx::Primitive::Triangles, triangles, binding, state);
}

// One GS triangle strip as a triangle list, so that every band of the effect goes in one draw.
void AppendStrip(std::vector<gfx::Vertex2D> &triangles, const std::vector<gfx::Vertex2D> &strip) {
    for (size_t i = 2; i < strip.size(); i++) {
        triangles.push_back(strip[i - 2]);
        triangles.push_back(strip[i - 1]);
        triangles.push_back(strip[i]);
    }
}

bool HazeSkipsIdentityNear() {
    static const bool enabled = [] {
        const char *value = std::getenv("DC_HAZE_SKIP_IDENTITY");
        return value != nullptr && std::strcmp(value, "1") == 0;
    }();
    return enabled;
}

// DC_HAZE_SCALE (1 by default, or 0.5 or 0.25) sizes the far haze's coverage image as a fraction of the visible frame,
// on a target with its own depth that takes the main depth by a nearest resize. Anything else keeps full resolution.
float HazeScale() {
    static const float scale = [] {
        const char *value = std::getenv("DC_HAZE_SCALE");
        if (value != nullptr && std::strcmp(value, "0.5") == 0) {
            return 0.5f;
        }
        if (value != nullptr && std::strcmp(value, "0.25") == 0) {
            return 0.25f;
        }
        return 1.0f;
    }();
    return scale;
}

// The heat haze's depth range. Coverage and composite both start at the front of the far focus band and end past its
// back, so the haze begins well away from the player. A far focus that gives no ordered finite range keeps the earlier
// range from the near focus.
struct HazeRange {
    float near_front;
    float near_back;
    float far_front;
    float far_back;
    bool  shared; // one range for both coverage images and both composites
};

HazeRange HazeRangeOf(const float *focus) {
    const float front = std::max(kHazeNear, focus[1] - 84.0f);
    const float back = focus[1] + 84.0f;
    if (std::isfinite(front) && std::isfinite(back) && back > front) {
        return {front, back, front, back, true};
    }
    return {focus[0] - 84.0f, focus[0] + 84.0f, std::max(kHazeNear, focus[0] - 84.0f), focus[1] + 84.0f, false};
}

// What the low-resolution far image is made from, or no image when the frame and renderer cannot support it.
struct LowHaze {
    gfx::TextureHandle image = gfx::kNullTexture;
    BeyondArea         area;
    float              width = 0.0f; // the image's logical size
    float              height = 0.0f;
};

// The pixels of the main target that lie in rect, as the logical rectangle those whole pixels cover.
gfx::LogicalRect PixelAligned(const gfx::LogicalMapping &mapping, const gfx::LogicalRect &rect) {
    const auto edge = [](float logical, float scale, float offset, uint32_t limit) {
        return std::clamp(std::round(static_cast<double>(logical) * scale + offset), 0.0, static_cast<double>(limit));
    };
    const double x0 = edge(rect.x, mapping.scale_x, mapping.offset_x, mapping.pixel_width);
    const double x1 = edge(rect.x + rect.w, mapping.scale_x, mapping.offset_x, mapping.pixel_width);
    const double y0 = edge(rect.y, mapping.scale_y, mapping.offset_y, mapping.pixel_height);
    const double y1 = edge(rect.y + rect.h, mapping.scale_y, mapping.offset_y, mapping.pixel_height);
    return {static_cast<float>((x0 - mapping.offset_x) / mapping.scale_x),
            static_cast<float>((y0 - mapping.offset_y) / mapping.scale_y),
            static_cast<float>((x1 - x0) / mapping.scale_x), static_cast<float>((y1 - y0) / mapping.scale_y)};
}

bool SameMapping(const gfx::LogicalMapping &a, const gfx::LogicalMapping &b) {
    const auto close = [](float x, float y) { return std::abs(x - y) <= 1e-4f * std::max(1.0f, std::abs(x)); };
    return a.pixel_width == b.pixel_width && a.pixel_height == b.pixel_height && close(a.scale_x, b.scale_x) &&
           close(a.scale_y, b.scale_y) && close(a.offset_x, b.offset_x) && close(a.offset_y, b.offset_y);
}

// Allocates the target and checks everything the path needs, or returns an empty LowHaze. The snapshot must map
// the frame as the main target does (not letterboxed against it), the target must really be smaller in pixels
// (render scale and ceil are why the logical size alone does not say), and the depth copy must be available.
LowHaze PrepareLowHaze(gfx::TextureHandle snapshot, const gfx::LogicalRect &visible) {
    LowHaze                               low;
    const float                           scale = HazeScale();
    const std::optional<gfx::TextureInfo> main_info = gfx::GetTextureInfo(gfx::kMainTarget);
    if (scale >= 1.0f || snapshot == gfx::kNullTexture || !main_info ||
        !std::isfinite(visible.x) || !std::isfinite(visible.y) || !(visible.w > 0.0f) || !(visible.h > 0.0f)) {
        return low;
    }
    const gfx::LogicalMapping main_mapping = gfx::GetLogicalMapping(gfx::kMainTarget);
    if (!SameMapping(main_mapping, gfx::GetLogicalMapping(snapshot))) {
        return low;
    }
    const gfx::LogicalRect source = PixelAligned(main_mapping, visible);
    if (!(source.w > 0.0f) || !(source.h > 0.0f)) {
        return low;
    }
    const float                           width = std::max(1.0f, std::ceil(source.w * scale));
    const float                           height = std::max(1.0f, std::ceil(source.h * scale));
    gfx::TextureHandle                    image = gfx::NamedRenderTarget("dof haze low", static_cast<uint32_t>(width),
                                                                         static_cast<uint32_t>(height), true, false, false);
    const std::optional<gfx::TextureInfo> info = gfx::GetTextureInfo(image);
    if (image == gfx::kNullTexture || !info || info->pixel_width >= main_info->pixel_width ||
        info->pixel_height >= main_info->pixel_height || !gfx::CanCopyMainDepth(image)) {
        return low;
    }
    low.image = image;
    low.area = {width, height, source, true};
    low.width = width;
    low.height = height;
    return low;
}

} // namespace

// Retail shrinks the frame into frame_image twice (half width, then quarter width) and draws the
// copies back over it in bands whose depth is that of the focus planes, alpha blended and depth
// tested GEQUAL without writes: only what lies beyond a plane takes the blurred copy. The first
// pass alternates the near plane's two depths column by column, the second the far plane's, and
// with blur the second pass's columns wander sideways by up to blur sixteenths of a pixel (the
// towns' heat haze).
//
// The copies held the whole frame, so whatever stood nearer than a plane was blurred into the
// scenery around it, and the quarter-width copy came back as blocks. The ordinary depth-of-field
// passes retain their half- and quarter-size images of only the scenery beyond each plane. Heat
// haze instead uses two full-resolution targets sharing the frame's depth buffer. Each target gains
// coverage over eight depth planes. The wobble starts at the front of the near focus band and ends
// beyond the far focus plane, with planes spaced in 1/z. Foreground objects stay stable while the
// distant scenery gains haze gradually. The two outermost columns do not wander past the image edges.
PORT_OVERRIDE void DepthOfField(float *focus, int level, int alpha, int blur) {
    int phase;
    int i;
    int j;
    int k;

    gfx::TextureHandle target = gfx::CurrentRenderTarget();
    bool               haze = blur > 0;
    const HazeRange    range = HazeRangeOf(focus);
    gfx::LogicalRect   visible = gfx::VisibleLogicalRect(target);
    int                left = std::min(0, static_cast<int>(std::floor(visible.x)));
    int                right = std::max(0x280, static_cast<int>(std::ceil(visible.x + visible.w)));
    uint32_t           half_width = static_cast<uint32_t>(right - left + 1) / 2;
    uint32_t           half_height = SCREEN_HALF_HEIGHT;
    uint32_t           quarter_width = (half_width + 1) / 2;
    uint32_t           quarter_height = half_height / 2;
    Span               span = {static_cast<float>(left), static_cast<float>(right),
                               haze ? static_cast<float>(left) : 0.0f,
                               haze ? static_cast<float>(right - left) : static_cast<float>(half_width),
                               haze ? 2.0f : 1.0f};

    sceVu0FVECTOR depth_point[4] = {
        {0.0f, 0.0f, focus[0],         1.0f},
        {0.0f, 0.0f, focus[0] + 30.0f, 1.0f},
        {0.0f, 0.0f, focus[1],         1.0f},
        {0.0f, 0.0f, focus[1] + 20.0f, 1.0f},
    };
    int screen[4][4] = {};

    for (i = 0; i < 4; i++) {
        sceVu0ApplyMatrix(depth_point[i], mgRenderInfo.screen, depth_point[i]);
        depth_point[i][2] /= depth_point[i][3];
        screen[i][2] = static_cast<int>(depth_point[i][2]);
    }

    if (haze) {
        // The coverage image and its composite use the same front, leaving nearer geometry stable.
        sceVu0FVECTOR near_front = {0.0f, 0.0f, range.near_front, 1.0f};
        sceVu0ApplyMatrix(near_front, mgRenderInfo.screen, near_front);
        screen[0][2] = screen[1][2] = static_cast<int>(near_front[2] / near_front[3]);
        sceVu0FVECTOR far_front = {0.0f, 0.0f, range.far_front, 1.0f};
        sceVu0ApplyMatrix(far_front, mgRenderInfo.screen, far_front);
        screen[2][2] = screen[3][2] = static_cast<int>(far_front[2] / far_front[3]);
    }

    gfx::TextureHandle snapshot = gfx::NamedRenderTarget("dof frame", 0x280, SCREEN_HEIGHT, false, false, true);
    gfx::TextureHandle beyond = haze ? gfx::kNullTexture : gfx::NamedRenderTarget("dof beyond", 0x280, SCREEN_HEIGHT, true, true);
    gfx::TextureHandle far_image = gfx::kNullTexture;
    // Matching fronts and nonfolding far strips let the later composite replace near alpha.
    // The identity-coordinate near copy only adds accumulation and blend rounding to RGB.
    const bool fronts_match = haze && blur < 0x200 && level >= 2 && target == gfx::kMainTarget &&
                              std::isfinite(focus[0]) && std::isfinite(focus[1]) &&
                              (range.shared || (focus[0] - 84.0f >= kHazeNear && focus[1] + 84.0f >= range.far_front)) &&
                              mgPixelTest.bits.date == 0;
    // A smaller far image needs the shared range, so its depth copy and its composite agree on one front.
    LowHaze low;
    if (fronts_match && range.shared) {
        low = PrepareLowHaze(snapshot, visible);
    }
    bool skip_near = false;
    if (fronts_match && HazeSkipsIdentityNear()) {
        far_image = low.image != gfx::kNullTexture ? low.image
                                                   : gfx::NamedRenderTarget("dof far", 0x280, SCREEN_HEIGHT, true, true);
        skip_near = far_image != gfx::kNullTexture;
    }
    gfx::TextureHandle near_image = skip_near ? gfx::kNullTexture
                                    : haze    ? gfx::NamedRenderTarget("dof near", 0x280, SCREEN_HEIGHT, true, true)
                                              : gfx::NamedRenderTarget("dof near", half_width, half_height, true);
    bool               ready = snapshot != gfx::kNullTexture && (haze || beyond != gfx::kNullTexture) &&
                               (skip_near || near_image != gfx::kNullTexture) && target == gfx::kMainTarget &&
                               gfx::SnapshotFrame(snapshot);

    if (ready) {
        if (!skip_near) {
            DrawBeyond(haze ? near_image : beyond, snapshot, haze ? range.near_front : focus[0],
                       haze ? range.near_back : focus[0], haze);
            if (!haze) {
                Resize(beyond, span.left, 0.0f, span.right, kScreenHeight, near_image, half_width, half_height);
            }
        }

        if (level >= 2) {
            if (haze) {
                // The smaller image first; a refused depth copy falls back to the full-resolution one, same range.
                bool low_drawn = false;
                if (low.image != gfx::kNullTexture) {
                    low_drawn = DrawBeyond(low.image, snapshot, range.far_front, range.far_back, true, low.area);
                    far_image = low_drawn ? low.image : gfx::kNullTexture;
                    low = low_drawn ? low : LowHaze{};
                }
                if (!low_drawn) {
                    if (far_image == gfx::kNullTexture) {
                        far_image = gfx::NamedRenderTarget("dof far", 0x280, SCREEN_HEIGHT, true, true);
                    }
                    if (far_image != gfx::kNullTexture) {
                        DrawBeyond(far_image, snapshot, range.far_front, range.far_back, true);
                    }
                }
            } else {
                gfx::TextureHandle half = gfx::NamedRenderTarget("dof far half", half_width, half_height, true);
                gfx::TextureHandle quarter = gfx::NamedRenderTarget("dof far quarter", quarter_width, quarter_height, true);
                far_image = gfx::NamedRenderTarget("dof far", half_width, half_height, true);

                if (half != gfx::kNullTexture && quarter != gfx::kNullTexture && far_image != gfx::kNullTexture) {
                    DrawBeyond(beyond, snapshot, focus[1], focus[1], false);
                    Resize(beyond, span.left, 0.0f, span.right, kScreenHeight, half, half_width, half_height);
                    Resize(half, 0.0f, 0.0f, static_cast<float>(half_width), static_cast<float>(half_height), quarter,
                           quarter_width, quarter_height);
                    Resize(quarter, 0.0f, 0.0f, static_cast<float>(quarter_width), static_cast<float>(quarter_height),
                           far_image, half_width, half_height);
                } else {
                    far_image = gfx::kNullTexture;
                }
            }
        }

        gfx::SetRenderTarget(target);
    }

    MGPortRestoreRegisters();
    MGPortCurrent().texa.AEM = 1;
    MGPortCurrent().texa.TA0 = 128;

    const draw2d::Services &services = draw2d::Get();
    sceGsTest               test = mgPixelTest;
    test.bits.ate = 0;
    test.bits.aref = 0;
    test.bits.atst = 1;
    test.bits.zte = 1;
    test.bits.ztst = 2;
    services.set_test(&test);
    sceGsZbuf zbuffer = mgZBuffer;
    zbuffer.bits.zmsk = 1;
    services.set_zbuf(&zbuffer);

    std::vector<gfx::Vertex2D> triangles;
    std::vector<gfx::Vertex2D> strip;

    // The far bands keep their full-frame positions and jitter; only where they sample moves, onto the smaller image
    // that covers the visible frame from the pixel-aligned source rectangle.
    Span far_span = span;
    if (low.image != gfx::kNullTexture && far_image == low.image) {
        const gfx::LogicalRect &source = low.area.source;
        far_span.sample_left = (span.left - source.x) / source.w * low.width;
        far_span.texels = (span.right - span.left) / source.w * low.width;
        far_span.row_scale = 2.0f * low.height / source.h;
        far_span.sample_top = -source.y * low.height / source.h;
    }

    for (phase = 0; phase < 1 && !skip_near; phase++) {
        for (k = 0; k < 8; k++) {
            strip.clear();

            for (j = 0; j < 41; j++) {
                int   x = (j << 8) + 0x6C00;
                int   v = k << 9;
                int   v_bottom = std::min(v + 0x200, SCREEN_HALF_HEIGHT << 4);
                float across = static_cast<float>(j) / 40.0f;
                strip.push_back(StripVertex(span, x, v + GS_Y_OFFSET, screen[(j + phase) % 2][2], across, v, alpha));
                strip.push_back(StripVertex(span, x, v_bottom + GS_Y_OFFSET, screen[(j + phase) % 2][2], across,
                                            v_bottom, alpha));
            }

            AppendStrip(triangles, strip);
        }
    }

    gfx::DrawState state = services.draw_state();

    if (ready && !skip_near) {
        DrawBands(triangles, near_image, state);
    }

    triangles.clear();

    if (level >= 2) {
        if (blur > 0) {
            for (i = 0; i < 21; i++) {
                for (j = 0; j < 15; j++) {
                    if (blur > 0) {
                        Jitter(i, j) += 0.2f * (static_cast<float>(blur) * (static_cast<float>(rand()) / 2147483648.0f - 0.5f));

                        if (Jitter(i, j) < 0.0f) {
                            Jitter(i, j) = 0.0f;
                        }

                        if (Jitter(i, j) > static_cast<float>(blur)) {
                            Jitter(i, j) = static_cast<float>(blur);
                        }
                    } else {
                        Jitter(i, j) = 0.0f;
                    }
                }
            }
        }

        if (blur > 0) {
            alpha = 0x80;
        }

        for (j = 0; j < SCREEN_HALF_HEIGHT / 16; j++) {
            strip.clear();

            for (i = 0; i < 21; i++) {
                int   x = (i << 9) + 0x6C00;
                int   v = j << 8;
                int   y = v + GS_Y_OFFSET;
                int   y_bottom = y + 0x100;
                float across = static_cast<float>(i) / 20.0f;
                int   v_bottom = v + 0x100;

                if (blur > 0 && i > 0 && i < 20) {
                    strip.push_back(StripVertex(far_span, x - static_cast<int>(Jitter(i, j)), y, screen[2 + i % 2][2],
                                                across, v, alpha));
                    strip.push_back(StripVertex(far_span, x - static_cast<int>(Jitter(i, j + 1)), y_bottom,
                                                screen[2 + i % 2][2], across, v_bottom, alpha));
                } else {
                    strip.push_back(StripVertex(far_span, x, y, screen[2 + i % 2][2], across, v, alpha));
                    strip.push_back(StripVertex(far_span, x, y_bottom, screen[2 + i % 2][2], across, v_bottom, alpha));
                }
            }

            AppendStrip(triangles, strip);
        }
    }

    if (ready && far_image != gfx::kNullTexture) {
        DrawBands(triangles, far_image, state);
    }

    services.set_test(nullptr);
    services.set_zbuf(nullptr);
}

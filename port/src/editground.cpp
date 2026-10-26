#include "editground.hpp"

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "camera.hpp"
#include "camerafollow.hpp"
#include "dataread.hpp"
#include "editarea.hpp"
#include "editground_port.hpp"
#include "editpartsinfo.hpp"
#include "frame.hpp"
#include "framevu1.hpp"
#include "mapparts.hpp"
#include "mathutil.hpp"
#include "mglib.hpp"
#include "objanime.hpp"
#include "platform/config.hpp"
#include "rect.hpp"
#include "savedata.hpp"
#include "vector3.hpp"

#pragma clang diagnostic ignored "-Wwritable-strings"

// Retail's DrawPartsCursor. LoadObjectParts keeps a part's preview frame in CMapParts' s32
// preview_frame, cast from one of the part's own level-of-detail frames, and nothing else stores
// there; the frame is recovered by matching those bits against the part's frames.

namespace {

struct ShadowPartCounts {
    unsigned calls = 0;
    unsigned mutable_scanned = 0;
    unsigned mutable_selected = 0;
    unsigned fixed_scanned = 0;
    unsigned fixed_selected = 0;
    unsigned shades = 0;
    unsigned shadows = 0;
    unsigned precise = 0;
    unsigned fast = 0;
    double   shade_ms = 0.0;
    double   shade_max_ms = 0.0;
    double   shadow_ms = 0.0;
    double   shadow_max_ms = 0.0;
};

ShadowPartCounts g_far_shadow_counts;

bool ShadowPartCountsEnabled() {
    static const bool enabled = [] {
        const char *setting = std::getenv("DC_PERF_GAME_PHASES");
        return setting != nullptr && std::strcmp(setting, "1") == 0;
    }();
    return enabled;
}

float Magnitude(float value) {
    return value < 0.0f ? -value : value;
}

int CheckDelete(CEditArea *area, CMapParts *parts, float x, float y, float z) {
    CVector3_i_ position;

    area->GetPos(&position, x, y, z);
    int column = position.x;
    int row = position.z;
    int kind = parts->subtype;

    switch (area->GetMapNo()) {
        case TOWN_MATATAKI:
            if (kind == MAP_PARTS_SUBTYPE_BRIDGE) {
                break;
            }

            switch (area->GetAreaID()) {
                case 0:
                    if (column == 5 && row == 0) {
                        return 1;
                    }

                    if (column == 2 && row == 7) {
                        return 1;
                    }

                    break;
                case 1:
                    if (column == 3 && row == 5) {
                        return 1;
                    }

                    if (column == 11 && row == 3) {
                        return 1;
                    }

                    break;
                case 2:
                    if (column == 4 && row == 0) {
                        return 1;
                    }

                    if (column == 3 && row == 7) {
                        return 1;
                    }

                    break;
            }

            break;
    }

    return 0;
}

CFrame *PreviewFrame(CMapParts *parts, s32 truncated) {
    if (truncated == 0) {
        return nullptr;
    }

    for (int k = 3; k >= 0; k--) {
        CFrame        *frame = parts->frame[k];
        std::uintptr_t bits = reinterpret_cast<std::uintptr_t>(frame) & 0xFFFFFFFFu;

        if (frame != nullptr && bits == static_cast<u32>(truncated)) {
            return frame;
        }
    }

    return nullptr;
}

} // namespace

PORT_OVERRIDE void CEditGround::DrawPartsCursor(int plot, float *position, float *model_pos, int rot_y, float *rotation, int area_no) {
    [[maybe_unused]] static int old_parts = -1;
    CVector3_f_                 cell;
    CVector3_i_                 grid;
    sceVu0FMATRIX               light_direction;
    sceVu0FMATRIX               light_colour;
    sceVu0FVECTOR               ambient;
    int                         area_code;
    CEditArea                  *area;
    CMapParts                  *source;
    int                         width;
    int                         height;
    int                         fits;
    int                         occupant;
    EDITPARTS_INFO             *info;
    CFrame                     *preview;
    CFrame                     *model;
    int                         saved_draw;
    int                         subtype;
    float                       step;

    if (plot < 0 || plot >= 24) {
        sceVu0CopyVector(model_pos, position);
        return;
    }

    area_code = GetAreaCode(position[0], position[1], position[2]);

    if (area_code < 0) {
        sceVu0CopyVector(model_pos, position);
        return;
    }

    area = areas[area_code];
    source = &plot_parts[plot];
    width = source->GetWidth();
    height = source->GetHeight();

    if (area->CheckAreaRect(position[0], position[1], position[2], width, height) == 0) {
        sceVu0CopyVector(model_pos, position);
        return;
    }

    area->GetPos(&grid, position[0], position[1], position[2]);
    fits = area->CheckParts(source, position[0], position[1], position[2], rot_y);
    occupant = area->SearchPartsID(position[0], position[1], position[2]);

    if (source->subtype == MAP_PARTS_SUBTYPE_ON_RIVER) {
        if (CheckDelete(area, source, position[0], position[1], position[2]) != 0) {
            fits = 0;
        }

        if (occupant >= 0 && occupant < 128 && parts[occupant].handle != 1) {
            fits = 0;
        }
    }

    if (parts_info != nullptr) {
        info = parts_info->GetPartsInfo(plot);

        if (info != nullptr) {
            fits &= info->placed < info->stock;
        }
    }

    area->GetPos(&cell, grid.x, grid.y, grid.z);
    position[0] = cell.x;
    position[1] = 5.0f + cell.y;
    position[2] = cell.z;
    position[3] = 0.0f;
    preview = fits != 0 ? PreviewFrame(source, source->preview_frame) : PreviewFrame(source, source->blocked_preview_frame);

    // The preview is the part's finest level of detail, where retail showed its coarsest.
    if (preview != nullptr && source->frame[0] != nullptr) {
        preview = source->frame[0];
    }

    if (width % 2 == 1) {
        position[0] += 0.5f * area->GetUnitSize();
    }

    if (height % 2 == 1) {
        position[2] += 0.5f * area->GetUnitSize();
    }

    if (fits == 0) {
        // Draw the cursor dim and without point lights where the part cannot go.
        MGGetAmbient(ambient);
        MGGetPLight(light_direction, light_colour);
        sceVu0FVECTOR dim = {60.0f, 60.0f, 60.0f, 128.0f};
        MGSetAmbient(dim);
        MGSetPLight(mgZeroMatrix, mgZeroMatrix);
    }

    cursor.unit_size = area->GetUnitSize();

    if (cursor.area == area_no) {
        cursor.Draw(position, width, height);
    }

    if (fits == 0) {
        MGSetAmbient(ambient);
        MGSetPLight(light_direction, light_colour);
    }

    position[1] += 50.0f;
    sceVu0FVECTOR goal;
    sceVu0CopyVector(goal, position);
    step = goal[0] - model_pos[0];

    if (Magnitude(step) < 1.0f) {
        model_pos[0] = goal[0];
    } else {
        model_pos[0] += step / 8.0f;
    }

    step = goal[1] - model_pos[1];

    if (Magnitude(step) < 1.0f) {
        model_pos[1] = goal[1];
    } else {
        model_pos[1] += step / 8.0f;
    }

    step = goal[2] - model_pos[2];

    if (Magnitude(step) < 1.0f) {
        model_pos[2] = goal[2];
    } else {
        model_pos[2] += step / 8.0f;
    }

    if (source->category_no == area_no) {
        subtype = source->subtype;

        if (preview != nullptr && subtype != MAP_PARTS_SUBTYPE_RIVER && subtype != MAP_PARTS_SUBTYPE_BRIDGE) {
            model = nullptr;

            if (subtype == MAP_PARTS_SUBTYPE_ON_RIVER) {
                model = preview->SearchFrame("kawa");

                if (model != nullptr) {
                    saved_draw = model->attr.draw_on;
                    model->attr.draw_on = 2;
                }

                if (occupant >= 0 && occupant < 128) {
                    CMapParts *target = &parts[occupant];
                    target->GetRotation(rotation);
                }
            }

            float *angle = rotation;
            preview->SetPosition(model_pos);
            preview->SetRotation(angle[0], angle[1], angle[2]);
            MGDraw(preview);

            if (model != nullptr) {
                model->attr.draw_on = saved_draw;
            }
        }
    }
}

// Retail's Draw, whose parts step down a level of detail at 50, 300, 500 and 800 from the eye.
// None of those distances is nearer than video.detail_distance, so a part within it draws the
// finest level the caller allows.
PORT_OVERRIDE void CEditGround::Draw(float time, int pass, int lowest, int highest, int fixed_lowest, int fixed_highest) {
    sceVu0FVECTOR lod_distance = {50.0f, 300.0f, 500.0f, 800.0f};
    sceVu0FVECTOR position;
    sceVu0FVECTOR ambient;
    float         detail = ConfigDetailDistance();
    int           i;

    for (i = 0; i < 4; i++) {
        if (lod_distance[i] < detail) {
            lod_distance[i] = detail;
        }
    }

    for (i = 0; i < 64; i++) {
        if (fixed_parts[i].category_no == pass) {
            fixed_parts[i].DrawParts(time, lod_distance, fixed_lowest, fixed_highest, nullptr);
        }
    }

    CMapParts *object = parts;

    for (i = 0; i < 128; i++, object++) {
        if (object->category_no != pass) {
            continue;
        }

        if (object->handle < 0) {
            continue;
        }

        if (object->area >= 0 && (object->area >= 4 || area_visible[object->area] == 0)) {
            continue;
        }

        if (!(clip_plane[3] <= 0.0f)) {
            object->GetPosition(position);

            if (!(DistVector(position, clip_plane) <= clip_plane[3]) && !object->ChangeDigData()) {
                continue;
            }
        }

        sceVu0FVECTOR focus_ambient = {32.0f, 32.0f, 128.0f, 128.0f};
        MGGetAmbient(ambient);

        if (focus_parts_id == i) {
            MGSetAmbient(focus_ambient);
        }

        object->DrawParts(time, lod_distance, lowest, highest, nullptr);
        MGSetAmbient(ambient);
    }
}

namespace {

// The shadows of the parts whose depth from the eye lies between near_distance and far_distance.
// A part within full of the eye takes the precise volume program whatever its draw_distance; past
// it, one deeper than 300 takes the fast program and one with a draw_distance casts nothing past it.
void DrawShadowBetween(CEditGround &ground, int pass, float near_distance, float far_distance, float full,
                       ShadowPartCounts *counts = nullptr) {
    sceVu0FVECTOR position;
    sceVu0FVECTOR view;
    sceVu0FVECTOR clip_position;
    int           i;
    CMapParts    *object;
    int           fast;
    float         fast_distance = full > 300.0f ? full : 300.0f;

    object = ground.parts;
    if (counts != nullptr) {
        counts->mutable_scanned += 128;
        counts->fixed_scanned += 64;
    }

    for (i = 0; i < 128; i++, object++) {
        if (object->category_no < 0) {
            continue;
        }

        if (object->handle < 0) {
            continue;
        }

        if (object->area >= 0 && (object->area >= 4 || ground.area_visible[object->area] == 0)) {
            continue;
        }

        if (!(ground.clip_plane[3] <= 0.0f)) {
            object->GetPosition(clip_position);

            if (!(DistVector(clip_position, ground.clip_plane) <= ground.clip_plane[3]) && !object->ChangeDigData()) {
                continue;
            }
        }

        object->GetPosition(position);
        position[3] = 1.0f;
        sceVu0ApplyMatrix(view, mgRenderInfo.view_scaled, position);

        if (!(object->draw_distance <= 0.0f) && pass == 0) {
            float distance = DistVector(view);

            if (object->draw_distance < distance && !(distance < full)) {
                continue;
            }
        }

        if (view[2] > near_distance && view[2] < far_distance) {
            if (counts != nullptr) {
                ++counts->mutable_selected;
                ++counts->shades;
            }
            const auto shade_start = counts != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            object->DrawShade();
            if (counts != nullptr) {
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shade_start).count();
                counts->shade_ms += ms;
                counts->shade_max_ms = std::max(counts->shade_max_ms, ms);
            }

            if (object->shadow_frame != nullptr) {
                fast = pass != 0;

                if (view[2] > fast_distance) {
                    fast = 1;
                }

                if (counts != nullptr) {
                    ++counts->shadows;
                    ++(fast != 0 ? counts->fast : counts->precise);
                }
                const auto shadow_start = counts != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                object->DrawShadow(fast);
                if (counts != nullptr) {
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shadow_start).count();
                    counts->shadow_ms += ms;
                    counts->shadow_max_ms = std::max(counts->shadow_max_ms, ms);
                }
            }
        }
    }

    for (int j = 0; j < 64; j++) {
        CMapParts *fixed = &ground.fixed_parts[j];

        if (fixed->category_no < 0) {
            continue;
        }

        if (fixed->handle < 0) {
            continue;
        }

        sceVu0CopyVector(position, fixed->pos);
        position[3] = 1.0f;
        sceVu0ApplyMatrix(view, mgRenderInfo.view_scaled, position);

        if (view[2] > near_distance && view[2] < far_distance) {
            if (counts != nullptr) {
                ++counts->fixed_selected;
                ++counts->shades;
            }
            const auto shade_start = counts != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
            fixed->DrawShade();
            if (counts != nullptr) {
                const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shade_start).count();
                counts->shade_ms += ms;
                counts->shade_max_ms = std::max(counts->shade_max_ms, ms);
            }

            if (fixed->shadow_frame != nullptr) {
                fast = pass != 0;

                if (view[2] > fast_distance) {
                    fast = 1;
                }

                if (counts != nullptr) {
                    ++counts->shadows;
                    ++(fast != 0 ? counts->fast : counts->precise);
                }
                const auto shadow_start = counts != nullptr ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
                fixed->DrawShadow(fast);
                if (counts != nullptr) {
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - shadow_start).count();
                    counts->shadow_ms += ms;
                    counts->shadow_max_ms = std::max(counts->shadow_max_ms, ms);
                }
            }
        }
    }
}

} // namespace

// Retail's DrawShadow, which casts a part's shadow by its depth from the eye: EditLoop composites
// the parts nearer than the map's shadow_near (320 by default) at shadow_mode (0x34) and those out
// to shadow_far (740) at the fainter shadow_mode_2 (0x20), and past that a part casts none. A part
// deeper than 300 takes the fast volume program, and one with a draw_distance casts nothing past
// it. A part within video.shadow_distance of the eye belongs to the near band, with the precise
// program and whatever its draw_distance, so its shadow is as dark and as exact as up close; those
// of them deeper than shadow_near are left out of the near band here and cast by
// EdGroundDrawShadowBeyondNear. The overhead view of georama mode (pass 1) keeps its own.
PORT_OVERRIDE void CEditGround::DrawShadow(int pass, float near_distance, float far_distance) {
    float full = pass == 0 ? ConfigShadowDistance() : 0.0f;

    if (near_distance < 1.0f) {
        near_distance = -10000000;
    } else {
        if (near_distance < full) {
            near_distance = full;
        }

        if (far_distance < near_distance) {
            far_distance = near_distance;
        }
    }

    DrawShadowBetween(*this, pass, near_distance, far_distance, full);
}

void EdGroundDrawShadowBeyondNear(CEditGround *ground, float shadow_near) {
    ShadowPartCounts *counts = ShadowPartCountsEnabled() ? &g_far_shadow_counts : nullptr;
    float             full = ConfigShadowDistance();

    if (full > shadow_near) {
        DrawShadowBetween(*ground, 0, shadow_near, full, full, counts);
    }
    if (counts != nullptr && ++counts->calls == 120) {
        std::fprintf(stderr,
                     "cpu farshadowparts %u calls: mutable %u/%u fixed %u/%u shade %u shadow %u precise %u fast %u\n",
                     counts->calls, counts->mutable_selected, counts->mutable_scanned,
                     counts->fixed_selected, counts->fixed_scanned, counts->shades, counts->shadows,
                     counts->precise, counts->fast);
        std::fprintf(stderr,
                     "cpu farshadowparts calls wall ms: shade total %.3f max %.3f shadow total %.3f max %.3f over %u frames\n",
                     counts->shade_ms, counts->shade_max_ms, counts->shadow_ms, counts->shadow_max_ms, counts->calls);
        *counts = {};
    }
}

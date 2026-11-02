// MoonMod code that used to live in ps2/src/editloop.cpp: loading a Georama variant's own model.

#include "editloop.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "boxvu0.hpp"
#include "dataread.hpp"
#include "dispctrl.hpp"
#include "edit_in.hpp"
#include "editpartsinfo.hpp"
#include "mapparts.hpp"

namespace dc::game {

namespace {

// editloop.cpp's static GetCameraFrame.
CFrame *GetCameraFrame(CMapParts *parts) {
    if (parts->camera_frame == NULL) {
        return NULL;
    }

    parts->camera_frame->SetPosition(parts->pos[0], parts->pos[1], parts->pos[2]);
    parts->camera_frame->SetRotation(parts->rotation.x, parts->rotation.y, parts->rotation.z);
    return parts->camera_frame;
}

} // namespace

/**
 * A Georama variant's own model. Reads the scene file once (kept for the session, since the
 * loaded frames may point into it), finds the part archive by name and loads its models over the
 * copy of the template part, keeping the template's footprint, functions and plot data.
 */
bool ChronicleLoadModPartModel(CMapParts *parts, const char *scene, const char *part) {
    struct SceneFile {
        char   path[64];
        u_int *data;
    };
    static SceneFile scenes[8] = {};
    u_int           *data = NULL;
    for (SceneFile &file : scenes) {
        if (file.data != NULL && strcmp(file.path, scene) == 0) data = file.data;
    }
    if (data == NULL) {
        SceneFile *slot = NULL;
        for (SceneFile &file : scenes) {
            if (file.data == NULL) {
                slot = &file;
                break;
            }
        }
        if (slot == NULL) return false;
        const int capacity = 8 * 1024 * 1024 + 4096;
        char     *raw = (char *) malloc(capacity + 64);
        u_int    *buffer = (u_int *) (((uintptr_t) raw + 63) & ~(uintptr_t) 63);
        int       size = 0;
        if (raw == NULL) return false;
        if (LoadFile2((char *) scene, buffer, &size, 0) == 0 || size <= 0 || size > 8 * 1024 * 1024) {
            free(raw);
            return false;
        }
        strcpy(slot->path, scene);
        slot->data = buffer;
        data = buffer;
    }

    EPARTS_ARCHIVE *record = (EPARTS_ARCHIVE *) SearchPTS(data, (char *) part);
    if (record == NULL || record->lod0_size <= 0) return false;
    char *names[9] = {NULL};
    names[0] = (char *) record + record->lod0_offset;
    if (record->lod1_size > 0) names[1] = (char *) record + record->lod1_offset;
    if (record->lod2_size > 0) names[2] = (char *) record + record->lod2_offset;
    if (record->lod3_size > 0) names[3] = (char *) record + record->lod3_offset;
    if (record->shadow_size > 0) names[4] = (char *) record + record->shadow_offset;
    if (record->collision_size > 0) names[5] = (char *) record + record->collision_offset;
    if (record->shade_size > 0) names[6] = (char *) record + record->shade_offset;
    if (record->extra_size > 0) names[7] = (char *) record + record->extra_offset;
    if (record->camera_size > 0) names[8] = (char *) record + record->camera_offset;

    for (int k = 0; k < 4; k++) parts->SetFrame(NULL, k);
    parts->shadow_frame = NULL;
    parts->shade_frame = NULL;
    parts->ripple_frame = NULL;
    parts->camera_frame = NULL;
    LoadMapObject(parts, (u_int **) names, &DataBuffer__2);
    if (parts->frame[0] == NULL) return false;

    CBoxVu0 bound;
    CBoxVu0 part_bound;
    parts->frame[0]->GetBoundBox(&bound, 1);
    if (CFrame *collision = parts->GetCollisionFrame()) {
        collision->GetBoundBox(&part_bound, 1);
        VectorMaxMin(bound.max, bound.min, bound.max, bound.min, part_bound.max, part_bound.min);
    }
    if (CFrame *camera = GetCameraFrame(parts)) {
        camera->GetBoundBox(&part_bound, 1);
        VectorMaxMin(bound.max, bound.min, bound.max, bound.min, part_bound.max, part_bound.min);
    }
    memcpy(&parts->bound, &bound, sizeof(CBoxVu0));
    for (int k = 3; k >= 0; k--) {
        if (parts->frame[k] != NULL) {
            parts->preview_frame = (int) parts->frame[k];
            break;
        }
    }
    std::fprintf(stderr, "moonmod: Georama variant part %d uses model %s from %s\n", parts->parts_no, part, scene);
    return true;
}

} // namespace dc::game

#pragma once

namespace dc::render {
struct Draw3DVisual;
} // namespace dc::render
using dc::render::Draw3DVisual;

// Resolve a texture-keyed OBJ/GLB replacement after Chronicle has built the
// native visual and before its mesh is uploaded.
void ModsModelHook(Draw3DVisual &visual, const char *name, const float (*vertex)[4], int vertex_count,
                   const void *block);
// Keep replacement geometry aligned with the native model's animation.
void ModsModelSkin(Draw3DVisual &visual, const float (*vertex)[4], const void *block);
void ModsModelVisualReleased(Draw3DVisual &visual);
void ModsSkinBegin(const float (*mdt_vertices)[4]);
void ModsSkinVertex(const float (*mdt_vertices)[4], unsigned vertex, const float base[4][4],
                    const float bone[4][4], const float inverse[4][4], float weight);

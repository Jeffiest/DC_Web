#pragma once

namespace dc::render {
struct PortDecodedTexture;
} // namespace dc::render
using dc::render::PortDecodedTexture;

/// Apply the winning enabled MoonFactory package PNG for a native texture name.
/// Returns true when a package image replaced the decoded native texture.
bool ModsTextureOverride(const char *name, PortDecodedTexture &texture);

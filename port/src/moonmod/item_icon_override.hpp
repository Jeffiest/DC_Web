#pragma once

#include "playable_chara.hpp"

namespace dc::game {
class CTexture;
} // namespace dc::game

// A mod's own menu icon for an item (a registered item or weapon): the PNG behind the native resource override at
// `game_path` (e.g. "icons/<stem>.png"), drawn whole into the 32x32 cell RetCTex hands out. "" removes it.
void SetItemIconOverride(int item_no, const char *game_path);

/// The texture of an item's mod icon, read on first use; null where it has none or the file is unusable.
dc::game::CTexture *ItemIconOverrideTexture(int item_no);

/// A HUD icon's texture (render.hud_icon_set with an image): the PNG behind the native resource override at
/// `game_path`, drawn whole into a 32x32 cell; read on first use, null where the file is missing or unusable.
dc::game::CTexture *HudImageTexture(const char *game_path);

/// A character portrait (a registered character's, which no menu atlas has): the PNG behind the native resource override at
/// `game_path`, drawn whole into a 128x128 cell; read on first use, null where the file is missing or unusable.
dc::game::CTexture *PortraitImageTexture(const char *game_path);

/// A registered character's portrait (playable_chara.hpp) for the switch ring, the status and weapon pages: the PNG
/// his entry names; null for the game's six (the menu atlas has theirs) and for one without a portrait, who shows
/// the face of the character he fights as.
inline dc::game::CTexture *CharaPortraitTexture(int chara) {
    const dc::game::PlayableChara *extra = dc::game::ExtraChara(chara);
    return extra != nullptr && !extra->portrait.empty() ? PortraitImageTexture(extra->portrait.c_str()) : nullptr;
}

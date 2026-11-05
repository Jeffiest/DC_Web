#pragma once

namespace dc::game {

class CDungeonMap;

// Draws the dungeon map as the live floor seen from above (game.aerial_map) in place of the tile
// map. False when the map is hidden, the option is off or the aerial targets cannot be made: the
// caller then draws its tile map.
bool DungeonAerialDrawMiniMap(CDungeonMap *map, float *pos, float angle);

// Whether the last CDungeonMap::DrawMiniMap drew the aerial map, on which the monsters are their
// models and their retail symbols are left out.
bool DungeonAerialMapDrawn();

} // namespace dc::game

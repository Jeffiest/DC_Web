# Aerial dungeon map

The PC dungeon map renders the floor and its models from directly above. Discovery, the map item, the crystal, traps, chests, Atla and monster visibility follow the native minimap rules.

`game.aerial_map` defaults to true. When it is false, or the aerial render targets cannot be created, `CDungeonMap::DrawMiniMap` draws its tile map instead (`game.dungeon_aerial_map` and `game.fit_dungeon_map` still size that one). The native Dungeon Map option chooses large, medium or small, or hides the map. The map stays inside the 640 by 480 logical frame and follows the player when zoomed beyond the floor's visible bounds.

Configuration in `config.json`:

| Key under `game` | Default | Purpose |
| --- | --- | --- |
| `aerial_map` | `true` | Enable the aerial view |
| `aerial_map_ceiling` | `400` | Ceiling cut above the player, in world units |
| `aerial_map_zoom` | `1` | Magnification |
| `aerial_map_model_scale` | `8` | Enlarge character, chest and Atla models |
| `aerial_map_size` | `1.4` | Scale the native map size choices |
| `aerial_map_markers` | `false` | Add coloured dots for distant objects |

While the aerial map is visible, F1/F2 shrink/grow it; F5/F6 shrink/grow models; F7 increases zoom (Shift+F7 decreases it); Shift+F5/F6 lower/raise the ceiling cut. Tuning is saved immediately.

The still floor, closed chests, Atla and sleeping monsters are cached. Discovery or layout changes, native object state, map identity, configuration and render-target replacement invalidate that image; the player and awake monsters draw every tick. The overhead pass uses an orthographic projection (`MGPortOrthographicScope`) and disables interpolation, restoring the normal projection afterward.

`CDungeonMap::DrawMap` also leaves out cells hidden behind walls from the camera (`dungeon_visibility.hpp`) beyond two cells away. It keeps a one-cell halo, fails open for special or unknown parts, and is bypassed on the Moon Sea and georama 5.

Regression suites: `DungeonAerialCache`, `DungeonAerialProjection`, `DungeonVisibility`, and `PlatformConfig.AerialMap`.

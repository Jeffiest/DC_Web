#include "moonmod/api.hpp"

#include <array>
#include <ranges>



namespace moonmod {
namespace {

// Names remain stable within API major 1. A port advertises only operations it has
// actually bound to game state; every other catalog entry is queryable as unsupported.
constexpr ApiOperationSpec kApiCatalog[] = {
    {"info.get", "info", "get", "any", "", "Read game, build, platform, and MoonFactory API identity."},
    {"events.list", "events", "list", "any", "", "List stable runtime and game events available to this port."},
    {"events.subscribe", "events", "subscribe", "any", "", "Subscribe to a documented lifecycle or simulation event."},
    {"events.unsubscribe", "events", "unsubscribe", "any", "", "Remove an event subscription owned by this mod."},
    {"player.get", "player", "get", "any", "game.read", "Read the active player and controlled character state."},
    {"player.set", "player", "set", "simulation", "game.write", "Set current HP or Chronicle maximum HP, or Chronicle water, within the selected character's native bounds."},
    {"player.heal", "player", "heal", "simulation", "game.write", "Apply bounded healing to the active player character through native game state."},
    {"player.damage", "player", "damage", "simulation", "game.write", "Apply bounded health damage to the active player character through native game state."},
    {"player.status_add", "player", "status_add", "simulation", "game.write", "Apply a registered status effect to a player."},
    {"player.status_remove", "player", "status_remove", "simulation", "game.write", "Remove a mod-owned or supported status effect."},
    {"player.thirst_get", "player", "thirst_get", "any", "game.read", "Read the active character's water gauge, capacity, and native thirst drain rate in Chronicle."},
    {"player.thirst_set", "player", "thirst_set", "simulation", "game.write", "Set or restore the active or named character's water gauge with native cap clamping in Chronicle."},
    {"player.tuning_set", "player", "tuning_set", "simulation", "game.write", "Set this mod's Chronicle player tuning: run speed, swing recovery speed, hit invincibility and recoil, gauge refill, thirst drain, weapon wear, map and monster reveal; restored when the mod unloads."},
    {"player.tuning_clear", "player", "tuning_clear", "simulation", "game.write", "Take back this mod's Chronicle player tuning."},
    {"animations.player_register", "animations", "player_register", "simulation", "content.register", "Add a mod's own motion (a moonmod-motion JSON file in its resources/) to a Chronicle playable character; it is appended to the character's motions whenever its model loads."},
    {"animations.player_unregister", "animations", "player_unregister", "simulation", "content.register", "Take back one of this mod's playable-character motions."},
    {"animations.player_list", "animations", "player_list", "simulation", "game.read", "This mod's playable-character motions, with the motion number each has on the loaded character."},
    {"animations.monster_register", "animations", "monster_register", "simulation", "content.register", "Add a mod's own motion (a moonmod-motion JSON file in its resources/) to a native monster model, with optional hit spheres; it is appended to the model whenever a floor loads it."},
    {"animations.monster_unregister", "animations", "monster_unregister", "simulation", "content.register", "Take back one of this mod's monster motions (from the next load of its model)."},
    {"animations.monster_list", "animations", "monster_list", "simulation", "game.read", "This mod's monster motions, with the motion number and first frame each has on a loaded floor model."},
    {"player.moves_set", "player", "moves_set", "simulation", "game.write", "Give Chronicle characters this mod's own attacks: a trigger, a stretch of a native motion at its own speed, a lunge, a spin, timed hits, bursts, sounds and trails."},
    {"player.moves_clear", "player", "moves_clear", "simulation", "game.write", "Take back this mod's Chronicle moves."},
    {"player.form_set", "player", "form_set", "simulation", "game.write", "Turn the active Chronicle character into another model (a mod's pack) for a while, with stand-in frames and the weapon hidden."},
    {"player.move_play", "player", "move_play", "simulation", "game.write", "Start one of this mod's own Chronicle moves now, whatever its trigger (a chained move)."},
    {"player.form_clear", "player", "form_clear", "simulation", "game.write", "Give the active Chronicle character his own model back."},
    {"player.invincible", "player", "invincible", "simulation", "game.write", "Give the active Chronicle player one-shot invincibility frames in a dungeon."},
    {"player.fatal_guard", "player", "fatal_guard", "simulation", "game.write", "Set Chronicle charges that turn a fatal hit on a character into 1 HP."},
    {"party.companion_set", "party", "companion_set", "simulation", "game.write", "Bring a party member out beside the player as an engine-driven Chronicle companion, or send it back."},
    {"party.companion_get", "party", "companion_get", "any", "game.read", "Read which Chronicle companion is out."},
    {"player.ridepod_get", "player", "ridepod_get", "any", "game.read", "Read the ridepod's energy, core capacity, defence, voice state, and fitted parts in Chronicle Two."},
    {"player.ridepod_set", "player", "ridepod_set", "simulation", "game.write", "Adjust the ridepod's energy, capacity, shield kits, voice state, or fitted parts in Chronicle Two."},
    {"player.transform_get", "player", "transform_get", "any", "game.read", "Read Monica's monster badge levels, experience, unlock state, and active form in Chronicle Two."},
    {"player.transform_set", "player", "transform_set", "simulation", "game.write", "Adjust a monster badge's level, experience, or unlock state in Chronicle Two."},
    {"party.list", "party", "list", "any", "game.read", "Read party members and their stable handles."},
    {"party.active", "party", "active", "any", "game.read", "Read the active party member."},
    {"party.set_active", "party", "set_active", "simulation", "game.write", "Change the active party member when the game allows it."},
    {"party.join", "party", "join", "simulation", "game.write", "Have a registered playable character join a Chronicle story party."},
    {"party.leave", "party", "leave", "simulation", "game.write", "Have a registered playable character who joined leave the Chronicle story party."},
    {"party.town_active", "party", "town_active", "any", "game.read", "Read the visible town character in Chronicle."},
    {"party.town_set_active", "party", "town_set_active", "simulation", "game.write", "Change the visible town character in Chronicle without changing the dungeon party."},
    {"party.recruit", "party", "recruit", "simulation", "game.write", "Recruit a supported character through the game rules."},
    {"economy.get", "economy", "get", "any", "game.read", "Read the current wallet balance and native currency limit."},
    {"economy.add", "economy", "add", "simulation", "game.write", "Add currency through the game's wallet rules."},
    {"economy.spend", "economy", "spend", "simulation", "game.write", "Spend currency when the wallet has sufficient funds."},
    {"world.get", "world", "get", "any", "game.read", "Read world, map, region, current dungeon-floor identity, and native adventure day where exposed."},
    {"world.transition", "world", "transition", "simulation", "game.write", "Request a validated map, region, or floor transition."},
    {"world.register_dungeon", "world", "register_dungeon", "content", "content.register", "Register additional floors on a supported native dungeon map."},
    {"world.set_time", "world", "set_time", "simulation", "game.write", "Set time as a normalized fraction of the native world cycle."},
    {"world.story_flag_get", "world", "story_flag_get", "any", "game.read", "Read a native story flag by its game-specific numeric id."},
    {"world.story_flag_set", "world", "story_flag_set", "simulation", "game.write", "Set or clear a native story flag by its game-specific numeric id."},
    {"world.story_value_get", "world", "story_value_get", "any", "game.read", "Read a native integer story counter."},
    {"world.story_value_set", "world", "story_value_set", "simulation", "game.write", "Set a native integer story counter within the game's storage range."},
    {"world.map_flag_get", "world", "map_flag_get", "any", "game.read", "Read a native per-map flag such as treasure-box state."},
    {"world.map_flag_set", "world", "map_flag_set", "simulation", "game.write", "Set or clear a native per-map flag."},
    {"entities.query", "entities", "query", "any", "game.read", "Query native entities using filters supported by the active game adapter."},
    {"entities.get", "entities", "get", "any", "game.read", "Resolve a generation-checked entity handle."},
    {"entities.set_stats", "entities", "set_stats", "simulation", "game.write", "Set bounded health, defense, EXP, or Gilda fields on a live Chronicle enemy."},
    {"entities.scale_set", "entities", "scale_set", "simulation", "game.write", "Apply an owner-scoped, generation-checked visual size multiplier to a live Chronicle enemy and restore its native scale when removed."},
    {"entities.script_int_set", "entities", "script_int_set", "simulation", "game.write", "Set one of the integers a boss cast's scripts share (SET_GLOBAL_INT), through a live member of a cast the mod spawned."},
    {"entities.spawn", "entities", "spawn", "simulation", "game.write", "Spawn a registered entity definition."},
    {"entities.despawn", "entities", "despawn", "simulation", "game.write", "Despawn an entity spawned by this mod."},
    {"entities.set_active_limit", "entities", "set_active_limit", "simulation", "game.write", "Set how many monsters stay awake at once and report the floor's monster slot count."},
    {"combat.get", "combat", "get", "any", "game.read", "Read the supported native enemy health and status snapshot."},
    {"combat.damage", "combat", "damage", "simulation", "game.write", "Apply bounded scripted damage through a generation-checked enemy handle."},
    {"combat.status_add", "combat", "status_add", "simulation", "game.write", "Apply a bounded slow or stop timer to a live Chronicle enemy using its generation-checked handle."},
    {"combat.heal", "combat", "heal", "simulation", "game.write", "Restore bounded health to a live native enemy."},
    {"combat.register_player_formula", "combat", "register_player_formula", "simulation", "game.write", "Register an owned modifier for native incoming player damage, optionally filtered by weapon or character."},
    {"combat.register_formula", "combat", "register_formula", "content", "content.register", "Register an owned damage modifier, optionally filtered by weapon, enemy model, or target HP, with chance and uniform random multipliers."},
    {"locations.list", "locations", "list", "any", "game.read", "List built-in and registered mod locations supported by the active game."},
    {"locations.get", "locations", "get", "any", "game.read", "Read a built-in or registered location and native availability status."},
    {"locations.register", "locations", "register", "content", "content.register", "Register a namespaced map or location definition with transitions and resources."},
    {"dungeons.list", "dungeons", "list", "any", "game.read", "List native dungeons and mod-registered additions supported by this game."},
    {"dungeons.get", "dungeons", "get", "any", "game.read", "Read a native dungeon or registered extension and its floors."},
    {"dungeons.register", "dungeons", "register", "content", "content.register", "Register additional floors on a supported native dungeon map."},
    {"dungeons.selection", "dungeons", "selection", "any", "game.read", "Read the active Chronicle entrance floor selection, or nil."},
    {"dungeons.set_badges", "dungeons", "set_badges", "simulation", "game.write", "Publish owned native floor-list tier badges 0..5."},
    {"ui.open_skills", "ui", "open_skills", "simulation", "content.register", "Open the owning package skill tree in the manager."},
    {"dungeons.set_modifiers", "dungeons", "set_modifiers", "simulation", "game.write", "Set owned additive generation bonuses in percent (0..300): rooms, monsters, chests, chest_value; {} clears this owner."},
    {"dungeons.set_population", "dungeons", "set_population", "simulation", "game.write", "Scale generated monster and chest targets by realized room count using inclusive per-room min/max ratios; native minimums and engine slot limits still apply; {} clears this owner."},
    {"dungeons.set_enemy_survival", "dungeons", "set_enemy_survival", "simulation", "game.write", "Set this mod's chance for eligible ordinary Chronicle enemies to survive one lethal hit; {} clears this owner."},
    {"dungeons.set_layout", "dungeons", "set_layout", "simulation", "game.write", "Set room count, room area and room sizes for floors built after the call; an empty table restores the native layout."},
    {"dungeons.set_seed", "dungeons", "set_seed", "simulation", "game.write", "Build the next new floor from this seed, once (world.get().floor_seed reports a floor's seed); {} lets it pick its own."},
    {"dungeons.chests_get", "dungeons", "chests_get", "any", "game.read", "List the chests on the player's floor: chest number, large, opened, item_id, x, z (mimics left out)."},
    {"dungeons.set_opened_chests", "dungeons", "set_opened_chests", "simulation", "game.write", "Open these chests (numbers from game.chest.opened) as the next front floor and back floor are built, once; with set_seed a kept floor keeps its opened chests."},
    {"dungeons.set_exit_room", "dungeons", "set_exit_room", "simulation", "game.write", "Turn the walk-through exit room on or off for floors built after the call."},
    {"dungeons.floor_rules_get", "dungeons", "floor_rules_get", "any", "game.read", "Read active and per-floor dungeon restriction rules such as Limited Zones or White/Red Seals."},
    {"dungeons.floor_rules_set", "dungeons", "floor_rules_set", "simulation", "game.write", "Override active or per-floor dungeon restriction rules such as Limited Zones or White/Red Seals."},
    {"items.icon_set", "items", "icon_set", "simulation", "content.register", "Give a built-in Chronicle item a mod's own menu icon (icons/<stem>.png), or put the native one back."},
    {"world.dungeon_npc_set", "world", "dungeon_npc_set", "simulation", "game.write", "Stand a town villager's model on the Chronicle dungeon floor, inside the exit gate or beside the player, until the floor changes."},
    {"world.dungeon_npc_clear", "world", "dungeon_npc_clear", "simulation", "game.write", "Take this mod's figure off the dungeon floor."},
    {"ui.run_menu_set", "ui", "run_menu_set", "simulation", "game.write", "Set this mod's Chronicle run menu: tabs of rows, opened from Start in a dungeon in place of the pause."},
    {"ui.run_menu_clear", "ui", "run_menu_clear", "simulation", "game.write", "Take back this mod's run menu; Start pauses as retail does again."},
    {"ui.run_menu_open", "ui", "run_menu_open", "simulation", "game.write", "Open this mod's run menu (on a tab) when dungeon play next allows."},
    {"ui.run_menu_close", "ui", "run_menu_close", "simulation", "game.write", "Close this mod's run menu if it is open."},
    {"ui.boss_card", "ui", "boss_card", "simulation", "game.write", "A boss's entrance card in a Chronicle dungeon: letterbox bars and its name across the screen, play held still for a moment."},
    {"ui.choice_open", "ui", "choice_open", "simulation", "game.write", "Pause the Chronicle dungeon on a panel of 1-4 option cards until the player picks one (ui.choice_made)."},
    {"ui.choice_close", "ui", "choice_close", "simulation", "game.write", "Take back this mod's open choice panel, resuming play."},
    {"ui.map_open", "ui", "map_open", "simulation", "game.write", "Open the map screen over a dungeon (pausing it) or a town: the floor the player is on, or a map dungeons.map_snapshot gave (cells), with marks, a cursor, Triangle for a waypoint when waypoints = true and L2/R2 for other maps when pages = true (ui.map_choice, ui.map_waypoint, ui.map_page, ui.map_closed); opening again while this mod's map is up replaces it."},
    {"ui.map_close", "ui", "map_close", "simulation", "game.write", "Take back this mod's open map screen."},
    {"dungeons.map_snapshot", "dungeons", "map_snapshot", "simulation", "game.read", "The floor the player is on as far as they have seen it: {seed, width, cells, seen}, for ui.map_open and dungeons.set_seen later."},
    {"dungeons.set_seen", "dungeons", "set_seen", "simulation", "game.write", "Count the cells of a kept map (dungeons.map_snapshot) as seen again on the floor the player is on, when it was built from the same seed."},
    {"dungeons.set_waypoint", "dungeons", "set_waypoint", "simulation", "game.write", "Put the red waypoint flag on the mini map at cell x, y of the floor the player is on; {} takes it away."},
    {"weapons.style_set", "weapons", "style_set", "simulation", "game.write", "Give one owned Chronicle weapon a name and colour in the weapon menu, and a glow for its swing trail."},
    {"weapons.style_clear", "weapons", "style_clear", "simulation", "game.write", "Take back this mod's name, colour and glow from one weapon, or from all of them."},
    {"effects.burst", "effects", "burst", "simulation", "game.write", "Play one of Chronicle's own effects at a point: explosion, heal, sparks, guard or impact."},
    {"entities.tint", "entities", "tint", "simulation", "game.write", "Draw a live Chronicle enemy in a colour for a number of frames."},
    {"entities.knockback", "entities", "knockback", "simulation", "game.write", "Shove a live Chronicle enemy along a direction at a speed that slows to a stop."},
    {"dungeons.set_character_blocks", "dungeons", "set_character_blocks", "simulation", "game.write", "Put only the named characters' own doors and parts (Xiao's jumps, Goro's and Ruby's doors, Ungaga's and Osmond's parts) on new Chronicle floors."},
    {"dungeons.set_zones", "dungeons", "set_zones", "simulation", "game.write", "Lay Chronicle floors built from now on out as 2-24 zones joined by locked gates; a monster in each zone carries the next gate's key; open lists gates built open; shrines = true puts a spring in each zone's shrine room; a zone's source_floor gives it that floor's monsters, atla and chest items; exit_floor is where the floor's exit gate leads; smoke lists gates no key opens (game.dungeon.smoke_touched); shrine_heal = false keeps the springs from healing HP; sanctuary = true keeps monsters out of a shrine's circle; {} turns zones off."},
    {"dungeons.set_exits", "dungeons", "set_exits", "simulation", "game.write", "Keep the party from leaving a Chronicle dungeon for town by the Escape menu or the exit gate (town = \"shrines\"; the gate leads on), or give the exits back (town = \"any\")."},
    {"dungeons.warp", "dungeons", "warp", "simulation", "game.write", "Move the player at once to a cell (x, y) of the dungeon floor they are on."},
    {"dungeons.drop_gilda", "dungeons", "drop_gilda", "simulation", "game.write", "Put a pile of gold (amount) on the dungeon floor the player is on, at a world position (x, y, z); the player takes it by walking onto it."},
    {"dungeons.enter_floor", "dungeons", "enter_floor", "simulation", "game.write", "Build floor floor_id of the current dungeon (or, from a town, of dungeon_id) again from the start (its monsters come back) and enter it, the player arriving at cell x, y when given."},
    {"dungeons.set_arrival", "dungeons", "set_arrival", "simulation", "game.write", "Start the player on the next floor built at cell x, y, or in front of a zone's shrine (zone)."},
    {"dungeons.zones_get", "dungeons", "zones_get", "any", "game.read", "Read the zones asked for, the player's floor's zones, gates (zone, x, y, key, locked, carrier) and shrines (zone, x, y), and the zone the player stands in."},
    {"dungeons.set_warden_room", "dungeons", "set_warden_room", "simulation", "game.write", "Seal the room behind each new Chronicle floor's exit gate until every monster outside it is dead, optionally closing it behind the player."},
    {"dungeons.warden_room_get", "dungeons", "warden_room_get", "any", "game.read", "Read the current floor's Warden room: state, centre, size."},
    {"dungeons.warden_room_release", "dungeons", "warden_room_release", "simulation", "game.write", "Open the Warden room's doors that closed behind the player."},
    {"dungeons.set_rest_room", "dungeons", "set_rest_room", "simulation", "game.write", "Build new Chronicle floors as rest floors: a hall with a healing spring and an antechamber with the back floor's stairs, joined by the corridor that holds the way in and the exit."},
    {"dungeons.rest_room_get", "dungeons", "rest_room_get", "any", "game.read", "Read the current rest floor: its spring, back floor stairs and hall."},
    {"dungeons.set_npc_marks", "dungeons", "set_npc_marks", "simulation", "game.write", "Chronicle: {marks = {{npc, x, y, facing}}} stands the floor's own NPCs (loaded with its parts, such as the Fairy King, NPC 0) at cells of the floor the player is on; {} takes them away."},
    {"dungeons.set_npc_models", "dungeons", "set_npc_models", "simulation", "game.write", "Chronicle: {models = {{dungeon_id, npc = 1-3, pack = \"dun/.../<stem>.chr\" or \"gedit/...\", cfg (default <stem>.cfg; base.cfg for dun/mainchara), motion}}} loads characters into a dungeon's free NPC slots when its map loads, for set_npc_marks to stand; {} stops."},
    {"dungeons.play_event", "dungeons", "play_event", "simulation", "game.write", "Chronicle: {program = n} plays program n of the dungeon's event script (a scene) on the floor the player is on, as a monster's script asks for one; game.dungeon.event_ended follows when it is over."},
    {"world.set_roads", "world", "set_roads", "simulation", "game.write", "Chronicle: {enabled = true} makes walking into a town's gate game.town.road, the mod's to take (world.transition), not the world map."},
    {"dungeons.set_quick_passage", "dungeons", "set_quick_passage", "simulation", "game.write", "Chronicle: {enabled = true} goes between a town and a dungeon with no loading card, only the fades; {enabled = false} gives the card back."},
    {"dungeons.set_floor_select", "dungeons", "set_floor_select", "simulation", "game.write", "Chronicle: {enabled = false} turns the dungeon entrance's floor menu off, so it takes the player straight to the first floor; {enabled = true} gives it back."},
    {"dungeons.set_story_exits", "dungeons", "set_story_exits", "simulation", "game.write", "Chronicle floors of the game's own design: the gate that asks for the floor's key asks the mod's question (leave this floor?) instead; the gate stays shut and the mod takes the player on (game.dungeon.story_exit)."},
    {"dungeons.set_backrooms", "dungeons", "set_backrooms", "simulation", "game.write", "Free Chronicle back floors: the stairs ask whether to go down (enter at your own risk) instead of asking for the gate key."},
    {"dungeons.set_weapon_drops", "dungeons", "set_weapon_drops", "simulation", "game.write", "Set the rules for weapons in Chronicle chests: none the party holds, a chance of a stronger one, the share of large chests."},
    {"dungeons.chest_weapons_add", "dungeons", "chest_weapons_add", "simulation", "game.write", "Let Chronicle dungeon chests hold a registered weapon from a dungeon floor on, a percent of the time a chest holds a weapon."},
    {"dungeons.chest_weapons_clear", "dungeons", "chest_weapons_clear", "simulation", "game.write", "Take back the chest weapons this mod added."},
    {"dungeons.set_shortcuts", "dungeons", "set_shortcuts", "simulation", "game.write", "Ask for shortcut doors on each new Chronicle floor: dead ends loop back, locked, with their keys in the dead end."},
    {"dungeons.shortcuts_get", "dungeons", "shortcuts_get", "any", "game.read", "List the current floor's shortcut doors, their keys and whether each is still locked."},
    {"dungeons.floor_redirect", "dungeons", "floor_redirect", "simulation", "game.write", "Send a registered floor's exit gate to another registered floor of the same dungeon (Chronicle)."},
    {"dungeons.loot_get", "dungeons", "loot_get", "any", "game.read", "Read active floor treasure chest contents and monster drop tables."},
    {"dungeons.loot_override", "dungeons", "loot_override", "simulation", "game.write", "Override floor treasure chest contents or monster drop tables."},
    {"dungeons.back_floor_get", "dungeons", "back_floor_get", "any", "game.read", "Read Chronicle back-floor state, the active floor gate key, and whether the key is held."},
    {"dungeons.back_floor_enter", "dungeons", "back_floor_enter", "simulation", "game.write", "Queue Chronicle's native back-floor transition without consuming the gate key."},
    {"dungeons.spheda_get", "dungeons", "spheda_get", "any", "game.read", "Read the active Chronicle Two Spheda challenge, pin, ball, par, and gauge state."},
    {"dungeons.spheda_spawn", "dungeons", "spheda_spawn", "simulation", "game.write", "Reset and activate the active Chronicle Two Spheda course's pin and ball placement."},
    {"georama.get", "georama", "get", "any", "game.read", "Read settlement reconstruction state and placed pieces."},
    {"georama.element_set", "georama", "element_set", "simulation", "game.write", "Toggle a defined optional visual element on a native Georama plot."},
    {"georama.request_set", "georama", "request_set", "simulation", "game.write", "Set a saved native Georama request's completion state."},
    {"georama.request_known_set", "georama", "request_known_set", "simulation", "game.write", "Set whether a saved native Georama request is known to the player."},
    {"georama.condition_set", "georama", "condition_set", "simulation", "game.write", "Set whether a saved native Georama analysis condition is met."},
    {"georama.register", "georama", "register", "content", "content.register", "Register a Georama piece variant from a native template, optionally with its own model and footprint cells."},
    {"georama.place", "georama", "place", "simulation", "game.write", "Place a registered georama piece through native placement rules."},
    {"georama.remove", "georama", "remove", "simulation", "game.write", "Remove a placed georama piece through native rules."},
    {"georama.river_set", "georama", "river_set", "simulation", "game.write", "Set or clear one cell in a saved town's native river grid."},
    {"georama.atla_list", "georama", "atla_list", "any", "game.read", "Inspect Atla spheres on the active Chronicle dungeon floor."},
    {"georama.atla_spawn", "georama", "atla_spawn", "simulation", "game.write", "Put a mod's own Atla sphere on the active Chronicle dungeon floor."},
    {"georama.atla_collect", "georama", "atla_collect", "simulation", "game.write", "Collect an Atla sphere and release its Georama part or chip through Chronicle's native save path."},
    {"georama.satisfaction_get", "georama", "satisfaction_get", "any", "game.read", "Read saved Georama request completion and per-plot reconstruction state for Chronicle towns."},
    {"skills.list", "skills", "list", "any", "game.read", "List mod-owned skill trees and progression nodes."},
    {"skills.get", "skills", "get", "any", "game.read", "Read mod-owned skill ranks, prerequisites, and unlock state."},
    {"skills.register", "skills", "register", "content", "content.register", "Register a namespaced mod-owned skill tree and progression nodes."},
    {"skills.unlock", "skills", "unlock", "simulation", "game.write", "Advance a skill rank after mod-defined prerequisites pass."},
    {"skills.set_rank", "skills", "set_rank", "simulation", "game.write", "Set a skill rank within its registered progression limits."},
    {"models.register", "models", "register", "content", "content.register", "Register a package-owned model and its logical resource ids."},
    {"models.unregister", "models", "unregister", "content", "content.register", "Remove an unused model binding owned by this mod."},
    {"models.replace", "models", "replace", "content", "content.register", "Replace a model through conflict-checked native asset bindings."},
    {"models.resolve", "models", "resolve", "any", "game.read", "Resolve a native monster model table entry and report its asset paths and override state."},
    {"models.remove", "models", "remove", "content", "content.register", "Restore a native model row previously replaced by this mod."},
    {"animations.register", "animations", "register", "content", "content.register", "Bind a model-embedded motion index to a stable mod animation id."},
    {"animations.play", "animations", "play", "simulation", "game.write", "Play a registered animation on a compatible entity."},
    {"animations.stop", "animations", "stop", "simulation", "game.write", "Stop an animation started by this mod."},
    {"combat.styles_list", "combat", "styles_list", "any", "game.read", "List fighting-style definitions supported by the active game."},
    {"combat.styles_get", "combat", "styles_get", "any", "game.read", "Read a fighting-style move set, rules, and compatibility."},
    {"combat.styles_register", "combat", "styles_register", "content", "content.register", "Register a fighting style and its move/animation bindings."},
    {"combat.styles_set_active", "combat", "styles_set_active", "simulation", "game.write", "Switch to a compatible fighting style through native combat rules."},
    {"scripts.register_function", "scripts", "register_function", "content", "content.register", "Register a namespaced callable function exposed by this mod."},
    {"scripts.list_functions", "scripts", "list_functions", "any", "game.read", "List this mod's exported functions and exports from its loaded declared dependencies."},
    {"scripts.call_function", "scripts", "call_function", "simulation", "game.write", "Call an explicitly exposed mod function with validated JSON arguments."},
    {"scripts.unregister_function", "scripts", "unregister_function", "content", "content.register", "Remove a callable function owned by this mod."},
    {"items.list", "items", "list", "any", "game.read", "List built-in and registered mod item definitions."},
    {"items.get", "items", "get", "any", "game.read", "Read one item definition by stable id."},
    {"items.register", "items", "register", "content", "content.register", "Register a data-defined item, optionally with its own menu icon."},
    {"items.give", "items", "give", "simulation", "game.write", "Give a built-in or registered item to an inventory."},
    {"items.remove", "items", "remove", "simulation", "game.write", "Remove a quantity of an item from an inventory."},
    {"items.use", "items", "use", "simulation", "game.write", "Use an owned item through validated native item rules."},
    {"items.quick_slots", "items", "quick_slots", "any", "game.read", "Read the three native quick-item slots and their counts."},
    {"items.quick_slot_select", "items", "quick_slot_select", "simulation", "game.write", "Select one of the three native dungeon quick-item slots for normal in-game use."},
    {"items.quick_slot_use", "items", "quick_slot_use", "simulation", "game.write", "Select a native dungeon quick-item slot and request its normal action-button use."},
    {"shops.list", "shops", "list", "any", "game.read", "List native town and dungeon shops and their item lineups."},
    {"shops.get", "shops", "get", "any", "game.read", "Read one native shop's goods, currency mode, and item buy/sell prices."},
    {"shops.register_item", "shops", "register_item", "content", "content.register", "Add a native or mod-registered item or weapon to a shop's goods list."},
    {"shops.remove_item", "shops", "remove_item", "content", "content.register", "Remove an item or weapon from a shop's goods list."},
    {"shops.set_items", "shops", "set_items", "content", "content.register", "Replace a native Chronicle shop's item list with an owner-restored list of up to 20 goods."},
    {"shops.set_price", "shops", "set_price", "content", "content.register", "Set the buy and sell prices for an item or weapon in native shops."},
    {"weapons.list", "weapons", "list", "any", "game.read", "List built-in and registered weapon definitions."},
    {"weapons.get", "weapons", "get", "any", "game.read", "Read a weapon definition and current state."},
    {"weapons.definition_set", "weapons", "definition_set", "content", "content.register", "Apply an owner-scoped override to a built-in Chronicle weapon definition; Chronicle restores the original data when that owner unloads."},
    {"weapons.register", "weapons", "register", "content", "content.register", "Register a data-defined weapon, optionally with its own model and menu icon."},
    {"weapons.give", "weapons", "give", "simulation", "game.write", "Give a native or registered weapon to a compatible character."},
    {"weapons.equip", "weapons", "equip", "simulation", "game.write", "Equip a weapon through the game's equipment rules."},
    {"weapons.remove", "weapons", "remove", "simulation", "game.write", "Remove one owned, unequipped Chronicle weapon instance."},
    {"weapons.upgrade", "weapons", "upgrade", "simulation", "game.write", "Upgrade a weapon through supported progression rules."},
    {"weapons.stats_set", "weapons", "stats_set", "simulation", "game.write", "Set bounded stats and properties on one owned Chronicle weapon instance."},
    {"weapons.durability_set", "weapons", "durability_set", "simulation", "game.write", "Set current durability on one owned Chronicle weapon instance."},
    {"weapons.abs_get", "weapons", "abs_get", "any", "game.read", "Read accumulated ABS experience, next-level ABS threshold, and synthesis points on an owned weapon."},
    {"weapons.abs_add", "weapons", "abs_add", "simulation", "game.write", "Grant ABS experience or synthesis points to an owned weapon; a negative amount takes unspent ABS away."},
    {"weapons.attachments_get", "weapons", "attachments_get", "any", "game.read", "Read fitted attachments or synthesis capacity on an owned weapon."},
    {"weapons.attachment_insert", "weapons", "attachment_insert", "simulation", "game.write", "Insert an attachment into an owned weapon slot or synthesize an attachment into a weapon."},
    {"weapons.attachment_remove", "weapons", "attachment_remove", "simulation", "game.write", "Remove a fitted attachment from an owned weapon slot back to the attachment bag."},
    {"weapons.build_up_list", "weapons", "build_up_list", "any", "game.read", "List native and mod-registered build-up evolution targets for a weapon."},
    {"weapons.build_up_register", "weapons", "build_up_register", "content", "content.register", "Register a custom build-up evolution path from a source weapon to a target weapon."},
    {"weapons.synthesize", "weapons", "synthesize", "simulation", "game.write", "Status-break a qualifying owned weapon into a SynthSphere attachment in Chronicle."},
    {"weapons.spectrumize", "weapons", "spectrumize", "simulation", "game.write", "Spectrumize an owned weapon or item into a synthesized attachment in Chronicle Two."},
    {"photos.ideas_list", "photos", "ideas_list", "any", "game.read", "List learnt photo ideas, carried photos, and photographed subject ids in Chronicle Two."},
    {"photos.idea_unlock", "photos", "idea_unlock", "simulation", "game.write", "Learn a Chronicle Two photo idea through the native invention save data."},
    {"photos.scoops_list", "photos", "scoops_list", "any", "game.read", "List Chronicle Two scoops with known, obtained, photographed, and learnt state."},
    {"inventions.list", "inventions", "list", "any", "game.read", "List native and mod-registered Chronicle Two invention recipes."},
    {"inventions.register", "inventions", "register", "content", "content.register", "Register a namespaced three-idea Chronicle Two invention recipe and its materials."},
    {"inventions.craft", "inventions", "craft", "simulation", "game.write", "Craft an unlocked native or mod-registered invention through native inventory and material rules."},
    {"characters.list", "characters", "list", "any", "game.read", "List character definitions and visible instances."},
    {"characters.get", "characters", "get", "any", "game.read", "Read character stats, state, and stable handle."},
    {"characters.register", "characters", "register", "content", "content.register", "Register a data-defined enemy or town villager (kind = 'npc')."},
    {"characters.register_playable", "characters", "register_playable", "content", "content.register", "Register a playable character past the game's six (pack, portrait, the character he fights as); never in a save."},
    {"characters.spawn", "characters", "spawn", "simulation", "game.write", "Spawn a registered character or enemy."},
    {"quests.get", "quests", "get", "any", "game.read", "Read this mod's quest definitions and objective progress."},
    {"quests.register", "quests", "register", "content", "content.register", "Register namespaced quest objectives and optional currency, item, or weapon rewards."},
    {"quests.start", "quests", "start", "simulation", "game.write", "Start a registered quest."},
    {"quests.advance", "quests", "advance", "simulation", "game.write", "Advance an objective through validated quest state."},
    {"scripts.list_hooks", "scripts", "list_hooks", "any", "game.read", "List supported native script and simulation hooks."},
    {"scripts.call", "scripts", "call", "simulation", "game.write", "Call the active native hook implementation once from its owned override callback."},
    {"scripts.override", "scripts", "override", "content", "scripts.override", "Install or replace an owned callback for a supported native script or simulation hook."},
    {"content.list", "content", "list", "any", "game.read", "List registered typed data definitions owned by enabled mods."},
    {"content.register", "content", "register", "content", "content.register", "Register a typed item, weapon, character, dungeon, or Georama piece definition supported by this game."},
    {"content.unregister", "content", "unregister", "content", "content.register", "Remove a mod-owned pending definition before it is materialized in native game tables."},
    {"save.get", "save", "get", "any", "game.read", "Read namespaced save data and migration version."},
    {"save.slot_id", "save", "slot_id", "any", "game.read", "Read the active numbered save slot and its persistent generation in Chronicle."},
    {"save.set", "save", "set", "simulation", "game.write", "Write bounded namespaced save data."},
    {"save.migrate", "save", "migrate", "load", "game.write", "Run an owned save migration at the save-load boundary."},
    {"save.autosave_set", "save", "autosave_set", "any", "game.write", "Enable or disable Chronicle's guarded native autosave writer."},
    {"input.bind", "input", "bind", "any", "game.write", "Bind a namespaced action to a keyboard or controller input."},
    {"input.unbind", "input", "unbind", "any", "game.write", "Remove an input binding owned by this mod."},
    {"input.state", "input", "state", "any", "game.read", "Read a named action state without exposing raw device memory."},
    {"input.consume", "input", "consume", "input", "game.write", "Consume a named action in the active input context."},
    {"input.axis", "input", "axis", "any", "game.read", "Read a processed gamepad stick or trigger axis as a normalized value."},
    {"input.wheel_mode_set", "input", "wheel_mode_set", "simulation", "game.write", "Set this mod's Chronicle mouse-wheel routing mode."},
    {"input.block_native", "input", "block_native", "any", "game.write", "Hide named pad buttons from the game itself for a few frames (mods still read them)."},
    {"input.context_push", "input", "context_push", "any", "game.write", "Push an owner-scoped modal input context onto the priority stack."},
    {"input.context_pop", "input", "context_pop", "any", "game.write", "Remove an input context owned by this mod using its token."},
    {"input.context_get", "input", "context_get", "any", "game.read", "Read the active input context and priority stack."},
    {"audio.play", "audio", "play", "simulation", "game.write", "Play a registered sound or music cue."},
    {"audio.stop", "audio", "stop", "simulation", "game.write", "Stop playback owned by this mod."},
    {"audio.register", "audio", "register", "content", "content.register", "Register a supported audio resource."},
    {"audio.se_play", "audio", "se_play", "simulation", "game.write", "Play one of the game's own sound effects by its table number."},
    {"audio.bgm_play", "audio", "bgm_play", "simulation", "game.write", "Load one of the game's music sets and play a track of it."},
    {"audio.bgm_stop", "audio", "bgm_stop", "simulation", "game.write", "Stop the game's music."},
    {"audio.music_set", "audio", "music_set", "content", "content.register", "Play a package recording in place of a game music sequence."},
    {"audio.music_clear", "audio", "music_clear", "content", "content.register", "Stop replacing a game music sequence."},
    {"render.layer_register", "render", "layer_register", "render", "content.register", "Register a bounded owner-scoped HUD text layer."},
    {"render.layer_remove", "render", "layer_remove", "render", "content.register", "Remove a HUD text layer owned by this mod."},
    {"render.bar_register", "render", "bar_register", "render", "content.register", "Register or update a bounded owner-scoped HUD progress bar."},
    {"render.bar_remove", "render", "bar_remove", "render", "content.register", "Remove a HUD progress bar owned by this mod."},
    {"render.hud_icon_set", "render", "hud_icon_set", "render", "content.register", "Show or update a HUD icon beside the floor plate: a native status badge or a png."},
    {"render.hud_icon_remove", "render", "hud_icon_remove", "render", "content.register", "Remove a HUD icon owned by this mod."},
    {"render.floor_label_set", "render", "floor_label_set", "render", "content.register", "Show up to six characters on the floor plate in place of its number."},
    {"render.floor_label_clear", "render", "floor_label_clear", "render", "content.register", "Give the floor plate its number back."},
    {"render.camera_get", "render", "camera_get", "any", "game.read", "Read the active dungeon camera pose."},
    {"render.camera_set", "render", "camera_set", "render", "game.write", "Set bounded distance, height, or angle on the active dungeon follow camera."},
    {"text.get", "text", "get", "any", "game.read", "Read a game message by its language-file key, and whether a mod, the language file or the disc gives it."},
    {"text.set", "text", "set", "content", "content.register", "Set a game message by its language-file key; applies when its file next loads."},
    {"text.clear", "text", "clear", "content", "content.register", "Take back a game message this mod set."},
    {"events.natives", "events", "natives", "any", "game.read", "List the retail event-script natives events.call can run in town or dungeon."},
    {"events.call", "events", "call", "simulation", "game.write", "Run one retail event-script getter, check or flag native outside a running event."},
    {"monsters.natives", "monsters", "natives", "any", "game.read", "List the retail monster AI natives monsters.call can run."},
    {"monsters.call", "monsters", "call", "simulation", "game.write", "Run one retail monster AI native on a live monster, as its own script would."},
    {"dungeons.lighting_get", "dungeons", "lighting_get", "any", "game.read", "Read the active floor's fog, background and ambient light, front and back side."},
    {"dungeons.lighting_set", "dungeons", "lighting_set", "simulation", "game.write", "Set the active floor's fog, background or ambient light until the next floor loads."},
    {"render.town_camera_set", "render", "town_camera_set", "simulation", "game.write", "Enable or disable an owner-scoped overhead camera during safe town walking."},
    {"render.first_person_toggle", "render", "first_person_toggle", "simulation", "game.write", "Request the native dungeon first-person view transition."},
    {"ui.register", "ui", "register", "content", "content.register", "Register an owned button in the in-game MoonFactory manager."},
    {"ui.remove", "ui", "remove", "content", "content.register", "Remove an owned MoonFactory manager button."},
    {"ui.navigate", "ui", "navigate", "content", "content.register", "Open a loaded package and one of its tabs in the MoonFactory manager."},
    {"ui.notify", "ui", "notify", "any", "game.write", "Show a bounded notice in the mod manager."},
    {"ui.sheet_set", "ui", "sheet_set", "any", "game.write", "Set this mod's character sheet: the header of the Journal's Character page beside its skill tree."},
    {"ui.sheet_clear", "ui", "sheet_clear", "any", "game.write", "Take back this mod's character sheet."},
    {"ui.sheet_open", "ui", "sheet_open", "any", "game.write", "Open the Journal's Character page during play."},
    {"physics.query", "physics", "query", "simulation", "game.read", "Find live monster actor origins inside a sphere; this does not test collision geometry."},
    {"physics.velocity_get", "physics", "velocity_get", "simulation", "game.read", "Read velocity from a stable entity handle."},
    {"physics.velocity_set", "physics", "velocity_set", "simulation", "game.write", "Set velocity through the simulation adapter."},
    {"resources.load", "resources", "load", "any", "content.register", "Load a package-owned resource using adapter rules."},
    {"resources.release", "resources", "release", "any", "content.register", "Release a resource handle owned by this mod."},
    {"resources.resolve", "resources", "resolve", "any", "game.read", "Resolve a logical resource id and report its source."},
    {"resources.override", "resources", "override", "content", "content.register", "Override a native asset path with a package resource."},
    {"resources.unoverride", "resources", "unoverride", "content", "content.register", "Remove a native asset override owned by this mod."},
    {"localization.register", "localization", "register", "content", "content.register", "Register mod-owned localized strings."},
    {"localization.get", "localization", "get", "any", "game.read", "Resolve a localized string with fallback rules."},
    {"commands.register", "commands", "register", "content", "content.register", "Register a namespaced developer or player command."},
    {"commands.execute", "commands", "execute", "ui", "game.write", "Execute an enabled command through its owning callback."},
    {"commands.unregister", "commands", "unregister", "content", "content.register", "Remove a command owned by this mod."},
    {"compat.list", "compat", "list", "any", "", "List API operation support for this game and build."},
    {"compat.supports", "compat", "supports", "any", "", "Query support for a versioned API operation."},
    {"debug.inspect", "debug", "inspect", "any", "debug.inspect", "Read a bounded snapshot of the current native scene context."},
    {"debug.profile", "debug", "profile", "any", "debug.inspect", "Read bounded per-mod resource and registration counters."},
    {"debug.overlay_register", "debug", "overlay_register", "render", "debug.inspect", "Register or update an owner-scoped one-line diagnostic overlay."},
    {"title.mode_register", "title", "mode_register", "content", "content.register", "Add or update this mod's title mode: a title-menu row that opens a screen the mod drives."},
    {"title.mode_set", "title", "mode_set", "any", "game.write", "Change this mod's title mode screen: heading, info lines, menu entries and cursor."},
    {"title.mode_remove", "title", "mode_remove", "content", "content.register", "Remove this mod's title mode."},
    {"session.get", "session", "get", "any", "game.read", "Read whether the game is in the title, a story save or a mod's sandbox session."},
    {"session.start", "session", "start", "any", "game.write", "From this mod's open title mode screen, start a sandbox session on a registered dungeon floor: a fresh game with no save slot."},
    {"session.finish", "session", "finish", "simulation", "game.write", "End this mod's sandbox session and return to its title mode screen."},
    {"api.list", "api", "list", "any", "", "List all catalogued operations and capability status."},
    {"api.supports", "api", "supports", "any", "", "Query one operation capability by canonical path."},
    {"api.call", "api", "call", "any", "", "Invoke an operation by canonical path."},
};

constexpr EventSpec kEventCatalog[] = {
    {"runtime.started", "runtime", "both", "The MoonFactory runtime has started."},
    {"runtime.stopping", "runtime", "both", "The MoonFactory runtime is stopping."},
    {"runtime.loaded", "runtime", "both", "A mod package has loaded."},
    {"runtime.unloaded", "runtime", "both", "A mod package has unloaded."},
    {"settings.changed", "settings", "both", "A setting owned by this mod changed in the manager."},
    {"game.player.move", "player", "chronicle", "A mod move (player.moves_set) started: its id, the mod that set it and the character."},
    {"game.dungeon.warden_room", "dungeons", "chronicle", "The Warden room opened (state open, center) or the player walked in (state entered, center, size, shut)."},
    {"game.dungeon.backroom", "dungeons", "chronicle", "The player answered the free back floor's prompt: entered."},
    {"game.dungeon.event_ended", "dungeons", "chronicle", "A scene started with dungeons.play_event is over (to that mod only): program, dungeon_id, floor_id. The floor's monsters are gone after it."},
    {"game.dungeon.story_exit", "dungeons", "chronicle", "The player answered a fixed floor's gate (dungeons.set_story_exits): left, dungeon_id, floor_id."},
    {"game.dungeon_npc.near", "world", "chronicle", "The player walked up to this mod's figure on the dungeon floor: id."},
    {"ui.run_menu_action", "ui", "chronicle", "The player used a row of this mod's run menu: tab and row ids."},
    {"ui.run_menu_shown", "ui", "chronicle", "This mod's run menu opened or closed: open, tab."},
    {"ui.choice_made", "ui", "chronicle", "The player picked an option on this mod's choice panel (index from 1), or backed out (cancelled)."},
    {"ui.map_choice", "ui", "chronicle", "The player pressed Cross on a mark of this mod's map screen (mark from 1, x, y); the map has closed."},
    {"ui.map_waypoint", "ui", "chronicle", "The player put (set = true) or took away the waypoint at x, y on this mod's map screen."},
    {"ui.map_page", "ui", "chronicle", "The player pressed L2 (way -1) or R2 (way 1) on this mod's map screen opened with pages = true; open another map to turn the page."},
    {"ui.map_closed", "ui", "chronicle", "This mod's map screen closed."},
    {"ui.action", "ui", "both", "A manager action owned by this mod was activated."},
    {"game.tick", "game", "both", "A simulation frame was pumped."},
    {"game.scene.will_enter", "scene", "both", "A scene is about to initialize."},
    {"game.scene.entered", "scene", "both", "A scene finished initialization."},
    {"game.dungeon.generating", "dungeons", "both", "Before a Chronicle procedural floor is generated; dungeon_id, floor_id, back_floor."},
    {"game.dungeon.generated", "dungeons", "chronicle", "A Chronicle procedural floor was built; dungeon_id, floor_id, back_floor, floor_seed (dungeons.set_seed builds it again), room_count."},
    {"game.world.location_changed", "world", "both", "A world location changed; published at scene initialization when detectable and polled within scenes."},
    {"game.item_used", "items", "both", "A mod API item-use call completed."},
    {"game.player.thirst_changed", "player", "chronicle", "The active character's Chronicle water gauge changed, drained empty, or was restored."},
    {"game.photo.taken", "photos", "chronicle-two", "Max took a photo; reports its subject, map, and scoop or idea recognition state."},
    {"game.invention.created", "inventions", "chronicle-two", "A Chronicle Two invention was discovered in the menu or crafted through the API."},
    {"game.spheda.completed", "dungeons", "chronicle-two", "A Chronicle Two Spheda challenge was recorded as cleared."},
    {"game.item_given", "items", "both", "An item or weapon was added through a MoonFactory grant API."},
    {"game.item_picked_up", "items", "both", "A native dungeon pickup was added to the player's inventory."},
    {"game.item_removed", "items", "both", "An item was removed through the MoonFactory inventory API."},
    {"game.shop.purchased", "shops", "both", "A shop purchase or shop inventory change completed."},
    {"game.enemy_spawned", "characters", "both", "A registered mod-owned enemy spawned successfully."},
    {"game.enemy_defeated", "combat", "both", "The game accepted an enemy death."},
    {"game.dungeon.zone_entered", "dungeons", "chronicle", "The player walked into another zone of a zoned floor (or arrived on one): zone, previous (-1 on arrival), zones."},
    {"game.dungeon.zone_gate_opened", "dungeons", "chronicle", "The player opened a zone gate with its key: zone (the zone behind it), key, x, y."},
    {"game.dungeon.shrine_reached", "dungeons", "chronicle", "The player stands in front of a zone's shrine spring (dungeons.set_zones shrines): zone, x, y; once until they walk away."},
    {"game.dungeon.defeated", "dungeons", "chronicle", "The party was knocked out in a dungeon and is about to be sent to town, which halves its Gilda: dungeon_id, floor_id, gilda (before)."},
    {"game.dungeon.town_refused", "dungeons", "chronicle", "dungeons.set_exits kept the party from leaving for town: via (\"menu\" or \"gate\")."},
    {"game.dungeon.zone_gate_locked", "dungeons", "chronicle", "The player reached a locked zone gate without its key: zone, key, x, y."},
    {"game.dungeon.smoke_touched", "dungeons", "chronicle", "The player walked up to a smoke gate (dungeons.set_zones smoke): zone, x, y; once until they walk away."},
    {"game.dungeon.door_opened", "dungeons", "chronicle", "The player opened a shortcut door with its key: key, x, y."},
    {"game.dungeon.door_locked", "dungeons", "chronicle", "The player reached a locked shortcut door without its key: key, x, y."},
    {"game.companion.hit", "party", "chronicle", "The companion's blow landed on an enemy: entity, damage, killed, reward_exp."},
    {"game.player.guarded", "player", "chronicle", "A fatal hit left a character on 1 HP and spent one of its player.fatal_guard charges."},
    {"game.enemy_hit", "combat", "chronicle", "A native weapon hit damaged an enemy; includes attacker, weapon, target, model, element, and final damage."},
    {"game.weapon.upgraded", "weapons", "both", "A native weapon upgrade changed the weapon."},
    {"game.weapon.equipped", "weapons", "both", "The active weapon changed through a supported API."},
    {"game.weapon.abs_gained", "weapons", "both", "An owned weapon gained ABS experience or synthesis points."},
    {"game.weapon.built_up", "weapons", "both", "A weapon evolved into a build-up target or synthesized into a sphere/spectol."},
    {"game.weapon.broken", "weapons", "both", "A weapon broke or was status-broken / spectrumized."},
    {"game.chest.opened", "dungeons", "both", "The player opened a dungeon chest (dungeon_id, floor_id, back_floor, chest, item_id, large), or a mod updated a loot table."},
    {"game.floor.cleared", "dungeons", "both", "A dungeon floor was cleared or its restriction rules changed."},
    {"game.party.active_changed", "party", "both", "The active party member changed through a supported API."},
    {"game.party.recruited", "party", "chronicle-two", "A townsperson joined or replaced the current companion."},
    {"game.story_flag_changed", "world", "both", "A native story flag changed through a supported API."},
    {"game.story_value_changed", "world", "both", "A native story counter changed through a supported API."},
    {"game.map_flag_changed", "world", "both", "A native map flag changed through a supported API."},
    {"game.georama.request_changed", "georama", "both", "A native Georama request changed through a supported API."},
    {"game.georama.element_changed", "georama", "chronicle", "A native Chronicle plot element changed."},
    {"game.georama.atla_collected", "georama", "chronicle", "Toan picked up a mod's own Atla sphere (georama.atla_spawn)."},
    {"game.georama.condition_changed", "georama", "chronicle-two", "A Chronicle Two analysis condition changed."},
    {"game.georama.request_known_changed", "georama", "chronicle-two", "A Chronicle Two Georama request became known or unknown."},
    {"game.georama.river_changed", "georama", "chronicle-two", "A Chronicle Two river cell changed."},
    {"game.georama.piece_placed", "georama", "both", "A Georama piece was placed."},
    {"game.georama.piece_removed", "georama", "both", "A Georama piece was removed."},
    {"game.npc.talk", "characters", "chronicle", "The player pressed talk at a town villager the mod registered (characters.register kind = 'npc') that has no native talk event."},
    {"game.title.opened", "title", "chronicle", "The player opened this mod's title mode screen from the title menu."},
    {"game.title.selected", "title", "chronicle", "The player confirmed an entry on this mod's title mode screen, or pressed Circle (entry 'back')."},
    {"game.session.started", "session", "chronicle", "A mod's sandbox session started: a fresh game with no save slot."},
    {"game.session.ended", "session", "chronicle", "A sandbox session ended: reason 'defeat', 'left_dungeon', 'ended' or 'loaded_save'."},
    {"quests.started", "quests", "both", "A registered quest became active."},
    {"quests.objective_advanced", "quests", "both", "A quest objective advanced."},
    {"quests.completed", "quests", "both", "All objectives in a quest are complete."},
    {"skills.train_requested", "skills", "both", "The player requested training a skill in the owning package tree."},
    {"skills.rank_changed", "skills", "both", "A skill rank changed."},
    {"skills.unlocked", "skills", "both", "A skill's rank first became positive."},
};

constexpr std::array<std::string_view, 8> kKnownPermissions = {
    "storage.read", "storage.write", "game.read", "game.write", "content.register", "scripts.override",
    "debug.inspect", "native.extension",
};

} // namespace

std::span<const ApiOperationSpec> ApiCatalog() { return kApiCatalog; }
std::span<const EventSpec> EventCatalog() { return kEventCatalog; }

const ApiOperationSpec *FindApiOperation(std::string_view path) {
    const auto found = std::ranges::find(kApiCatalog, path, &ApiOperationSpec::path);
    return found == std::end(kApiCatalog) ? nullptr : &*found;
}

bool IsCoreApiOperation(std::string_view path) {
    return path == "info.get" || path == "compat.list" || path == "compat.supports" ||
           path == "api.list" || path == "api.supports" || path == "api.call" ||
           path == "events.list" || path == "events.subscribe" || path == "events.unsubscribe" ||
           path == "save.get" || path == "save.set" ||
           path == "save.migrate" ||
           path == "quests.get" || path == "quests.register" || path == "quests.start" ||
           path == "quests.advance" ||
           path == "localization.register" || path == "localization.get" || path == "resources.load" ||
           path == "resources.resolve" || path == "resources.release" || path == "resources.override" ||
           path == "resources.unoverride" || path == "ui.register" ||
           path == "ui.open_skills" || path == "ui.remove" || path == "ui.navigate" || path == "ui.notify" ||
           path == "ui.sheet_set" || path == "ui.sheet_clear" || path == "ui.sheet_open" || path == "commands.register" ||
           path == "commands.execute" || path == "commands.unregister" ||
           path == "scripts.register_function" || path == "scripts.list_functions" || path == "scripts.call_function" ||
           path == "scripts.unregister_function" || path == "scripts.list_hooks" || path == "scripts.call" ||
           path == "scripts.override" ||
           path == "skills.list" || path == "skills.get" || path == "skills.register" ||
           path == "skills.unlock" || path == "skills.set_rank" ||
           path == "debug.profile";
}

namespace {

std::string UnsupportedReason(std::string_view game_id, const ApiOperationSpec &operation) {
    const std::string game = game_id == "chronicle-two" ? "Chronicle Two" : "Chronicle";
    if (operation.domain == "photos") {
        return "Chronicle has no photography, idea, or scoop system; this domain is bound only in Chronicle Two.";
    }
    if (operation.domain == "inventions") {
        return "Chronicle has no photo-invention system; this domain is bound only in Chronicle Two.";
    }
    if (operation.domain == "player") {
        if (game_id == "chronicle") {
            if (operation.operation == "ridepod_get" || operation.operation == "ridepod_set") {
                return "Chronicle has no ridepod; this operation is bound only in Chronicle Two.";
            }
            if (operation.operation == "transform_get" || operation.operation == "transform_set") {
                return "Chronicle has no monster-badge transformation system; this operation is bound only in Chronicle Two.";
            }
        } else if (operation.operation == "thirst_get" || operation.operation == "thirst_set") {
            return "Chronicle Two has no per-character water gauge; its thirst ailment is exposed through player.status_add/remove.";
        } else if (operation.operation == "tuning_set" || operation.operation == "tuning_clear" ||
                   operation.operation == "invincible") {
            return "Chronicle Two has no player tuning or invincibility binding yet; these operations are bound only in Chronicle.";
        }
    }
    if (operation.domain == "locations") {
        return game + " does not yet load mod-authored maps or their transition metadata.";
    }
    if (operation.domain == "dungeons") {
        if (game_id == "chronicle-two") {
            if (operation.operation == "back_floor_get" || operation.operation == "back_floor_enter") {
                return "Chronicle Two has no Dark Cloud back-floor state; back-floor entry is bound only in Chronicle.";
            }
            return "Chronicle Two can append floors to existing dungeons, but does not yet register complete new dungeon graphs.";
        }
        if (operation.operation == "spheda_get" || operation.operation == "spheda_spawn") {
            return "Chronicle has no Spheda minigame; Spheda operations are bound only in Chronicle Two.";
        }
        return "Chronicle can append procedural floors to native dungeons 0-4, but does not yet register standalone maps, custom geometry, or authored room assets.";
    }
    if (operation.domain == "georama") {
        if (game_id == "chronicle-two") {
            if (operation.path == "georama.atla_list" || operation.path == "georama.atla_collect" || operation.path == "georama.atla_spawn") {
                return "Chronicle Two has no Atla spheres; Atla collection is bound only in Chronicle.";
            }
            if (operation.path == "georama.satisfaction_get") {
                return "Chronicle Two Georama progression is exposed through its native request, condition, and river APIs; the Chronicle satisfaction view is not bound.";
            }
            if (operation.path == "georama.element_set") {
                return "Chronicle Two's Georama pieces do not use Chronicle's per-plot optional-element table.";
            }
            if (operation.operation == "request_set") {
                return "Chronicle Two can reveal existing requests and change analysis conditions, but request completion rewards and linked native progression are not bound yet.";
            }
            return "This Chronicle Two Georama operation has no verified native binding yet.";
        }
        return "Chronicle does not expose separate saved analysis-condition or river-grid state; its Georama APIs operate on native plot elements, requests, and placed pieces.";
    }
    if (operation.domain == "entities") {
        if (game_id == "chronicle-two") {
            return "Chronicle Two supports live dungeon-monster handles and can despawn enemies created by characters.spawn; NPC and world-object spawning are not bound yet.";
        }
        return "Chronicle supports generation-checked enemy handles and owner-only despawn for mod-spawned non-mimics; NPC and world-object spawning are not bound yet.";
    }
    if (operation.domain == "combat") {
        if (game_id == "chronicle-two") {
            if (operation.path.starts_with("combat.styles_")) {
                return "Fighting-style work is deferred while authored model rigs and native move-animation bindings remain out of scope.";
            }
            if (operation.path == "combat.register_formula") {
                return "This Chronicle Two build has no combat-formula registry or native damage hook.";
            }
            return "This Chronicle Two combat operation has no verified native binding.";
        }
        if (operation.path == "combat.register_formula") {
            return "This Chronicle build has no combat-formula registry or native damage hook.";
        }
        return "Fighting-style work is deferred while authored model rigs and native move-animation bindings remain out of scope.";
    }
    if (operation.domain == "characters") {
        if (game_id == "chronicle-two" && operation.operation == "spawn") {
            return "Chronicle Two can spawn registered enemies only while an active dungeon manager and inherited game assets are available.";
        }
        if (game_id == "chronicle-two" && operation.operation == "register") {
            return "Chronicle Two can register data-defined enemy variants that inherit a native model and AI script.";
        }
        return "Playable-character records are readable, but mod-defined characters and spawn routines are not bound yet.";
    }
    if (operation.domain == "models") {
        return "The port has no package-backed native model loader binding for this operation yet.";
    }
    if (operation.domain == "animations") {
        return "The port has no validated native rig and animation binding for this operation yet.";
    }
    if (operation.domain == "scripts") {
        return "The active scene event-program hook is supported; this operation does not target any other native script VM.";
    }
    if (operation.domain == "content") {
        return "Typed item and weapon paths exist, but the generic content registry is not bound to native game tables yet.";
    }
    if (operation.domain == "render") {
        return "Bounded follow-camera orbit changes are available; owned render callbacks and draw layers are not bound.";
    }
    if (operation.domain == "physics") {
        if (game_id == "chronicle-two") {
            return "Chronicle Two exposes live-monster velocity reads and writes plus bounded sphere queries over live monster actor origins; native collider and world-geometry tests are not exposed.";
        }
        if (operation.path == "physics.query") {
            return "Chronicle can query live monster actor origins inside a bounded sphere; native collider and world-geometry tests are not exposed.";
        }
        if (operation.path == "physics.velocity_set") {
            return "Chronicle writes native enemy movement intent; the next AI or movement step may replace it.";
        }
        return "Chronicle exposes live enemy movement intent through physics.velocity_get; collision geometry queries are not exposed.";
    }
    if (operation.domain == "weapons" && operation.operation == "upgrade") {
        return game + " weapon upgrade progression is not yet bound to the mod API.";
    }
    if (operation.path == "weapons.synthesize" && game_id == "chronicle-two") {
        return "Chronicle Two uses weapons.spectrumize to spectrumize weapons and items into synthesized attachments.";
    }
    if (operation.path == "weapons.spectrumize" && game_id == "chronicle") {
        return "Chronicle uses weapons.synthesize to status-break level 5+ weapons into SynthSphere attachments.";
    }
    if (operation.path == "weapons.attachment_remove" && game_id == "chronicle-two") {
        return "Chronicle Two synthesizes attachments permanently into weapons and does not have removable attachment slots.";
    }
    if (operation.domain == "party" && game_id == "chronicle") {
        if (operation.path == "party.recruit") {
            return "Chronicle has a story-ordered playable cast but no separate NPC companion recruitment routine.";
        }
        return "Chronicle native party switching is available only during safe active-dungeon play.";
    }
    return game + " has no tested native binding for " + std::string(operation.path) + " yet.";
}

} // namespace

void CompleteApiCapabilityLedger(std::string_view game_id, std::vector<ApiCapability> &capabilities) {
    for (const ApiOperationSpec &operation : ApiCatalog()) {
        if (IsCoreApiOperation(operation.path)) continue;
        const auto found = std::ranges::find(capabilities, operation.path, &ApiCapability::path);
        if (found != capabilities.end()) continue;
        capabilities.push_back({std::string(operation.path), false, UnsupportedReason(game_id, operation)});
    }
}

std::string_view ApiCallStatusName(ApiCallStatus status) {
    switch (status) {
        case ApiCallStatus::Ok: return "ok";
        case ApiCallStatus::Unsupported: return "unsupported";
        case ApiCallStatus::InvalidArgument: return "invalid_argument";
        case ApiCallStatus::PermissionDenied: return "permission_denied";
        case ApiCallStatus::PhaseViolation: return "phase_violation";
        case ApiCallStatus::Failed: return "failed";
    }
    return "failed";
}

bool IsKnownPermission(std::string_view permission) {
    return std::ranges::find(kKnownPermissions, permission) != kKnownPermissions.end();
}

} // namespace moonmod

if(NOT DEFINED EXE OR NOT EXISTS "${EXE}")
    message(FATAL_ERROR "Chronicle executable is missing: ${EXE}")
endif()
if(NOT DEFINED ROOT OR NOT IS_ABSOLUTE "${ROOT}")
    message(FATAL_ERROR "ROOT must be an absolute smoke-fixture path")
endif()

set(save_root "${ROOT}/save")
set(data_root "${ROOT}/data")
set(mod_root "${save_root}/mods/moonfactory.preflight")
file(MAKE_DIRECTORY "${mod_root}" "${mod_root}/resources" "${save_root}/moonmod" "${data_root}")
file(WRITE "${data_root}/fixture.dat" "MoonFactory headless preflight fixture\n")
file(WRITE "${mod_root}/resources/probe.chr" "MoonFactory model binding preflight fixture\n")
file(WRITE "${mod_root}/mod.json" [=[
{"schema_version":1,"id":"moonfactory.preflight","name":"MoonFactory Preflight","version":"1.0.0","author":"MoonFactory","description":"Executable runtime preflight probe.","framework_api":1,"games":["chronicle"],"entry":"main.lua","permissions":["game.read","game.write","content.register","debug.inspect"]}
]=])
file(WRITE "${mod_root}/main.lua" [=[
local mod = {}
function mod.on_load()
  local operations = assert(moonmod.api.list())
  assert(#operations >= 128)
  for _, operation in ipairs(operations) do
    assert(type(operation.path) == "string" and operation.path ~= "")
    assert(operation.status == "supported" or operation.status == "unsupported")
    assert(operation.supported == (operation.status == "supported"))
    assert(type(operation.reason) == "string")
    if not operation.supported then assert(operation.reason ~= "") end
    moonmod.log("info", "MOONFACTORY_PREFLIGHT_API|" .. operation.path .. "|" .. operation.status .. "|" .. operation.reason)
  end
  for _, path in ipairs({"scripts.register_function", "scripts.list_functions", "scripts.call_function", "scripts.unregister_function"}) do
    local supported, reason = moonmod.api.supports(path)
    assert(supported and reason == "", path)
  end
  assert(#assert(game.scripts.list_functions()).functions == 0)
  local exported = assert(game.scripts.register_function({
    id = "preflight.double", description = "Chronicle executable function probe"
  }, function(arguments) return arguments.value * 2 end))
  assert(exported.id == "moonfactory.preflight:preflight.double" and exported.exposed)
  local functions = assert(game.scripts.list_functions())
  assert(#functions.functions == 1)
  assert(functions.functions[1].id == exported.id and functions.functions[1].owner_mod_id == "moonfactory.preflight")
  assert(assert(game.scripts.call_function({id = "preflight.double", arguments = {value = 21}})) == 42)
  assert(assert(game.scripts.unregister_function({id = "preflight.double"})).removed)
  local removed, removed_error = game.scripts.call_function({id = "preflight.double"})
  assert(removed == nil and removed_error.code == "failed")
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_SCRIPT_FUNCTIONS_OK")
  for _, path in ipairs({"skills.list", "skills.get", "skills.register", "skills.unlock", "skills.set_rank"}) do
    local supported, reason = moonmod.api.supports(path)
    assert(supported and reason == "", path)
  end
  local previous_focus = game.skills.get({id = "preflight.focus"})
  local previous_power = game.skills.get({id = "preflight.power"})
  if previous_power ~= nil then assert(game.skills.set_rank({id = "preflight.power", rank = 0})) end
  if previous_focus ~= nil then assert(game.skills.set_rank({id = "preflight.focus", rank = 0})) end
  local tree = assert(game.skills.register({
    id = "preflight",
    title = "Preflight Skills",
    skills = {
      {id = "focus", title = "Focus", max_rank = 2},
      {id = "power", title = "Power", max_rank = 1,
       prerequisites = {{id = "focus", rank = 2}}}
    }
  }))
  assert(tree.id == "moonfactory.preflight:preflight" and #tree.skills == 2)
  local locked_power = assert(game.skills.get({id = "preflight.power"}))
  assert(locked_power.rank == 0 and not locked_power.available and not locked_power.prerequisites_met)
  local blocked, blocked_error = game.skills.unlock({id = "preflight.power"})
  assert(blocked == nil and blocked_error.code == "failed")
  assert(assert(game.skills.unlock({id = "preflight.focus"})).rank == 1)
  assert(assert(game.skills.unlock({id = "preflight.focus"})).rank == 2)
  assert(assert(game.skills.unlock({id = "preflight.power"})).rank == 1)
  assert(assert(game.skills.set_rank({id = "preflight.focus", rank = 1})).rank == 1)
  local skill_list = assert(game.skills.list())
  local tree_skill_count = 0
  local power
  for _, skill in ipairs(skill_list.skills) do
    if skill.tree_id == tree.id then
      tree_skill_count = tree_skill_count + 1
      if skill.id_local == "power" then power = skill end
    end
  end
  assert(tree_skill_count == 2 and power and power.rank == 1 and not power.prerequisites_met)
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_SKILLS_OK|" .. tree_skill_count)
  local heal_supported, heal_reason = moonmod.api.supports("combat.heal")
  assert(heal_supported and heal_reason == "")
  local healed, heal_error = game.combat.heal({entity = "chronicle:monster:0:1", amount = 1})
  assert(healed == nil and heal_error.code == "failed")
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_COMBAT_HEAL_NO_DUNGEON")
  local model_supported, model_reason = moonmod.api.supports("models.resolve")
  assert(model_supported and model_reason == "")
  local model = assert(game.models.resolve({monster_id = 0}))
  assert(model.model == "e01a" and model.source == "native_table")
  assert(model.assets[1].game_path == "dun/monstor/e01a.chr" and not model.assets[1].overridden)
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_MODEL_RESOLVE_OK")
  local model_asset = assert(game.resources.override({
    path = "resources/probe.chr", game_path = "dun/monstor/moon_preflight.chr"
  }))
  assert(model_asset.status == "registered")
  local model_binding = assert(game.models.register({
    id = "preflight-model", template_monster_id = 0, model = "moon_preflight"
  }))
  local removed_model_binding = assert(game.models.unregister({id = "preflight-model"}))
  assert(removed_model_binding.removed and removed_model_binding.id == model_binding.id)
  local missing_model_binding, missing_model_error = game.models.resolve({id = model_binding.id})
  assert(missing_model_binding == nil and missing_model_error ~= nil)
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_MODEL_UNREGISTER_OK")
  local unregister_supported, unregister_reason = moonmod.api.supports("content.unregister")
  assert(unregister_supported and unregister_reason == "")
  local temporary_character = assert(game.content.register({
    type = "character",
    definition = {id = "unregister-probe", kind = "enemy", name = "Unregister Probe", template_monster_id = 0}
  }))
  local registered_content = assert(game.content.list())
  local found_temporary = false
  for _, entry in ipairs(registered_content.entries) do
    if entry.id == temporary_character.id then found_temporary = true end
  end
  assert(found_temporary and temporary_character.native_monster_id >= 1024)
  local unregistered_character = assert(game.content.unregister({type = "character", id = "unregister-probe"}))
  assert(unregistered_character.removed and unregistered_character.id == temporary_character.id)
  local removed_content = assert(game.content.list())
  for _, entry in ipairs(removed_content.entries) do assert(entry.id ~= temporary_character.id) end
  local missing_character, missing_character_error = game.models.resolve({monster_id = temporary_character.native_monster_id})
  assert(missing_character == nil and missing_character_error ~= nil)
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_CONTENT_UNREGISTER_OK")
  local dungeons = assert(game.dungeons.list())
  assert(#dungeons == 7)
  assert(dungeons[1].id == "chronicle:dungeon:divine-beast-cave")
  assert(dungeons[7].id == "chronicle:dungeon:demon-shaft" and dungeons[7].floor_count == 100)
  local demon_shaft = assert(game.dungeons.get("demon-shaft"))
  assert(demon_shaft.status == "native" and demon_shaft.deepest_floor_reached == nil)
  local dungeon_supported, dungeon_reason = moonmod.api.supports("dungeons.register")
  assert(dungeon_supported and dungeon_reason == "")
  local procedural_extension = assert(game.dungeons.register({
    id = "headless-vault", name = "Headless Vault", dungeon = 0,
    floors = {
      {id = "annex", name = "Headless Vault Annex", source_floor = 2},
      {id = "mirror", source_floor = "annex"}
    }
  }))
  assert(procedural_extension.floor_count == 17 and procedural_extension.mod_floor_count == 2)
  assert(procedural_extension.floors[1].native_floor_id == 15)
  assert(procedural_extension.floors[2].native_floor_id == 16)
  local alias_supported, alias_reason = moonmod.api.supports("world.register_dungeon")
  assert(alias_supported and alias_reason == "")
  local alias_extension = assert(game.world.register_dungeon({
    id = "headless-owl-annex", name = "Headless Owl Annex", dungeon = 1,
    floors = {{id = "annex", source_floor = 3}}
  }))
  assert(alias_extension.floor_count == 18 and alias_extension.floors[1].native_floor_id == 17)
  local resolved_extension = assert(game.dungeons.get("headless-vault"))
  assert(resolved_extension.id == "moonfactory.preflight:headless-vault")
  dungeons = assert(game.dungeons.list())
  assert(#dungeons == 9)
  assert(dungeons[1].floor_count == 17 and dungeons[2].floor_count == 18)
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_DUNGEONS_OK|" .. #dungeons)
  local locations_supported, locations_reason = moonmod.api.supports("locations.list")
  assert(locations_supported and locations_reason == "")
  local locations, locations_error = game.locations.list()
  assert(locations == nil and locations_error.code == "failed")
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_LOCATIONS_NO_SAVE")
  local transition_supported, transition_reason = moonmod.api.supports("world.transition")
  assert(transition_supported and transition_reason == "")
  local transition, transition_error = game.world.transition({id = "chronicle:location:norune-village"})
  assert(transition == nil and transition_error.code == "failed")
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_WORLD_TRANSITION_NO_SAVE")
  local player_supported, player_reason = moonmod.api.supports("player.set")
  assert(player_supported and player_reason == "")
  local player_state, player_error = game.player.set({water = 50})
  assert(player_state == nil and player_error.code == "failed")
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_PLAYER_WATER_NO_SAVE")
  local party_supported, party_reason = moonmod.api.supports("party.set_active")
  assert(party_supported and party_reason == "")
  local party_state, party_error = game.party.set_active({id = "toan"})
  assert(party_state == nil and party_error.code == "failed")
  assert(string.find(party_error.message, "no active save data", 1, true))
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_PARTY_SWITCH_NO_SAVE")
  local equip_supported, equip_reason = moonmod.api.supports("weapons.equip")
  assert(equip_supported and equip_reason == "")
  local equipped, equip_error = game.weapons.equip({id = "chronicle:weapon:258", character = "toan"})
  assert(equipped == nil and equip_error.code == "failed")
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_WEAPON_EQUIP_NO_SAVE")
  local inspect_supported, inspect_reason = moonmod.api.supports("debug.inspect")
  assert(inspect_supported and inspect_reason == "")
  local runtime_snapshot = assert(game.debug.inspect())
  assert(type(runtime_snapshot.scene_id) == "string" and type(runtime_snapshot.native_mode) == "number")
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_DEBUG_INSPECT_OK")
  moonmod.log("info", "MOONFACTORY_PREFLIGHT_CATALOG_OK|" .. #operations)
end
return mod
]=])
file(WRITE "${save_root}/moonmod/profile.json" [=[
{"format_version":1,"enabled":{"moonfactory.preflight":true},"settings":{}}
]=])

execute_process(
    COMMAND "${EXE}" --data "${data_root}" --save "${save_root}" --offscreen --frames 1
    RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 120)
set(log "${out}${err}")
file(WRITE "${ROOT}/preflight.log" "${log}")

string(FIND "${log}" "MOONFACTORY_PREFLIGHT_CATALOG_OK|" catalog_marker_at)
string(FIND "${log}" "MOONFACTORY_PREFLIGHT_SCRIPT_FUNCTIONS_OK" script_functions_marker_at)
string(FIND "${log}" "MOONFACTORY_PREFLIGHT_SKILLS_OK|2" skills_marker_at)
string(FIND "${log}" "MOONFACTORY_PREFLIGHT_COMBAT_HEAL_NO_DUNGEON" combat_marker_at)
string(FIND "${log}" "MOONFACTORY_PREFLIGHT_MODEL_RESOLVE_OK" model_marker_at)
string(FIND "${log}" "MOONFACTORY_PREFLIGHT_MODEL_UNREGISTER_OK" model_unregister_marker_at)
string(FIND "${log}" "MOONFACTORY_PREFLIGHT_CONTENT_UNREGISTER_OK" content_unregister_marker_at)
string(FIND "${log}" "MOONFACTORY_PREFLIGHT_DUNGEONS_OK|9" dungeon_marker_at)
string(FIND "${log}" "MOONFACTORY_PREFLIGHT_LOCATIONS_NO_SAVE" locations_marker_at)
string(FIND "${log}" "MOONFACTORY_PREFLIGHT_WORLD_TRANSITION_NO_SAVE" transition_marker_at)
string(FIND "${log}" "MOONFACTORY_PREFLIGHT_PLAYER_WATER_NO_SAVE" player_water_marker_at)
string(FIND "${log}" "MOONFACTORY_PREFLIGHT_PARTY_SWITCH_NO_SAVE" party_switch_marker_at)
string(FIND "${log}" "MOONFACTORY_PREFLIGHT_WEAPON_EQUIP_NO_SAVE" weapon_equip_marker_at)
string(FIND "${log}" "MOONFACTORY_PREFLIGHT_DEBUG_INSPECT_OK" debug_inspect_marker_at)
string(FIND "${log}" "moonmod[error]" mod_error_at)
string(FIND "${log}" "File open error \"meswin/systeme.bin\"" missing_asset_at)
string(REPLACE "\r\n" "\n" normalized_log "${log}")
string(REPLACE "\n" ";" log_lines "${normalized_log}")
set(api_paths "")
set(api_count_marker "")
foreach(log_line IN LISTS log_lines)
    string(FIND "${log_line}" "MOONFACTORY_PREFLIGHT_API|" api_marker_at)
    if(api_marker_at GREATER -1)
        string(LENGTH "MOONFACTORY_PREFLIGHT_API|" api_marker_length)
        math(EXPR api_payload_at "${api_marker_at} + ${api_marker_length}")
        string(SUBSTRING "${log_line}" ${api_payload_at} -1 api_payload)
        string(FIND "${api_payload}" "|" path_delimiter_at)
        if(path_delimiter_at LESS 1)
            message(FATAL_ERROR "Malformed Chronicle API preflight record: ${log_line}\n${log}")
        endif()
        string(SUBSTRING "${api_payload}" 0 ${path_delimiter_at} api_path)
        list(FIND api_paths "${api_path}" duplicate_path_at)
        if(duplicate_path_at GREATER -1)
            message(FATAL_ERROR "Duplicate Chronicle API preflight record for ${api_path}\n${log}")
        endif()
        list(APPEND api_paths "${api_path}")
    endif()
    string(FIND "${log_line}" "MOONFACTORY_PREFLIGHT_CATALOG_OK|" count_marker_at)
    if(count_marker_at GREATER -1)
        string(LENGTH "MOONFACTORY_PREFLIGHT_CATALOG_OK|" count_marker_length)
        math(EXPR count_payload_at "${count_marker_at} + ${count_marker_length}")
        string(SUBSTRING "${log_line}" ${count_payload_at} -1 api_count_marker)
    endif()
endforeach()
list(LENGTH api_paths api_record_count)
if(catalog_marker_at LESS 0 OR script_functions_marker_at LESS 0 OR skills_marker_at LESS 0 OR combat_marker_at LESS 0 OR model_marker_at LESS 0 OR model_unregister_marker_at LESS 0 OR content_unregister_marker_at LESS 0 OR dungeon_marker_at LESS 0 OR locations_marker_at LESS 0 OR transition_marker_at LESS 0 OR player_water_marker_at LESS 0 OR party_switch_marker_at LESS 0 OR weapon_equip_marker_at LESS 0 OR debug_inspect_marker_at LESS 0 OR mod_error_at GREATER -1 OR
   missing_asset_at LESS 0 OR NOT rc EQUAL 4 OR NOT api_count_marker STREQUAL "${api_record_count}" OR
   api_record_count LESS 128)
    message(FATAL_ERROR "Chronicle runtime preflight did not pass. Exit: ${rc}; API records: ${api_record_count}/${api_count_marker}\n${log}")
endif()
message("MOONFACTORY_PREFLIGHT_OK|${api_record_count}; retail game boot waits for extracted data")

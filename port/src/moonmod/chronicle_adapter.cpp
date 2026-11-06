#include "doorway_view.hpp"
#include "moonmod/messages_moonmod.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/symlinks.hpp"
#include "moonmod/game_hooks.hpp"
#include "moonmod/choice_panel.hpp"
#include "moonmod/map_screen.hpp"
#include "moonmod/sky_screen.hpp"
#include "moonmod/run_menu.hpp"
#include "game/dungeoneventman.hpp"
#include "game/mod_atla.hpp"
#include "game/randomitem.hpp"
#include "moonmod/dungeon_npc.hpp"
#include "healeffect.hpp"
#include "hit_machingun_effect.hpp"
#include "hitmark.hpp"
#include "itembombeffect.hpp"
#include "moonmod/companion.hpp"
#include "moonmod/ghost.hpp"
#include "moonmod/coop_avatars.hpp"
#include "moonmod/net_session.hpp"
#include "moonmod/coop_save.hpp"
#include "moonmod/audio_api.hpp"
#include "moonmod/combat_formula.hpp"
#include "moonmod/native_resource_overrides.hpp"
#include "moonmod/runtime.hpp"
#include "moonmod/target_hud.hpp"
#include "moonmod/title_modes.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cctype>
#include <cmath>
#include <sstream>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

#include "itemdata.hpp"
#include "charm_slots.hpp"
#include "gear.hpp"
#include "dodge_run.hpp"
#include "gauge_rules.hpp"
#include "item_icon_override.hpp"
#include "data/data_read.hpp"
#include "userstatus.hpp"
#include "menu_inventory.hpp"
#include "menu_misc.hpp"
#include "btitem.hpp"
#include "mainitemmodel.hpp"
#include "btmisc.hpp"
#include "btactstatus.hpp"
#include "battlemenu.hpp"
#include "savedata.hpp"
#include "main.hpp"
#include "mainselect.hpp"
#include "gamemode.hpp"
#include "app/gameloop.hpp"
#include "editloop.hpp"
#include "editmapscript.hpp"
#include "editground.hpp"
#include "editarea.hpp"
#include "editpartsinfo.hpp"
#include "mapparts.hpp"
#include "dun/gameloop.hpp"
#include "dungeonmap.hpp"
#include "dungeonparts.hpp"
#include "dungeon.h"
#include "language.h"
#include "monstorunit.hpp"
#include "runscript.hpp"
#include "snd.hpp"
#include "localize/localize.hpp"
#include "weaponelement.hpp"
#include "camerafollow.hpp"
#include "collisiondata.hpp"
#include "gamepad.hpp"
#include "motionmodel.hpp"
#include "weaponeffect.hpp"
#include "platform/input.hpp"
#include "shop.hpp"
#include "save/save_slots.hpp"
#include "shot_effect_pack.hpp"
#include "shot_freefuncs.hpp"

#include "editloop3.hpp"
#include "dataread.hpp"
#include "btsysscript.hpp"
#include "weaponlevelup.hpp"
#include "playable_chara.hpp"
namespace dc::game {}
using namespace dc::game;
using namespace dc::app;
using namespace dc::localize;
using namespace dc::save;

namespace moonmod {

void StartTeleportLight(); // a teleport arrival's light; ChronicleZoneGatesTick plays it
extern int g_teleport_in;
int ChronicleNativeMonsterModelId(dc::game::CMonstorUnit *unit, int slot);
ApiCallResult SetHitFeel(std::string_view owner, const Json &args); // combat.feel_set, below
ApiCallResult ClearHitFeel(std::string_view owner);
namespace {

ApiCallResult Invalid(std::string message);
bool ReadBoundedInt(const Json &object, const char *key, int minimum, int maximum, int &out);

CombatDamageFormulaRegistry g_combat_damage_formulas;
int           g_native_block_mask = 0;
std::uint64_t g_native_block_until = 0;
CombatDamageFormulaRegistry g_player_damage_formulas;
std::map<std::string, Json> g_floor_badges;
std::vector<std::pair<std::string, std::array<int,4>>> g_layout_requests;
void RestoreLayoutRequest() {
    const auto a = g_layout_requests.empty() ? std::array<int,4>{} : g_layout_requests.back().second;
    DngLayoutRooms=a[0]; DngLayoutArea=a[1]; DngLayoutRoomMin=a[2]; DngLayoutRoomMax=a[3];
}
std::map<std::string, std::array<int, 4>> g_floor_modifiers;
std::vector<std::pair<std::string, std::array<double, 4>>> g_population_requests;
std::vector<std::pair<std::string, std::array<float, 2>>> g_town_camera_requests;
std::vector<std::pair<std::string, ChroniclePlayerTuning>> g_player_tuning_requests;
ChroniclePlayerTuning g_player_tuning;
void RefreshPlayerTuning() {
    ChroniclePlayerTuning combined;
    for (const auto &[owner, tuning] : g_player_tuning_requests) {
        combined.run_speed *= tuning.run_speed;
        combined.attack_anim_speed *= tuning.attack_anim_speed;
        combined.invincible_bonus_frames = std::min(120, combined.invincible_bonus_frames + tuning.invincible_bonus_frames);
        combined.recoil_scale *= tuning.recoil_scale;
        combined.gauge_refill *= tuning.gauge_refill;
        combined.thirst_drain *= tuning.thirst_drain;
        combined.durability_wear *= tuning.durability_wear;
        combined.map_reveal = combined.map_reveal || tuning.map_reveal;
        combined.monster_reveal = combined.monster_reveal || tuning.monster_reveal;
        combined.map_hidden = combined.map_hidden || tuning.map_hidden;
        combined.map_window = combined.map_window || tuning.map_window;
        combined.status_duration *= tuning.status_duration;
        combined.status_immune |= tuning.status_immune;
        combined.swap_delay *= tuning.swap_delay;
        combined.dodge_power = std::max(combined.dodge_power, tuning.dodge_power);
        combined.dodge_invincible_frames = std::max(combined.dodge_invincible_frames, tuning.dodge_invincible_frames);
        combined.dodge_gauge = std::max(combined.dodge_gauge, tuning.dodge_gauge);
        combined.dodge_hop = combined.dodge_hop || tuning.dodge_hop;
        combined.dodge_run = std::max(combined.dodge_run, tuning.dodge_run);
        combined.lightness = combined.lightness || tuning.lightness;
        for (std::size_t c = 0; c < combined.stamina.size(); ++c) {
            combined.stamina[c] = std::max(combined.stamina[c], tuning.stamina[c]);
        }
        combined.extra_shots = std::min(2, combined.extra_shots + tuning.extra_shots);
        combined.shots_pierce = combined.shots_pierce || tuning.shots_pierce;
        combined.reach_scale *= tuning.reach_scale;
        combined.no_first_person = combined.no_first_person || tuning.no_first_person;
        combined.item_heal_lock = std::max(combined.item_heal_lock, tuning.item_heal_lock);
        combined.lock_flick = combined.lock_flick || tuning.lock_flick;
        combined.lock_sight = combined.lock_sight || tuning.lock_sight;
        combined.lock_camera = combined.lock_camera || tuning.lock_camera;
        combined.soft_lock = std::max(combined.soft_lock, tuning.soft_lock);
        combined.input_buffer = std::max(combined.input_buffer, tuning.input_buffer);
        // The earliest chain point any mod asks for (0, off, counts only when none asks).
        combined.chain_cancel = combined.chain_cancel == 0 || tuning.chain_cancel == 0 ? std::max(combined.chain_cancel, tuning.chain_cancel)
                                                                                      : std::min(combined.chain_cancel, tuning.chain_cancel);
    }
    combined.swap_delay = std::clamp(combined.swap_delay, 0.0f, 4.0f);
    // The combination stays inside the range one call may ask for.
    for (float *scale : {&combined.run_speed, &combined.attack_anim_speed, &combined.recoil_scale,
                         &combined.gauge_refill, &combined.thirst_drain, &combined.durability_wear, &combined.status_duration,
                         &combined.reach_scale}) {
        *scale = std::clamp(*scale, 0.25f, 4.0f);
    }
    g_player_tuning = combined;
}
std::vector<std::pair<std::string, int>> g_wheel_mode_requests;
int g_town_character = CHARA_TOAN;
int g_town_loaded_character = CHARA_TOAN;
bool g_town_character_reload = false;
std::vector<std::pair<std::string, int>> g_enemy_survival_requests;
std::array<std::uint32_t, MONSTOR_SLOT_MAX> g_enemy_survival_attempted_generation{};
CMonstorUnit *g_enemy_survival_unit = nullptr;
void RestorePopulationRequest() {
    DngMonstersPerRoomMin = DngMonstersPerRoomMax = 0.0;
    DngChestsPerRoomMin = DngChestsPerRoomMax = 0.0;
    if (g_population_requests.empty()) return;
    const auto &values = g_population_requests.back().second;
    DngMonstersPerRoomMin = values[0]; DngMonstersPerRoomMax = values[1];
    DngChestsPerRoomMin = values[2]; DngChestsPerRoomMax = values[3];
}
void RestoreEnemySurvivalRequest() {
    DngEnemySurvivalChance = g_enemy_survival_requests.empty() ? 0 : g_enemy_survival_requests.back().second;
}
std::map<CDungeonMap *, std::array<int, DNG_BOX_MAX>> g_chest_value_rewards;
void RefreshFloorModifiers() {
    std::array<int, 4> total{};
    for (const auto &[owner, values] : g_floor_modifiers)
        for (int i = 0; i < 4; ++i) total[i] = std::clamp(total[i] + values[i], i == 2 ? -100 : 0, 300);
    DngRoomBonusPercent = total[0]; DngMonsterBonusPercent = total[1];
    DngChestBonusPercent = total[2]; DngChestValuePercent = total[3];
}
ApiCallResult SetFloorModifiers(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return {ApiCallStatus::InvalidArgument, nullptr, "expected percentage object"};
    const std::array<const char *,4> names = {"rooms", "monsters", "chests", "chest_value"};
    std::array<int,4> values{};
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        const auto found = std::find(names.begin(), names.end(), field.key());
        if (found == names.end() || !field.value().is_number_integer()) return {ApiCallStatus::InvalidArgument, nullptr, "unknown field or noninteger percentage"};
        const auto number = field.value().get<std::int64_t>();
        // chests alone may go below the retail count, down to -100 (no regular chests)
        const int lowest = std::string_view(field.key()) == "chests" ? -100 : 0;
        if (number < lowest || number > 300) {
            return {ApiCallStatus::InvalidArgument, nullptr, lowest < 0 ? "chests must be -100..300" : "percentage must be 0..300"};
        }
        values[found - names.begin()] = static_cast<int>(number);
    }
    g_floor_modifiers[std::string(owner)] = values;
    RefreshFloorModifiers();
    return {ApiCallStatus::Ok, {{"rooms", DngRoomBonusPercent}, {"monsters", DngMonsterBonusPercent},
                              {"chests", DngChestBonusPercent}, {"chest_value", DngChestValuePercent}}, {}};
}

ApiCallResult SetPopulationScaling(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {monsters_per_room = {min = 1.5, max = 3}, chests_per_room = {min = 0.75, max = 1.5}}, or {} to clear this mod's scaling");
    }
    const Json &request = args[0];
    if (request.empty()) {
        std::erase_if(g_population_requests, [owner](const auto &entry) { return entry.first == owner; });
        RestorePopulationRequest();
        return {ApiCallStatus::Ok, {{"monsters_per_room", {{"min", DngMonstersPerRoomMin}, {"max", DngMonstersPerRoomMax}}},
                                    {"chests_per_room", {{"min", DngChestsPerRoomMin}, {"max", DngChestsPerRoomMax}}}}, {}};
    }
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "monsters_per_room" && field.key() != "chests_per_room")
            return Invalid("unsupported population scaling field '" + field.key() + "'");
    }
    auto read_range = [&request](const char *key, double minimum, double maximum, double defaults[2]) -> bool {
        if (!request.contains(key)) return true;
        const Json &range = request[key];
        if (!range.is_object() || range.size() != 2 || !range.contains("min") || !range.contains("max") ||
            !range["min"].is_number() || !range["max"].is_number()) return false;
        defaults[0] = range["min"].get<double>();
        defaults[1] = range["max"].get<double>();
        return std::isfinite(defaults[0]) && std::isfinite(defaults[1]) &&
            defaults[0] >= minimum && defaults[1] <= maximum && defaults[0] <= defaults[1];
    };
    double monsters[2] = {0.0, 0.0}, chests[2] = {0.0, 0.0};
    if (!read_range("monsters_per_room", 0.0, 3.0, monsters))
        return Invalid("monsters_per_room requires finite min/max values with 0 <= min <= max <= 3");
    if (!read_range("chests_per_room", 0.0, 1.5, chests))
        return Invalid("chests_per_room requires finite min/max values with 0 <= min <= max <= 1.5");
    std::erase_if(g_population_requests, [owner](const auto &entry) { return entry.first == owner; });
    g_population_requests.push_back({std::string(owner), {monsters[0], monsters[1], chests[0], chests[1]}});
    RestorePopulationRequest();
    return {ApiCallStatus::Ok, {{"monsters_per_room", {{"min", DngMonstersPerRoomMin}, {"max", DngMonstersPerRoomMax}}},
                                {"chests_per_room", {{"min", DngChestsPerRoomMin}, {"max", DngChestsPerRoomMax}}}}, {}};
}

ApiCallResult SetEnemySurvivalChance(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {chance_percent = 0..100}, or {} to clear this mod's enemy survival setting");
    }
    if (args[0].empty()) {
        std::erase_if(g_enemy_survival_requests, [owner](const auto &entry) { return entry.first == owner; });
        RestoreEnemySurvivalRequest();
        return {ApiCallStatus::Ok, {{"chance_percent", DngEnemySurvivalChance}}, {}};
    }
    int chance = 0;
    if (args[0].size() != 1 || !ReadBoundedInt(args[0], "chance_percent", 0, 100, chance)) {
        return Invalid("chance_percent must be an integer from 0 to 100");
    }
    std::erase_if(g_enemy_survival_requests, [owner](const auto &entry) { return entry.first == owner; });
    g_enemy_survival_requests.push_back({std::string(owner), chance});
    RestoreEnemySurvivalRequest();
    return {ApiCallStatus::Ok, {{"chance_percent", DngEnemySurvivalChance}}, {}};
}

std::string CurrentLanguageTag() {
    switch (LanguageCode) {
        case LANG_JAPANESE: return "ja";
        case LANG_ENGLISH_US: return "en-US";
        case LANG_ENGLISH_UK: return "en-GB";
        case LANG_FRENCH: return "fr";
        case LANG_GERMAN: return "de";
        case LANG_ITALIAN: return "it";
        case LANG_SPANISH: return "es";
        default: return "en";
    }
}

struct InputContextFrame {
    std::string token;
    std::string owner;
    std::string id;
};

struct InputContextRegistry {
    std::vector<InputContextFrame> stack;
    std::map<std::string, std::string, std::less<>> action_contexts;
    std::uint64_t next_token = 1;

    std::string Active() const { return stack.empty() ? "default" : stack.back().id; }

    std::string ContextFor(std::string_view action) const {
        const auto found = action_contexts.find(action);
        return found == action_contexts.end() ? "default" : found->second;
    }

    bool IsActive(std::string_view action) const { return ContextFor(action) == Active(); }

    bool Push(std::string_view owner, std::string_view local_id, std::string &token, std::string &qualified_id) {
        const auto owned = std::ranges::count_if(stack, [owner](const InputContextFrame &frame) {
            return frame.owner == owner;
        });
        if (stack.size() >= 64 || owned >= 16 || next_token == 0) return false;
        token = "input-context-" + std::to_string(next_token++);
        qualified_id = std::string(owner) + ":" + std::string(local_id);
        stack.push_back({token, std::string(owner), qualified_id});
        ClearPresses();
        return true;
    }

    bool Pop(std::string_view owner, std::string_view token, std::string &removed_id) {
        const auto found = std::ranges::find_if(stack, [owner, token](const InputContextFrame &frame) {
            return frame.owner == owner && frame.token == token;
        });
        if (found == stack.end()) return false;
        removed_id = found->id;
        stack.erase(found);
        ClearPresses();
        return true;
    }

    void BindAction(std::string_view action, std::string context) { action_contexts[std::string(action)] = std::move(context); }
    void UnbindAction(std::string_view action) { action_contexts.erase(std::string(action)); }

    void UnloadOwner(std::string_view owner) {
        std::erase_if(stack, [owner](const InputContextFrame &frame) { return frame.owner == owner; });
        const std::string prefix = std::string(owner) + ":";
        auto action = action_contexts.begin();
        while (action != action_contexts.end()) {
            if (std::string_view(action->first).starts_with(prefix)) {
                while (InputModActionConsumePress(action->first)) {}
                action = action_contexts.erase(action);
            } else {
                ++action;
            }
        }
        ClearPresses();
    }

    void ClearPresses() const {
        for (const auto &[action, context] : action_contexts) {
            while (InputModActionConsumePress(action)) {}
        }
    }
};

void (*g_character_switch_routine)(int, int) = ::selectChrUnit;

using ChronicleGeoramaPlaceRoutine = int (*)(dc::game::CEditGround *, int, float, float, float, int);
using ChronicleGeoramaRemoveRoutine = int (*)(dc::game::CEditGround *, int *, int *, float, float, float);

int PlaceChronicleGeoramaPart(dc::game::CEditGround *ground, int part_id, float x, float y, float z, int rotation) {
    return ground->SetMapParts(part_id, x, y, z, rotation);
}

int RemoveChronicleGeoramaPart(dc::game::CEditGround *ground, int *out_part_id, int *out_rotation,
                               float x, float y, float z) {
    return ground->DeleteMapParts(out_part_id, out_rotation, x, y, z);
}

ChronicleGeoramaPlaceRoutine g_georama_place_routine = PlaceChronicleGeoramaPart;
ChronicleGeoramaRemoveRoutine g_georama_remove_routine = RemoveChronicleGeoramaPart;

InputContextRegistry g_input_contexts;

// MoonMod: the playable characters' API ids: the game's six, then those a mod registers (characters.register_playable,
// playable_chara.hpp), indexed like the engine's characters. A view of the registry, so it grows as they register.
struct CharacterIdView {
    struct Iterator {
        using value_type = const char *;
        using difference_type = std::ptrdiff_t;
        int index = 0;
        const char *operator*() const { return CharaIdName(index); }
        Iterator &operator++() { ++index; return *this; }
        Iterator operator++(int) { Iterator old = *this; ++index; return old; }
        bool operator==(const Iterator &) const = default;
        difference_type operator-(const Iterator &other) const { return index - other.index; }
    };
    const char *operator[](int index) const { return CharaIdName(index); }
    std::size_t size() const { return static_cast<std::size_t>(CharaCount()); }
    Iterator begin() const { return {0}; }
    Iterator end() const { return {CharaCount()}; }
};
constexpr CharacterIdView kCharacterIds;
struct NativeDungeonDefinition {
    const char *id;
    const char *name;
    int native_id;
};
constexpr std::array<NativeDungeonDefinition, DUNGEON_COUNT> kNativeDungeons = {{
    {"chronicle:dungeon:divine-beast-cave", "Divine Beast Cave", DUNGEON_DIVINE_BEAST_CAVE},
    {"chronicle:dungeon:wise-owl-forest", "Wise Owl Forest", DUNGEON_WISE_OWL_FOREST},
    {"chronicle:dungeon:shipwreck", "Shipwreck", DUNGEON_SHIPWRECK},
    {"chronicle:dungeon:sun-and-moon-temple", "Sun and Moon Temple", DUNGEON_SUN_MOON_TEMPLE},
    {"chronicle:dungeon:moon-sea", "Moon Sea", DUNGEON_MOON_SEA},
    {"chronicle:dungeon:gallery-of-time", "Gallery of Time", DUNGEON_GALLERY_OF_TIME},
    {"chronicle:dungeon:demon-shaft", "Demon Shaft", DUNGEON_DEMON_SHAFT},
}};
constexpr std::array<int, DUNGEON_COUNT> kNativeDungeonFloorCounts = {15, 17, 18, 18, 15, 25, 100};
constexpr int kChronicleMaximumRegisteredFloorCount = 25;
constexpr std::size_t kChronicleMaximumRegisteredFloorsPerDungeon = 8;
constexpr int kChronicleFirstModMapId = 70;
constexpr int kChronicleLastModMapId = 79;
struct NativeLocationDefinition {
    const char *id;
    const char *name;
    const char *kind;
};
constexpr std::array<NativeLocationDefinition, WORLD_PLACE_COUNT> kNativeLocations = {{
    {"chronicle:location:norune-village", "Norune Village", "settlement"},
    {"chronicle:location:divine-beast-cave", "Divine Beast Cave", "dungeon"},
    {"chronicle:location:matataki-village", "Matataki Village", "settlement"},
    {"chronicle:location:wise-owl-forest", "Wise Owl Forest", "dungeon"},
    {"chronicle:location:field-s04", "Field s04", "field"},
    {"chronicle:location:queens", "Queens", "settlement"},
    {"chronicle:location:shipwreck", "Shipwreck", "dungeon"},
    {"chronicle:location:muska-lacka", "Muska Lacka", "settlement"},
    {"chronicle:location:field-s32", "Field s32", "field"},
    {"chronicle:location:sun-and-moon-temple", "Sun and Moon Temple", "dungeon"},
    {"chronicle:location:field-s13", "Field s13", "field"},
    {"chronicle:location:yellow-drops", "Yellow Drops", "settlement"},
    {"chronicle:location:moon-sea", "Moon Sea", "dungeon"},
    {"chronicle:location:field-s30", "Field s30", "field"},
    {"chronicle:location:gallery-of-time", "Gallery of Time", "dungeon"},
    {"chronicle:location:demon-shaft", "Demon Shaft", "challenge"},
}};
constexpr std::array<int, 11> kChronicleModItemSlots = {143, 144, 156, 157, 158, 184, 194, 214, 215, 223, 232};
constexpr unsigned int kChronicleCureItemFlags = ITEMUSE_CURE_FREEZE | ITEMUSE_CURE_POISON |
                                                 ITEMUSE_CURE_CURSE | ITEMUSE_CURE_GOO;

struct ModItem {
    std::string id;
    std::string owner;
    std::string name;
    int template_item = ITEM_BREAD;
    std::string effect_kind = "heal";
    int effect_amount = 0;
    std::string effect_status;
    int native_item_no = -1;
    std::string icon;
};

struct ModWeapon {
    std::string id;
    std::string owner_mod;
    std::string name;
    int template_weapon = -1;
    int owner_character = -1;
    int attack = -1;
    int durability = -1;
    int attack_max = -1;
    int magic_max = -1;
    int native_item_no = -1;
    std::string model;
    std::string icon;
    std::vector<std::string> description; /**< Up to 4 lines for the weapon menu's card, in place of the template's. */
    double menu_scale = 1.0; /**< How big the weapon menu draws its model, against its holder's weapons. */
};

struct ModCharacter {
    std::string id;
    std::string owner;
    std::string name;
    std::string model;
    std::string script;
    std::string model_id;
    int template_model_no = -1;
    int asset_template_model_no = -1;
    int native_model_no = -1;
    int hp = 0;
    int defense = 0;
    int reward_exp = 0;
    int reward_money = 0;
};

struct ModModel {
    std::string id;
    std::string owner;
    std::string model;
    std::string script;
    int template_model_no = -1;
    bool custom_script = false;
};

struct ModModelReplacement {
    std::string owner;
    std::string original_model;
};

struct ModAnimation {
    std::string id;
    std::string owner;
    std::string model_id;
    int motion_index = -1;
};

struct ModAnimationPartPlayback {
    bool used = false;
    int previous_motion = -1;
    int previous_flags = 0;
    float previous_speed = -1.0f;
};

struct ModAnimationPlayback {
    std::string owner;
    std::string animation_id;
    int motion_index = -1;
    std::array<ModAnimationPartPlayback, 3> parts{};
};

struct ModDungeonFloor {
    std::string id;
    std::string local_id;
    std::string name;
    int source_floor = -1;
    int native_floor_id = -1;
    std::string map; // own floor pack: dun/<map>.cfg + dun/mpd_pack/<map>.mpd, empty = native
    bool has_enemy_override = false;
    std::vector<std::string> enemy_character_ids;
    std::vector<int> enemy_model_ids;
};

struct ModDungeon {
    std::string id;
    std::string owner;
    std::string name;
    int native_dungeon_id = -1;
    std::vector<ModDungeonFloor> floors;
};

// A town villager a mod added (characters.register kind = "npc"); native entry `index` of editloop.hpp's table.
struct ModNpc {
    std::string id;
    std::string owner;
    std::string name;
    int town_id = 0;
    std::string model;
    std::array<float, 4> position{};
    float facing_degrees = 0.0f;
    int talk_event = 0;
    int index = -1;
};

std::map<std::string, ModNpc, std::less<>> g_mod_npcs;

struct ModGeoramaPiece {
    std::string id;
    std::string owner;
    std::string name;
    int template_part_id = -1;
    int town_id = -1;
    int stock = 1;
    int native_part_id = -1;
    std::string model_scene;
    std::string model_part;
    int footprint_width = 0;
    int footprint_height = 0;
    std::vector<int> footprint_cells;
};

struct ModLocation {
    std::string id;
    std::string owner;
    std::string name;
    int native_map_id = -1;
    int source_map_id = -1;
};

struct PendingDungeonFloorEntry {
    int dungeon_id = -1;
    int floor_id = -1;
};

struct ChronicleMonsterHandleRecord {
    std::uint32_t generation = 0;
    std::string owner;
    std::string character_id;
    int native_model_no = -1;
    int pending_damage_result = static_cast<int>(MonsterDamageResult::None);
};

// The arena origin of a boss being spawned: its spawn script runs inside the native set-up, before the actor is
// recorded as the mod's.
struct PendingArenaOrigin {
    int slot = -1;
    std::array<float, 3> origin{};
    bool grounded = false;
    float script_y = 0.0f;
    int cast_no = -1;
    int bank = -1;
};
PendingArenaOrigin g_pending_arena_origin;
// A boss placed with an arena origin keeps it (and its native story requests stay off) until a new actor takes its
// slot, so its death script still runs in its arena after the kill ends the handle.
struct ArenaSlot {
    bool active = false;
    std::array<float, 3> origin{};
    bool grounded = false; // every height the script asks for is the floor's (a flying boss kept on the ground)
    float script_y = 0.0f; // the height its script last asked for, which it reads back as its own
    bool fading = false;   // its life is gone and it fades out in place of its defeat scene
    int cast_no = -1;      // the slot it holds in its arena (characters.spawn arena_slot), or -1
    int bank = -1;         // its cast's own shared script integers (a bank no other living cast holds), or -1
};
std::array<ArenaSlot, MONSTOR_SLOT_MAX> g_arena_slots{};
// The cast bank last opened (by a boss, arena_slot 0, or a cast's first member) and at which arena origin.
struct LastCastBank {
    int bank = -1;
    std::array<float, 3> origin{};
};
LastCastBank g_last_cast_bank{};

struct ChronicleEntityScaleRecord {
    std::uint32_t generation = 0;
    CMonstorUnit *unit = nullptr;
    std::string owner;
    std::array<float, 3> original{1.0f, 1.0f, 1.0f};
    float factor = 1.0f;
};

struct ChronicleDebugOverlay {
    std::string id;
    std::string owner;
    std::string text;
    bool render_layer = false;
    bool render_bar = false;
    bool boss_bar = false;
    std::string label;
    double current = 0.0;
    double maximum = 1.0;
};

struct ChronicleShopBackup {
    int shop_no = -1;
    std::array<s16, 20> goods{};
};

struct ChronicleMonsterDropBackup {
    int monster_id = -1;
    s16 rare_item = -1;
    s16 steal_item = -1;
    s16 attachment_kind = 0;
};

int ChronicleAilmentForName(std::string_view name) {
    if (name == "freeze") return AILMENT_FREEZE;
    if (name == "poison") return AILMENT_POISON;
    if (name == "curse") return AILMENT_CURSE;
    if (name == "goo") return AILMENT_GOO;
    return 0;
}

unsigned int ChronicleCureFlagForName(std::string_view name) {
    if (name == "freeze") return ITEMUSE_CURE_FREEZE;
    if (name == "poison") return ITEMUSE_CURE_POISON;
    if (name == "curse") return ITEMUSE_CURE_CURSE;
    if (name == "goo") return ITEMUSE_CURE_GOO;
    return 0;
}

std::string_view ChronicleStatusForCureFlag(unsigned int flags) {
    if (flags == ITEMUSE_CURE_FREEZE) return "freeze";
    if (flags == ITEMUSE_CURE_POISON) return "poison";
    if (flags == ITEMUSE_CURE_CURSE) return "curse";
    if (flags == ITEMUSE_CURE_GOO) return "goo";
    return {};
}

std::map<std::string, ModItem, std::less<>> g_mod_items;
std::map<int, std::string> g_mod_item_by_native_id;
std::map<int, std::pair<COM_ITEM_INFO, ITEM_DATA>> g_original_mod_item_slots;
std::map<std::string, ModWeapon, std::less<>> g_mod_weapons;
std::map<int, std::string> g_mod_weapon_by_native_id;
std::map<int, std::pair<u32, u32>> g_original_weapon_buildup_masks;
struct ChronicleWeaponDefinitionOverride {
    std::string owner;
    WEAPON_DATA original{};
};
std::map<int, ChronicleWeaponDefinitionOverride> g_weapon_definition_overrides;
std::map<int, std::vector<int>> g_mod_weapon_buildups;
std::map<int, ChronicleShopBackup> g_original_shop_lists;
std::map<int, std::string> g_shop_list_owners;
// Goods mods add (shops.register_item): kept apart from a shop's list, so they claim no shop and come back after any
// mod's shops.set_items (Expanded Data's daily stock rewrites shops 1, 8 and 10, where Opar's staffs are sold).
struct ChronicleShopAddition {
    std::string owner;
    int         item_no = -1;
};
std::map<int, std::vector<ChronicleShopAddition>> g_shop_additions;
std::map<int, ITEM_MONEY_ENTRY> g_original_shop_prices;
std::map<int, std::string> g_shop_price_owners;
std::map<int, ChronicleMonsterDropBackup> g_original_monster_drops;
std::map<std::string, ModCharacter, std::less<>> g_mod_characters;
std::map<std::string, ModModel, std::less<>> g_mod_models;
std::map<int, ModModelReplacement> g_model_replacements;
std::map<std::string, ModAnimation, std::less<>> g_mod_animations;
std::map<std::string, ModAnimationPlayback, std::less<>> g_mod_animation_plays;
std::map<std::string, ChronicleDebugOverlay, std::less<>> g_debug_overlays;
constexpr std::size_t kMaximumDebugOverlays = 12;
constexpr std::size_t kMaximumDebugOverlayText = 48;
constexpr std::size_t kMaximumDebugOverlayLine = 64;
std::map<std::string, ModDungeon, std::less<>> g_mod_dungeons;
std::map<std::string, ModGeoramaPiece, std::less<>> g_mod_georama_pieces;
std::map<int, std::string> g_mod_georama_piece_by_native_id;
std::map<std::string, ModLocation, std::less<>> g_mod_locations;
std::optional<PendingDungeonFloorEntry> g_pending_dungeon_floor_entry;
std::optional<PendingDungeonFloorEntry> g_active_dungeon_floor_entry;
bool g_pending_back_floor_request = false;
int g_pending_back_floor_dungeon = -1;
const CDngStatusData *g_thirst_status = nullptr;
int g_thirst_character = -1;
std::array<float, CHARA_MAX> g_thirst_water{};
bool g_thirst_baseline = false;
std::array<std::vector<BT_ENEMY_FLOOR>, DUNGEON_COUNT> g_modded_enemy_layouts;
std::array<std::vector<BT_ENEMY_FLOOR>, DUNGEON_COUNT> g_modded_ura_enemy_layouts;
std::array<BT_ENEMY_FLOOR *, DUNGEON_COUNT> g_native_enemy_layouts{};
std::array<BT_ENEMY_FLOOR *, DUNGEON_COUNT> g_native_ura_enemy_layouts{};
std::array<ChronicleMonsterHandleRecord, MONSTOR_SLOT_MAX> g_monster_handle_records;
std::array<std::uint32_t, MONSTOR_SLOT_MAX> g_monster_handle_generations{};
std::array<ChronicleEntityScaleRecord, MONSTOR_SLOT_MAX> g_monster_scale_records;
// entities.set_stats damage_scale: what each monster's hits on the player are multiplied by. A new actor in the slot,
// or a new floor, sets it back to 1.
std::array<float, MONSTOR_SLOT_MAX> g_monster_damage_scales = [] {
    std::array<float, MONSTOR_SLOT_MAX> scales{};
    scales.fill(1.0f);
    return scales;
}();
// entities.set_stats hit_reach: game units added to each of a monster's body spheres when a blow is checked against it
// (a weak point out of a sword's reach). A new actor in the slot, or a new floor, sets it back to 0.
std::array<float, MONSTOR_SLOT_MAX> g_monster_hit_reach{};
// entities.set_stats hit_share: each body sphere takes from every character the share of a blow it takes from the
// character it suits best (a story boss hurt only by the hero the story brought, met by any party). Reset as above.
std::array<bool, MONSTOR_SLOT_MAX> g_monster_hit_share{};
std::array<int, MONSTOR_MODEL_MAX> g_loaded_monster_model_ids = [] {
    std::array<int, MONSTOR_MODEL_MAX> ids{};
    ids.fill(-1);
    return ids;
}();
CMonstorUnit *g_monster_handle_unit = nullptr;

bool ReadBoundedInt(const Json &object, const char *key, int minimum, int maximum, int &out);
bool ReadId(const Json &argument, std::string &out);
bool ReadVector3(const Json &value, std::array<float, 4> &out, float limit);
void PrepareMonsterHandleUnit(CMonstorUnit *unit);
void AdvanceMonsterHandleGeneration(int slot);
std::uint32_t EnsureMonsterHandleGeneration(CMonstorUnit *unit, int slot);
std::string MonsterHandleId(int slot, std::uint32_t generation);
void RestoreMonsterScale(int slot);
void RestoreMonsterScalesByOwner(std::string_view owner);
bool ReadMonsterHandle(std::string_view handle, int &slot, std::uint32_t &generation);
bool AllocateModGeoramaPieceSlot(std::string_view id, const std::filesystem::path &save_root, int &part_id,
                                std::string &error);

bool InputId(std::string_view value) {
    if (value.empty() || value.size() > 64) return false;
    for (unsigned char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' ||
              c == '-' || c == '.')) return false;
    }
    return true;
}

bool NativeMonsterAssetStem(std::string_view value) {
    if (value.empty() || value.size() > 15) return false;
    for (unsigned char c : value) {
        if (!((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '-')) return false;
    }
    return true;
}

std::string NativeMonsterAssetPath(std::string_view stem, std::string_view extension) {
    return "dun/monstor/" + std::string(stem) + "." + std::string(extension);
}

bool NativeMonsterAssetReady(std::string_view owner, std::string_view game_path) {
    const auto file = FindNativeResourceOverride(game_path);
    if (!file || !NativeResourceOverrideOwnedBy(owner, game_path)) return false;
    std::error_code error;
    const std::uintmax_t size = std::filesystem::file_size(*file, error);
    if (error || size == 0 || size > 2ull * 1024ull * 1024ull) return false;
    const std::uintmax_t padded_size = ((size + 2047u) / 2048u) * 2048u;
    return padded_size <= 2ull * 1024ull * 1024ull;
}

bool NativeMonsterAssetStemInUse(std::string_view stem) {
    for (int model_id = 0; model_id < 167; ++model_id) {
        const MONSTOR_MODEL *definition = GetMonstorTable(model_id);
        if (definition == nullptr) continue;
        for (const auto &name : definition->model_name) {
            const char *end = std::find(name, name + sizeof(name), '\0');
            if (std::string_view(name, static_cast<std::size_t>(end - name)) == stem) return true;
        }
    }
    for (const auto &[id, character] : g_mod_characters) {
        (void)id;
        if (character.model == stem) return true;
    }
    return std::ranges::any_of(g_mod_models, [stem](const auto &entry) { return entry.second.model == stem; });
}

// An optional asset stem a registered item or weapon names (`model`, `icon`): the mod must own the native resource
// override at <folder>/<stem>.<extension>. Empty stem when the field is absent.
bool ReadOwnedAssetStem(const Json &definition, const char *key, std::string_view mod_id, std::string_view folder,
                        std::string_view extension, std::string &stem, std::string &error) {
    stem.clear();
    const auto field = definition.find(key);
    if (field == definition.end()) return true;
    if (!field->is_string() || !NativeMonsterAssetStem(field->get_ref<const std::string &>())) {
        error = std::string(key) + " must be a lowercase asset stem of 1-15 letters, digits, '_' or '-'";
        return false;
    }
    stem = field->get<std::string>();
    const std::string game_path = std::string(folder) + "/" + stem + "." + std::string(extension);
    if (!NativeMonsterAssetReady(mod_id, game_path)) {
        error = std::string(key) + " requires this mod to own a 1 byte through 2 MiB resources.override for " + game_path;
        return false;
    }
    return true;
}

bool QualifyModContentId(std::string_view mod_id, std::string_view local_id, std::string &qualified_id,
                         std::string_view kind) {
    const std::size_t separator = local_id.find(':');
    if (separator == std::string_view::npos) {
        if (!InputId(local_id)) return false;
        qualified_id = std::string(mod_id) + ":" + std::string(local_id);
        return true;
    }
    if (local_id.find(':', separator + 1) != std::string_view::npos || local_id.substr(0, separator) != mod_id ||
        !InputId(local_id.substr(separator + 1))) {
        (void)kind;
        return false;
    }
    qualified_id = std::string(local_id);
    return true;
}

ApiCallResult Invalid(std::string message) {
    return {ApiCallStatus::InvalidArgument, nullptr, std::move(message)};
}

ApiCallResult Failed(std::string message) {
    return {ApiCallStatus::Failed, nullptr, std::move(message)};
}

CDngStatusData *CurrentStatus() {
    return SaveData == nullptr ? nullptr : SaveData->GetDngStatus();
}

bool IsActiveDungeonStatus(const CDngStatusData *status) {
    return mode == GAME_MODE_DUNGEON && status != nullptr && selectMapNo >= 0 && selectMapNo < DUNGEON_COUNT &&
           status->cur_georama == selectMapNo;
}

Json CharacterJson(const CDngStatusData &status, int character_id) {
    return {{"id", kCharacterIds[character_id]},
            {"character_id", character_id},
            {"active", status.cur_chara == character_id}};
}

Json CharacterDefinitionJson(const CDngStatusData &status, int character_id, int party_size) {
    Json out = {{"id", kCharacterIds[character_id]},
            {"character_id", character_id},
            {"kind", "playable"},
            {"in_party", character_id < party_size || CharaJoined(character_id)},
            {"active", status.cur_chara == character_id},
            {"hp", status.hp[character_id]},
            {"max_hp", status.max_hp[character_id]}};
    // MoonMod: a registered character also says his name and whom he fights as.
    if (const PlayableChara *extra = ExtraChara(character_id)) {
        out["name"] = extra->name;
        out["fights_as"] = kCharacterIds[extra->fights_as];
    }
    return out;
}

Json ModCharacterJson(const ModCharacter &character) {
    return {{"id", character.id},
            {"kind", "enemy"},
            {"name", character.name},
            {"template_monster_id", character.template_model_no},
            {"native_monster_id", character.native_model_no},
            {"status", "registered"},
            {"mod_owned", true},
            {"model", character.model},
            {"model_id", character.model_id.empty() ? Json(nullptr) : Json(character.model_id)},
            {"script", character.script},
            {"stats", {{"hp", character.hp}, {"defense", character.defense},
                       {"reward_exp", character.reward_exp}, {"reward_money", character.reward_money}}},
            {"assets", Json::object()}};
}

Json ModModelJson(const ModModel &model) {
    const std::string model_path = NativeMonsterAssetPath(model.model, "chr");
    const std::string script_path = NativeMonsterAssetPath(model.script, "stb");
    const bool model_owned = NativeMonsterAssetReady(model.owner, model_path);
    const bool script_overridden = FindNativeResourceOverride(script_path).has_value();
    const bool script_ready = !model.custom_script || NativeMonsterAssetReady(model.owner, script_path);
    const auto character = std::ranges::find_if(g_mod_characters, [&model](const auto &entry) {
        return entry.second.model_id == model.id;
    });
    bool loaded_on_current_floor = false;
    int native_model_no = -1;
    if (character != g_mod_characters.end()) {
        native_model_no = character->second.native_model_no;
        loaded_on_current_floor = std::ranges::find(g_loaded_monster_model_ids,
                                                     character->second.asset_template_model_no) !=
                                  g_loaded_monster_model_ids.end();
    }
    return {{"id", model.id}, {"owner", model.owner}, {"model", model.model}, {"script", model.script},
            {"template_monster_id", model.template_model_no},
            {"status", model_owned && script_ready ? "registered" : "unavailable"},
            {"reason", model_owned && script_ready ? "" :
                "the owning mod's model or custom script override is unavailable or exceeds Chronicle's 2 MiB loader limit"},
            {"native_monster_id", native_model_no < 0 ? Json(nullptr) : Json(native_model_no)},
            {"loaded_on_current_floor", loaded_on_current_floor},
            {"source", model_owned ? "package_override" : "missing"},
            {"assets", Json::array({{{"slot", "model"}, {"game_path", model_path}, {"overridden", model_owned}},
                                    {{"slot", "script"}, {"game_path", script_path},
                                     {"overridden", script_overridden}}})}};
}

ApiCallResult RegisterModel(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id, template_monster_id, model, and optional script}");
    }
    const Json &definition = args[0];
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "id" && field.key() != "template_monster_id" && field.key() != "model" &&
            field.key() != "script") {
            return Invalid("model definitions only support id, template_monster_id, model, and script");
        }
    }
    const auto id_field = definition.find("id");
    const auto model_field = definition.find("model");
    if (id_field == definition.end() || !id_field->is_string() || model_field == definition.end() ||
        !model_field->is_string()) {
        return Invalid("model definition requires string id and model fields");
    }
    std::string qualified_id;
    if (!QualifyModContentId(mod_id, id_field->get_ref<const std::string &>(), qualified_id, "model")) {
        return Invalid("model id must be local or use this mod's namespace and safe identifier characters");
    }
    const std::string &model_name = model_field->get_ref<const std::string &>();
    if (!NativeMonsterAssetStem(model_name)) {
        return Invalid("model must be a lowercase native monster asset stem of 1-15 safe characters");
    }
    int template_model_no = -1;
    if (!ReadBoundedInt(definition, "template_monster_id", 0, 32767, template_model_no) ||
        GetMonstorTable(template_model_no) == nullptr) {
        return Invalid("template_monster_id must identify an existing native or registered monster model");
    }
    MONSTOR_MODEL *template_definition = GetMonstorTable(template_model_no);
    const char *script_begin = template_definition->script_name;
    const char *script_end = std::find(script_begin, script_begin + sizeof(template_definition->script_name), '\0');
    if (script_end == script_begin + sizeof(template_definition->script_name)) {
        return Failed("template monster definition contains an unterminated script filename");
    }
    const auto script_field = definition.find("script");
    const bool custom_script = script_field != definition.end();
    if (custom_script && !script_field->is_string()) return Invalid("script must be a native monster asset stem string");
    const std::string script_name = custom_script ? script_field->get<std::string>() :
        std::string(script_begin, script_end);
    if (!NativeMonsterAssetStem(script_name)) {
        return Invalid("script must be a lowercase native monster asset stem of 1-15 safe characters");
    }
    if (g_mod_models.contains(qualified_id)) return Invalid("a model with this stable id is already registered");
    if (g_mod_models.size() >= 32) return Failed("Chronicle currently supports up to 32 custom model bindings");
    if (NativeMonsterAssetStemInUse(model_name)) {
        return Invalid("model asset stem is already used by a native or registered model; additions need a new stem");
    }
    const std::string model_path = NativeMonsterAssetPath(model_name, "chr");
    if (!NativeMonsterAssetReady(mod_id, model_path)) {
        return Failed("models.register requires this mod to own a 1 byte through 2 MiB resources.override for " + model_path);
    }
    if (custom_script) {
        const std::string script_path = NativeMonsterAssetPath(script_name, "stb");
        if (!NativeMonsterAssetReady(mod_id, script_path)) {
            return Failed("a custom script requires this mod to own a 1 byte through 2 MiB resources.override for " + script_path);
        }
    }
    ModModel model;
    model.id = qualified_id;
    model.owner = mod_id;
    model.model = model_name;
    model.script = script_name;
    model.template_model_no = template_model_no;
    model.custom_script = custom_script;
    const std::string id = model.id;
    g_mod_models.emplace(id, std::move(model));
    return {ApiCallStatus::Ok, ModModelJson(g_mod_models.at(id)), {}};
}

ApiCallResult UnregisterModel(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("id") || !args[0]["id"].is_string()) {
        return Invalid("expected {id = local_model_id}");
    }
    std::string id;
    if (!QualifyModContentId(mod_id, args[0]["id"].get_ref<const std::string &>(), id, "model")) {
        return Invalid("id must be a local model identifier owned by this mod");
    }
    const auto found = g_mod_models.find(id);
    if (found == g_mod_models.end() || found->second.owner != mod_id) {
        return Invalid("model binding is missing or owned by another mod");
    }
    if (std::ranges::any_of(g_mod_characters, [&id](const auto &entry) {
            return entry.second.model_id == id;
        })) {
        return Failed("a registered character uses this model binding and must be unregistered first");
    }
    if (std::ranges::any_of(g_mod_animations, [&id](const auto &entry) {
            return entry.second.model_id == id;
        })) {
        return Failed("a registered embedded-motion definition uses this model binding and must be removed first");
    }
    g_mod_models.erase(found);
    return {ApiCallStatus::Ok, {{"id", id}, {"type", "model"}, {"removed", true}}, {}};
}

bool ChronicleNativeMonsterModelLoaded(int model_id) {
    if (mode != GAME_MODE_DUNGEON || NowMonstorUnit == nullptr) return false;
    const int loaded_count = std::clamp(NowMonstorUnit->model_count, 0,
                                        static_cast<int>(g_loaded_monster_model_ids.size()));
    return std::ranges::find(g_loaded_monster_model_ids.begin(),
                             g_loaded_monster_model_ids.begin() + loaded_count, model_id) !=
           g_loaded_monster_model_ids.begin() + loaded_count;
}

bool SameNativeResourcePath(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t index = 0; index < left.size(); ++index) {
        unsigned char left_char = static_cast<unsigned char>(left[index]);
        unsigned char right_char = static_cast<unsigned char>(right[index]);
        if (left_char == '\\') left_char = '/';
        if (right_char == '\\') right_char = '/';
        if (std::tolower(left_char) != std::tolower(right_char)) return false;
    }
    return true;
}

std::string ChronicleModelOverrideRemovalGuard(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("game_path") ||
        !args[0]["game_path"].is_string()) {
        return {};
    }
    const std::string &requested_path = args[0]["game_path"].get_ref<const std::string &>();
    for (const auto &[id, model] : g_mod_models) {
        (void)id;
        if (model.owner != owner) continue;
        if (SameNativeResourcePath(requested_path, NativeMonsterAssetPath(model.model, "chr")) ||
            (model.custom_script &&
             SameNativeResourcePath(requested_path, NativeMonsterAssetPath(model.script, "stb")))) {
            return "resources.unoverride cannot remove an asset referenced by a registered model; model definitions remain registered until the game process exits";
        }
    }
    for (const auto &[model_id, replacement] : g_model_replacements) {
        if (replacement.owner != owner) continue;
        const MONSTOR_MODEL *definition = GetMonstorTable(model_id);
        if (definition == nullptr) continue;
        const char *model_begin = definition->model_name[0];
        const char *model_end = std::find(model_begin, model_begin + sizeof(definition->model_name[0]), '\0');
        if (model_end == model_begin + sizeof(definition->model_name[0])) continue;
        if (SameNativeResourcePath(requested_path,
                                   NativeMonsterAssetPath(std::string_view(model_begin, model_end - model_begin), "chr"))) {
            return "resources.unoverride cannot remove the .chr asset used by an active native model replacement; call models.remove first";
        }
    }
    return {};
}

void RestoreChronicleModelReplacements(std::string_view owner = {}) {
    for (auto replacement = g_model_replacements.begin(); replacement != g_model_replacements.end();) {
        if (!owner.empty() && replacement->second.owner != owner) {
            ++replacement;
            continue;
        }
        if (MONSTOR_MODEL *definition = GetMonstorTable(replacement->first); definition != nullptr) {
            std::memset(definition->model_name[0], 0, sizeof(definition->model_name[0]));
            std::memcpy(definition->model_name[0], replacement->second.original_model.data(),
                        replacement->second.original_model.size());
        }
        replacement = g_model_replacements.erase(replacement);
    }
}

ApiCallResult ReplaceNativeModel(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 2 ||
        !args[0].contains("monster_id") || !args[0].contains("model") || !args[0]["model"].is_string()) {
        return Invalid("expected {monster_id = native monster id, model = owned replacement model stem}");
    }
    int model_id = -1;
    if (!ReadBoundedInt(args[0], "monster_id", 0, 166, model_id)) {
        return Invalid("monster_id must identify a native Chronicle model from 0 to 166");
    }
    const std::string &replacement_model = args[0]["model"].get_ref<const std::string &>();
    if (!NativeMonsterAssetStem(replacement_model)) {
        return Invalid("model must be a lowercase native monster asset stem of 1-15 safe characters");
    }
    const std::string replacement_path = NativeMonsterAssetPath(replacement_model, "chr");
    if (!NativeMonsterAssetReady(mod_id, replacement_path)) {
        return Failed("models.replace requires this mod to own a 1 byte through 2 MiB resources.override for " +
                      replacement_path);
    }
    if (ChronicleNativeMonsterModelLoaded(model_id)) {
        return Failed("the native monster model is loaded on the current floor; replace it after leaving that floor");
    }
    MONSTOR_MODEL *definition = GetMonstorTable(model_id);
    if (definition == nullptr) return Invalid("monster_id does not identify a native Chronicle model");
    const char *current_begin = definition->model_name[0];
    const char *current_end = std::find(current_begin, current_begin + sizeof(definition->model_name[0]), '\0');
    if (current_end == current_begin + sizeof(definition->model_name[0])) {
        return Failed("native model table contains an unterminated model filename");
    }
    const std::string current_model(current_begin, current_end);
    auto replacement = g_model_replacements.find(model_id);
    if (replacement != g_model_replacements.end() && replacement->second.owner != mod_id) {
        return {ApiCallStatus::PermissionDenied, nullptr,
                "another mod already owns the replacement binding for this native model"};
    }
    if (current_model == replacement_model) {
        return Invalid("replacement model stem must differ from the current native model; use models.remove to restore it");
    }
    if (replacement == g_model_replacements.end()) {
        g_model_replacements.emplace(model_id, ModModelReplacement{std::string(mod_id), current_model});
    }
    std::memset(definition->model_name[0], 0, sizeof(definition->model_name[0]));
    std::memcpy(definition->model_name[0], replacement_model.data(), replacement_model.size());
    return {ApiCallStatus::Ok,
            {{"monster_id", model_id}, {"previous_model", current_model}, {"model", replacement_model},
             {"game_path", replacement_path}, {"changed", true}},
            {}};
}

ApiCallResult RemoveNativeModelReplacement(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("monster_id")) {
        return Invalid("expected {monster_id = native Chronicle model id}");
    }
    int model_id = -1;
    if (!ReadBoundedInt(args[0], "monster_id", 0, 166, model_id)) {
        return Invalid("monster_id must identify a native Chronicle model from 0 to 166");
    }
    const auto replacement = g_model_replacements.find(model_id);
    if (replacement == g_model_replacements.end()) return Failed("this native model has no active MoonMod replacement");
    if (replacement->second.owner != mod_id) {
        return {ApiCallStatus::PermissionDenied, nullptr,
                "a mod can only remove its own native model replacement"};
    }
    if (ChronicleNativeMonsterModelLoaded(model_id)) {
        return Failed("the native monster model is loaded on the current floor; remove it after leaving that floor");
    }
    MONSTOR_MODEL *definition = GetMonstorTable(model_id);
    if (definition == nullptr) return Failed("the native model table entry is no longer available");
    const std::string previous_model(definition->model_name[0],
        std::find(definition->model_name[0], definition->model_name[0] + sizeof(definition->model_name[0]), '\0'));
    const std::string restored_model = replacement->second.original_model;
    std::memset(definition->model_name[0], 0, sizeof(definition->model_name[0]));
    std::memcpy(definition->model_name[0], restored_model.data(), restored_model.size());
    g_model_replacements.erase(replacement);
    return {ApiCallStatus::Ok,
            {{"monster_id", model_id}, {"previous_model", previous_model}, {"model", restored_model},
             {"removed", true}, {"changed", true}},
            {}};
}

bool ReadCharacterId(const Json &selector, int &character_id) {
    const Json *id = nullptr;
    if (selector.is_string()) {
        id = &selector;
    } else if (selector.is_object()) {
        const auto found = selector.find("id");
        if (found != selector.end()) id = &*found;
    }
    if (id == nullptr || !id->is_string()) return false;
    const std::string &name = id->get_ref<const std::string &>();
    const auto found = std::find_if(kCharacterIds.begin(), kCharacterIds.end(), [&name](const char *candidate) {
        return name == candidate;
    });
    if (found == kCharacterIds.end()) return false;
    character_id = static_cast<int>(found - kCharacterIds.begin());
    return true;
}

ApiCallResult ListCharacters(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("characters.list takes no arguments");
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    const int party_size = status->GetPartySize();
    if (party_size < 0 || party_size > static_cast<int>(kCharacterIds.size())) {
        return Failed("the saved party size is out of range");
    }
    Json result = Json::array();
    for (int character_id = 0; character_id < static_cast<int>(kCharacterIds.size()); ++character_id) {
        result.push_back(CharacterDefinitionJson(*status, character_id, party_size));
    }
    for (const auto &[id, character] : g_mod_characters) {
        (void)id;
        result.push_back(ModCharacterJson(character));
    }
    return {ApiCallStatus::Ok, std::move(result), {}};
}

ApiCallResult GetCharacter(const Json &args) {
    if (!args.is_array() || args.size() != 1) return Invalid("expected one character id or {id = character_id}");
    const Json *selector_id = args[0].is_string() ? &args[0] :
                              args[0].is_object() && args[0].contains("id") ? &args[0]["id"] : nullptr;
    if (selector_id != nullptr && selector_id->is_string()) {
        const auto mod_character = g_mod_characters.find(selector_id->get_ref<const std::string &>());
        if (mod_character != g_mod_characters.end()) {
            return {ApiCallStatus::Ok, ModCharacterJson(mod_character->second), {}};
        }
    }
    int character_id = -1;
    if (!ReadCharacterId(args[0], character_id)) {
        return Invalid("expected a Chronicle playable character id: toan, xiao, goro, ruby, ungaga, osmond, or a registered one");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    const int party_size = status->GetPartySize();
    if (party_size < 0 || party_size > static_cast<int>(kCharacterIds.size())) {
        return Failed("the saved party size is out of range");
    }
    return {ApiCallStatus::Ok, CharacterDefinitionJson(*status, character_id, party_size), {}};
}

Json ModNpcJson(const ModNpc &npc) {
    const int slot = ChronicleModVillagerSlot(npc.index);
    return {{"id", npc.id}, {"kind", "npc"}, {"name", npc.name}, {"owner", npc.owner}, {"town_id", npc.town_id},
            {"model", npc.model}, {"game_path", "gedit/e0" + std::to_string(npc.town_id + 1) + "/chara/" + npc.model + ".chr"},
            {"position", {{"x", npc.position[0]}, {"y", npc.position[1]}, {"z", npc.position[2]}}},
            {"facing", npc.facing_degrees}, {"talk_event", npc.talk_event == 0 ? Json(nullptr) : Json(npc.talk_event)},
            {"in_town", slot >= 0}, {"villager_slot", slot < 0 ? Json(nullptr) : Json(slot)}, {"status", "registered"}};
}

ApiCallResult RegisterNpc(std::string_view mod_id, const Json &definition) {
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "id" && field.key() != "kind" && field.key() != "name" && field.key() != "town_id" &&
            field.key() != "model" && field.key() != "position" && field.key() != "facing" &&
            field.key() != "talk_event") {
            return Invalid("npc definitions accept only id, kind, name, town_id, model, position, facing, and talk_event");
        }
    }
    const auto id_it = definition.find("id");
    const auto name_it = definition.find("name");
    const auto model_it = definition.find("model");
    const auto position_it = definition.find("position");
    if (id_it == definition.end() || !id_it->is_string() || name_it == definition.end() || !name_it->is_string() ||
        model_it == definition.end() || !model_it->is_string() || position_it == definition.end()) {
        return Invalid("npc definition requires string id, name and model fields and a position");
    }
    std::string qualified_id;
    if (!QualifyModContentId(mod_id, id_it->get_ref<const std::string &>(), qualified_id, "npc")) {
        return Invalid("npc id must be local or use this mod's namespace and safe identifier characters");
    }
    const std::string &name = name_it->get_ref<const std::string &>();
    if (name.empty() || name.size() > 63 || name.find('\0') != std::string::npos) {
        return Invalid("npc name must contain 1-63 bytes");
    }
    int town_id = -1;
    if (!ReadBoundedInt(definition, "town_id", 0, TOWN_COUNT - 1, town_id)) {
        return Invalid("town_id must be a Georama town from 0 through 4");
    }
    const std::string &model = model_it->get_ref<const std::string &>();
    if (model.empty() || model.size() > 31 ||
        !std::ranges::all_of(model, [](unsigned char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; })) {
        return Invalid("model must be a villager model stem of 1-31 lowercase letters, digits or '_'");
    }
    const std::string game_path = "gedit/e0" + std::to_string(town_id + 1) + "/chara/" + model + ".chr";
    if (!NativeMonsterAssetReady(mod_id, game_path) && !dc::data::DataFileExists(game_path)) {
        return Failed("model must be a villager model in that town (" + game_path +
                      ") or this mod's own 1 byte through 2 MiB resources.override for it");
    }
    std::array<float, 4> position{};
    if (!ReadVector3(*position_it, position, 100000.0f)) return Invalid("position must be {x, y, z} within 100000");
    double facing = 0.0;
    if (const auto facing_it = definition.find("facing"); facing_it != definition.end()) {
        if (!facing_it->is_number() || !std::isfinite(facing_it->get<double>())) return Invalid("facing must be degrees");
        facing = facing_it->get<double>();
    }
    int talk_event = 0;
    if (definition.contains("talk_event") && !ReadBoundedInt(definition, "talk_event", 1, 0xFFFF, talk_event)) {
        return Invalid("talk_event must be a native event number from 1 through 65535");
    }
    if (const auto existing = g_mod_npcs.find(qualified_id); existing != g_mod_npcs.end()) {
        return Invalid("an npc with this stable id is already registered");
    }
    int index = -1;
    for (int candidate = 0; candidate < CHRONICLE_MOD_VILLAGER_COUNT && index < 0; ++candidate) {
        if (std::ranges::none_of(g_mod_npcs, [candidate](const auto &entry) { return entry.second.index == candidate; })) {
            index = candidate;
        }
    }
    if (index < 0) return Failed("Chronicle supports up to 4 mod villagers (the free villager slots)");
    const float facing_radians = static_cast<float>(facing * 3.14159265358979323846 / 180.0);
    if (!SetChronicleModVillager(index, town_id, model.c_str(), position.data(), facing_radians, talk_event)) {
        return Failed("the native villager table rejected this npc");
    }
    ModNpc npc{qualified_id, std::string(mod_id), name, town_id, model, position, static_cast<float>(facing), talk_event, index};
    auto [entry, inserted] = g_mod_npcs.emplace(qualified_id, std::move(npc));
    (void)inserted;
    return {ApiCallStatus::Ok, ModNpcJson(entry->second), {}};
}

void UnloadModNpcs(std::string_view mod_id) {
    for (auto it = g_mod_npcs.begin(); it != g_mod_npcs.end();) {
        if (mod_id.empty() || it->second.owner == mod_id) {
            SetChronicleModVillager(it->second.index, 0, "", nullptr, 0.0f, 0);
            it = g_mod_npcs.erase(it);
        } else {
            ++it;
        }
    }
}

// characters.register_playable: a playable character past the game's six (playable_chara.hpp), e.g. the Gauntlet's
// Opar. He takes the next index (6, 7, ...) and keeps it; a mod that registers the same id again updates him.
std::map<std::string, std::string, std::less<>> g_playable_owners;

ApiCallResult RegisterPlayable(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id, name, pack, portrait?, title?, fights_as?, hp?, defense?}");
    }
    const Json &definition = args[0];
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "id" && field.key() != "name" && field.key() != "pack" && field.key() != "portrait" &&
            field.key() != "fights_as" && field.key() != "hp" && field.key() != "defense" && field.key() != "title") {
            return Invalid("characters.register_playable received an unsupported field: " + field.key());
        }
    }
    for (const char *key : {"id", "name", "pack"}) {
        if (!definition.contains(key) || !definition[key].is_string()) return Invalid(std::string(key) + " must be a string");
    }
    PlayableChara entry;
    entry.id = definition["id"].get<std::string>();
    entry.name = definition["name"].get<std::string>();
    entry.pack = definition["pack"].get<std::string>();
    if (definition.contains("portrait")) {
        if (!definition["portrait"].is_string()) return Invalid("portrait must be a string");
        entry.portrait = definition["portrait"].get<std::string>();
    }
    // title: what the item menu says of him under his picture, as it says "Boy from Nolun" of Toan
    if (definition.contains("title")) {
        if (!definition["title"].is_string() || definition["title"].get<std::string>().size() > 40) {
            return Invalid("title must be a string of up to 40 bytes");
        }
        entry.title = definition["title"].get<std::string>();
    }
    if (definition.contains("fights_as")) {
        const int native = definition["fights_as"].is_string() ? FindPlayableChara(definition["fights_as"].get<std::string>()) : -1;
        if (native < 0 || native >= SAVE_CHARA_COUNT) return Invalid("fights_as must be toan, xiao, goro, ruby, ungaga or osmond");
        entry.fights_as = native;
    }
    // Unless given, he starts with the health and defence of the character he fights as.
    constexpr int kStartHp[6] = {70, 60, 100, 90, 110, 100};
    constexpr int kStartDefense[6] = {3, 1, 12, 23, 38, 46};
    entry.hp = kStartHp[entry.fights_as];
    entry.defense = kStartDefense[entry.fights_as];
    if (definition.contains("hp") && !ReadBoundedInt(definition, "hp", 1, 999, entry.hp)) return Invalid("hp must be an integer from 1 to 999");
    if (definition.contains("defense") && !ReadBoundedInt(definition, "defense", 0, 999, entry.defense)) {
        return Invalid("defense must be an integer from 0 to 999");
    }
    const auto owner = g_playable_owners.find(entry.id);
    if (owner != g_playable_owners.end() && owner->second != mod_id) return Failed("another mod already registered that playable character");
    const std::string id = entry.id;
    std::string error;
    const int index = RegisterPlayableChara(std::move(entry), error);
    if (index < 0) return Invalid(error);
    g_playable_owners[id] = std::string(mod_id);
    return {ApiCallStatus::Ok, {{"id", id}, {"character_id", index}, {"count", CharaCount()}}, {}};
}

ApiCallResult RegisterCharacter(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected one data-defined enemy definition object");
    }
    const Json &definition = args[0];
    if (const auto kind = definition.find("kind"); kind != definition.end() && kind->is_string() &&
                                                   kind->get_ref<const std::string &>() == "npc") {
        return RegisterNpc(mod_id, definition);
    }
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "id" && field.key() != "kind" && field.key() != "name" &&
            field.key() != "template_monster_id" && field.key() != "stats" && field.key() != "assets" &&
            field.key() != "model") {
            return Invalid("enemy definition contains an unsupported field");
        }
    }
    const auto id_it = definition.find("id");
    const auto name_it = definition.find("name");
    if (id_it == definition.end() || !id_it->is_string() || name_it == definition.end() || !name_it->is_string()) {
        return Invalid("enemy definition requires string id and name fields");
    }
    const std::string local_id = id_it->get<std::string>();
    std::string qualified_id;
    const std::size_t separator = local_id.find(':');
    if (separator == std::string::npos) {
        if (!InputId(local_id)) return Invalid("enemy id must use letters, digits, '.', '_' or '-'");
        qualified_id = std::string(mod_id) + ":" + local_id;
    } else {
        if (local_id.find(':', separator + 1) != std::string::npos || local_id.substr(0, separator) != mod_id ||
            !InputId(std::string_view(local_id).substr(separator + 1))) {
            return Invalid("fully qualified enemy ids must use this mod's namespace");
        }
        qualified_id = local_id;
    }
    const std::string &name = name_it->get_ref<const std::string &>();
    if (name.empty() || name.size() > 63 || name.find('\0') != std::string::npos) {
        return Invalid("enemy name must contain 1-63 bytes");
    }
    const auto kind = definition.find("kind");
    if (kind != definition.end() && (!kind->is_string() || kind->get<std::string>() != "enemy")) {
        return Invalid("Chronicle data-defined characters support kind = 'enemy' or 'npc'");
    }
    const auto assets = definition.find("assets");
    if (assets != definition.end() &&
        ((!assets->is_object() && !assets->is_array()) || !assets->empty())) {
        return Invalid("enemy asset fields are unsupported; select a package-owned binding with model instead");
    }
    const ModModel *model_binding = nullptr;
    const auto model_ref = definition.find("model");
    if (model_ref != definition.end()) {
        if (!model_ref->is_string()) return Invalid("model must identify a model registered by this mod");
        const auto found_model = g_mod_models.find(model_ref->get<std::string>());
        if (found_model == g_mod_models.end()) return Invalid("model does not identify a registered custom model");
        if (found_model->second.owner != mod_id) return Failed("a mod can only use custom models that it owns");
        model_binding = &found_model->second;
        if (!NativeMonsterAssetReady(mod_id, NativeMonsterAssetPath(model_binding->model, "chr"))) {
            return Failed("the selected custom model's native .chr override is not active");
        }
        if (model_binding->custom_script &&
            !NativeMonsterAssetReady(mod_id, NativeMonsterAssetPath(model_binding->script, "stb"))) {
            return Failed("the selected custom model's native .stb override is not active");
        }
    }
    int template_model_no = model_binding == nullptr ? -1 : model_binding->template_model_no;
    const auto template_field = definition.find("template_monster_id");
    if (template_field != definition.end()) {
        if (!ReadBoundedInt(definition, "template_monster_id", 0, 32767, template_model_no)) {
            return Invalid("template_monster_id must identify an existing native or registered monster model");
        }
        if (model_binding != nullptr && template_model_no != model_binding->template_model_no) {
            return Invalid("template_monster_id must match the selected registered model's template");
        }
    } else if (model_binding == nullptr) {
        return Invalid("template_monster_id is required when the character does not select a registered model");
    }
    if (GetMonstorTable(template_model_no) == nullptr) {
        return Invalid("template_monster_id must identify an existing native or registered monster model");
    }
    int asset_template_model_no = template_model_no;
    std::string inherited_model_id;
    if (template_model_no >= 0x400) {
        const auto parent = std::ranges::find_if(g_mod_characters, [template_model_no](const auto &entry) {
            return entry.second.native_model_no == template_model_no;
        });
        if (parent == g_mod_characters.end()) {
            return Failed("the registered enemy template has no retained native asset source");
        }
        asset_template_model_no = parent->second.asset_template_model_no;
        inherited_model_id = parent->second.model_id;
    }
    const auto stats = definition.find("stats");
    if (stats != definition.end() && !stats->is_object()) return Invalid("stats must be an object");
    const Json empty_stats = Json::object();
    const Json &stat_values = stats == definition.end() ? empty_stats : *stats;
    MONSTOR_MODEL *base = GetMonstorTable(template_model_no);
    int hp = base->max_hp;
    int defense = base->defense;
    int reward_exp = base->exp;
    int reward_money = base->money;
    auto read_stat = [&stat_values](const char *key, int minimum, int maximum, int &value) {
        if (!stat_values.contains(key)) return true;
        return ReadBoundedInt(stat_values, key, minimum, maximum, value);
    };
    if (!read_stat("hp", 1, 1000000, hp) || !read_stat("defense", 0, 32767, defense) ||
        !read_stat("reward_exp", 0, 32767, reward_exp) ||
        !read_stat("reward_money", 0, std::numeric_limits<int>::max(), reward_money)) {
        return Invalid("enemy hp, defense, reward_exp, and reward_money values are outside Chronicle's native ranges");
    }
    for (auto stat = stat_values.begin(); stat != stat_values.end(); ++stat) {
        if (stat.key() != "hp" && stat.key() != "defense" && stat.key() != "reward_exp" &&
            stat.key() != "reward_money") {
            return Invalid("unsupported Chronicle enemy stat field '" + stat.key() + "'");
        }
    }
    if (g_mod_characters.contains(qualified_id)) return Invalid("a character with this stable id is already registered");
    if (g_mod_characters.size() >= 128) return Failed("Chronicle currently supports up to 128 mod enemy definitions per session");

    int native_model_no = -1;
    MONSTOR_MODEL *template_definition = GetMonstorTable(template_model_no);
    std::string model_name = model_binding == nullptr ? std::string(template_definition->model_name[0]) :
        model_binding->model;
    std::string script_name = model_binding == nullptr ? std::string(template_definition->script_name) :
        model_binding->script;
    if (!RegisterMoonModMonstorVariant(template_model_no, hp, defense, reward_exp, reward_money,
                                       model_name.c_str(), script_name.c_str(), &native_model_no)) {
        return Failed("the native monster model table has no available mod variant slot");
    }
    MONSTOR_MODEL *registered = GetMonstorTable(native_model_no);
    if (registered == nullptr) return Failed("the native monster variant could not be resolved after registration");
    ModCharacter character;
    character.id = qualified_id;
    character.owner = mod_id;
    character.name = name;
    character.model = registered->model_name[0];
    character.script = registered->script_name;
    character.template_model_no = template_model_no;
    character.asset_template_model_no = model_binding == nullptr ? asset_template_model_no : native_model_no;
    character.native_model_no = native_model_no;
    character.model_id = model_binding == nullptr ? inherited_model_id : model_binding->id;
    character.hp = hp;
    character.defense = defense;
    character.reward_exp = reward_exp;
    character.reward_money = reward_money;
    const std::string id = character.id;
    g_mod_characters.emplace(id, std::move(character));
    return {ApiCallStatus::Ok, ModCharacterJson(g_mod_characters.at(id)), {}};
}

ApiCallResult GetPlayer(const Json &args) {
    if (!args.is_array() || !args.empty()) {
        return Invalid("player.get takes no arguments");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) {
        return Failed("the game has no active save data");
    }
    const int character_id = status->cur_chara;
    if (character_id < CHARA_TOAN || character_id >= CharaCount()) {
        return Failed("the active character id is not recognized");
    }
    Json result = {{"character", kCharacterIds[character_id]},
                   {"character_id", character_id},
                   {"hp", status->hp[character_id]},
                   {"max_hp", status->max_hp[character_id]},
                   {"water", status->water_now[character_id]},
                   {"max_water", status->water_max[character_id]}};
    const int equipped = status->equipped_weapon_slot[character_id];
    if (equipped >= 0 && equipped < 11) {
        const auto &weapon = status->chara_weapons[character_id][equipped];
        if (weapon.item_no >= ITEM_WEAPON_START) result["weapon"] = {{"id", "chronicle:weapon:" + std::to_string(weapon.item_no)},
            {"item_no", weapon.item_no}, {"slot", equipped}, {"attack", weapon.attack}, {"endurance", weapon.endurance},
            {"speed", weapon.speed}, {"durability", weapon.durability_f}, {"durability_max", weapon.durability}};
    }
    if (IsActiveDungeonStatus(status)) {
        float position[4]{};
        CharaMain.GetPosition(position);
        if (!std::isfinite(position[0]) || !std::isfinite(position[1]) || !std::isfinite(position[2])) {
            return Failed("the active player position contains a non-finite coordinate");
        }
        result["position"] = {{"x", position[0]}, {"y", position[1]}, {"z", position[2]}};
        float rotation[4]{};
        CharaMain.GetRotation(rotation);
        if (std::isfinite(rotation[1])) result["heading"] = rotation[1]; // the way the player faces: (sin, cos) in (x, z)
        result["motion"] = CharaMain.motion_no; // the motion the hero plays (co-op's avatars play it on other screens)
    }
    return {ApiCallStatus::Ok, std::move(result), {}};
}

bool ReadPlayerTarget(const Json &definition, const CDngStatusData &status, int &character_id) {
    character_id = status.cur_chara;
    const auto character = definition.find("character");
    if (character == definition.end()) return true;
    if (!character->is_string()) return false;
    const std::string &target = character->get_ref<const std::string &>();
    if (target == "active") return true;
    const auto found = std::find_if(kCharacterIds.begin(), kCharacterIds.end(), [&target](const char *candidate) {
        return target == candidate;
    });
    if (found == kCharacterIds.end()) return false;
    character_id = static_cast<int>(found - kCharacterIds.begin());
    return true;
}

ApiCallResult HealPlayer(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {amount = integer, character = optional 'active' or a Chronicle character id}");
    }
    const Json &definition = args[0];
    const auto amount_it = definition.find("amount");
    if (amount_it == definition.end() || !amount_it->is_number_integer()) {
        return Invalid("amount must be an integer from 1 to 32767");
    }
    const int amount = amount_it->get<int>();
    if (amount < 1 || amount > 32767) {
        return Invalid("amount must be an integer from 1 to 32767");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) {
        return Failed("the game has no active save data");
    }
    int character_id = status->cur_chara;
    if (!ReadPlayerTarget(definition, *status, character_id)) {
        return Invalid("character must be 'active' or a Chronicle playable character id");
    }
    if (character_id < CHARA_TOAN || character_id >= CharaCount()) {
        return Failed("the player health target is not recognized");
    }
    const int before = status->hp[character_id];
    status->AddNowLife(character_id, static_cast<s16>(amount), 0.0f);
    return {ApiCallStatus::Ok,
            {{"character", kCharacterIds[character_id]},
             {"character_id", character_id},
             {"hp", status->hp[character_id]},
             {"max_hp", status->max_hp[character_id]},
             {"healed", status->hp[character_id] - before}},
            {}};
}

ApiCallResult DamagePlayer(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {amount = integer, character = optional 'active' or a Chronicle character id}");
    }
    const Json &definition = args[0];
    const auto amount_it = definition.find("amount");
    if (amount_it == definition.end() || !amount_it->is_number_integer()) {
        return Invalid("amount must be an integer from 1 to 32767");
    }
    const int amount = amount_it->get<int>();
    if (amount < 1 || amount > 32767) {
        return Invalid("amount must be an integer from 1 to 32767");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) {
        return Failed("the game has no active save data");
    }
    int character_id = status->cur_chara;
    if (!ReadPlayerTarget(definition, *status, character_id)) {
        return Invalid("character must be 'active' or a Chronicle playable character id");
    }
    if (character_id < CHARA_TOAN || character_id >= CharaCount()) {
        return Failed("the player health target is not recognized");
    }
    const int before = status->hp[character_id];
    status->AddNowLife(character_id, static_cast<s16>(-amount), 0.0f);
    return {ApiCallStatus::Ok,
            {{"character", kCharacterIds[character_id]},
             {"character_id", character_id},
             {"hp", status->hp[character_id]},
             {"max_hp", status->max_hp[character_id]},
             {"damaged", before - status->hp[character_id]}},
            {}};
}

Json ChronicleThirstPayload(const CDngStatusData &status, int character_id, float previous, float current,
                            std::string_view source, std::string_view reason);

ApiCallResult SetPlayer(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {hp = integer} or {water = integer}, each with optional character");
    }
    const Json &definition = args[0];
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "hp" && field.key() != "water" && field.key() != "max_hp" && field.key() != "character") {
            return Invalid("player.set supports hp, max_hp or water and an optional character");
        }
    }
    const auto hp_it = definition.find("hp");
    const auto water_it = definition.find("water");
    const auto max_hp_it = definition.find("max_hp");
    if (int(hp_it != definition.end()) + int(water_it != definition.end()) + int(max_hp_it != definition.end()) != 1) {
        return Invalid("player.set requires exactly one of hp, max_hp or water");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    int character_id = status->cur_chara;
    if (!ReadPlayerTarget(definition, *status, character_id)) {
        return Invalid("character must be 'active' or a Chronicle playable character id");
    }
    if (character_id < CHARA_TOAN || character_id >= CharaCount()) return Failed("the player target is not recognized");

    if (max_hp_it != definition.end()) {
        int maximum = 0;
        if (!ReadBoundedInt(definition, "max_hp", 1, 999, maximum)) return Invalid("max_hp must be an integer from 1 to 999");
        const int before = status->max_hp[character_id];
        status->max_hp[character_id] = static_cast<s16>(maximum);
        status->hp[character_id] = std::min<int>(status->hp[character_id], maximum);
        status->next_hp[character_id] = std::min<int>(status->next_hp[character_id], maximum);
        return {ApiCallStatus::Ok, {{"character", kCharacterIds[character_id]}, {"previous_max_hp", before},
                                  {"max_hp", maximum}, {"hp", status->hp[character_id]}}, {}};
    }

    if (hp_it != definition.end()) {
        if (!hp_it->is_number_integer()) return Invalid("hp must be an integer from 0 through max_hp");
        const int maximum = status->max_hp[character_id];
        long long requested = -1;
        if (hp_it->is_number_unsigned()) {
            const unsigned long long value = hp_it->get<unsigned long long>();
            if (value > static_cast<unsigned long long>(std::max(0, maximum))) {
                return Invalid("hp must be within the selected character's native health range");
            }
            requested = static_cast<long long>(value);
        } else {
            requested = hp_it->get<long long>();
            if (requested < 0 || requested > maximum) {
                return Invalid("hp must be within the selected character's native health range");
            }
        }
        if (maximum < 0 || maximum > 32767) return Failed("the native maximum HP is outside the supported setter range");
        status->AddNowLife(character_id, 0, 0.0f);
        const int before = status->hp[character_id];
        if (before < 0 || before > maximum) return Failed("the native current HP is outside the supported setter range");
        status->AddNowLife(character_id, static_cast<s16>(static_cast<int>(requested) - before), 0.0f);
        return {ApiCallStatus::Ok,
                {{"character", kCharacterIds[character_id]}, {"character_id", character_id},
                 {"previous_hp", before}, {"hp", status->hp[character_id]}, {"max_hp", status->max_hp[character_id]},
                 {"changed", status->hp[character_id] != before}},
                {}};
    }

    if (!water_it->is_number_integer()) return Invalid("water must be an integer from 0 through max_water");
    const float maximum = status->water_max[character_id];
    const float before = status->water_now[character_id];
    if (!std::isfinite(maximum) || !std::isfinite(before) || maximum < 0.0f || maximum > 32767.0f ||
        before < 0.0f || before > maximum) {
        return Failed("the native water gauge is outside the supported setter range");
    }
    long long requested = -1;
    if (water_it->is_number_unsigned()) {
        const unsigned long long value = water_it->get<unsigned long long>();
        if (static_cast<double>(value) > maximum) return Invalid("water must be within the selected character's native range");
        requested = static_cast<long long>(value);
    } else {
        requested = water_it->get<long long>();
        if (requested < 0 || static_cast<double>(requested) > maximum) {
            return Invalid("water must be within the selected character's native range");
        }
    }
    const bool changed = before != static_cast<float>(requested);
    status->water_now[character_id] = static_cast<float>(requested);
    status->drink_next[character_id] = static_cast<s16>(requested);
    status->drink_step[character_id] = 0;
    if (g_thirst_baseline && g_thirst_status == status) {
        g_thirst_water[character_id] = status->water_now[character_id];
    }
    if (changed) {
        SharedRuntime().QueueEvent("game.player.thirst_changed",
                                   ChronicleThirstPayload(*status, character_id, before,
                                                          status->water_now[character_id], "api", "set"));
    }
    return {ApiCallStatus::Ok,
            {{"character", kCharacterIds[character_id]}, {"character_id", character_id},
             {"previous_water", before}, {"water", status->water_now[character_id]},
             {"max_water", maximum}, {"changed", changed}},
            {}};
}

std::string ItemStableId(int item_no);

float ChronicleThirstDrainPerFrame(const CDngStatusData &status, int character_id) {
    if (status.step_disable != 0 || status.water_drain_disable != 0) return 0.0f;
    if (character_id != status.cur_chara || status.water_now[character_id] <= 0.0f) return 0.0f;
    float drain = 0.003f * (1.0f + 0.2f * static_cast<float>(status.cur_georama));
    if (status.res_limit_zone_current == RES_LIMIT_ZONE_THIRST) drain *= 5.0f;
    if (NowWeaponHave != nullptr) {
        if ((NowWeaponHave->flags & 0x8) != 0) drain *= 0.8f;
        if ((NowWeaponHave->flags & 0x10) != 0) drain *= 2.0f;
    }
    return drain * g_player_tuning.thirst_drain;
}

Json ChronicleThirstPayload(const CDngStatusData &status, int character_id, float previous, float current,
                            std::string_view source, std::string_view reason) {
    const float maximum = status.water_max[character_id];
    const bool dried_out = previous > 0.0f && current <= 0.0f;
    return {{"character", kCharacterIds[character_id]},
            {"character_id", character_id},
            {"previous_water", previous},
            {"water", current},
            {"max_water", maximum},
            {"drain_per_frame", ChronicleThirstDrainPerFrame(status, character_id)},
            {"drained_out", current <= 0.0f},
            {"dried_out", dried_out},
            {"source", source},
            {"reason", reason},
            {"frame", SharedRuntime().Frame()}};
}

ApiCallResult GetThirst(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("player.thirst_get takes no arguments");
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    const int character_id = status->cur_chara;
    if (character_id < CHARA_TOAN || character_id >= CharaCount()) {
        return Failed("the active character id is not recognized");
    }
    if (!std::isfinite(status->water_now[character_id]) || !std::isfinite(status->water_max[character_id])) {
        return Failed("the native water gauge contains a non-finite value");
    }
    const bool drain_enabled = status->step_disable == 0 && status->water_drain_disable == 0 &&
                               status->water_now[character_id] > 0.0f;
    return {ApiCallStatus::Ok,
            {{"character", kCharacterIds[character_id]},
             {"character_id", character_id},
             {"water", status->water_now[character_id]},
             {"max_water", status->water_max[character_id]},
             {"drain_per_frame", ChronicleThirstDrainPerFrame(*status, character_id)},
             {"drain_enabled", drain_enabled},
             {"water_drain_disable", status->water_drain_disable != 0},
             {"step_disable", status->step_disable != 0},
             {"empty", status->water_now[character_id] <= 0.0f},
             {"frame", SharedRuntime().Frame()}},
            {}};
}

ApiCallResult SetThirst(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {water = number} or {restore = number}, each with optional character");
    }
    const Json &definition = args[0];
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "water" && field.key() != "restore" && field.key() != "character") {
            return Invalid("player.thirst_set supports only water, restore, and optional character");
        }
    }
    const auto water_it = definition.find("water");
    const auto restore_it = definition.find("restore");
    if ((water_it == definition.end()) == (restore_it == definition.end())) {
        return Invalid("player.thirst_set requires exactly one of water or restore");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    int character_id = status->cur_chara;
    if (!ReadPlayerTarget(definition, *status, character_id)) {
        return Invalid("character must be 'active' or a Chronicle playable character id");
    }
    if (character_id < CHARA_TOAN || character_id >= CharaCount()) {
        return Failed("the player water target is not recognized");
    }
    const float maximum = status->water_max[character_id];
    const float previous = status->water_now[character_id];
    if (!std::isfinite(maximum) || !std::isfinite(previous) || maximum < 0.0f || maximum > 32767.0f ||
        previous < 0.0f || previous > maximum) {
        return Failed("the native water gauge is outside the supported setter range");
    }
    const auto &value = water_it != definition.end() ? *water_it : *restore_it;
    if (!value.is_number()) return Invalid("water and restore must be finite numbers");
    const double requested = value.get<double>();
    if (!std::isfinite(requested) || std::abs(requested) > 32767.0) {
        return Invalid("water and restore must be finite numbers within the native gauge range");
    }
    float next = water_it != definition.end() ? static_cast<float>(requested)
                                               : previous + static_cast<float>(requested);
    next = std::clamp(next, 0.0f, maximum);
    const bool changed = next != previous;
    status->water_now[character_id] = next;
    status->drink_next[character_id] = static_cast<s16>(next);
    status->drink_step[character_id] = 0;
    if (g_thirst_baseline && g_thirst_status == status) {
        g_thirst_water[character_id] = next;
    }
    Json result = ChronicleThirstPayload(*status, character_id, previous, next,
                                         "api", water_it != definition.end() ? "set" : "restore");
    result["changed"] = changed;
    if (changed) {
        SharedRuntime().QueueEvent("game.player.thirst_changed", result);
    }
    return {ApiCallStatus::Ok, std::move(result), {}};
}

bool ChronicleAtlaPartContents(int dungeon_id, int atra_id, Json &contents) {
    if (atra_id < 0) return false;
    if (atra_id < 0x28) {
        const SV_EDIT_PARTS_INFO *plot = SaveData == nullptr ? nullptr : SaveData->GetEditPartsInfo(dungeon_id, atra_id);
        contents = {{"kind", "part"}, {"part_id", atra_id}, {"obtained", plot != nullptr && plot->obtained != 0},
                    {"placed_count", plot == nullptr ? 0 : plot->placed},
                    {"stock", plot == nullptr ? 0 : plot->stock}};
        return true;
    }
    const int chip_id = atra_id - 0x28;
    s16 *chips = SaveData == nullptr ? nullptr : SaveData->GetElemData(dungeon_id);
    bool collected = false;
    if (chips != nullptr) {
        for (int index = 0; index < 128; ++index) {
            if (chips[index] == chip_id) {
                collected = true;
                break;
            }
        }
    }
    contents = {{"kind", "chip"}, {"chip_id", chip_id}, {"obtained", collected}};
    return true;
}

ApiCallResult ListAtla(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("georama.atla_list takes no arguments");
    if (mode != GAME_MODE_DUNGEON || NowDngMap == nullptr) {
        return Failed("Atla inspection requires an active dungeon floor");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr || UserStatus == nullptr || static_cast<CUserStatus *>(status) != UserStatus) {
        return Failed("the game has no active dungeon save data");
    }
    if (selectMapNo < 0 || selectMapNo >= DUNGEON_COUNT) return Failed("the active dungeon id is not recognized");
    if (NowDngMap->atra_num < 0 || NowDngMap->atra_num > static_cast<int>(std::size(NowDngMap->atra))) {
        return Failed("the active floor's Atla table is outside the supported range");
    }
    Json spheres = Json::array();
    for (int slot = 0; slot < NowDngMap->atra_num; ++slot) {
        const ATRA_BOLL &ball = NowDngMap->atra[slot];
        if (ball.used == 0) continue;
        if (const ModAtla *atla = ModAtlaOf(ball.atra_no)) {
            // MoonMod: a mod's sphere (georama.atla_spawn)
            spheres.push_back({{"slot", slot}, {"open", true}, {"position", {{"x", ball.pos[0]}, {"y", ball.pos[1]}, {"z", ball.pos[2]}}},
                               {"contents", {{"kind", "mod"}, {"id", atla->id}, {"owner", atla->owner}, {"name", atla->name}}}});
            continue;
        }
        if (ball.atra_no < 0 || ball.atra_no >= 100) {
            return Failed("the active floor contains an Atla slot index outside the native registry");
        }
        const ATRA_SAVE &registry = status->atra_registry[selectMapNo][ball.atra_no];
        Json contents = nullptr;
        ChronicleAtlaPartContents(selectMapNo, registry.id, contents);
        Json entry = {{"slot", slot}, {"native_atra_no", ball.atra_no}, {"open", true},
                      {"position", {{"x", ball.pos[0]}, {"y", ball.pos[1]}, {"z", ball.pos[2]}}},
                      {"atra_id", registry.id}, {"registry_floor", registry.floor},
                      {"floor_id", ball.zone_floor > 0 ? ball.zone_floor - 1 : (UserStatus != nullptr ? UserStatus->cur_floor : 0)},
                      {"registry_refcount", registry.refcount}, {"contents", std::move(contents)}};
        spheres.push_back(std::move(entry));
    }
    return {ApiCallStatus::Ok,
            {{"dungeon_id", selectMapNo},
             {"dungeon", kNativeDungeons[selectMapNo].id},
             {"floor_id", status->cur_floor},
             {"back_floor", BtUraDongeon != 0},
             {"count", spheres.size()},
             {"spheres", std::move(spheres)}},
            {}};
}

// MoonMod: a mod's own Atla sphere on the active floor, {id, name}: put on a free spot as the floor's Atla are,
// opened by Toan like them, and holding nothing of the game's; game.georama.atla_collected tells the mod {id, owner,
// name} when it is picked up. It lasts for the floor; the mod puts it again on a later visit if it still wants it.
ApiCallResult SpawnModAtla(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("id") || !args[0]["id"].is_string() ||
        !args[0].contains("name") || !args[0]["name"].is_string()) {
        return Invalid("expected {id = string, name = string, position = optional {x, y, z}}");
    }
    // position: where it floats (beside another sphere, say); without it, a free spot as the floor's own are put
    float at[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const bool placed = args[0].contains("position");
    if (placed) {
        const Json &p = args[0]["position"];
        if (!p.is_object() || !p.contains("x") || !p.contains("z") || !p["x"].is_number() || !p["z"].is_number() ||
            (p.contains("y") && !p["y"].is_number())) {
            return Invalid("position must be {x, y?, z} numbers");
        }
        at[0] = p["x"].get<float>();
        at[1] = p.value("y", 0.0f);
        at[2] = p["z"].get<float>();
    }
    const std::string id = args[0]["id"].get<std::string>();
    const std::string name = args[0]["name"].get<std::string>();
    if (id.empty() || id.size() > 48 || name.empty() || name.size() > 40) {
        return Invalid("id must be 1-48 bytes and name 1-40 bytes");
    }
    if (mode != GAME_MODE_DUNGEON || NowDngMap == nullptr) return Failed("a mod's Atla needs an active dungeon floor");
    for (int slot = 0; slot < NowDngMap->atra_num && slot < static_cast<int>(std::size(NowDngMap->atra)); ++slot) {
        const ModAtla *atla = ModAtlaOf(NowDngMap->atra[slot].atra_no);
        if (atla != nullptr && NowDngMap->atra[slot].used != 0 && atla->owner == mod_id && atla->id == id) {
            return Failed("that Atla is already on this floor");
        }
    }
    const int index = RegisterModAtla(mod_id, id, name);
    if (index < 0) return Failed("there is no room for another mod Atla");
    int slot = -1;
    if (placed) {
        slot = NowDngMap->PutAtraBoll(at, kModAtlaBase + index);
    } else {
        slot = NowDngMap->PutAtraBollAnywhere(kModAtlaBase + index);
    }
    if (slot < 0) return Failed("the floor has no room or no free spot for another Atla");
    const ATRA_BOLL &ball = NowDngMap->atra[slot];
    return {ApiCallStatus::Ok,
            {{"id", id}, {"slot", slot}, {"position", {{"x", ball.pos[0]}, {"y", ball.pos[1]}, {"z", ball.pos[2]}}}},
            {}};
}

ApiCallResult CollectAtla(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("slot")) {
        return Invalid("expected {slot = 0..39}");
    }
    int slot = -1;
    if (!ReadBoundedInt(args[0], "slot", 0, static_cast<int>(sizeof(CDungeonMap::atra) / sizeof(ATRA_BOLL)) - 1, slot)) return Invalid("slot must be an integer from 0 to 39");
    if (mode != GAME_MODE_DUNGEON || NowDngMap == nullptr) {
        return Failed("Atla collection requires an active dungeon floor");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr || UserStatus == nullptr || static_cast<CUserStatus *>(status) != UserStatus) {
        return Failed("the game has no active dungeon save data");
    }
    if (status->cur_chara != CHARA_TOAN) {
        return Failed("only Toan can collect Atla in Chronicle");
    }
    if (selectMapNo < 0 || selectMapNo >= DUNGEON_COUNT) return Failed("the active dungeon id is not recognized");
    if (slot < 0 || slot >= NowDngMap->atra_num || slot >= static_cast<int>(std::size(NowDngMap->atra))) {
        return Failed("slot does not identify an Atla sphere on the active floor");
    }
    ATRA_BOLL &ball = NowDngMap->atra[slot];
    if (ball.used == 0) return Failed("the selected Atla sphere is no longer open");
    if (ball.atra_no < 0 || ball.atra_no >= 100) {
        return Failed("the selected Atla slot index is outside the native registry (a mod's sphere is picked up by Toan)");
    }
    const int atra_id = status->atra_registry[selectMapNo][ball.atra_no].id;
    if (atra_id < 0 || atra_id > 0x28 + 127) return Failed("the selected Atla has no valid part or chip identity");
    Json before = nullptr;
    ChronicleAtlaPartContents(selectMapNo, atra_id, before);
    // (an atla laid in a zone of another floor is that floor's: dungeons.set_zones source_floor)
    getAtraToSaveData(atra_id, ball.atra_no, SaveData, selectMapNo, ball.zone_floor > 0 ? ball.zone_floor - 1 : status->cur_floor);
    for (int event = 0; event < DNG_EVENT_MAX; ++event) {
        if (NowDngMap->events[event].kind == DNG_EVENT_ATRA && NowDngMap->events[event].index == slot) {
            NowDngMap->events[event].kind = DNG_EVENT_NONE;
            break;
        }
    }
    ball.used = 0;
    Json after = nullptr;
    ChronicleAtlaPartContents(selectMapNo, atra_id, after);
    return {ApiCallStatus::Ok,
            {{"slot", slot},
             {"native_atra_no", ball.atra_no},
             {"atra_id", atra_id},
             {"dungeon_id", selectMapNo},
             {"dungeon", kNativeDungeons[selectMapNo].id},
             {"floor_id", status->cur_floor},
             {"contents_before", std::move(before)},
             {"contents_after", std::move(after)},
             {"collected", true},
             {"changed", true}},
            {}};
}

ApiCallResult GetGeoramaSatisfaction(const Json &args) {
    if (!args.is_array() || args.size() > 1 || (args.size() == 1 && !args[0].is_object())) {
        return Invalid("expected no arguments or {town_id = integer from 0 to 4}");
    }
    if (SaveData == nullptr) return Failed("the game has no active save data");
    int only_town = -1;
    if (!args.empty()) {
        if (args[0].size() != 1 || !args[0].contains("town_id")) {
            return Invalid("expected {town_id = integer from 0 to 4}");
        }
        if (!ReadBoundedInt(args[0], "town_id", 0, TOWN_COUNT - 1, only_town)) {
            return Invalid("town_id must be an integer from 0 to 4");
        }
    }
    static constexpr std::array<const char *, TOWN_COUNT> kTownNames = {
        "Norune Village", "Matataki Village", "Queens", "Muska Lacka", "Yellow Drops"};
    Json towns = Json::array();
    for (int town_id = 0; town_id < TOWN_COUNT; ++town_id) {
        if (only_town >= 0 && town_id != only_town) continue;
        const SV_GEORAMA_DATA *town = SaveData->GetGrdData(town_id);
        if (town == nullptr || town->request_count < 0 || town->request_count > 24) {
            return Failed("the saved Georama town data is outside the supported range");
        }
        int completed = 0;
        Json requests = Json::array();
        for (int request = 0; request < town->request_count; ++request) {
            const bool done = town->request_complete[request] != 0;
            if (done) ++completed;
            requests.push_back({{"request_id", request}, {"completed", done}});
        }
        Json plots = Json::array();
        int placed_parts = 0;
        for (int plot_id = 0; plot_id < 24; ++plot_id) {
            const SV_EDIT_PARTS_INFO &plot = town->parts_info[plot_id];
            placed_parts += std::max(0, static_cast<int>(plot.placed));
            Json elements = Json::array();
            for (int element_id = 0; element_id < 6; ++element_id) {
                elements.push_back(plot.element_enabled[element_id] != 0);
            }
            plots.push_back({{"plot_id", plot_id}, {"obtained", plot.obtained != 0},
                             {"placed_count", plot.placed}, {"stock", plot.stock},
                             {"elements", std::move(elements)}});
        }
        const int percent = town->request_count == 0
                                ? 0
                                : static_cast<int>((completed * 100LL + town->request_count / 2) /
                                                   town->request_count);
        towns.push_back({{"town_id", town_id},
                         {"id", "chronicle:town:" + std::to_string(town_id)},
                         {"name", kTownNames[town_id]},
                         {"request_count", town->request_count},
                         {"completed_requests", completed},
                         {"satisfaction_percent", std::clamp(percent, 0, 100)},
                         {"placed_part_count", placed_parts},
                         {"requests", std::move(requests)},
                         {"plots", std::move(plots)}});
    }
    return {ApiCallStatus::Ok,
            {{"derivation", "satisfaction_percent is the rounded share of saved native Georama requests completed"},
             {"towns", std::move(towns)}},
            {}};
}

bool ChronicleBackFloorAvailable(int dungeon_id) {
    return dungeon_id >= 0 && dungeon_id < DUNGEON_COUNT && BtUraEnemyLayoutList[dungeon_id] != nullptr;
}

ApiCallResult GetBackFloor(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("dungeons.back_floor_get takes no arguments");
    if (mode != GAME_MODE_DUNGEON || NowDngMap == nullptr) {
        return Failed("back-floor inspection requires an active dungeon floor");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active dungeon save data");
    if (selectMapNo < 0 || selectMapNo >= DUNGEON_COUNT) return Failed("the active dungeon id is not recognized");
    const int key_item_no = NowDngMap->gate_key_no;
    const bool key_held = key_item_no > 0 && status->CheckItemGet(key_item_no) > 0;
    const int key_remaining = key_item_no > 0 ? status->CheckItemGet(key_item_no) : 0;
    return {ApiCallStatus::Ok,
            {{"active", BtUraDongeon != 0},
             {"available", ChronicleBackFloorAvailable(selectMapNo)},
             {"unlocked", key_item_no <= 0 || key_held},
             {"dungeon_id", selectMapNo},
             {"dungeon", kNativeDungeons[selectMapNo].id},
             {"floor_id", status->cur_floor},
             {"gate_key", {{"native_item_no", key_item_no},
                           {"id", key_item_no > 0 ? ItemStableId(key_item_no) : ""},
                           {"held", key_held},
                           {"count", key_remaining}}},
             {"pending", g_pending_back_floor_request},
             {"frame", SharedRuntime().Frame()}},
            {}};
}

ApiCallResult EnterBackFloor(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("dungeons.back_floor_enter takes no arguments");
    if (mode != GAME_MODE_DUNGEON || NowDngMap == nullptr) {
        return Failed("back-floor entry requires an active dungeon floor");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active dungeon save data");
    if (selectMapNo < 0 || selectMapNo >= DUNGEON_COUNT) return Failed("the active dungeon id is not recognized");
    if (!ChronicleBackFloorAvailable(selectMapNo)) {
        return Failed("the active dungeon has no native back-floor enemy layout");
    }
    if (BtUraDongeon != 0) {
        return {ApiCallStatus::Ok,
                {{"dungeon_id", selectMapNo}, {"floor_id", status->cur_floor}, {"active", true},
                 {"queued", false}, {"changed", false}, {"key_consumed", false}},
                {}};
    }
    g_pending_back_floor_request = true;
    g_pending_back_floor_dungeon = selectMapNo;
    return {ApiCallStatus::Ok,
            {{"dungeon_id", selectMapNo}, {"floor_id", status->cur_floor}, {"active", false},
             {"queued", true}, {"changed", true}, {"key_consumed", false},
             {"note", "the native back-floor request is applied when the active event script reaches its next boundary"}},
            {}};
}

ApiCallResult ListParty(const Json &args) {
    if (!args.is_array() || !args.empty()) {
        return Invalid("party.list takes no arguments");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) {
        return Failed("the game has no active save data");
    }
    const int party_size = status->GetPartySize();
    if (party_size < 0 || party_size > static_cast<int>(kCharacterIds.size())) {
        return Failed("the saved party size is out of range");
    }
    Json result = Json::array();
    for (int character_id = 0; character_id < CharaCount(); ++character_id) {
        if (InParty(*status, character_id)) result.push_back(CharacterJson(*status, character_id));
    }
    return {ApiCallStatus::Ok, std::move(result), {}};
}

// MoonMod: a registered character joins a story party, or leaves it, apart from its size (playable_chara.hpp's
// CharaJoined): {character = id}. The six join by the story's own events; a party past six (the Gauntlet's) holds him
// by its size and he cannot leave it here.
ApiCallResult SetPartyJoined(const Json &args, bool join) {
    const char *name = join ? "party.join" : "party.leave";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("character")) {
        return Invalid(std::string("expected {character = <registered character id>} for ") + name);
    }
    int target_id = -1;
    if (!ReadCharacterId(args[0]["character"], target_id)) return Invalid("character must name a playable character");
    if (target_id < SAVE_CHARA_COUNT) {
        return Invalid("only a registered character joins or leaves this way; the six join by the story's events");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    if (!join) {
        if (target_id < status->party_size) return Failed("the character is in the party by its size, not by joining");
        if (status->cur_chara == target_id) return Failed("the character leads the party; switch to another first");
    }
    const bool was = CharaJoined(target_id);
    SetCharaJoined(target_id, join);
    return {ApiCallStatus::Ok,
            {{"character", kCharacterIds[target_id]}, {"joined", join}, {"changed", was != join},
             {"party_count", PartyCount(*status)}},
            {}};
}

ApiCallResult GetActivePartyMember(const Json &args) {
    if (!args.is_array() || !args.empty()) {
        return Invalid("party.active takes no arguments");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) {
        return Failed("the game has no active save data");
    }
    const int character_id = status->cur_chara;
    if (character_id < CHARA_TOAN || character_id >= CharaCount()) {
        return Failed("the active character id is not recognized");
    }
    return {ApiCallStatus::Ok, CharacterJson(*status, character_id), {}};
}

bool TownCharacterAvailable(int character_id) {
    if (character_id == CHARA_TOAN) return true;
    CDngStatusData *status = CurrentStatus();
    return status != nullptr && character_id > CHARA_TOAN && character_id <= CHARA_OSMOND &&
           status->max_hp[character_id] > 0;
}

ApiCallResult GetTownActivePartyMember(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("party.town_active takes no arguments");
    if (mode != GAME_MODE_EDIT || MapNo < 0 || MapNo >= TOWN_COUNT) {
        return Failed("town character selection requires an active town scene");
    }
    if (!TownCharacterAvailable(g_town_character)) g_town_character = CHARA_TOAN;
    return {ApiCallStatus::Ok, {{"id", kCharacterIds[g_town_character]}, {"active", kCharacterIds[g_town_character]}}, {}};
}

ApiCallResult SetTownActivePartyMember(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("id")) {
        return Invalid("expected {id = toan|xiao|goro|ruby|ungaga|osmond|<registered id>}");
    }
    int target_id = -1;
    if (!ReadCharacterId(args[0], target_id)) {
        return Invalid("id must name a Chronicle playable character: toan, xiao, goro, ruby, ungaga, osmond, or a registered one");
    }
    if (mode != GAME_MODE_EDIT || GameMode != ED_MODE_WALK || MapNo < 0 || MapNo >= TOWN_COUNT) {
        return Failed("town character selection requires an active town scene");
    }
    if (!TownCharacterAvailable(target_id)) return Failed("the selected character has not joined the native party");
    const int previous_id = g_town_character;
    g_town_character = target_id;
    g_town_character_reload = target_id != g_town_loaded_character;
    return {ApiCallStatus::Ok,
            {{"previous", kCharacterIds[previous_id]}, {"active", kCharacterIds[target_id]},
             {"changed", previous_id != target_id}}, {}};
}

ApiCallResult SetActivePartyMember(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("id")) {
        return Invalid("expected {id = toan|xiao|goro|ruby|ungaga|osmond|<registered id>}");
    }
    int target_id = -1;
    if (!ReadCharacterId(args[0], target_id)) {
        return Invalid("id must name a Chronicle playable character: toan, xiao, goro, ruby, ungaga, osmond, or a registered one");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    const int party_size = status->GetPartySize();
    if (party_size < 1 || party_size > static_cast<int>(kCharacterIds.size())) {
        return Failed("the saved party size is out of range");
    }
    if (!InParty(*status, target_id)) return Failed("the selected character is not in the native party");
    const int previous_id = status->cur_chara;
    if (previous_id < CHARA_TOAN || previous_id >= CharaCount() || !InParty(*status, previous_id)) {
        return Failed("the active character is not a member of the native party");
    }
    if (previous_id == target_id) {
        return {ApiCallStatus::Ok,
                {{"previous", kCharacterIds[previous_id]}, {"active", kCharacterIds[target_id]}, {"changed", false}},
                {}};
    }
    if (!IsActiveDungeonStatus(status)) {
        return Failed("Chronicle character switching requires an active dungeon scene");
    }
    if (BtActStatus.action_on != 0 || BtActStatus.movement_locked != 0) {
        return Failed("the native game currently prevents character switching during this action");
    }
    // A scene (a floor's entry, a gate) picks its motions by the character it began with: switching under it leaves
    // the new character on motions it doesn't have, and the scene waits on them for good.
    if (gameTask != GAME_TASK_PLAY) {
        return Failed("the native game currently prevents character switching while a scene plays");
    }
    if (status->res_limit_zone_current >= 0 && status->res_limit_zone_current < 6) {
        return Failed("the native game currently prevents character switching in this restricted zone");
    }
    if (previous_id == CHARA_OSMOND && BtActStatus.ground_kind == 10) {
        return Failed("the native game currently prevents Osmond from switching on this ground type");
    }
    if (status->hp[target_id] <= 0 ||
        (status->GetActiveCharaStatus(target_id) & AILMENT_KNOCKED_OUT) != 0) {
        return Failed("the selected character is knocked out or has no health");
    }
    if (g_character_switch_routine == nullptr) return Failed("the native character switch routine is unavailable");
    g_character_switch_routine(target_id, 0);
    if (status->cur_chara != target_id) return Failed("the native game did not complete the character switch");
    return {ApiCallStatus::Ok,
            {{"previous", kCharacterIds[previous_id]}, {"active", kCharacterIds[target_id]}, {"changed", true}},
            {}};
}

std::string ChronicleMapDirectory(int map_id) {
    const int directory_id = map_id - 10;
    char directory[16] = {};
    std::snprintf(directory, sizeof(directory), "gedit/s%02d", directory_id);
    return directory;
}

std::string ChronicleModLocationStableId(const ModLocation &location) {
    return "chronicle:location:" + location.id;
}

Json ModLocationJson(const ModLocation &location) {
    const int progress = SaveData->VisitMap(location.native_map_id, 0);
    return {{"id", ChronicleModLocationStableId(location)}, {"name", location.name}, {"kind", "field"},
            {"native_map_id", location.native_map_id}, {"frame", nullptr}, {"visited", progress > 0},
            {"native_progress", progress}, {"source", "mod_package"}, {"owner", location.owner},
            {"source_map_id", location.source_map_id}, {"status", "registered"}};
}

ApiCallResult RegisterLocation(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 3 ||
        !args[0].contains("id") || !args[0]["id"].is_string() || !args[0].contains("name") ||
        !args[0]["name"].is_string() || !args[0].contains("source_map_id")) {
        return Invalid("expected {id = local_id, name = display name, source_map_id = native field map id}");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "id" && field.key() != "name" && field.key() != "source_map_id") {
            return Invalid("locations.register received an unsupported field: " + field.key());
        }
    }
    std::string id;
    if (!QualifyModContentId(mod_id, args[0]["id"].get_ref<const std::string &>(), id, "location")) {
        return Invalid("id must be a local identifier owned by the calling mod");
    }
    const std::string &name = args[0]["name"].get_ref<const std::string &>();
    if (name.empty() || name.size() > 127 || name.find('\0') != std::string::npos ||
        std::ranges::any_of(name, [](unsigned char character) { return character < 0x20; })) {
        return Invalid("name must contain 1-127 printable UTF-8 bytes");
    }
    int source_map_id = -1;
    if (!ReadBoundedInt(args[0], "source_map_id", 0, 79, source_map_id)) {
        return Invalid("source_map_id must be a native Chronicle map id");
    }
    if (SaveData == nullptr) return Failed("the game has no active save data");

    int source_place = -1;
    for (int place = 0; place < WORLD_PLACE_COUNT; ++place) {
        if (MenuGrobalMapNoTbl[place] == source_map_id &&
            std::string_view(kNativeLocations[place].kind) == "field") {
            source_place = place;
            break;
        }
    }
    if (source_place < 0) return Failed("source_map_id must identify a built-in Chronicle field map");
    const std::string source_directory = ChronicleMapDirectory(source_map_id);
    if (!dc::data::DataDirectoryIndexed(source_directory)) {
        return Failed("the selected native field assets are not loaded by the game's data reader");
    }
    const auto existing = g_mod_locations.find(id);
    if (existing != g_mod_locations.end()) {
        const ModLocation &location = existing->second;
        if (location.owner != mod_id) return Failed("another mod owns this location id");
        if (location.name != name || location.source_map_id != source_map_id) {
            return Failed("location id is already registered with a different definition");
        }
        return {ApiCallStatus::Ok, ModLocationJson(location), {}};
    }
    if (g_mod_locations.size() >= static_cast<std::size_t>(kChronicleLastModMapId - kChronicleFirstModMapId + 1)) {
        return Failed("Chronicle's reserved mod map range is full (map ids 70-79)");
    }
    int native_map_id = -1;
    for (int candidate = kChronicleFirstModMapId; candidate <= kChronicleLastModMapId; ++candidate) {
        const bool used = std::ranges::any_of(g_mod_locations, [candidate](const auto &entry) {
            return entry.second.native_map_id == candidate;
        });
        if (!used && !dc::data::DataDirectoryIndexed(ChronicleMapDirectory(candidate))) {
            native_map_id = candidate;
            break;
        }
    }
    if (native_map_id < 0) {
        return Failed("Chronicle's reserved mod map range is full or occupied by native data");
    }

    std::string alias_error;
    if (!RegisterNativeResourcePathAlias(mod_id, ChronicleMapDirectory(native_map_id), source_directory,
                                         8ull * 1024ull * 1024ull, alias_error)) {
        return Failed("could not bind the mod map directory: " + alias_error);
    }
    ModLocation location{.id = id, .owner = std::string(mod_id), .name = name,
                         .native_map_id = native_map_id, .source_map_id = source_map_id};
    auto [registered, inserted] = g_mod_locations.emplace(id, std::move(location));
    if (!inserted) return Failed("location id is already registered");
    return {ApiCallStatus::Ok, ModLocationJson(registered->second), {}};
}

Json NativeLocationJson(int place) {
    const NativeLocationDefinition &definition = kNativeLocations[place];
    const int map_id = MenuGrobalMapNoTbl[place];
    const bool is_dungeon = map_id >= 200;
    const int progress = is_dungeon ? SaveData->QuestDungeon(map_id - 200, 0)
                                    : SaveData->VisitMap(map_id, 0);
    return {{"id", definition.id}, {"name", definition.name}, {"kind", definition.kind},
            {"native_map_id", map_id}, {"frame", TownOrDngPos[place].frame},
            {"visited", progress > 0}, {"native_progress", progress}, {"source", "chronicle"}};
}

const ModLocation *FindModLocationByMapId(int map_id) {
    const auto found = std::ranges::find_if(g_mod_locations, [map_id](const auto &entry) {
        return entry.second.native_map_id == map_id;
    });
    return found == g_mod_locations.end() ? nullptr : &found->second;
}

bool IsSettlementOrRegisteredLocationMap(int map_id) {
    if (FindModLocationByMapId(map_id) != nullptr) return true;
    for (int place = 0; place < WORLD_PLACE_COUNT; ++place) {
        if (std::string_view(kNativeLocations[place].kind) == "settlement" &&
            MenuGrobalMapNoTbl[place] == map_id) {
            return true;
        }
    }
    return false;
}

ApiCallResult ListLocations(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("locations.list takes no arguments");
    if (SaveData == nullptr) return Failed("the game has no active save data");
    Json result = Json::array();
    for (int place = 0; place < WORLD_PLACE_COUNT; ++place) result.push_back(NativeLocationJson(place));
    for (const auto &[id, location] : g_mod_locations) {
        (void)id;
        result.push_back(ModLocationJson(location));
    }
    return {ApiCallStatus::Ok, std::move(result), {}};
}

ApiCallResult GetLocation(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("id")) {
        return Invalid("expected {id = built-in location id}");
    }
    std::string id;
    if (!ReadId(args[0]["id"], id)) return Invalid("expected a stable built-in location id");
    if (SaveData == nullptr) return Failed("the game has no active save data");
    for (const auto &[qualified_id, location] : g_mod_locations) {
        const std::string stable_id = ChronicleModLocationStableId(location);
        const std::size_t separator = qualified_id.rfind(':');
        const std::string_view local_id = separator == std::string::npos
                                              ? std::string_view(qualified_id)
                                              : std::string_view(qualified_id).substr(separator + 1);
        if (id == stable_id || id == qualified_id || (location.owner == mod_id && id == local_id)) {
            return {ApiCallStatus::Ok, ModLocationJson(location), {}};
        }
    }
    for (int place = 0; place < WORLD_PLACE_COUNT; ++place) {
        const std::string_view stable_id = kNativeLocations[place].id;
        const std::size_t separator = stable_id.rfind(':');
        const std::string_view local_id = separator == std::string_view::npos ? stable_id : stable_id.substr(separator + 1);
        if (id == stable_id || id == local_id) return {ApiCallStatus::Ok, NativeLocationJson(place), {}};
    }
    return Failed("no built-in Chronicle location has this id");
}

ApiCallResult TransitionWorld(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("id")) {
        return Invalid("expected {id = built-in Chronicle location id}");
    }
    std::string requested_id;
    if (!ReadId(args[0]["id"], requested_id)) {
        return Invalid("expected a stable Chronicle location or registered dungeon-floor id");
    }
    if (SaveData == nullptr) return Failed("the game has no active save data");

    const ModDungeonFloor *requested_floor = nullptr;
    const ModDungeon *requested_dungeon = nullptr;
    for (const auto &[dungeon_id, dungeon] : g_mod_dungeons) {
        const auto floor = std::ranges::find(dungeon.floors, requested_id, &ModDungeonFloor::id);
        if (floor != dungeon.floors.end()) {
            requested_dungeon = &dungeon;
            requested_floor = &*floor;
            break;
        }
    }

    if (requested_dungeon != nullptr && requested_floor != nullptr) {
        const int current_map = MapNo;
        const bool active_town = mode == GAME_MODE_EDIT && IsSettlementOrRegisteredLocationMap(current_map);
        const bool active_dungeon = mode == GAME_MODE_DUNGEON;
        if (!active_town && !active_dungeon) {
            return Failed("a registered dungeon-floor transition requires an active town or dungeon scene");
        }
        if (NextMapNo >= 0) return Failed("a native map transition is already pending");

        CDngStatusData *status = CurrentStatus();
        if (active_dungeon && selectMapNo == requested_dungeon->native_dungeon_id && status != nullptr &&
            status->cur_floor == requested_floor->native_floor_id) {
            return {ApiCallStatus::Ok,
                    {{"id", requested_floor->id}, {"dungeon_id", requested_dungeon->id},
                     {"native_dungeon_id", requested_dungeon->native_dungeon_id},
                     {"native_floor_id", requested_floor->native_floor_id},
                     {"queued", false}, {"changed", false}},
                    {}};
        }

        g_pending_dungeon_floor_entry = PendingDungeonFloorEntry{
            requested_dungeon->native_dungeon_id, requested_floor->native_floor_id};
        MapJump(requested_dungeon->native_dungeon_id + 200, -1);
        return {ApiCallStatus::Ok,
                {{"id", requested_floor->id}, {"dungeon_id", requested_dungeon->id},
                 {"native_dungeon_id", requested_dungeon->native_dungeon_id},
                 {"native_floor_id", requested_floor->native_floor_id},
                 {"context", "dungeon_floor"}, {"queued", true}, {"changed", true}},
                {}};
    }

    const ModLocation *requested_location = nullptr;
    for (const auto &[qualified_id, location] : g_mod_locations) {
        const std::string stable_id = ChronicleModLocationStableId(location);
        const std::size_t separator = qualified_id.rfind(':');
        const std::string_view local_id = separator == std::string::npos
                                              ? std::string_view(qualified_id)
                                              : std::string_view(qualified_id).substr(separator + 1);
        if (requested_id == stable_id || requested_id == qualified_id ||
            (location.owner == mod_id && requested_id == local_id)) {
            requested_location = &location;
            break;
        }
    }
    if (requested_location != nullptr) {
        const int current_map = MapNo;
        const bool active_town = mode == GAME_MODE_EDIT && IsSettlementOrRegisteredLocationMap(current_map);
        const bool active_dungeon = mode == GAME_MODE_DUNGEON;
        if (!active_town && !active_dungeon) {
            return Failed("a mod location transition requires an active settlement, mod location, or dungeon scene");
        }
        if (NextMapNo >= 0) return Failed("a native map transition is already pending");
        const ModLocation &location = *requested_location;
        if (mode == GAME_MODE_EDIT && current_map == location.native_map_id) {
            return {ApiCallStatus::Ok,
                    {{"id", ChronicleModLocationStableId(location)}, {"native_map_id", location.native_map_id},
                     {"source_map_id", location.source_map_id}, {"queued", false}, {"changed", false}},
                    {}};
        }
        MapJump(location.native_map_id, -1);
        return {ApiCallStatus::Ok,
                {{"id", ChronicleModLocationStableId(location)}, {"native_map_id", location.native_map_id},
                 {"source_map_id", location.source_map_id}, {"context", "mod_location"},
                 {"queued", true}, {"changed", true}},
                {}};
    }

    int destination_place = -1;
    for (int place = 0; place < WORLD_PLACE_COUNT; ++place) {
        const std::string_view stable_id = kNativeLocations[place].id;
        const std::size_t separator = stable_id.rfind(':');
        const std::string_view local_id = separator == std::string_view::npos ? stable_id : stable_id.substr(separator + 1);
        if (requested_id == stable_id || requested_id == local_id) {
            destination_place = place;
            break;
        }
    }
    if (destination_place < 0) return Failed("no built-in Chronicle location or registered mod floor has this id");
    if (std::string_view(kNativeLocations[destination_place].kind) != "settlement") {
        return Failed("world.transition accepts native settlement locations or registered mod dungeon floors");
    }

    const int current_map = MapNo;
    const bool active_town = mode == GAME_MODE_EDIT && IsSettlementOrRegisteredLocationMap(current_map);
    const bool active_dungeon = mode == GAME_MODE_DUNGEON;
    if (!active_town && !active_dungeon) {
        return Failed("a native settlement transition requires an active town or dungeon scene");
    }
    if (NextMapNo >= 0) return Failed("a native map transition is already pending");

    const int destination_map = MenuGrobalMapNoTbl[destination_place];
    if (active_town && current_map == destination_map) {
        return {ApiCallStatus::Ok,
                {{"id", kNativeLocations[destination_place].id}, {"native_map_id", destination_map},
                 {"queued", false}, {"changed", false}},
                {}};
    }

    MapJump(destination_map, -1);
    return {ApiCallStatus::Ok,
            {{"id", kNativeLocations[destination_place].id}, {"native_map_id", destination_map},
             {"context", "settlement"}, {"queued", true}, {"changed", true}},
            {}};
}

bool ReadChronicleStatus(const Json &args, std::string &name, int &ailment) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return false;
    const Json &request = args[0];
    const auto character = request.find("character");
    if (character != request.end() && (!character->is_string() || character->get<std::string>() != "active")) {
        return false;
    }
    const auto status = request.find("status");
    if (status == request.end() || !status->is_string()) return false;
    name = status->get<std::string>();
    if (name == "freeze") ailment = AILMENT_FREEZE;
    else if (name == "stamina") ailment = AILMENT_STAMINA;
    else if (name == "poison") ailment = AILMENT_POISON;
    else if (name == "curse") ailment = AILMENT_CURSE;
    else if (name == "goo") ailment = AILMENT_GOO;
    else return false;
    return true;
}

ApiCallResult AddPlayerStatus(const Json &args) {
    std::string name;
    int ailment = 0;
    if (!ReadChronicleStatus(args, name, ailment)) {
        return Invalid("expected {status = freeze|stamina|poison|curse|goo, character = optional 'active'}");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    const int character_id = status->cur_chara;
    if (character_id < CHARA_TOAN || character_id >= CharaCount()) return Failed("the active character id is not recognized");
    if (UserStatus == nullptr || static_cast<CUserStatus *>(status) != UserStatus || UserStatus->cur_chara != character_id) {
        return Failed("the native active-character status target is unavailable");
    }
    BtSetStatusErr(ailment);
    const bool applied = (UserStatus->ailments[character_id] & ailment) != 0;
    if (!applied) return Failed("the native status rules rejected this effect for the active character");
    return {ApiCallStatus::Ok,
            {{"status", name}, {"character", kCharacterIds[character_id]}, {"applied", true},
             {"active", true}, {"duration_frames", UserStatus->ailment_frames[character_id]}},
            {}};
}

ApiCallResult RemovePlayerStatus(const Json &args) {
    std::string name;
    int ailment = 0;
    if (!ReadChronicleStatus(args, name, ailment)) {
        return Invalid("expected {status = freeze|stamina|poison|curse|goo, character = optional 'active'}");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    const int character_id = status->cur_chara;
    if (character_id < CHARA_TOAN || character_id >= CharaCount()) return Failed("the active character id is not recognized");
    if (UserStatus == nullptr || static_cast<CUserStatus *>(status) != UserStatus || UserStatus->cur_chara != character_id) {
        return Failed("the native active-character status target is unavailable");
    }
    const bool removed = (UserStatus->ailments[character_id] & ailment) != 0;
    UserStatus->ailments[character_id] &= ~ailment;
    constexpr int kTimedAilments = AILMENT_FREEZE | AILMENT_STAMINA | AILMENT_POISON | AILMENT_GOO;
    if ((UserStatus->ailments[character_id] & kTimedAilments) == 0) UserStatus->ailment_frames[character_id] = 0;
    return {ApiCallStatus::Ok,
            {{"status", name}, {"character", kCharacterIds[character_id]}, {"removed", removed},
             {"active", (UserStatus->ailments[character_id] & ailment) != 0},
             {"duration_frames", UserStatus->ailment_frames[character_id]}},
            {}};
}

bool ReadCurrencyAmount(const Json &args, int &amount) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return false;
    const auto amount_it = args[0].find("amount");
    if (amount_it == args[0].end() || !amount_it->is_number_integer()) return false;
    if (amount_it->is_number_unsigned()) {
        const unsigned long long value = amount_it->get<unsigned long long>();
        if (value < 1 || value > 32767) return false;
        amount = static_cast<int>(value);
        return true;
    }
    const long long value = amount_it->get<long long>();
    if (value < 1 || value > 32767) return false;
    amount = static_cast<int>(value);
    return true;
}

ApiCallResult GetEconomy(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("economy.get takes no arguments");
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    return {ApiCallStatus::Ok, {{"currency", "gilda"}, {"amount", status->money}, {"max_amount", 65535}}, {}};
}

ApiCallResult AddEconomy(const Json &args) {
    int amount = 0;
    if (!ReadCurrencyAmount(args, amount)) return Invalid("expected {amount = integer from 1 to 32767}");
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    const int before = status->money;
    const int after = std::min(65535, before + amount);
    status->money = static_cast<u16>(after);
    return {ApiCallStatus::Ok,
            {{"currency", "gilda"}, {"previous", before}, {"amount", after}, {"added", after - before}}, {}};
}

ApiCallResult SpendEconomy(const Json &args) {
    int amount = 0;
    if (!ReadCurrencyAmount(args, amount)) return Invalid("expected {amount = integer from 1 to 32767}");
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    const int before = status->money;
    if (amount > before) return Failed("the wallet does not contain enough currency");
    const int after = before - amount;
    status->money = static_cast<u16>(after);
    return {ApiCallStatus::Ok,
            {{"currency", "gilda"}, {"previous", before}, {"amount", after}, {"spent", before - after}}, {}};
}

ApiCallResult GetWorld(const Json &args) {
    if (!args.is_array() || !args.empty()) {
        return Invalid("world.get takes no arguments");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) {
        return Failed("the game has no active save data");
    }
    const int area_id = status->cur_georama;
    const bool in_dungeon = mode == GAME_MODE_DUNGEON;
    const ModLocation *active_location = mode == GAME_MODE_EDIT ? FindModLocationByMapId(MapNo) : nullptr;
    const bool valid_area_id = area_id >= 0 && area_id <= 6;
    if (!valid_area_id && active_location == nullptr) {
        return Failed("the saved world area id is out of range");
    }
    const float native_time = SaveData->GetNowTime();
    if (!std::isfinite(native_time) || native_time < 0.0f || native_time >= 12.0f) {
        return Failed("the saved world time is out of range");
    }
    Json result = { {"context", in_dungeon ? "dungeon" : active_location != nullptr ? "location" : "georama"},
                    {"day", SaveData->GetDay()},
                    {"area_id", valid_area_id ? Json(area_id) : Json(nullptr)},
                    {"map_id", in_dungeon ? selectMapNo : active_location != nullptr ? MapNo : area_id},
                    {"sub_map_id", nullptr},
                    {"location_id", active_location == nullptr ? Json(nullptr) : Json(ChronicleModLocationStableId(*active_location))},
                    {"dungeon_id", in_dungeon ? Json(selectMapNo) : Json(nullptr)},
                    {"floor_id", in_dungeon && status->cur_floor >= 0 ? Json(status->cur_floor) : Json(nullptr)},
                    {"time", {{"fraction", native_time / 12.0f}, {"native_time", native_time},
                               {"native_cycle_hours", 12}}} };
    if (in_dungeon && NowDngMap != nullptr) {
        result["room_count"] = NowDngMap->room_num;
        result["floor_seed"] = NowDngMap->map_seed;
        result["back_floor"] = NowDngMap == &UraDungeonMap;
        result["room_bonus_percent"] = DngRoomBonusPercent;
        result["monster_bonus_percent"] = DngMonsterBonusPercent;
        result["chest_bonus_percent"] = DngChestBonusPercent;
    }
    if (mode == GAME_MODE_EDIT && MapNo >= 0 && MapNo < TOWN_COUNT) {
        // A town: its map, and whether the player is in Georama's edit mode (co-op sends a town once it's left).
        result["town_map"] = MapNo;
        result["editing"] = GameMode == ED_MODE_GEORAMA || GameMode == ED_MODE_GEORAMA_MENU_INIT || GameMode == ED_MODE_GEORAMA_MENU;
    }
    return {ApiCallStatus::Ok, std::move(result), {}};
}

ApiCallResult ReadWorldLocationForEvents() {
    ApiCallResult result = GetWorld(Json::array());
    if (result.status == ApiCallStatus::Ok) return result;

    // GetWorld needs save-backed time and area data. During scene loading those
    // fields can be briefly unavailable while the native loop/map globals are valid.
    if (mode == GAME_MODE_DUNGEON) {
        CDngStatusData *status = CurrentStatus();
        Json snapshot = {{"context", "dungeon"}, {"map_id", selectMapNo},
                         {"dungeon_id", selectMapNo},
                         {"floor_id", status != nullptr && status->cur_floor >= 0
                                          ? Json(status->cur_floor) : Json(nullptr)}};
        return {ApiCallStatus::Ok, std::move(snapshot), {}};
    }
    if (mode == GAME_MODE_EDIT) {
        const ModLocation *active_location = FindModLocationByMapId(MapNo);
        if (active_location != nullptr) {
            return {ApiCallStatus::Ok,
                    {{"context", "location"}, {"map_id", MapNo},
                     {"location_id", ChronicleModLocationStableId(*active_location)}}, {}};
        }
        CDngStatusData *status = CurrentStatus();
        const int area_id = status != nullptr && status->cur_georama >= 0 && status->cur_georama <= 6
                                ? status->cur_georama
                                : MapNo;
        return {ApiCallStatus::Ok, {{"context", "georama"}, {"map_id", area_id}}, {}};
    }
    return result;
}

ApiCallResult InspectNativeRuntime(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("debug.inspect takes no arguments");
    const char *scene_id = "other";
    switch (mode) {
        case GAME_MODE_TITLE: scene_id = "title"; break;
        case GAME_MODE_EDIT: scene_id = "edit"; break;
        case GAME_MODE_DUNGEON: scene_id = "dungeon"; break;
        case GAME_MODE_MENU: scene_id = "menu"; break;
        default: break;
    }
    const bool has_map = mode == GAME_MODE_EDIT || mode == GAME_MODE_DUNGEON;
    Json floor_id = nullptr;
    if (mode == GAME_MODE_DUNGEON) {
        CDngStatusData *status = CurrentStatus();
        if (status != nullptr && status->cur_floor >= 0) floor_id = status->cur_floor;
    }
    return {ApiCallStatus::Ok,
            {{"scene_id", scene_id}, {"native_mode", mode},
             {"native_map_id", has_map ? Json(MapNo) : Json(nullptr)}, {"native_floor_id", floor_id}},
            {}};
}

std::uint32_t GeoramaFloatBits(float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

std::string ChronicleGeoramaPlacementId(int town_id, int index, const SV_GRD_PART &part) {
    return "chronicle:georama-placement:" + std::to_string(town_id) + ":" + std::to_string(index) + ":" +
           std::to_string(part.part_id) + ":" + std::to_string(part.rot_y) + ":" +
           std::to_string(GeoramaFloatBits(part.pos_x)) + ":" + std::to_string(GeoramaFloatBits(part.pos_y)) + ":" +
           std::to_string(GeoramaFloatBits(part.pos_z));
}

int ChronicleSavedGeoramaPartId(const CEditGround &ground, const CMapParts &part) {
    int saved_id = part.handle;
    if (part.subtype == MAP_PARTS_SUBTYPE_ROAD || part.subtype == MAP_PARTS_SUBTYPE_RIVER ||
        part.subtype == MAP_PARTS_SUBTYPE_BRIDGE) {
        if (ground.plot_parts == nullptr) return saved_id;
        for (int index = 0; index < 24; ++index) {
            if (part.subtype == ground.plot_parts[index].subtype) {
                saved_id = ground.plot_parts[index].handle;
                break;
            }
        }
    }
    return saved_id;
}

ApiCallResult GetGeorama(const Json &args) {
    if (!args.is_array() || args.size() > 1 || (args.size() == 1 && !args[0].is_object())) {
        return Invalid("expected no arguments or {town_id = integer from 0 to 5}");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");

    int town_id = status->cur_georama;
    if (!args.empty()) {
        const Json &query = args[0];
        if (query.size() != 1 || !query.contains("town_id") || !query["town_id"].is_number_integer()) {
            return Invalid("expected {town_id = integer from 0 to 5}");
        }
        const std::int64_t requested = query["town_id"].get<std::int64_t>();
        if (requested < 0 || requested >= 6) return Invalid("town_id must be an integer from 0 to 5");
        town_id = static_cast<int>(requested);
    } else if (town_id < 0 || town_id >= 6) {
        return Failed("there is no active Georama town");
    }

    if (mode == GAME_MODE_EDIT && NowEditMap == town_id && pEditGround != nullptr &&
        pEditGround->map_no == town_id) {
        EditSave();
    }

    const SV_GEORAMA_DATA *town = SaveData->GetGrdData(town_id);
    if (town == nullptr || town->request_count < 0 || town->request_count > 24) {
        return Failed("the saved Georama town data is outside the supported range");
    }
    int part_count = 0;
    const SV_GRD_PART *parts = SaveData->GetParts(town_id, &part_count);
    if (parts == nullptr || part_count < 0 || part_count > 130) {
        return Failed("the saved Georama placement list is outside the supported range");
    }

    Json plots = Json::array();
    for (int plot_id = 0; plot_id < 24; ++plot_id) {
        const SV_EDIT_PARTS_INFO &plot = town->parts_info[plot_id];
        Json elements = Json::array();
        for (int element_id = 0; element_id < 6; ++element_id) {
            elements.push_back(plot.element_enabled[element_id] != 0);
        }
        plots.push_back({{"plot_id", plot_id},
                         {"completion_flags", plot.completion_flags},
                         {"obtained", plot.obtained != 0},
                         {"placed_count", plot.placed},
                         {"stock", plot.stock},
                         {"elements", std::move(elements)}});
    }

    Json placed_parts = Json::array();
    for (int index = 0; index < part_count; ++index) {
        const SV_GRD_PART &part = parts[index];
        if (part.part_id < 0 || !std::isfinite(part.pos_x) || !std::isfinite(part.pos_y) ||
            !std::isfinite(part.pos_z)) {
            return Failed("the saved Georama placement list contains an invalid record");
        }
        const int rotation = part.rot_y < 0 ? 3 : part.rot_y;
        const std::string placement_id = ChronicleGeoramaPlacementId(town_id, index, part);
        const auto mod_piece = g_mod_georama_piece_by_native_id.find(part.part_id);
        const std::string piece_id = mod_piece == g_mod_georama_piece_by_native_id.end()
                                         ? "chronicle:georama-part:" + std::to_string(part.part_id)
                                         : mod_piece->second;
        placed_parts.push_back({{"index", index},
                                {"part_id", part.part_id},
                                {"id", piece_id},
                                {"placement_id", placement_id},
                                {"rotation_quarter_turns", rotation},
                                {"position", {{"x", part.pos_x}, {"y", part.pos_y}, {"z", part.pos_z}}}});
    }

    Json result = {{"town_id", town_id},
                   {"id", "chronicle:town:" + std::to_string(town_id)},
                   {"request_count", town->request_count},
                   {"completed_requests", std::count_if(town->request_complete,
                                                         town->request_complete + town->request_count,
                                                         [](s32 value) { return value != 0; })},
                   {"plots", std::move(plots)},
                   {"placed_parts", std::move(placed_parts)}};
    if (mode == GAME_MODE_EDIT && NowEditMap == town_id && pEditGround != nullptr &&
        pEditGround->map_no == town_id) {
        Json placement_areas = Json::array();
        for (int area_index = 0; area_index < 4; ++area_index) {
            const CEditArea *area = pEditGround->areas[area_index];
            if (area == nullptr) break;
            if (area->width < 1 || area->width > 16 || area->height < 1 || area->height > 16 ||
                !std::isfinite(area->unit_size) || area->unit_size <= 0.0f ||
                !std::isfinite(area->unit_alt) || area->unit_alt <= 0.0f ||
                !std::isfinite(area->offset_x) || !std::isfinite(area->offset_y) || !std::isfinite(area->offset_z)) {
                return Failed("the active town has an invalid native placement area");
            }
            placement_areas.push_back({{"area_index", area_index}, {"width_cells", area->width},
                                       {"height_cells", area->height},
                                       {"origin", {{"x", area->offset_x}, {"y", area->offset_y}, {"z", area->offset_z}}},
                                       {"cell_size", area->unit_size}, {"height_step", area->unit_alt}});
        }
        Json native_pieces = Json::array();
        if (pEditGround->plot_parts != nullptr && pEditGround->parts_info == &EditPartsInfo) {
            for (int part_id = 0; part_id < 24; ++part_id) {
                CMapParts &native = pEditGround->plot_parts[part_id];
                const EDITPARTS_INFO *plot = EditPartsInfo.GetPartsInfo(part_id);
                if (native.handle < 0 || native.info == nullptr || plot == nullptr || plot->parts_no != part_id) continue;
                native_pieces.push_back({{"id", "chronicle:georama-part:" + std::to_string(part_id)},
                                         {"part_id", part_id}, {"loaded", true},
                                         {"subtype", native.subtype},
                                         {"width_cells", native.GetWidth()}, {"height_cells", native.GetHeight()},
                                         {"built_stock", plot->stock}, {"placed_count", plot->placed},
                                         {"available_built", std::max(0, plot->stock - plot->placed)}});
            }
        }
        Json mod_pieces = Json::array();
        for (const auto &[id, piece] : g_mod_georama_pieces) {
            if (piece.town_id >= 0 && piece.town_id != town_id) continue;
            const EDITPARTS_INFO *plot = EditPartsInfo.GetPartsInfo(piece.native_part_id);
            const bool loaded = pEditGround->plot_parts != nullptr && plot != nullptr &&
                                pEditGround->plot_parts[piece.native_part_id].handle == piece.native_part_id &&
                                pEditGround->plot_parts[piece.native_part_id].info != nullptr;
            mod_pieces.push_back({{"id", id}, {"name", piece.name},
                                  {"template_part_id", piece.template_part_id},
                                  {"part_id", piece.native_part_id}, {"stock", piece.stock},
                                  {"loaded", loaded}, {"available_built", loaded ? std::max(0, plot->stock - plot->placed) : 0},
                                  {"width_cells", loaded ? Json(pEditGround->plot_parts[piece.native_part_id].GetWidth()) : Json(nullptr)},
                                  {"height_cells", loaded ? Json(pEditGround->plot_parts[piece.native_part_id].GetHeight()) : Json(nullptr)}});
        }
        result["placement_areas"] = std::move(placement_areas);
        result["native_pieces"] = std::move(native_pieces);
        result["mod_pieces"] = std::move(mod_pieces);
    }
    return {ApiCallStatus::Ok, std::move(result), {}};
}

ApiCallResult PlaceGeorama(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 4 ||
        !args[0].contains("town_id") || !args[0].contains("part_id") || !args[0].contains("position") ||
        !args[0].contains("rotation_quarter_turns") || !args[0]["position"].is_object() ||
        args[0]["position"].size() != 3 || !args[0]["position"].contains("x") ||
        !args[0]["position"].contains("y") || !args[0]["position"].contains("z")) {
        return Invalid("expected {town_id = 0-4, part_id = 0-23, position = {x, y, z}, rotation_quarter_turns = 0-3}");
    }
    int town_id = 0;
    int part_id = -1;
    int rotation = 0;
    if (!ReadBoundedInt(args[0], "town_id", 0, TOWN_COUNT - 1, town_id) ||
        !ReadBoundedInt(args[0], "rotation_quarter_turns", 0, 3, rotation)) {
        return Invalid("town_id must be 0-4 and rotation_quarter_turns must be 0-3");
    }
    if (args[0]["part_id"].is_number_integer()) {
        if (!ReadBoundedInt(args[0], "part_id", 0, CHRONICLE_GEORAMA_PART_CAPACITY - 1, part_id)) {
            return Invalid("part_id must be a native id from 0 through 55 or a registered piece id");
        }
    } else if (args[0]["part_id"].is_string()) {
        const std::string piece_id = args[0]["part_id"].get<std::string>();
        const auto found = g_mod_georama_pieces.find(piece_id);
        if (found == g_mod_georama_pieces.end()) return Invalid("part_id does not identify a registered Georama variant");
        part_id = found->second.native_part_id;
    } else {
        return Invalid("part_id must be an integer or registered stable piece id");
    }
    const Json &position = args[0]["position"];
    float coordinates[3] = {};
    const char *axes[] = {"x", "y", "z"};
    for (int axis = 0; axis < 3; ++axis) {
        if (!position[axes[axis]].is_number()) return Invalid("position coordinates must be finite numbers");
        const double value = position[axes[axis]].get<double>();
        if (!std::isfinite(value) || std::abs(value) > 100000.0) {
            return Invalid("position coordinates must be finite and within +/-100000 world units");
        }
        coordinates[axis] = static_cast<float>(value);
    }
    if (SaveData == nullptr) return Failed("the game has no active save data");
    if (mode != GAME_MODE_EDIT || NowEditMap != town_id || pEditGround == nullptr ||
        pEditGround->map_no != town_id) {
        return Failed("Georama placement requires the requested town's active editor scene");
    }
    if (pEditGround->parts_info != &EditPartsInfo || pEditGround->plot_parts == nullptr) {
        return Failed("the active town's native piece table is unavailable");
    }
    if (pEditGround->CheckEffect() != 0) return Failed("wait for the native Georama build effect to finish");
    EDITPARTS_INFO *plot = EditPartsInfo.GetPartsInfo(part_id);
    const CMapParts &native_part = pEditGround->plot_parts[part_id];
    if (plot == nullptr || plot->parts_no != part_id || native_part.handle < 0 || native_part.info == nullptr) {
        return Failed("part_id does not identify a loaded native or registered piece in this town");
    }
    if (plot->stock <= plot->placed) return Failed("the selected native piece has no available built stock");
    if (std::ranges::none_of(pEditGround->parts, [](const CMapParts &part) { return part.handle < 0; })) {
        return Failed("the native town placement table is full");
    }
    if (g_georama_place_routine == nullptr) return Failed("the native Georama placement routine is unavailable");

    const int native_rotation = rotation == 3 ? -1 : rotation;
    const int native_slot = g_georama_place_routine(pEditGround, part_id, coordinates[0], coordinates[1],
                                                    coordinates[2], native_rotation);
    if (native_slot < 0 || native_slot >= 128) {
        return Failed("the game's native placement, fit, collision, or stock checks rejected the piece");
    }

    EditSave();
    int saved_count = 0;
    SV_GRD_PART *saved_parts = SaveData->GetParts(town_id, &saved_count);
    int saved_index = 0;
    for (int index = 0; index < native_slot; ++index) {
        if (pEditGround->parts[index].handle >= 0) ++saved_index;
    }
    if (saved_parts == nullptr || saved_count < 0 || saved_count > 130 || saved_index >= saved_count) {
        return Failed("native placement succeeded but the saved placement could not be read back");
    }
    const SV_GRD_PART &placed = saved_parts[saved_index];
    if (placed.part_id < 0 || !std::isfinite(placed.pos_x) || !std::isfinite(placed.pos_y) ||
        !std::isfinite(placed.pos_z)) {
        return Failed("native placement saved an invalid Georama record");
    }
    const std::string placement_id = ChronicleGeoramaPlacementId(town_id, saved_index, placed);
    const auto mod_piece = g_mod_georama_piece_by_native_id.find(placed.part_id);
    const std::string piece_id = mod_piece == g_mod_georama_piece_by_native_id.end()
                                     ? "chronicle:georama-part:" + std::to_string(placed.part_id)
                                     : mod_piece->second;
    return {ApiCallStatus::Ok,
            {{"town_id", town_id}, {"part_id", placed.part_id}, {"id", piece_id}, {"placement_id", placement_id},
             {"rotation_quarter_turns", placed.rot_y < 0 ? 3 : placed.rot_y},
             {"position", {{"x", placed.pos_x}, {"y", placed.pos_y}, {"z", placed.pos_z}}},
             {"remaining_built", std::max(0, plot->stock - plot->placed)}, {"changed", true}},
            {}};
}

ApiCallResult RemoveGeorama(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("placement_id") || !args[0]["placement_id"].is_string() ||
        args[0]["placement_id"].get_ref<const std::string &>().empty()) {
        return Invalid("expected {placement_id = current placement id from georama.get()}");
    }
    if (SaveData == nullptr) return Failed("the game has no active save data");
    if (mode != GAME_MODE_EDIT || NowEditMap < 0 || NowEditMap >= TOWN_COUNT || pEditGround == nullptr ||
        pEditGround->map_no != NowEditMap) {
        return Failed("Georama removal requires an active town editor scene");
    }
    if (pEditGround->parts_info != &EditPartsInfo) return Failed("the active town's native piece table is unavailable");
    if (pEditGround->CheckEffect() != 0) return Failed("wait for the native Georama build effect to finish");
    EditSave();

    int part_count = 0;
    SV_GRD_PART *parts = SaveData->GetParts(NowEditMap, &part_count);
    if (parts == nullptr || part_count < 0 || part_count > 130) {
        return Failed("the active town's saved placement list is outside the supported range");
    }
    const std::string requested_id = args[0]["placement_id"].get<std::string>();
    int saved_index = -1;
    for (int index = 0; index < part_count; ++index) {
        const SV_GRD_PART &part = parts[index];
        if (part.part_id < 0 || !std::isfinite(part.pos_x) || !std::isfinite(part.pos_y) ||
            !std::isfinite(part.pos_z)) {
            return Failed("the active town's saved placement list contains an invalid record");
        }
        const std::string current_id = ChronicleGeoramaPlacementId(NowEditMap, index, part);
        if (current_id == requested_id) saved_index = index;
    }
    if (saved_index < 0) return Failed("placement_id is stale or belongs to a different town");

    const SV_GRD_PART selected = parts[saved_index];
    CMapParts *live_part = nullptr;
    for (int index = 0; index < 128; ++index) {
        CMapParts &candidate = pEditGround->parts[index];
        if (candidate.handle < 0 || candidate.rot_y != selected.rot_y ||
            ChronicleSavedGeoramaPartId(*pEditGround, candidate) != selected.part_id) continue;
        float position[4] = {};
        candidate.GetPosition(position);
        if (std::abs(position[0] - selected.pos_x) <= 0.001f &&
            std::abs(position[1] - selected.pos_y) <= 0.001f &&
            std::abs(position[2] - selected.pos_z) <= 0.001f) {
            live_part = &candidate;
            break;
        }
    }
    if (live_part == nullptr) return Failed("the saved placement is not present in the active native editor");
    if (g_georama_remove_routine == nullptr) return Failed("the native Georama removal routine is unavailable");

    int removed_part_id = -1;
    int removed_rotation = -1;
    const int native_result = g_georama_remove_routine(pEditGround, &removed_part_id, &removed_rotation,
                                                       selected.pos_x, selected.pos_y, selected.pos_z);
    if (native_result < 0) {
        return Failed("the game's native removal and dependency checks rejected the piece");
    }
    EditSave();
    EDITPARTS_INFO *plot = removed_part_id >= 0 && removed_part_id < CHRONICLE_GEORAMA_PART_CAPACITY
                               ? EditPartsInfo.GetPartsInfo(removed_part_id)
                               : nullptr;
    const auto mod_piece = g_mod_georama_piece_by_native_id.find(selected.part_id);
    const std::string piece_id = mod_piece == g_mod_georama_piece_by_native_id.end()
                                     ? "chronicle:georama-part:" + std::to_string(selected.part_id)
                                     : mod_piece->second;
    return {ApiCallStatus::Ok,
            {{"town_id", NowEditMap}, {"placement_id", requested_id}, {"part_id", selected.part_id},
             {"id", piece_id},
             {"removed", true}, {"changed", true},
             {"rotation_quarter_turns", selected.rot_y < 0 ? 3 : selected.rot_y},
             {"position", {{"x", selected.pos_x}, {"y", selected.pos_y}, {"z", selected.pos_z}}},
             {"remaining_built", plot == nullptr ? Json(nullptr) : Json(std::max(0, plot->stock - plot->placed))}},
            {}};
}

Json NativeDungeonJson(const NativeDungeonDefinition &dungeon) {
    const int native_id = dungeon.native_id;
    Json result = {{"id", dungeon.id}, {"name", dungeon.name}, {"native_id", native_id},
                   {"floor_count", maxFloorTbl__3[native_id]},
                   {"native_floor_count", kNativeDungeonFloorCounts[native_id]},
                   {"added_floor_count", maxFloorTbl__3[native_id] - kNativeDungeonFloorCounts[native_id]},
                   {"registration_supported", native_id < 5}, {"mod_owned", false}, {"status", "native"}};
    CDngStatusData *status = CurrentStatus();
    result["deepest_floor_reached"] = status == nullptr ? Json(nullptr) : Json(status->floor_reached[native_id]);
    return result;
}

bool IsModFloorMapName(std::string_view name) {
    return !name.empty() && name.size() <= 15 && std::all_of(name.begin(), name.end(), [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

Json ModDungeonJson(const ModDungeon &dungeon) {
    Json floors = Json::array();
    for (const ModDungeonFloor &floor : dungeon.floors) {
        Json entry = {{"id", floor.id}, {"name", floor.name}, {"source_floor", floor.source_floor},
                      {"native_floor_id", floor.native_floor_id}, {"mod_owned", true},
                      {"status", "registered"}};
        if (floor.has_enemy_override) entry["enemies"] = floor.enemy_character_ids;
        if (!floor.map.empty()) entry["map"] = floor.map;
        floors.push_back(std::move(entry));
    }
    const int native_id = dungeon.native_dungeon_id;
    return {{"id", dungeon.id}, {"name", dungeon.name}, {"native_id", native_id},
            {"base_dungeon_id", kNativeDungeons[native_id].id},
            {"native_floor_count", kNativeDungeonFloorCounts[native_id]},
            {"floor_count", maxFloorTbl__3[native_id]}, {"mod_floor_count", dungeon.floors.size()},
            {"mod_owned", true}, {"status", "registered"}, {"floor_geometry", "native_procedural"},
            {"assets", Json::array()}, {"floors", std::move(floors)}};
}

Json ModGeoramaPieceJson(const ModGeoramaPiece &piece) {
    CHRONICLE_MOD_GEORAMA_PIECE native_piece{};
    const bool registered = GetChronicleModGeoramaPiece(piece.native_part_id, &native_piece);
    return {{"id", piece.id}, {"name", piece.name}, {"template_part_id", piece.template_part_id},
            {"native_part_id", piece.native_part_id}, {"town_id", piece.town_id < 0 ? Json(nullptr) : Json(piece.town_id)},
            {"stock", piece.stock}, {"mod_owned", true}, {"status", registered ? "registered" : "unavailable"},
            {"materialized", registered && native_piece.materialized != 0}, {"assets", Json::array()},
            {"footprint", piece.footprint_width == 0
                              ? Json(nullptr)
                              : Json{{"width", piece.footprint_width}, {"height", piece.footprint_height},
                                     {"cells", piece.footprint_cells.empty() ? Json(nullptr) : Json(piece.footprint_cells)}}}};
}

ApiCallResult RegisterGeoramaPiece(std::string_view mod_id, const std::filesystem::path &save_root,
                                  const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected one data-only Georama piece variant definition");
    }
    const Json &definition = args[0];
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "id" && field.key() != "name" && field.key() != "template_part_id" &&
            field.key() != "town_id" && field.key() != "stock" && field.key() != "assets" && field.key() != "model" &&
            field.key() != "footprint") {
            return Invalid("Georama piece definitions accept only id, name, template_part_id, town_id, stock, model, footprint, and empty assets");
        }
    }
    const auto id_it = definition.find("id");
    const auto name_it = definition.find("name");
    int template_part_id = -1;
    if (id_it == definition.end() || !id_it->is_string() || name_it == definition.end() || !name_it->is_string() ||
        !ReadBoundedInt(definition, "template_part_id", 0, CHRONICLE_GEORAMA_NATIVE_PART_COUNT - 1, template_part_id)) {
        return Invalid("piece definition requires string id/name fields and native template_part_id from 0 through 23");
    }
    const std::string &display_name = name_it->get_ref<const std::string &>();
    if (display_name.empty() || display_name.size() > 47 || display_name.find('\0') != std::string::npos) {
        return Invalid("piece name must contain 1-47 bytes and fit the native Georama menu");
    }
    std::string model_scene;
    std::string model_part;
    if (const auto model_it = definition.find("model"); model_it != definition.end()) {
        if (!model_it->is_object() || model_it->size() != 2 || !model_it->contains("scene") ||
            !model_it->contains("part") || !(*model_it)["scene"].is_string() || !(*model_it)["part"].is_string()) {
            return Invalid("model must be {scene = scene file path, part = part archive name}");
        }
        model_scene = (*model_it)["scene"].get<std::string>();
        model_part = (*model_it)["part"].get<std::string>();
        std::ranges::transform(model_scene, model_scene.begin(), [](unsigned char c) { return std::tolower(c); });
        const bool mod_scene = model_scene.starts_with("gedit/mod/");
        if (model_scene.size() > 63 || !model_scene.starts_with("gedit/") || !model_scene.ends_with(".scn") ||
            model_scene.find("..") != std::string::npos || model_part.empty() || model_part.size() > 31 ||
            !InputId(model_part)) {
            return Invalid("model.scene must be a gedit/.../*.scn path and model.part a part archive name");
        }
        if (mod_scene && !NativeResourceOverrideOwnedBy(mod_id, model_scene)) {
            return Failed("a gedit/mod/ scene must be this mod's own resource override");
        }
    }
    int footprint_width = 0;
    int footprint_height = 0;
    std::vector<int> footprint_cells;
    if (const auto footprint_it = definition.find("footprint"); footprint_it != definition.end()) {
        if (!footprint_it->is_object() || !ReadBoundedInt(*footprint_it, "width", 1, 32, footprint_width) ||
            !ReadBoundedInt(*footprint_it, "height", 1, 32, footprint_height) ||
            std::ranges::any_of(footprint_it->items(), [](const auto &field) {
                return field.key() != "width" && field.key() != "height" && field.key() != "cells";
            })) {
            return Invalid("footprint must be {width = 1-32, height = 1-32, cells = optional width*height values 0-255}");
        }
        if (const auto cells_it = footprint_it->find("cells"); cells_it != footprint_it->end()) {
            if (!cells_it->is_array() || cells_it->size() != static_cast<std::size_t>(footprint_width * footprint_height)) {
                return Invalid("footprint.cells must hold width*height values, row by row");
            }
            for (const Json &cell : *cells_it) {
                if (!cell.is_number_integer() || cell.get<std::int64_t>() < 0 || cell.get<std::int64_t>() > 255) {
                    return Invalid("footprint.cells values must be integers from 0 through 255");
                }
                footprint_cells.push_back(cell.get<int>());
            }
            if (std::ranges::all_of(footprint_cells, [](int cell) { return cell == 0; })) {
                return Invalid("footprint.cells must mark at least one cell as part of the piece");
            }
        }
    }
    int town_id = -1;
    int stock = 1;
    if ((definition.contains("town_id") &&
         !ReadBoundedInt(definition, "town_id", 0, TOWN_COUNT - 1, town_id)) ||
        (definition.contains("stock") && !ReadBoundedInt(definition, "stock", 1, 999, stock))) {
        return Invalid("town_id must be 0-4 and stock must be 1-999");
    }
    const auto assets_it = definition.find("assets");
    if (assets_it != definition.end() &&
        ((!assets_it->is_object() && !assets_it->is_array()) || !assets_it->empty())) {
        return Invalid("Georama variants inherit their native template model and collision; asset overrides are unsupported");
    }

    std::string local_id = id_it->get<std::string>();
    std::string qualified_id;
    const std::size_t separator = local_id.find(':');
    if (separator == std::string::npos) {
        if (!InputId(local_id)) return Invalid("piece id must use letters, digits, '.', '_' or '-'");
        qualified_id = std::string(mod_id) + ":" + local_id;
    } else {
        if (local_id.find(':', separator + 1) != std::string::npos || local_id.substr(0, separator) != mod_id ||
            !InputId(std::string_view(local_id).substr(separator + 1))) {
            return Invalid("fully qualified piece ids must use this mod's namespace");
        }
        qualified_id = std::move(local_id);
    }
    if (qualified_id.size() >= sizeof(CHRONICLE_MOD_GEORAMA_PIECE::id) ||
        mod_id.size() >= sizeof(CHRONICLE_MOD_GEORAMA_PIECE::owner)) {
        return Invalid("mod id and qualified Georama piece id exceed the native registry limits");
    }
    if (g_mod_georama_pieces.size() >= CHRONICLE_GEORAMA_MOD_PART_COUNT &&
        !g_mod_georama_pieces.contains(qualified_id)) {
        return Failed("Chronicle supports up to 32 registered Georama piece variants");
    }
    const auto existing = g_mod_georama_pieces.find(qualified_id);
    if (existing != g_mod_georama_pieces.end()) {
        const ModGeoramaPiece &piece = existing->second;
        if (piece.owner != mod_id || piece.name != display_name || piece.template_part_id != template_part_id ||
            piece.town_id != town_id || piece.stock != stock || piece.footprint_width != footprint_width ||
            piece.footprint_height != footprint_height || piece.footprint_cells != footprint_cells) {
            return Invalid("a registered Georama piece cannot change until its owning mod is unloaded");
        }
        return {ApiCallStatus::Ok, ModGeoramaPieceJson(piece), {}};
    }

    int native_part_id = -1;
    std::string allocation_error;
    if (!AllocateModGeoramaPieceSlot(qualified_id, save_root, native_part_id, allocation_error)) {
        return Failed(std::move(allocation_error));
    }
    const auto slot_owner = g_mod_georama_piece_by_native_id.find(native_part_id);
    if (slot_owner != g_mod_georama_piece_by_native_id.end() && slot_owner->second != qualified_id) {
        return Failed("the persisted native Georama slot is assigned to a different stable piece id");
    }
    if (!RegisterChronicleModGeoramaPiece(native_part_id, town_id, template_part_id, stock,
                                          qualified_id.c_str(), std::string(mod_id).c_str(), display_name.c_str())) {
        return Failed("the native Chronicle Georama extension table rejected this piece definition");
    }

    ModGeoramaPiece piece{qualified_id, std::string(mod_id), display_name, template_part_id, town_id,
                          stock, native_part_id};
    piece.model_scene = model_scene;
    piece.model_part = model_part;
    SetChronicleModGeoramaPieceModel(native_part_id, model_scene.c_str(), model_part.c_str());
    piece.footprint_width = footprint_width;
    piece.footprint_height = footprint_height;
    piece.footprint_cells = footprint_cells;
    std::vector<u8> native_cells(footprint_cells.begin(), footprint_cells.end());
    SetChronicleModGeoramaPieceFootprint(native_part_id, footprint_width, footprint_height,
                                         native_cells.empty() ? nullptr : native_cells.data());
    auto [registered_piece, inserted] = g_mod_georama_pieces.emplace(qualified_id, std::move(piece));
    (void)inserted;
    g_mod_georama_piece_by_native_id[native_part_id] = qualified_id;
    if (mode == GAME_MODE_EDIT && pEditGround != nullptr && pEditGround->plot_parts != nullptr) {
        MaterializeChronicleModGeoramaPieces(NowEditMap, pEditGround->plot_parts);
    }
    return {ApiCallStatus::Ok, ModGeoramaPieceJson(registered_piece->second), {}};
}

ApiCallResult RegisterDungeon(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected one Chronicle dungeon extension definition object");
    }
    const Json &definition = args[0];
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "id" && field.key() != "name" && field.key() != "dungeon" &&
            field.key() != "floors" && field.key() != "assets") {
            return Invalid("Chronicle dungeon definition contains an unsupported field");
        }
    }
    const auto id_it = definition.find("id");
    const auto name_it = definition.find("name");
    const auto floors_it = definition.find("floors");
    if (id_it == definition.end() || !id_it->is_string() || name_it == definition.end() || !name_it->is_string() ||
        floors_it == definition.end() || !floors_it->is_array() || floors_it->empty()) {
        return Invalid("dungeon definition requires string id/name and a non-empty floors array");
    }

    const std::string local_id = id_it->get<std::string>();
    std::string qualified_id;
    const std::size_t separator = local_id.find(':');
    if (separator == std::string::npos) {
        if (!InputId(local_id)) return Invalid("dungeon id must use letters, digits, '.', '_' or '-'");
        qualified_id = std::string(mod_id) + ":" + local_id;
    } else {
        if (local_id.find(':', separator + 1) != std::string::npos || local_id.substr(0, separator) != mod_id ||
            !InputId(std::string_view(local_id).substr(separator + 1))) {
            return Invalid("fully qualified dungeon ids must use this mod's namespace");
        }
        qualified_id = local_id;
    }
    const std::string &display_name = name_it->get_ref<const std::string &>();
    if (display_name.empty() || display_name.size() > 95 || display_name.find('\0') != std::string::npos) {
        return Invalid("dungeon name must contain 1-95 UTF-8 bytes");
    }
    const auto assets = definition.find("assets");
    if (assets != definition.end() && ((!assets->is_object() && !assets->is_array()) || !assets->empty())) {
        return Invalid("Chronicle dungeon extensions reuse native procedural maps and accept no asset overrides");
    }
    int native_dungeon_id = -1;
    if (!ReadBoundedInt(definition, "dungeon", 0, 4, native_dungeon_id)) {
        return Invalid("dungeon must identify a Chronicle procedural dungeon from 0 through 4");
    }
    if (floors_it->size() > kChronicleMaximumRegisteredFloorsPerDungeon) {
        return Failed("Chronicle supports up to 8 added floors per dungeon registration");
    }
    if (g_mod_dungeons.contains(qualified_id)) return Invalid("a mod dungeon with this stable id is already registered");

    const int current_floor_count = maxFloorTbl__3[native_dungeon_id];
    if (current_floor_count < kNativeDungeonFloorCounts[native_dungeon_id] ||
        current_floor_count > kChronicleMaximumRegisteredFloorCount) {
        return Failed("Chronicle's native floor count is outside the supported extension range");
    }
    if (current_floor_count + static_cast<int>(floors_it->size()) > kChronicleMaximumRegisteredFloorCount) {
        return Failed("Chronicle's save and floor tables support at most 25 floors in these dungeons");
    }

    ModDungeon dungeon;
    dungeon.id = qualified_id;
    dungeon.owner = mod_id;
    dungeon.name = display_name;
    dungeon.native_dungeon_id = native_dungeon_id;
    std::map<std::string, int, std::less<>> local_floor_ids;
    for (std::size_t index = 0; index < floors_it->size(); ++index) {
        const Json &floor_definition = floors_it->at(index);
        if (!floor_definition.is_object()) return Invalid("each floor definition must be an object");
        for (auto field = floor_definition.begin(); field != floor_definition.end(); ++field) {
            if (field.key() != "id" && field.key() != "name" && field.key() != "source_floor" &&
                field.key() != "enemies" && field.key() != "map") {
                return Invalid("Chronicle floor definitions accept only id, name, source_floor, enemies, and map");
            }
        }
        const auto floor_id_it = floor_definition.find("id");
        const auto source_floor_it = floor_definition.find("source_floor");
        if (floor_id_it == floor_definition.end() || !floor_id_it->is_string() ||
            source_floor_it == floor_definition.end()) {
            return Invalid("each added floor requires a local id and source_floor");
        }
        const std::string local_floor_id = floor_id_it->get<std::string>();
        if (!InputId(local_floor_id) || local_floor_ids.contains(local_floor_id)) {
            return Invalid("floor ids must be unique and use letters, digits, '.', '_' or '-'");
        }
        const auto floor_name_it = floor_definition.find("name");
        if (floor_name_it != floor_definition.end() && !floor_name_it->is_string()) {
            return Invalid("floor name must be a string");
        }
        const std::string floor_name = floor_name_it == floor_definition.end()
            ? display_name + " / " + local_floor_id
            : floor_name_it->get<std::string>();
        if (floor_name.empty() || floor_name.size() > 95 || floor_name.find('\0') != std::string::npos) {
            return Invalid("floor name must contain 1-95 UTF-8 bytes");
        }
        std::string floor_map;
        const auto map_it = floor_definition.find("map");
        if (map_it != floor_definition.end()) {
            if (!map_it->is_string()) return Invalid("floor map must be a floor pack name");
            floor_map = map_it->get<std::string>();
            if (!IsModFloorMapName(floor_map)) {
                return Invalid("floor map must be 1-15 lowercase letters, digits or '_'");
            }
            if (!NativeResourceOverrideOwnedBy(mod_id, "dun/" + floor_map + ".cfg") ||
                !NativeResourceOverrideOwnedBy(mod_id, "dun/mpd_pack/" + floor_map + ".mpd")) {
                return Failed("a floor map needs this mod's overrides of dun/<map>.cfg and dun/mpd_pack/<map>.mpd");
            }
        }
        bool has_enemy_override = false;
        std::vector<std::string> enemy_character_ids;
        std::vector<int> enemy_model_ids;
        const auto enemies_it = floor_definition.find("enemies");
        if (enemies_it != floor_definition.end()) {
            // (An empty Lua table arrives as an empty object: it asks for an empty encounter too.)
            const bool empty_table = enemies_it->is_object() && enemies_it->empty();
            if (!empty_table && (!enemies_it->is_array() || enemies_it->size() > 9)) {
                return Invalid("enemies must be an array of up to 9 registered character ids");
            }
            has_enemy_override = true;
            for (const Json &enemy_id_value : *enemies_it) {
                if (!enemy_id_value.is_string()) return Invalid("each enemy entry must be a registered character id");
                std::string enemy_id = enemy_id_value.get<std::string>();
                if (enemy_id.find(':') == std::string::npos && InputId(enemy_id)) {
                    enemy_id = std::string(mod_id) + ":" + enemy_id;
                }
                const auto enemy = g_mod_characters.find(enemy_id);
                if (enemy == g_mod_characters.end()) {
                    return Invalid("every floor enemy must identify a character registered by this mod");
                }
                if (enemy->second.owner != mod_id) {
                    return Failed("a mod can only place its own registered characters into a dungeon floor");
                }
                if (!enemy->second.model_id.empty()) {
                    const auto model = g_mod_models.find(enemy->second.model_id);
                    if (model == g_mod_models.end() ||
                        !NativeMonsterAssetReady(model->second.owner,
                                                 NativeMonsterAssetPath(model->second.model, "chr")) ||
                        (model->second.custom_script &&
                         !NativeMonsterAssetReady(model->second.owner,
                                                  NativeMonsterAssetPath(model->second.script, "stb")))) {
                        return Failed("a custom enemy model or script is unavailable for this floor");
                    }
                }
                enemy_character_ids.push_back(enemy_id);
                enemy_model_ids.push_back(enemy->second.native_model_no);
            }
        }
        int source_floor = -1;
        if (source_floor_it->is_number_integer()) {
            if (!ReadBoundedInt(floor_definition, "source_floor", 0,
                                current_floor_count + static_cast<int>(index) - 1, source_floor)) {
                return Invalid("numeric source_floor must identify an existing native or earlier registered floor");
            }
        } else if (source_floor_it->is_string()) {
            const std::string source_id = source_floor_it->get<std::string>();
            const auto earlier = local_floor_ids.find(source_id);
            if (!InputId(source_id) || earlier == local_floor_ids.end()) {
                return Invalid("string source_floor must name an earlier floor in this registration");
            }
            source_floor = earlier->second;
        } else {
            return Invalid("source_floor must be a native floor number or an earlier local floor id");
        }
        const int native_floor_id = current_floor_count + static_cast<int>(index);
        local_floor_ids.emplace(local_floor_id, native_floor_id);
        ModDungeonFloor floor;
        floor.id = qualified_id + ":" + local_floor_id;
        floor.local_id = local_floor_id;
        floor.name = floor_name;
        floor.source_floor = source_floor;
        floor.native_floor_id = native_floor_id;
        floor.map = std::move(floor_map);
        floor.has_enemy_override = has_enemy_override;
        floor.enemy_character_ids = std::move(enemy_character_ids);
        floor.enemy_model_ids = std::move(enemy_model_ids);
        dungeon.floors.push_back(std::move(floor));
    }

    if (g_modded_enemy_layouts[native_dungeon_id].empty()) {
        if (g_native_enemy_layouts[native_dungeon_id] == nullptr) {
            g_native_enemy_layouts[native_dungeon_id] = BtEnemyLayoutList[native_dungeon_id];
            g_native_ura_enemy_layouts[native_dungeon_id] = BtUraEnemyLayoutList[native_dungeon_id];
        }
        if (g_native_enemy_layouts[native_dungeon_id] == nullptr ||
            g_native_ura_enemy_layouts[native_dungeon_id] == nullptr ||
            current_floor_count != kNativeDungeonFloorCounts[native_dungeon_id]) {
            return Failed("Chronicle's native enemy-layout tables are unavailable or inconsistent");
        }
        g_modded_enemy_layouts[native_dungeon_id].reserve(kChronicleMaximumRegisteredFloorCount);
        g_modded_ura_enemy_layouts[native_dungeon_id].reserve(kChronicleMaximumRegisteredFloorCount);
        g_modded_enemy_layouts[native_dungeon_id].assign(
            g_native_enemy_layouts[native_dungeon_id],
            g_native_enemy_layouts[native_dungeon_id] + current_floor_count);
        g_modded_ura_enemy_layouts[native_dungeon_id].assign(
            g_native_ura_enemy_layouts[native_dungeon_id],
            g_native_ura_enemy_layouts[native_dungeon_id] + current_floor_count);
    }
    auto &enemy_layouts = g_modded_enemy_layouts[native_dungeon_id];
    auto &ura_enemy_layouts = g_modded_ura_enemy_layouts[native_dungeon_id];
    if (enemy_layouts.size() != static_cast<std::size_t>(current_floor_count) ||
        ura_enemy_layouts.size() != static_cast<std::size_t>(current_floor_count)) {
        return Failed("Chronicle's registered enemy-layout tables no longer match the native floor count");
    }
    for (const ModDungeonFloor &floor : dungeon.floors) {
        const BT_ENEMY_FLOOR *enemy_source = &enemy_layouts.at(floor.source_floor);
        const BT_ENEMY_FLOOR *ura_source = &ura_enemy_layouts.at(floor.source_floor);
        enemy_layouts.push_back(*enemy_source);
        ura_enemy_layouts.push_back(*ura_source);
        if (floor.has_enemy_override) {
            BT_ENEMY_FLOOR *layouts[2] = {&enemy_layouts.back(), &ura_enemy_layouts.back()};
            for (BT_ENEMY_FLOOR *layout : layouts) {
                for (std::size_t slot = 0; slot < std::size(layout->monster); ++slot) {
                    if (slot < floor.enemy_model_ids.size()) {
                        layout->monster[slot].monster_no = floor.enemy_model_ids[slot];
                    } else {
                        layout->monster[slot].monster_no = -1;
                    }
                }
            }
        }
    }
    BtEnemyLayoutList[native_dungeon_id] = enemy_layouts.data();
    BtUraEnemyLayoutList[native_dungeon_id] = ura_enemy_layouts.data();
    maxFloorTbl__3[native_dungeon_id] = current_floor_count + static_cast<int>(dungeon.floors.size());

    const std::string id = dungeon.id;
    g_mod_dungeons.emplace(id, std::move(dungeon));
    return {ApiCallStatus::Ok, ModDungeonJson(g_mod_dungeons.at(id)), {}};
}

ApiCallResult ListDungeons(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("dungeons.list takes no arguments");
    Json result = Json::array();
    for (const NativeDungeonDefinition &dungeon : kNativeDungeons) result.push_back(NativeDungeonJson(dungeon));
    for (const auto &[id, dungeon] : g_mod_dungeons) {
        (void)id;
        result.push_back(ModDungeonJson(dungeon));
    }
    return {ApiCallStatus::Ok, std::move(result), {}};
}

ApiCallResult GetDungeon(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1) {
        return Invalid("expected one dungeon id or {id = dungeon_id}");
    }
    std::string id;
    if (!ReadId(args[0], id)) return Invalid("expected a stable Chronicle dungeon id");
    if (id.find(':') == std::string::npos) {
        const auto mod_dungeon = g_mod_dungeons.find(std::string(mod_id) + ":" + id);
        if (mod_dungeon != g_mod_dungeons.end()) {
            return {ApiCallStatus::Ok, ModDungeonJson(mod_dungeon->second), {}};
        }
    } else {
        const auto mod_dungeon = g_mod_dungeons.find(id);
        if (mod_dungeon != g_mod_dungeons.end()) {
            return {ApiCallStatus::Ok, ModDungeonJson(mod_dungeon->second), {}};
        }
    }
    const auto found = std::ranges::find_if(kNativeDungeons, [&id](const NativeDungeonDefinition &dungeon) {
        const std::string_view stable_id = dungeon.id;
        constexpr std::string_view prefix = "chronicle:dungeon:";
        return id == stable_id || (stable_id.starts_with(prefix) && id == stable_id.substr(prefix.size()));
    });
    if (found == kNativeDungeons.end()) return Failed("no built-in Chronicle dungeon has this id");
    return {ApiCallStatus::Ok, NativeDungeonJson(*found), {}};
}

Json ModItemJson(const ModItem &item);
Json ModWeaponJson(const ModWeapon &weapon, bool include_state);
ApiCallResult RegisterItem(std::string_view mod_id, const std::filesystem::path &save_root, const Json &args);
ApiCallResult RegisterWeapon(std::string_view mod_id, const std::filesystem::path &save_root, const Json &args);

ApiCallResult ListContent(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("content.list takes no arguments");
    Json entries = Json::array();
    auto append = [&entries](std::string_view type, Json definition) {
        definition["content_type"] = type;
        entries.push_back(std::move(definition));
    };
    for (const auto &[id, item] : g_mod_items) { (void)id; append("item", ModItemJson(item)); }
    for (const auto &[id, weapon] : g_mod_weapons) { (void)id; append("weapon", ModWeaponJson(weapon, false)); }
    for (const auto &[id, character] : g_mod_characters) { (void)id; append("character", ModCharacterJson(character)); }
    for (const auto &[id, dungeon] : g_mod_dungeons) { (void)id; append("dungeon", ModDungeonJson(dungeon)); }
    for (const auto &[id, piece] : g_mod_georama_pieces) { (void)id; append("georama_piece", ModGeoramaPieceJson(piece)); }
    for (const auto &[id, npc] : g_mod_npcs) { (void)id; append("npc", ModNpcJson(npc)); }
    return {ApiCallStatus::Ok, {{"count", entries.size()}, {"entries", std::move(entries)}}, {}};
}

ApiCallResult RegisterContent(std::string_view mod_id, const std::filesystem::path &save_root, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 2 ||
        !args[0].contains("type") || !args[0]["type"].is_string() ||
        !args[0].contains("definition") || !args[0]["definition"].is_object()) {
        return Invalid("expected {type = 'item'|'weapon'|'character'|'dungeon'|'georama_piece', definition = {...}}");
    }
    const std::string type = args[0]["type"].get<std::string>();
    const Json wrapped = Json::array({args[0]["definition"]});
    if (type == "item") return RegisterItem(mod_id, save_root, wrapped);
    if (type == "weapon") return RegisterWeapon(mod_id, save_root, wrapped);
    if (type == "character") return RegisterCharacter(mod_id, wrapped);
    if (type == "dungeon") return RegisterDungeon(mod_id, wrapped);
    if (type == "georama_piece") return RegisterGeoramaPiece(mod_id, save_root, wrapped);
    return Invalid("content.register type must be item, weapon, character, dungeon, or georama_piece in Chronicle");
}

ApiCallResult UnregisterContent(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 2 ||
        !args[0].contains("type") || !args[0]["type"].is_string() ||
        !args[0].contains("id") || !args[0]["id"].is_string()) {
        return Invalid("expected {type = 'character'|'georama_piece', id = local_content_id}");
    }
    const std::string type = args[0]["type"].get<std::string>();
    if (type == "georama_piece") {
        std::string id;
        if (!QualifyModContentId(mod_id, args[0]["id"].get_ref<const std::string &>(), id, "Georama piece")) {
            return Invalid("id must be a local identifier owned by this mod");
        }
        const auto found = g_mod_georama_pieces.find(id);
        if (found == g_mod_georama_pieces.end() || found->second.owner != mod_id) {
            return Invalid("Georama piece registration is missing or owned by another mod");
        }
        if (SaveData == nullptr) return Failed("Georama piece removal requires an active save to check saved towns");
        const int native_part_id = found->second.native_part_id;
        for (int town_id = 0; town_id < TOWN_COUNT; ++town_id) {
            int saved_count = 0;
            SV_GRD_PART *saved = SaveData->GetParts(town_id, &saved_count);
            if (saved == nullptr || saved_count < 0 || saved_count > 130) {
                return Failed("the saved Georama placement list is unavailable or outside the supported range");
            }
            for (int index = 0; index < saved_count; ++index) {
                if (saved[index].part_id == native_part_id) {
                    return Failed("a saved town still contains this piece variant; remove every placement before unregistering it");
                }
            }
        }
        if (pEditGround != nullptr && pEditGround->plot_parts != nullptr) {
            for (const CMapParts &part : pEditGround->parts) {
                if (part.handle >= 0 && ChronicleSavedGeoramaPartId(*pEditGround, part) == native_part_id) {
                    return Failed("the active editor still contains this piece variant; remove it before unregistering");
                }
            }
        }
        if (!UnregisterChronicleModGeoramaPiece(native_part_id)) {
            return Failed("the native Chronicle Georama extension table rejected removal");
        }
        if (pEditGround != nullptr && pEditGround->plot_parts != nullptr &&
            native_part_id >= CHRONICLE_GEORAMA_NATIVE_PART_COUNT &&
            native_part_id < CHRONICLE_GEORAMA_PART_CAPACITY) {
            pEditGround->plot_parts[native_part_id].Initialize();
            pEditGround->plot_parts[native_part_id].handle = -1;
        }
        g_mod_georama_piece_by_native_id.erase(native_part_id);
        g_mod_georama_pieces.erase(found);
        return {ApiCallStatus::Ok, {{"id", id}, {"type", type}, {"removed", true}}, {}};
    }
    if (type != "character") {
        return Invalid("Chronicle can safely unregister character and unused Georama variants; item, weapon, and dungeon registrations use persistent native slots");
    }
    std::string id;
    if (!QualifyModContentId(mod_id, args[0]["id"].get_ref<const std::string &>(), id, "character")) {
        return Invalid("id must be a local identifier owned by this mod");
    }
    const auto found = g_mod_characters.find(id);
    if (found == g_mod_characters.end() || found->second.owner != mod_id) {
        return Invalid("character registration is missing or owned by another mod");
    }
    const int native_model_no = found->second.native_model_no;
    if (native_model_no < 0x400 || native_model_no >= 0x420) {
        return Failed("character does not have a removable MoonMod native variant slot");
    }

    for (const auto &[other_id, character] : g_mod_characters) {
        if (other_id != id &&
            (character.template_model_no == native_model_no || character.asset_template_model_no == native_model_no)) {
            return Failed("another registered character inherits this variant and must be unregistered first");
        }
    }
    for (const auto &[model_id, model] : g_mod_models) {
        (void)model_id;
        if (model.template_model_no == native_model_no) {
            return Failed("a registered model binding uses this character as its template");
        }
    }
    for (const auto &[dungeon_id, dungeon] : g_mod_dungeons) {
        (void)dungeon_id;
        for (const ModDungeonFloor &floor : dungeon.floors) {
            if (std::ranges::find(floor.enemy_character_ids, id) != floor.enemy_character_ids.end() ||
                std::ranges::find(floor.enemy_model_ids, native_model_no) != floor.enemy_model_ids.end()) {
                return Failed("a registered dungeon floor uses this character variant");
            }
        }
    }
    if (mode == GAME_MODE_DUNGEON && NowMonstorUnit != nullptr && g_monster_handle_unit == NowMonstorUnit) {
        if (std::ranges::find(g_loaded_monster_model_ids, native_model_no) != g_loaded_monster_model_ids.end()) {
            return Failed("this character variant is loaded in the active dungeon and cannot be unregistered yet");
        }
        for (const ChronicleMonsterHandleRecord &record : g_monster_handle_records) {
            if (record.generation != 0 && record.native_model_no == native_model_no &&
                !record.character_id.empty()) {
                return Failed("a live or tracked dungeon actor uses this character variant");
            }
        }
    }
    if (!UnregisterMoonModMonstorVariant(native_model_no)) {
        return Failed("the native monster table rejected removal of this character variant");
    }
    g_mod_characters.erase(found);
    return {ApiCallStatus::Ok, {{"id", id}, {"type", "character"}, {"removed", true}}, {}};
}

ApiCallResult SetWorldTime(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("fraction") || !args[0]["fraction"].is_number()) {
        return Invalid("expected {fraction = number from 0 (inclusive) to 1 (exclusive)}");
    }
    const double fraction = args[0]["fraction"].get<double>();
    if (!std::isfinite(fraction) || fraction < 0.0 || fraction >= 1.0) {
        return Invalid("fraction must be finite and in the range [0, 1)");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");

    const float native_time = static_cast<float>(fraction * 12.0);
    SaveData->SetNowTime(native_time);
    if (mode == GAME_MODE_EDIT) NowTime = native_time;
    return {ApiCallStatus::Ok,
            {{"time", {{"fraction", fraction}, {"native_time", native_time}, {"native_cycle_hours", 12}}}},
            {}};
}

bool ReadBoundedInt(const Json &object, const char *key, int minimum, int maximum, int &out);

ApiCallResult GetStoryFlag(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("id")) {
        return Invalid("expected {id = story_flag_id from 0 to 8191}");
    }
    int flag_id = 0;
    if (!ReadBoundedInt(args[0], "id", 0, 8191, flag_id)) {
        return Invalid("story flag id must be an integer from 0 to 8191");
    }
    if (SaveData == nullptr) return Failed("the game has no active save data");
    return {ApiCallStatus::Ok, {{"id", flag_id}, {"value", SaveData->GetGameFlag(flag_id) != 0}}, {}};
}

ApiCallResult SetStoryFlag(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 2 ||
        !args[0].contains("id") || !args[0].contains("value") || !args[0]["value"].is_boolean()) {
        return Invalid("expected {id = story_flag_id from 0 to 8191, value = boolean}");
    }
    int flag_id = 0;
    if (!ReadBoundedInt(args[0], "id", 0, 8191, flag_id)) {
        return Invalid("story flag id must be an integer from 0 to 8191");
    }
    if (SaveData == nullptr) return Failed("the game has no active save data");
    const bool previous = SaveData->GetGameFlag(flag_id) != 0;
    const bool value = args[0]["value"].get<bool>();
    SaveData->SetGameFlag(flag_id, value ? 1 : 0);
    return {ApiCallStatus::Ok,
            {{"id", flag_id}, {"previous", previous}, {"value", SaveData->GetGameFlag(flag_id) != 0},
             {"changed", previous != value}},
            {}};
}

ApiCallResult GetStoryValue(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("id")) {
        return Invalid("expected {id = story_value_id from 0 to 31}");
    }
    int value_id = 0;
    if (!ReadBoundedInt(args[0], "id", 0, 31, value_id)) {
        return Invalid("story value id must be an integer from 0 to 31");
    }
    if (SaveData == nullptr) return Failed("the game has no active save data");
    return {ApiCallStatus::Ok, {{"id", value_id}, {"value", SaveData->GetGameIntFlag(value_id)}}, {}};
}

ApiCallResult SetStoryValue(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 2 ||
        !args[0].contains("id") || !args[0].contains("value")) {
        return Invalid("expected {id = story_value_id from 0 to 31, value = signed 32-bit integer}");
    }
    int value_id = 0;
    int value = 0;
    if (!ReadBoundedInt(args[0], "id", 0, 31, value_id) ||
        !ReadBoundedInt(args[0], "value", std::numeric_limits<int>::min(), std::numeric_limits<int>::max(), value)) {
        return Invalid("story value id must be 0-31 and value must fit a signed 32-bit integer");
    }
    if (SaveData == nullptr) return Failed("the game has no active save data");
    const int previous = SaveData->GetGameIntFlag(value_id);
    SaveData->SetGameIntFlag(value_id, value);
    return {ApiCallStatus::Ok,
            {{"id", value_id}, {"previous", previous}, {"value", SaveData->GetGameIntFlag(value_id)},
             {"changed", previous != value}},
            {}};
}

ApiCallResult GetMapFlag(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 2 ||
        !args[0].contains("map_id") || !args[0].contains("flag_id")) {
        return Invalid("expected {map_id = 0-79, flag_id = 0-511}");
    }
    int map_id = 0;
    int flag_id = 0;
    if (!ReadBoundedInt(args[0], "map_id", 0, 79, map_id) ||
        !ReadBoundedInt(args[0], "flag_id", 0, 511, flag_id)) {
        return Invalid("map_id must be 0-79 and flag_id must be 0-511");
    }
    if (SaveData == nullptr) return Failed("the game has no active save data");
    return {ApiCallStatus::Ok,
            {{"map_id", map_id}, {"flag_id", flag_id}, {"value", SaveData->GetMapFlag(map_id, flag_id) != 0}}, {}};
}

ApiCallResult SetMapFlag(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 3 ||
        !args[0].contains("map_id") || !args[0].contains("flag_id") || !args[0].contains("value") ||
        !args[0]["value"].is_boolean()) {
        return Invalid("expected {map_id = 0-79, flag_id = 0-511, value = boolean}");
    }
    int map_id = 0;
    int flag_id = 0;
    if (!ReadBoundedInt(args[0], "map_id", 0, 79, map_id) ||
        !ReadBoundedInt(args[0], "flag_id", 0, 511, flag_id)) {
        return Invalid("map_id must be 0-79 and flag_id must be 0-511");
    }
    if (SaveData == nullptr) return Failed("the game has no active save data");
    const bool previous = SaveData->GetMapFlag(map_id, flag_id) != 0;
    const bool value = args[0]["value"].get<bool>();
    SaveData->SetMapFlag(map_id, flag_id, value ? 1 : 0);
    return {ApiCallStatus::Ok,
            {{"map_id", map_id}, {"flag_id", flag_id}, {"previous", previous},
             {"value", SaveData->GetMapFlag(map_id, flag_id) != 0}, {"changed", previous != value}},
            {}};
}

ApiCallResult SetGeoramaRequest(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 3 ||
        !args[0].contains("town_id") || !args[0].contains("request_id") || !args[0].contains("completed") ||
        !args[0]["completed"].is_boolean()) {
        return Invalid("expected {town_id = 0-5, request_id = 0-23, completed = boolean}");
    }
    int town_id = 0;
    int request_id = 0;
    if (!ReadBoundedInt(args[0], "town_id", 0, 5, town_id) ||
        !ReadBoundedInt(args[0], "request_id", 0, 23, request_id)) {
        return Invalid("town_id must be 0-5 and request_id must be 0-23");
    }
    if (SaveData == nullptr) return Failed("the game has no active save data");
    SV_GEORAMA_DATA *town = SaveData->GetGrdData(town_id);
    if (town == nullptr || town->request_count < 0 || town->request_count > 24) {
        return Failed("the saved Georama town data is outside the supported range");
    }
    if (request_id >= town->request_count) return Invalid("request_id is not defined for this Georama town");
    const bool previous = town->request_complete[request_id] != 0;
    const bool completed = args[0]["completed"].get<bool>();
    town->request_complete[request_id] = completed ? 1 : 0;
    return {ApiCallStatus::Ok,
            {{"town_id", town_id}, {"request_id", request_id}, {"previous", previous},
             {"completed", town->request_complete[request_id] != 0}, {"changed", previous != completed}},
            {}};
}

ApiCallResult SetGeoramaElement(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 4 ||
        !args[0].contains("town_id") || !args[0].contains("plot_id") ||
        !args[0].contains("element_id") || !args[0].contains("enabled") ||
        !args[0]["enabled"].is_boolean()) {
        return Invalid("expected {town_id = 0-5, plot_id = 0-23, element_id = 0-5, enabled = boolean}");
    }
    int town_id = 0;
    int plot_id = 0;
    int element_id = 0;
    if (!ReadBoundedInt(args[0], "town_id", 0, 5, town_id) ||
        !ReadBoundedInt(args[0], "plot_id", 0, 23, plot_id) ||
        !ReadBoundedInt(args[0], "element_id", 0, 5, element_id)) {
        return Invalid("town_id must be 0-5, plot_id 0-23, and element_id 0-5");
    }
    if (SaveData == nullptr) return Failed("the game has no active save data");
    if (mode != GAME_MODE_EDIT || NowEditMap != town_id) {
        return Failed("Georama element edits require the selected town's active editor scene");
    }

    SV_EDIT_PARTS_INFO *saved_plot = SaveData->GetEditPartsInfo(town_id, plot_id);
    EDITPARTS_INFO *loaded_plot = EditPartsInfo.GetPartsInfo(plot_id);
    if (saved_plot == nullptr || loaded_plot == nullptr || loaded_plot->parts_no != plot_id) {
        return Failed("the selected native Georama plot definition is unavailable");
    }
    EDITPARTS_ELEMENT &element = loaded_plot->elements[element_id];
    if (element.id < 0 || element.id >= 36) {
        return Invalid("element_id does not identify an optional element on this native plot");
    }

    const bool enabled = args[0]["enabled"].get<bool>();
    const bool previous = saved_plot->element_enabled[element_id] != 0;
    if (enabled && element.required_element >= 0) {
        if (element.required_element >= 6 ||
            loaded_plot->elements[element.required_element].id < 0 ||
            loaded_plot->elements[element.required_element].enabled == 0) {
            return Failed("the native plot requires another optional element to be enabled first");
        }
    }
    if (!enabled) {
        for (int dependent = 0; dependent < 6; ++dependent) {
            const EDITPARTS_ELEMENT &candidate = loaded_plot->elements[dependent];
            if (candidate.enabled != 0 && candidate.required_element == element_id) {
                return Failed("another enabled native plot element depends on this element");
            }
        }
    }

    const bool runtime_previous = element.enabled != 0;
    if (previous != enabled || runtime_previous != enabled) {
        saved_plot->element_enabled[element_id] = enabled ? 1 : 0;
        element.enabled = enabled ? 1 : 0;
    }
    return {ApiCallStatus::Ok,
            {{"town_id", town_id}, {"plot_id", plot_id}, {"element_id", element_id},
             {"previous", previous}, {"enabled", enabled},
             {"changed", previous != enabled || runtime_previous != enabled}},
            {}};
}

ApiCallResult SpawnCharacter(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {character = registered_enemy_id, position = {x, y, z}, rotation = optional {x, y, z}}");
    }
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "character" && field.key() != "position" && field.key() != "rotation" &&
            field.key() != "arena_origin" && field.key() != "grounded" && field.key() != "arena_slot") {
            return Invalid("character spawning only supports character, position, rotation, arena_origin, grounded and "
                           "arena_slot fields");
        }
    }
    const auto character_field = request.find("character");
    if (character_field == request.end() || !character_field->is_string()) {
        return Invalid("character must be a registered stable enemy id");
    }
    const std::string character_id = character_field->get<std::string>();
    const auto definition = g_mod_characters.find(character_id);
    if (definition == g_mod_characters.end()) return Invalid("character does not identify a registered mod enemy");
    if (definition->second.owner != mod_id) return Failed("a mod can only spawn enemy definitions that it owns");

    std::array<float, 4> position{};
    std::array<float, 4> rotation{{0.0f, 0.0f, 0.0f, 1.0f}};
    const auto position_field = request.find("position");
    if (position_field == request.end() || !ReadVector3(*position_field, position, 100000.0f)) {
        return Invalid("position must contain finite x, y, and z coordinates within 100000 game units");
    }
    const auto rotation_field = request.find("rotation");
    if (rotation_field != request.end() && !ReadVector3(*rotation_field, rotation, 10000.0f)) {
        return Invalid("rotation must contain finite x, y, and z angles within 10000 radians");
    }
    // A boss's script places it in its own arena, around the world origin; arena_origin is where that arena's
    // centre lies on this floor.
    std::array<float, 4> arena_origin{};
    const auto origin_field = request.find("arena_origin");
    if (origin_field != request.end() && !ReadVector3(*origin_field, arena_origin, 100000.0f)) {
        return Invalid("arena_origin must contain finite x, y, and z coordinates within 100000 game units");
    }
    bool grounded = false;
    if (const auto grounded_field = request.find("grounded"); grounded_field != request.end()) {
        if (!grounded_field->is_boolean() || origin_field == request.end()) {
            return Invalid("grounded is true or false, and goes with arena_origin");
        }
        grounded = grounded_field->get<bool>();
    }
    // A boss's cast: the slot this member holds in its native arena, by which the cast's scripts name each other.
    int cast_no = -1;
    if (const auto cast_field = request.find("arena_slot"); cast_field != request.end()) {
        if (!cast_field->is_number_integer() || origin_field == request.end() || cast_field->get<int>() < 0 ||
            cast_field->get<int>() >= 16) {
            return Invalid("arena_slot is a whole number from 0 to 15, and goes with arena_origin");
        }
        cast_no = cast_field->get<int>();
    }

    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(status) || unit == nullptr) {
        return Failed("characters.spawn requires an active Chronicle dungeon");
    }
    // Floor loads, scripted scenes and menus rebuild or borrow the native
    // model table; spawning then copies a half-loaded model. Only spawn while
    // the player has control.
    if (gameTask != GAME_TASK_PLAY) {
        return Failed("characters.spawn is available only while the player has control of the dungeon "
                      "(not during loading, scenes or menus); try again on a later tick");
    }
    PrepareMonsterHandleUnit(unit);

    if (unit->model_count < 0 || unit->model_count > MONSTOR_MODEL_MAX) return Failed("the active dungeon has an invalid loaded monster table");
    int loaded_model_slot = -1;
    for (int index = 0; index < unit->model_count; ++index) {
        if (g_loaded_monster_model_ids[index] == definition->second.asset_template_model_no) {
            loaded_model_slot = index;
            break;
        }
    }
    // A data-only variant whose native template the floor didn't load (a boss on an ordinary floor) loads it now,
    // into a free slot of the floor's model table.
    if (loaded_model_slot < 0 && definition->second.model_id.empty() &&
        ChronicleLoadMonsterModelForMod(unit, definition->second.asset_template_model_no, &loaded_model_slot)) {
        g_loaded_monster_model_ids[loaded_model_slot] = definition->second.asset_template_model_no;
    }
    if (loaded_model_slot < 0 && !definition->second.model_id.empty()) {
        const auto binding = g_mod_models.find(definition->second.model_id);
        if (binding == g_mod_models.end() ||
            !NativeMonsterAssetReady(binding->second.owner, NativeMonsterAssetPath(binding->second.model, "chr")) ||
            (binding->second.custom_script &&
             !NativeMonsterAssetReady(binding->second.owner, NativeMonsterAssetPath(binding->second.script, "stb")))) {
            return Failed("the custom model's package-owned native assets are no longer active or exceed Chronicle's loader limit");
        }
        (void)ChronicleLoadMonsterModelForMod(unit, definition->second.asset_template_model_no,
                                              &loaded_model_slot);
    }
    if (loaded_model_slot < 0) {
        if (!definition->second.model_id.empty()) {
            return Failed("the custom model could not be loaded on this floor; the native model table may be full or not ready");
        }
        return Failed("the enemy's native template is not loaded on this floor and the floor's model table is full");
    }

    int expected_actor_slot = -1;
    for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) {
        if (unit->monster[slot].state == -1) {
            expected_actor_slot = slot;
            break;
        }
    }
    if (expected_actor_slot < 0) return Failed("the active dungeon has no free monster actor slots");
    if (origin_field != request.end()) {
        // A cast shares its script integers among its own members only: the floor's other monsters use the same
        // numbers for their own ends. Its boss (arena_slot 0) opens the cast's bank, cleared as its arena's floor load
        // leaves them; the rest join the bank its arena origin last opened. A boss rising where another fell (still
        // fading out there) opens a bank of its own.
        int bank = -1;
        if (cast_no >= 0) {
            if (cast_no > 0 && g_last_cast_bank.bank >= 0 && g_last_cast_bank.origin[0] == arena_origin[0] &&
                g_last_cast_bank.origin[1] == arena_origin[1] && g_last_cast_bank.origin[2] == arena_origin[2] &&
                std::ranges::any_of(g_arena_slots, [&](const ArenaSlot &other) {
                    return other.active && other.bank == g_last_cast_bank.bank;
                })) {
                bank = g_last_cast_bank.bank;
            } else {
                // one no living cast holds (a fallen boss's cast may still be fading out under its own)
                bank = 0;
                while (bank < MONSTOR_SLOT_MAX - 1 && std::ranges::any_of(g_arena_slots, [&](const ArenaSlot &other) {
                           return other.active && other.bank == bank;
                       })) {
                    ++bank;
                }
                dc::game::ResetMonsterScriptGlobals(bank);
                g_last_cast_bank = {bank, {arena_origin[0], arena_origin[1], arena_origin[2]}};
            }
        }
        g_pending_arena_origin = {expected_actor_slot, {arena_origin[0], arena_origin[1], arena_origin[2]}, grounded, 0.0f,
                                  cast_no, bank};
    }
    const int spawned = unit->SetupViewMonstor(loaded_model_slot, position.data(), -1);
    const float spawn_height = g_pending_arena_origin.script_y;
    const int spawn_bank = g_pending_arena_origin.bank;
    g_pending_arena_origin = {};
    if (spawned == 0 || unit->current_monster != expected_actor_slot) {
        return Failed("the native dungeon manager could not activate the enemy actor");
    }

    const int slot = unit->current_monster;
    MONSTOR &monster = unit->monster[slot];
    monster.max_hp = definition->second.hp;
    monster.hp = definition->second.hp;
    monster.defense = static_cast<s16>(definition->second.defense);
    monster.exp = definition->second.reward_exp;
    monster.money = definition->second.reward_money;
    // A model the game hides until something reveals it (a mimic until its chest opens, a boss until its own scene
    // brings it on) would stay hidden: not drawn and not hittable, while still attacking (a Warden Dran). A mod's
    // enemy comes on revealed; mimics keep their chest.
    const int kind = unit->model[loaded_model_slot].kind;
    if (monster.revealed == 0 && kind != MONSTER_KIND_MIMIC_SMALL && kind != MONSTER_KIND_MIMIC_LARGE) monster.revealed = 1; // its reveal program runs, as a mimic's does
    unit->chara[slot][0].SetRotation(rotation[0], rotation[1], rotation[2]);

    const std::uint32_t generation = EnsureMonsterHandleGeneration(unit, slot);
    ChronicleMonsterActorOwned(unit, slot, mod_id, character_id, definition->second.native_model_no);
    if (origin_field != request.end()) {
        g_arena_slots[slot] = {true,  {arena_origin[0], arena_origin[1], arena_origin[2]}, grounded, spawn_height, false, cast_no,
                               spawn_bank};
    }
    float player_position[4]{};
    CharaMain.GetPosition(player_position);
    const float dx = position[0] - player_position[0];
    const float dy = position[1] - player_position[1];
    const float dz = position[2] - player_position[2];
    const float player_distance = std::sqrt(dx * dx + dy * dy + dz * dz);
    if (monster.revealed != 0 && std::isfinite(player_distance) && player_distance < monster.clip_distance) {
        // SetupViewMonstor starts every actor dormant. A mod spawn close to the
        // player is intentionally live immediately, including while a native
        // scene transition is holding the monster simulation. CheckViewLevel
        // applies the normal view budget on the next simulation step.
        monster.state = 2;
    }
    const std::string handle = MonsterHandleId(slot, generation);
    Json result = ModCharacterJson(definition->second);
    result["id"] = handle;
    result["entity_handle"] = handle;
    result["status"] = "alive";
    result["slot"] = slot;
    result["generation"] = generation;
    result["position"] = {{"x", position[0]}, {"y", position[1]}, {"z", position[2]}};
    result["rotation"] = {{"x", rotation[0]}, {"y", rotation[1]}, {"z", rotation[2]}};
    result["health"] = {{"current", monster.hp}, {"maximum", monster.max_hp}};
    result["stats"] = {{"hp", monster.max_hp}, {"defense", monster.defense},
                       {"reward_exp", monster.exp}, {"reward_money", monster.money}};
    SharedRuntime().QueueEvent("game.enemy_spawned",
        {{"entity", handle}, {"character_id", character_id}, {"slot", slot}, {"generation", generation},
         {"native_model_id", definition->second.native_model_no},
         {"position", {{"x", position[0]}, {"y", position[1]}, {"z", position[2]}}},
         {"source_mod", mod_id}, {"frame", SharedRuntime().Frame()}});
    return {ApiCallStatus::Ok, std::move(result), {}};
}

Json DungeonLayoutJson() {
    return {{"rooms", DngLayoutRooms > 0 ? Json(DngLayoutRooms) : Json("native")},
            {"area", DNG_BUILD_AREA}, {"room_min", DNG_BUILD_ROOM_MIN},
            {"room_max", DNG_BUILD_ROOM_MIN + DNG_BUILD_ROOM_SPAN - 1},
            {"grid", DNG_GRID}, {"room_limit", DNG_ROOM_MAX}};
}

ApiCallResult SetExitRoom(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("enabled") || !args[0]["enabled"].is_boolean()) {
        return Invalid("expected {enabled = true or false}");
    }
    const bool previous = DngExitRoomEnabled != 0;
    DngExitRoomEnabled = args[0]["enabled"].get<bool>() ? 1 : 0;
    return {ApiCallStatus::Ok,
            {{"enabled", DngExitRoomEnabled != 0}, {"previous", previous},
             {"current_floor_has_room", DngExitRoomValid != 0}},
            {}};
}

// dungeons.set_opened_chests: chests (by their number on the floor) the next build of the front floor and of its back
// floor opens straight away, once each. A floor built again from its kept seed puts its chests in the same order.
std::vector<int> g_opened_chests[2];
bool g_opened_chests_set[2] = {false, false};
ApiCallResult SetOpenedChests(const Json &args) {
    const auto usage = "expected {chests = {0, 3}, back_chests = {1}} (chest numbers 0-" + std::to_string(DNG_BOX_MAX - 1) +
                       "), or {} to open none";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
    std::vector<int> lists[2];
    for (const auto &field : args[0].items()) {
        const int side = field.key() == "chests" ? 0 : field.key() == "back_chests" ? 1 : -1;
        if (side < 0 || !(field.value().is_array() || (field.value().is_object() && field.value().empty()))) return Invalid(usage);
        for (const Json &chest : field.value()) {
            if (!chest.is_number_integer() || chest.get<long long>() < 0 || chest.get<long long>() >= DNG_BOX_MAX) {
                return Invalid("chest numbers must be integers from 0 to " + std::to_string(DNG_BOX_MAX - 1));
            }
            lists[side].push_back(chest.get<int>());
        }
    }
    for (int side = 0; side < 2; side++) {
        g_opened_chests[side] = lists[side];
        g_opened_chests_set[side] = !lists[side].empty();
    }
    return {ApiCallStatus::Ok, {{"chests", lists[0].size()}, {"back_chests", lists[1].size()}}, {}};
}

// dungeons.chests_get: the chests on the floor the player is on, by the numbers game.chest.opened and
// dungeons.set_opened_chests use. A mimic's box is left out.
ApiCallResult GetChests(const Json &args) {
    if (!args.is_array() || args.size() > 1 || (args.size() == 1 && !(args[0].is_object() && args[0].empty()))) {
        return Invalid("dungeons.chests_get takes no arguments");
    }
    if (!NowDngMap || !UserStatus || mode != GAME_MODE_DUNGEON) return Failed("no dungeon floor is loaded");
    Json chests = Json::array();
    for (int chest = 0; chest < DNG_BOX_MAX; ++chest) {
        const auto &box = NowDngMap->boxes[chest];
        if (box.used == 0) continue;
        bool openable = false;
        for (const auto &event : NowDngMap->events) {
            if (event.index == chest && event.kind == DNG_EVENT_MIMIC) { openable = false; break; }
            if (event.index == chest && event.kind == DNG_EVENT_TREASURE_BOX) openable = true;
        }
        if (!openable && box.closed != 0) continue; // a mimic
        chests.push_back({{"chest", chest}, {"large", box.kind == TREASURE_BOX_LARGE}, {"opened", box.closed == 0},
                          {"item_id", box.item_no}, {"x", box.pos[0]}, {"z", box.pos[2]}});
    }
    return {ApiCallStatus::Ok, {{"back_floor", NowDngMap == &UraDungeonMap}, {"chests", chests}}, {}};
}

// dungeons.set_seed: the next floor built for the first time is built from this seed (once), so a mod that keeps a
// floor's seed gets the same floor back. A floor that can't be crossed is still built again from another seed;
// world.get().floor_seed reports the one it was built from.
ApiCallResult SetDungeonSeed(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() > 1 ||
        (args[0].size() == 1 && !args[0].contains("seed"))) {
        return Invalid("expected {seed = 0-2147483647}, or {} to let the next floor pick its own");
    }
    const int previous = DngNextSeed;
    if (args[0].empty()) {
        DngNextSeed = -1;
    } else {
        const Json &seed = args[0]["seed"];
        if (!seed.is_number_integer() || seed.get<long long>() < 0 || seed.get<long long>() > 2147483647LL) {
            return Invalid("seed must be an integer from 0 to 2147483647");
        }
        DngNextSeed = seed.get<int>();
    }
    return {ApiCallStatus::Ok,
            {{"seed", DngNextSeed >= 0 ? Json(DngNextSeed) : Json("random")},
             {"previous", previous >= 0 ? Json(previous) : Json("random")}},
            {}};
}

ApiCallResult SetDungeonLayout(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {rooms = 2-" + std::to_string(DNG_ROOM_MAX) + ", area = 8-" + std::to_string(DNG_GRID - 2) +
                       ", room_min = 3-6, room_max = room_min-6}, or {} for the native layout");
    }
    const Json &request = args[0];
    if (request.empty()) {
        std::erase_if(g_layout_requests, [owner](const auto &entry) { return entry.first == owner; });
        RestoreLayoutRequest();
        return {ApiCallStatus::Ok, DungeonLayoutJson(), {}};
    }
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "rooms" && field.key() != "area" && field.key() != "room_min" && field.key() != "room_max") {
            return Invalid("unsupported dungeon layout field '" + field.key() + "'");
        }
    }
    int rooms = 0, area = DNG_GRID - 2, room_min = 3, room_max = 4;
    if (!ReadBoundedInt(request, "rooms", 2, DNG_ROOM_MAX, rooms)) {
        return Invalid("rooms must be an integer from 2 to " + std::to_string(DNG_ROOM_MAX));
    }
    if (request.contains("area") && !ReadBoundedInt(request, "area", 8, DNG_GRID - 2, area)) {
        return Invalid("area must be an integer from 8 to " + std::to_string(DNG_GRID - 2) +
                       " (the last two columns hold the back-floor road and its border)");
    }
    // Chronicle puts a room's door on the middle cell of a wall (width / 2). A
    // 2-cell wall has no middle, so the door lands on a corner, which has no
    // door piece: the corridor dead-ends against the corner with a gap.
    if (request.contains("room_min") && !ReadBoundedInt(request, "room_min", 3, 6, room_min)) {
        return Invalid("room_min must be an integer from 3 to 6 (a 2-cell wall cannot hold a Chronicle door)");
    }
    if (request.contains("room_max") && !ReadBoundedInt(request, "room_max", room_min, 6, room_max)) {
        return Invalid("room_max must be an integer from room_min to 6");
    }
    if (room_max < room_min) room_max = room_min;
    if (room_max + 1 > area) return Invalid("area must be larger than room_max");
    std::erase_if(g_layout_requests, [owner](const auto &entry) { return entry.first == owner; });
    g_layout_requests.push_back({std::string(owner), {rooms, area, room_min, room_max}});
    RestoreLayoutRequest();
    return {ApiCallStatus::Ok, DungeonLayoutJson(), {}};
}

ApiCallResult SetActiveMonsterLimit(const Json &args) {
    int active = 0;
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !ReadBoundedInt(args[0], "active", 1, MONSTOR_SLOT_MAX, active)) {
        return Invalid("expected {active = 1-" + std::to_string(MONSTOR_SLOT_MAX) + "}");
    }
    const int previous = MonstorActiveLimit;
    MonstorActiveLimit = active;
    return {ApiCallStatus::Ok, {{"active", active}, {"previous", previous}, {"slots", MONSTOR_SLOT_MAX}}, {}};
}

Json ModAnimationJson(const ModAnimation &animation) {
    return {{"id", animation.id}, {"model", animation.model_id}, {"motion_index", animation.motion_index},
            {"status", "registered"}, {"motion_validation", "deferred_until_model_is_loaded"}};
}

ApiCallResult RegisterAnimation(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 3 ||
        !args[0].contains("id") || !args[0]["id"].is_string() || !args[0].contains("model") ||
        !args[0]["model"].is_string() || !args[0].contains("motion_index")) {
        return Invalid("expected {id = local animation id, model = owned registered model id, motion_index = 0-255}");
    }
    std::string animation_id;
    std::string model_id;
    if (!QualifyModContentId(mod_id, args[0]["id"].get_ref<const std::string &>(), animation_id, "animation")) {
        return Invalid("animation id must be a stable identifier in this mod's namespace");
    }
    if (!QualifyModContentId(mod_id, args[0]["model"].get_ref<const std::string &>(), model_id, "model")) {
        return Invalid("model must identify a registered model owned by this mod");
    }
    int motion_index = -1;
    if (!ReadBoundedInt(args[0], "motion_index", 0, 255, motion_index)) {
        return Invalid("motion_index must be an integer from 0 to 255");
    }
    const auto model = g_mod_models.find(model_id);
    if (model == g_mod_models.end()) return Invalid("model does not identify a registered custom model");
    if (model->second.owner != mod_id) {
        return {ApiCallStatus::PermissionDenied, nullptr, "a mod can only bind animations to its own model"};
    }
    const std::string model_path = NativeMonsterAssetPath(model->second.model, "chr");
    if (!NativeMonsterAssetReady(mod_id, model_path)) {
        return Failed("the selected custom model's native .chr override is not active");
    }
    if (g_mod_animations.contains(animation_id)) return Invalid("an animation with this stable id is already registered");
    if (g_mod_animations.size() >= 256) return Failed("the runtime has reached its 256 registered animation limit");
    ModAnimation animation{animation_id, std::string(mod_id), model_id, motion_index};
    g_mod_animations.emplace(animation_id, std::move(animation));
    return {ApiCallStatus::Ok, ModAnimationJson(g_mod_animations.at(animation_id)), {}};
}

ApiCallResult PlayAnimation(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 2 ||
        !args[0].contains("animation") || !args[0].contains("entity") || !args[0]["animation"].is_string()) {
        return Invalid("expected {animation = registered animation id, entity = owned live entity handle}");
    }
    std::string animation_id;
    if (!QualifyModContentId(mod_id, args[0]["animation"].get_ref<const std::string &>(), animation_id,
                             "animation")) {
        return Invalid("animation must identify an animation registered by this mod");
    }
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    if (!ReadId(args[0]["entity"], handle) || !ReadMonsterHandle(handle, slot, generation)) {
        return Invalid("entity must be a generation-checked Chronicle monster handle");
    }
    const auto animation = g_mod_animations.find(animation_id);
    if (animation == g_mod_animations.end()) return Invalid("animation is not registered by this mod");
    if (animation->second.owner != mod_id) {
        return {ApiCallStatus::PermissionDenied, nullptr, "a mod can only play its own registered animations"};
    }
    const ChronicleMonsterHandleRecord &known_spawn = g_monster_handle_records[slot];
    if (known_spawn.owner.empty() || known_spawn.character_id.empty() || known_spawn.generation != generation) {
        return Failed("animations.play only supports mod-owned enemies created by characters.spawn");
    }
    if (known_spawn.owner != mod_id) return {ApiCallStatus::PermissionDenied, nullptr,
                                             "a mod can only animate entities that it spawned"};
    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(status) || unit == nullptr) return Failed("animations.play requires an active Chronicle dungeon");
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) {
        return Failed("the entity handle is stale because its monster slot was reused");
    }
    const ChronicleMonsterHandleRecord &spawn = g_monster_handle_records[slot];
    const MONSTOR &monster = unit->monster[slot];
    if ((monster.state != 1 && monster.state != 2) || monster.hp <= 0) {
        return Failed("animations.play requires a living entity");
    }
    const auto character = g_mod_characters.find(spawn.character_id);
    if (character == g_mod_characters.end() || character->second.owner != mod_id ||
        character->second.model_id != animation->second.model_id) {
        return Failed("the spawned character does not use the model bound to this animation");
    }

    ModAnimationPlayback playback{std::string(mod_id), animation_id, animation->second.motion_index, {}};
    const auto prior = g_mod_animation_plays.find(handle);
    bool found_part = false;
    for (int part_index = 0; part_index < 3; ++part_index) {
        CCharacter &part = unit->chara[slot][part_index];
        if (part.frame == nullptr) continue;
        int local_motion_index = -1;
        if (part.GetMotionParam(animation->second.motion_index, &local_motion_index, nullptr, nullptr, nullptr) == nullptr) {
            return Failed("the loaded character model does not contain this registered motion_index");
        }
        found_part = true;
        ModAnimationPartPlayback &state = playback.parts[part_index];
        state.used = true;
        state.previous_motion = part.motion_no;
        state.previous_flags = part.motion_flags;
        state.previous_speed = part.motion_speed;
        if (prior != g_mod_animation_plays.end() && prior->second.owner == mod_id &&
            prior->second.motion_index == part.motion_no && prior->second.parts[part_index].used) {
            state.previous_motion = prior->second.parts[part_index].previous_motion;
            state.previous_flags = prior->second.parts[part_index].previous_flags;
            state.previous_speed = prior->second.parts[part_index].previous_speed;
        }
    }
    if (!found_part) return Failed("the spawned character's native model is not loaded");
    for (int part_index = 0; part_index < 3; ++part_index) {
        if (playback.parts[part_index].used) unit->chara[slot][part_index].SetMotion(playback.motion_index, 4);
    }
    g_mod_animation_plays[handle] = playback;
    return {ApiCallStatus::Ok,
            {{"animation", animation_id}, {"entity", handle}, {"motion_index", playback.motion_index},
             {"status", "playing"}},
            {}};
}

ApiCallResult StopAnimation(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("entity")) {
        return Invalid("expected {entity = handle with a mod-started animation}");
    }
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    if (!ReadId(args[0]["entity"], handle) || !ReadMonsterHandle(handle, slot, generation)) {
        return Invalid("entity must be a generation-checked Chronicle monster handle");
    }
    const auto found = g_mod_animation_plays.find(handle);
    if (found == g_mod_animation_plays.end()) return Failed("this entity has no active MoonMod animation");
    if (found->second.owner != mod_id) {
        return {ApiCallStatus::PermissionDenied, nullptr, "a mod can only stop animations that it started"};
    }
    const ModAnimationPlayback playback = found->second;
    g_mod_animation_plays.erase(found);
    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(status) || unit == nullptr) {
        return {ApiCallStatus::Ok, {{"entity", handle}, {"status", "expired"}, {"changed", false}}, {}};
    }
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation ||
        g_monster_handle_records[slot].generation != generation || g_monster_handle_records[slot].owner != mod_id ||
        unit->monster[slot].state == -1 || unit->monster[slot].hp <= 0) {
        return {ApiCallStatus::Ok, {{"entity", handle}, {"status", "expired"}, {"changed", false}}, {}};
    }
    bool changed = false;
    bool restored_previous = false;
    for (int part_index = 0; part_index < 3; ++part_index) {
        const ModAnimationPartPlayback &state = playback.parts[part_index];
        if (!state.used) continue;
        CCharacter &part = unit->chara[slot][part_index];
        if (part.frame == nullptr || part.motion_no != playback.motion_index) continue;
        int previous_local_index = -1;
        if (state.previous_motion < 0 ||
            part.GetMotionParam(state.previous_motion, &previous_local_index, nullptr, nullptr, nullptr) == nullptr) {
            continue;
        }
        changed |= part.motion_no != state.previous_motion || part.motion_flags != state.previous_flags ||
                   part.motion_speed != state.previous_speed;
        part.SetMotion(state.previous_motion, state.previous_flags, state.previous_speed);
        restored_previous = true;
    }
    return {ApiCallStatus::Ok,
            {{"entity", handle}, {"status", "stopped"}, {"changed", changed},
             {"restored_previous_motion", restored_previous}},
            {}};
}

void UnloadChronicleAnimations(std::string_view owner) {
    std::vector<std::string> handles;
    for (const auto &[handle, playback] : g_mod_animation_plays) {
        if (playback.owner == owner) handles.push_back(handle);
    }
    for (const std::string &handle : handles) {
        (void)StopAnimation(owner, Json::array({Json{{"entity", handle}}}));
    }
    std::erase_if(g_mod_animations, [owner](const auto &entry) { return entry.second.owner == owner; });
    std::erase_if(g_mod_animation_plays, [owner](const auto &entry) { return entry.second.owner == owner; });
}

ApiCallResult GetEntity(const Json &args) {
    if (!args.is_array() || args.size() != 1) return Invalid("expected one entity handle or {id = entity_handle}");
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    if (!ReadId(args[0], handle) || !ReadMonsterHandle(handle, slot, generation)) {
        return Invalid("entity handle must be a Chronicle monster handle");
    }

    Json result = {{"id", handle}, {"type", "monster"}, {"slot", slot}, {"generation", generation}};
    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(status) || unit == nullptr) {
        result["status"] = "expired";
        return {ApiCallStatus::Ok, std::move(result), {}};
    }
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) {
        result["status"] = "expired";
        return {ApiCallStatus::Ok, std::move(result), {}};
    }
    const MONSTOR &monster = unit->monster[slot];
    if (monster.state == -1) {
        result["status"] = "expired";
        return {ApiCallStatus::Ok, std::move(result), {}};
    }
    if (monster.hp <= 0) {
        result["status"] = "dead";
        return {ApiCallStatus::Ok, std::move(result), {}};
    }

    float position[4]{};
    unit->chara[slot][0].GetPosition(position);
    result["status"] = "alive";
    result["position"] = {{"x", position[0]}, {"y", position[1]}, {"z", position[2]}};
    result["health"] = {{"current", monster.hp}, {"maximum", monster.max_hp}};
    result["combat"] = {{"defense", monster.defense}, {"reward_exp", monster.exp}, {"reward_money", monster.money},
                         {"drop_item", monster.drop_item}};
    result["definition"] = {{"kind", monster.kind}, {"family", monster.attachment_kind}, {"name_id", monster.name_no},
                            {"native_model_id", ChronicleNativeMonsterModelId(unit, slot)}};
    const ChronicleMonsterHandleRecord &record = g_monster_handle_records[slot];
    if (record.generation == generation && !record.character_id.empty()) {
        result["character"] = record.character_id;
        result["mod_owned"] = true;
        result["owner"] = record.owner;
    } else {
        result["mod_owned"] = false;
    }
    return {ApiCallStatus::Ok, std::move(result), {}};
}

ApiCallResult SetEntityStats(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("entity")) {
        return Invalid("expected {entity = generation_checked_handle, hp?, max_hp?, defense?, reward_exp?, reward_money?, drop_item?, "
                       "damage_scale?, hit_reach?, hit_share?}");
    }
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "entity" && field.key() != "hp" && field.key() != "max_hp" &&
            field.key() != "defense" && field.key() != "reward_exp" && field.key() != "reward_money" &&
            field.key() != "drop_item" && field.key() != "damage_scale" && field.key() != "hit_reach" &&
            field.key() != "hit_share") {
            return Invalid("entities.set_stats contains an unsupported field");
        }
    }
    if (request.size() < 2) return Invalid("entities.set_stats requires at least one stat to change");
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    if (!ReadId(request["entity"], handle) || !ReadMonsterHandle(handle, slot, generation)) {
        return Invalid("entity must be a generation-checked Chronicle monster handle");
    }

    const bool set_hp = request.contains("hp");
    const bool set_max_hp = request.contains("max_hp");
    const bool set_defense = request.contains("defense");
    const bool set_exp = request.contains("reward_exp");
    const bool set_money = request.contains("reward_money");
    const bool set_drop = request.contains("drop_item");
    const bool set_damage_scale = request.contains("damage_scale");
    float requested_damage_scale = 1.0f;
    if (set_damage_scale) {
        const Json &value = request["damage_scale"];
        if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < 0.0 || value.get<double>() > 100.0) {
            return Invalid("damage_scale must be a multiplier from 0 to 100");
        }
        requested_damage_scale = value.get<float>();
    }
    const bool set_hit_reach = request.contains("hit_reach");
    float requested_hit_reach = 0.0f;
    if (set_hit_reach) {
        const Json &value = request["hit_reach"];
        if (!value.is_number() || !std::isfinite(value.get<double>()) || value.get<double>() < 0.0 || value.get<double>() > 200.0) {
            return Invalid("hit_reach must be from 0 to 200 game units");
        }
        requested_hit_reach = value.get<float>();
    }
    const bool set_hit_share = request.contains("hit_share");
    if (set_hit_share && !request["hit_share"].is_boolean()) return Invalid("hit_share must be true or false");
    const bool requested_hit_share = set_hit_share && request["hit_share"].get<bool>();
    int requested_hp = 0, requested_max_hp = 0, requested_defense = 0;
    int requested_exp = 0, requested_money = 0;
    int requested_drop = -1;
    if ((set_hp && !ReadBoundedInt(request, "hp", 1, 1000000, requested_hp)) ||
        (set_max_hp && !ReadBoundedInt(request, "max_hp", 1, 1000000, requested_max_hp)) ||
        (set_defense && !ReadBoundedInt(request, "defense", 0, 32767, requested_defense)) ||
        (set_exp && !ReadBoundedInt(request, "reward_exp", 0, 32767, requested_exp)) ||
        (set_money && !ReadBoundedInt(request, "reward_money", 0, std::numeric_limits<int>::max(), requested_money)) ||
        (set_drop && !ReadBoundedInt(request, "drop_item", -1, ITEM_WEAPON_END - 1, requested_drop))) {
        return Invalid("hp/max_hp must be within 1..1000000, defense 0..32767, reward_exp 0..32767, reward_money 0..INT_MAX, and drop_item -1 or a native item number");
    }
    if (set_drop && requested_drop != -1) {
        const COM_ITEM_INFO *drop_info = requested_drop < ITEM_ATTACH_START ? nullptr : GetCommonItemInfo(requested_drop);
        if (drop_info == nullptr || drop_info->kind < ITEMKIND_ATTACH || drop_info->kind > ITEMKIND_WEAPON ||
            (requested_drop >= ITEM_WEAPON_START &&
             (requested_drop >= ITEM_WEAPON_END || GetWeaponDataInfo(requested_drop - ITEM_WEAPON_START) == nullptr))) {
            return Invalid("drop_item must be -1 or a valid built-in attachment, key, consumable, or weapon number");
        }
    }

    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(status) || unit == nullptr) {
        return Failed("entities.set_stats requires an active Chronicle dungeon");
    }
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) {
        return Failed("the entity handle is stale because its monster slot was reused");
    }
    MONSTOR &monster = unit->monster[slot];
    if (monster.state != 2 || monster.hp <= 0 || monster.max_hp <= 0 || monster.hp > monster.max_hp ||
        monster.revealed == 0) {
        return Failed("stats can only be changed on a live, revealed Chronicle enemy");
    }
    const int hp_before = monster.hp;
    const int max_hp_before = monster.max_hp;
    const int defense_before = monster.defense;
    const int exp_before = monster.exp;
    const int money_before = monster.money;
    const int drop_before = monster.drop_item;
    const float damage_scale_before = g_monster_damage_scales[slot];
    const float hit_reach_before = g_monster_hit_reach[slot];
    const bool hit_share_before = g_monster_hit_share[slot];
    const int final_max_hp = set_max_hp ? requested_max_hp : monster.max_hp;
    if (set_hp && requested_hp > final_max_hp) return Invalid("hp cannot exceed the enemy's resulting max_hp");
    int final_hp = monster.hp;
    if (set_hp) {
        final_hp = requested_hp;
    } else if (set_max_hp) {
        final_hp = static_cast<int>((static_cast<std::int64_t>(monster.hp) * final_max_hp + monster.max_hp / 2) /
                                    monster.max_hp);
        final_hp = std::clamp(final_hp, 1, final_max_hp);
    }
    monster.max_hp = final_max_hp;
    monster.hp = final_hp;
    if (set_defense) monster.defense = static_cast<s16>(requested_defense);
    if (set_exp) monster.exp = requested_exp;
    if (set_money) monster.money = requested_money;
    if (set_drop) monster.drop_item = static_cast<s16>(requested_drop);
    if (set_damage_scale) g_monster_damage_scales[slot] = requested_damage_scale;
    if (set_hit_reach) g_monster_hit_reach[slot] = requested_hit_reach;
    if (set_hit_share) g_monster_hit_share[slot] = requested_hit_share;
    const bool changed = hp_before != monster.hp || max_hp_before != monster.max_hp ||
                         defense_before != monster.defense || exp_before != monster.exp || money_before != monster.money ||
                         drop_before != monster.drop_item || damage_scale_before != g_monster_damage_scales[slot] ||
                         hit_reach_before != g_monster_hit_reach[slot] || hit_share_before != g_monster_hit_share[slot];
    return {ApiCallStatus::Ok,
            {{"entity", handle}, {"changed", changed},
             {"previous", {{"hp", hp_before}, {"max_hp", max_hp_before}, {"defense", defense_before},
                           {"reward_exp", exp_before}, {"reward_money", money_before}, {"drop_item", drop_before},
                           {"damage_scale", damage_scale_before}, {"hit_reach", hit_reach_before},
                           {"hit_share", hit_share_before}}},
             {"current", {{"hp", monster.hp}, {"max_hp", monster.max_hp}, {"defense", monster.defense},
                           {"reward_exp", monster.exp}, {"reward_money", monster.money}, {"drop_item", monster.drop_item},
                           {"damage_scale", g_monster_damage_scales[slot]}, {"hit_reach", g_monster_hit_reach[slot]},
                           {"hit_share", g_monster_hit_share[slot]}}}}, {}};
}

// A boss cast's shared script integers (SET_GLOBAL_INT), through a live member: what a part of the fight would have
// written had it lived to write it (a broken hand that died outside its own attack).
ApiCallResult SetEntityScriptInt(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("entity") ||
        !args[0].contains("index") || !args[0].contains("value")) {
        return Invalid("expected {entity = generation_checked_handle, index = 0-255, value = integer}");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "entity" && field.key() != "index" && field.key() != "value") {
            return Invalid("entities.script_int_set accepts only entity, index and value");
        }
    }
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    if (!ReadId(args[0]["entity"], handle) || !ReadMonsterHandle(handle, slot, generation)) {
        return Invalid("entity must be a generation-checked Chronicle monster handle");
    }
    const Json &index = args[0]["index"];
    const Json &value = args[0]["value"];
    if (!index.is_number_integer() || index.get<int>() < 0 || index.get<int>() >= MONSTOR_SLOT_MAX) {
        return Invalid("index is a whole number from 0 to 255");
    }
    if (!value.is_number_integer() || value.get<std::int64_t>() < INT32_MIN || value.get<std::int64_t>() > INT32_MAX) {
        return Invalid("value is a 32-bit whole number");
    }
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(CurrentStatus()) || unit == nullptr) {
        return Failed("entities.script_int_set requires an active Chronicle dungeon");
    }
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) return Failed("the entity handle is stale because its monster slot was reused");
    if (unit != g_monster_handle_unit || unit->monster[slot].state != 2 || unit->monster[slot].hp <= 0 ||
        !g_arena_slots[slot].active || g_arena_slots[slot].bank < 0) {
        return Failed("the entity must be a live member of a boss cast (characters.spawn arena_slot)");
    }
    int *integer = dc::game::MonsterScriptGlobal(g_arena_slots[slot].bank, index.get<int>());
    if (integer == nullptr) return Failed("the cast has no such script integer");
    const int previous = *integer;
    *integer = value.get<int>();
    return {ApiCallStatus::Ok, {{"entity", handle}, {"index", index.get<int>()}, {"previous", previous}, {"value", *integer}}, {}};
}

ApiCallResult SetEntityScale(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("entity") ||
        !args[0].contains("scale")) {
        return Invalid("expected {entity = generation_checked_handle, scale = multiplier from 0.5 to 3.0}");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "entity" && field.key() != "scale") return Invalid("entities.scale_set accepts only entity and scale");
    }
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    if (!ReadId(args[0]["entity"], handle) || !ReadMonsterHandle(handle, slot, generation)) {
        return Invalid("entity must be a generation-checked Chronicle monster handle");
    }
    if (!args[0]["scale"].is_number()) return Invalid("scale must be a finite multiplier from 0.5 to 3.0");
    const double requested = args[0]["scale"].get<double>();
    if (!std::isfinite(requested) || requested < 0.5 || requested > 3.0) {
        return Invalid("scale must be a finite multiplier from 0.5 to 3.0");
    }
    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(status) || unit == nullptr) return Failed("entities.scale_set requires an active Chronicle dungeon");
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) return Failed("the entity handle is stale because its monster slot was reused");
    const MONSTOR &monster = unit->monster[slot];
    if (monster.state != 2 || monster.hp <= 0 || monster.revealed == 0) {
        return Failed("scale can only be changed on a live, revealed Chronicle enemy");
    }
    ChronicleEntityScaleRecord &record = g_monster_scale_records[slot];
    if (record.generation != generation || record.unit != unit) {
        std::array<float, 3> original{};
        for (int axis = 0; axis < 3; ++axis) {
            const float value = unit->chara[slot][0].scale[axis];
            if (!std::isfinite(value) || value <= 0.0f) return Failed("the enemy has an invalid native model scale");
            original[axis] = value;
        }
        record = {};
        record.generation = generation;
        record.unit = unit;
        record.owner = std::string(owner);
        record.original = original;
    } else if (record.owner != owner) {
        return Failed("another mod already owns the visual scale for this enemy");
    }
    const float factor = static_cast<float>(requested);
    const float previous_factor = record.factor;
    const std::array<float, 3> previous = {unit->chara[slot][0].scale[0], unit->chara[slot][0].scale[1],
                                           unit->chara[slot][0].scale[2]};
    unit->chara[slot][0].SetScale(record.original[0] * factor, record.original[1] * factor,
                                  record.original[2] * factor);
    record.factor = factor;
    const bool changed = previous[0] != unit->chara[slot][0].scale[0] ||
                         previous[1] != unit->chara[slot][0].scale[1] ||
                         previous[2] != unit->chara[slot][0].scale[2];
    return {ApiCallStatus::Ok,
            {{"entity", handle}, {"changed", changed}, {"previous_factor", previous_factor},
             {"scale", factor}, {"owner", record.owner}}, {}};
}

ApiCallResult GetEntityVelocity(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {entity = generation_checked_monster_handle}");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "entity") return Invalid("physics.velocity_get accepts only the entity field");
    }
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    if (!ReadId(args[0]["entity"], handle) || !ReadMonsterHandle(handle, slot, generation)) {
        return Invalid("entity must be a generation-checked Chronicle monster handle");
    }
    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(status) || unit == nullptr) {
        return Failed("physics.velocity_get requires an active Chronicle dungeon");
    }
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) {
        return Failed("the entity handle is stale because its monster slot was reused");
    }
    const MONSTOR &monster = unit->monster[slot];
    if (monster.state != 2) {
        float player_position[4]{};
        float entity_position[4]{};
        CharaMain.GetPosition(player_position);
        unit->chara[slot][0].GetPosition(entity_position);
        const float dx = entity_position[0] - player_position[0];
        const float dy = entity_position[1] - player_position[1];
        const float dz = entity_position[2] - player_position[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        return Failed("the selected enemy is not active (native state " + std::to_string(monster.state) +
                      ", player distance " + std::to_string(distance) + ", activation distance " +
                      std::to_string(monster.clip_distance) + ")");
    }
    if (monster.hp <= 0 || monster.max_hp <= 0 || monster.hp > monster.max_hp) {
        return Failed("the selected entity is not a live, valid enemy");
    }

    float velocity[3]{};
    std::string source = "stopped";
    if (monster.stop_timer <= 0) {
        const bool in_knockback = !(monster.knockback[0] <= 0.0f);
        const float speed = in_knockback ? monster.knockback[0] : monster.movement_speed;
        const float *direction = in_knockback ? monster.knockback_direction : monster.movement;
        if (!std::isfinite(speed) || speed < 0.0f || !std::isfinite(direction[0]) ||
            !std::isfinite(direction[1]) || !std::isfinite(direction[2])) {
            return Failed("the native monster movement state is not finite");
        }
        for (int axis = 0; axis < 3; ++axis) velocity[axis] = direction[axis] * speed;
        source = in_knockback ? "knockback" : "native_movement";
        if (monster.falls != 0 && monster.ground_distance > 0.0001f) velocity[1] -= 0.2f;
    }
    if (!std::isfinite(velocity[0]) || !std::isfinite(velocity[1]) || !std::isfinite(velocity[2])) {
        return Failed("the native monster movement state produced a non-finite velocity");
    }
    return {ApiCallStatus::Ok,
            {{"entity", handle}, {"units", "game_units_per_frame"}, {"source", source},
             {"semantics", "native movement intent before collision resolution"},
             {"velocity", {{"x", velocity[0]}, {"y", velocity[1]}, {"z", velocity[2]}}}},
            {}};
}

ApiCallResult SetEntityVelocity(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 2 ||
        !args[0].contains("entity") || !args[0].contains("velocity")) {
        return Invalid("expected {entity = generation_checked_monster_handle, velocity = {x, y, z}}");
    }
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    if (!ReadId(args[0]["entity"], handle) || !ReadMonsterHandle(handle, slot, generation)) {
        return Invalid("entity must be a generation-checked Chronicle monster handle");
    }
    std::array<float, 4> requested{};
    if (!ReadVector3(args[0]["velocity"], requested, 100000.0f)) {
        return Invalid("velocity must contain finite x, y, and z components within 100000 game units per frame");
    }

    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(status) || unit == nullptr) {
        return Failed("physics.velocity_set requires an active Chronicle dungeon");
    }
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) {
        return Failed("the entity handle is stale because its monster slot was reused");
    }
    MONSTOR &monster = unit->monster[slot];
    if (monster.state != 2) {
        float player_position[4]{};
        float entity_position[4]{};
        CharaMain.GetPosition(player_position);
        unit->chara[slot][0].GetPosition(entity_position);
        const float dx = entity_position[0] - player_position[0];
        const float dy = entity_position[1] - player_position[1];
        const float dz = entity_position[2] - player_position[2];
        const float distance = std::sqrt(dx * dx + dy * dy + dz * dz);
        return Failed("velocity can only be set for an active enemy (native state " + std::to_string(monster.state) +
                      ", player distance " + std::to_string(distance) + ", activation distance " +
                      std::to_string(monster.clip_distance) + ")");
    }
    if (monster.hp <= 0 || monster.max_hp <= 0 || monster.hp > monster.max_hp) {
        return Failed("velocity can only be set for a live, valid enemy");
    }
    if (monster.stop_timer > 0) {
        return Failed("the native stop status currently prevents this enemy from moving");
    }

    const ApiCallResult previous = GetEntityVelocity(Json::array({Json{{"entity", handle}}}));
    if (previous.status != ApiCallStatus::Ok) return previous;

    // Chronicle stores enemy movement as a direction and a scalar speed. Its getter
    // includes the native falling term, so compensate for gravity here to make a
    // read immediately after this write return the requested vector.
    std::array<double, 3> movement = {requested[0], requested[1], requested[2]};
    const bool falling = monster.falls != 0 && monster.ground_distance > 0.0001f;
    if (falling) movement[1] += 0.2;
    const double speed = std::sqrt(movement[0] * movement[0] + movement[1] * movement[1] + movement[2] * movement[2]);
    if (!std::isfinite(speed) || speed > std::numeric_limits<float>::max()) {
        return Invalid("velocity magnitude is outside Chronicle's native movement range");
    }

    const bool changed = previous.value.at("velocity").at("x").get<float>() != requested[0] ||
                         previous.value.at("velocity").at("y").get<float>() != requested[1] ||
                         previous.value.at("velocity").at("z").get<float>() != requested[2] ||
                         monster.knockback[0] > 0.0f;
    if (speed == 0.0) {
        monster.movement[0] = monster.movement[1] = monster.movement[2] = 0.0f;
        monster.movement_speed = 0.0f;
    } else {
        const double inverse_speed = 1.0 / speed;
        monster.movement[0] = static_cast<float>(movement[0] * inverse_speed);
        monster.movement[1] = static_cast<float>(movement[1] * inverse_speed);
        monster.movement[2] = static_cast<float>(movement[2] * inverse_speed);
        monster.movement_speed = static_cast<float>(speed);
    }
    monster.movement[3] = 1.0f;
    // A direct mod velocity replaces an in-progress hit impulse. Keeping the
    // native knockback speed would make the movement loop immediately overwrite
    // the vector written above.
    monster.knockback[0] = 0.0f;

    const ApiCallResult current = GetEntityVelocity(Json::array({Json{{"entity", handle}}}));
    if (current.status != ApiCallStatus::Ok) return current;
    return {ApiCallStatus::Ok,
            {{"entity", handle}, {"units", "game_units_per_frame"}, {"source", current.value.value("source", "native_movement")},
             {"semantics", "native movement intent before collision resolution"}, {"changed", changed},
             {"previous_velocity", previous.value.at("velocity")}, {"velocity", current.value.at("velocity")}},
            {}};
}

ApiCallResult QueryPhysics(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {shape = 'sphere', center = {x, y, z}, radius = game_units}");
    }
    const Json &query = args[0];
    for (auto field = query.begin(); field != query.end(); ++field) {
        if (field.key() != "shape" && field.key() != "center" && field.key() != "radius") {
            return Invalid("unsupported physics.query field");
        }
    }
    if (!query.contains("shape") || !query["shape"].is_string() || query["shape"].get<std::string>() != "sphere" ||
        !query.contains("center") || !query.contains("radius")) {
        return Invalid("physics.query currently requires shape 'sphere', center, and radius");
    }
    std::array<float, 4> center{};
    if (!ReadVector3(query["center"], center, 1000000.0f) || !query["radius"].is_number()) {
        return Invalid("center must contain finite game coordinates within +/-1000000 and radius must be 0-10000");
    }
    const double radius = query["radius"].get<double>();
    if (!std::isfinite(radius) || radius < 0.0 || radius > 10000.0) {
        return Invalid("radius must be finite and within 0-10000 game units");
    }

    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(status) || unit == nullptr) {
        return Failed("physics.query requires an active Chronicle dungeon");
    }
    PrepareMonsterHandleUnit(unit);
    const double radius_squared = radius * radius;
    Json matches = Json::array();
    for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) {
        const MONSTOR &monster = unit->monster[slot];
        if (monster.state != 2 || monster.hp <= 0) continue;
        const std::uint32_t generation = EnsureMonsterHandleGeneration(unit, slot);
        float position[4]{};
        unit->chara[slot][0].GetPosition(position);
        for (int axis = 0; axis < 3; ++axis) {
            if (!std::isfinite(position[axis])) return Failed("a live monster has a non-finite native position");
        }
        const double dx = static_cast<double>(position[0]) - center[0];
        const double dy = static_cast<double>(position[1]) - center[1];
        const double dz = static_cast<double>(position[2]) - center[2];
        const double distance_squared = dx * dx + dy * dy + dz * dz;
        if (distance_squared > radius_squared) continue;
        matches.push_back({{"entity", MonsterHandleId(slot, generation)}, {"type", "monster"}, {"slot", slot},
                           {"position", {{"x", position[0]}, {"y", position[1]}, {"z", position[2]}}},
                           {"distance", std::sqrt(distance_squared)}});
    }
    return {ApiCallStatus::Ok,
            {{"shape", "sphere"},
             {"semantics", "live monster actor origins inside or on the sphere; no collider or world geometry test"},
             {"units", "game_units"}, {"center", {{"x", center[0]}, {"y", center[1]}, {"z", center[2]}}},
             {"radius", radius}, {"count", matches.size()}, {"matches", std::move(matches)}},
            {}};
}

ApiCallResult DespawnEntity(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1) {
        return Invalid("expected one mod-spawned entity handle or {id = entity_handle}");
    }
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    if (!ReadId(args[0], handle) || !ReadMonsterHandle(handle, slot, generation)) {
        return Invalid("entity handle must be a Chronicle generation-checked monster handle");
    }

    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(status) || unit == nullptr) {
        return Failed("entities.despawn requires an active Chronicle dungeon");
    }
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) {
        return Failed("the entity handle is stale because its monster slot was reused");
    }

    ChronicleMonsterHandleRecord &record = g_monster_handle_records[slot];
    if (record.generation != generation || record.owner.empty() || record.character_id.empty()) {
        return Failed("Chronicle can only despawn enemy actors spawned by characters.spawn");
    }
    if (record.owner != mod_id) return Failed("a mod can only despawn enemies that it spawned");

    MONSTOR &monster = unit->monster[slot];
    if (monster.state == -1) {
        return {ApiCallStatus::Ok, {{"id", handle}, {"status", "expired"}, {"changed", false}}, {}};
    }
    if ((monster.state != 1 && monster.state != 2) || monster.hp <= 0) {
        return Failed("the selected enemy is already in a native death or transition state");
    }
    if (monster.attachment_kind == 8 || monster.revealed == 0) {
        return Failed("a mimic actor cannot be despawned because the native dungeon map retains its chest trigger");
    }

    // Remove attack hit records which still name this actor as their source.
    if (NowColData != nullptr) {
        const int owner_id = slot * 5 + 200;
        for (int index = 0; index < 96; ++index) {
            if (NowColData->active[index] != 0 &&
                (NowColData->hit[index].owner == owner_id || NowColData->hit[index].monster_no == slot)) {
                NowColData->active[index] = 0;
            }
        }
    }

    // Native enemy projectiles use user_id_2 to retain their source monster slot.
    if (NowShotEffect != nullptr) {
        for (int effect_index = 0; effect_index < 5; ++effect_index) {
            CSHOT_EFFECT &effect = NowShotEffect->effect[effect_index];
            for (int projectile = 0; projectile < 8; ++projectile) {
                if (effect.active[projectile] != 0 && effect.user_id_2[projectile] == slot) {
                    effect.OffEffect(projectile);
                }
            }
        }
    }

    if (monster.state == 1 || monster.state == 2) {
        if (unit->alive_count > 0) --unit->alive_count;
    }
    monster.state = -1;
    monster.hp = 0;
    monster.collision_poly = nullptr;
    monster.collision_poly_count = 0;
    monster.stop_timer = 0;
    monster.poison_timer = 0;
    monster.anger_timer = 0;
    monster.slow_timer = 0;
    monster.movement[0] = monster.movement[1] = monster.movement[2] = 0.0f;
    monster.movement_speed = 0.0f;
    monster.turn_target[0] = monster.turn_target[1] = monster.turn_target[2] = 0.0f;
    monster.turn_speed = 0.0f;
    monster.knockback[0] = monster.knockback[1] = 0.0f;
    monster.knockback_direction[0] = monster.knockback_direction[1] = monster.knockback_direction[2] = 0.0f;
    monster.event_flag2_pending = 0;
    unit->script_state[slot] = 0;
    if (unit->script[slot] != nullptr) unit->script[slot]->used = 0;
    for (int index = 0; index < 16; ++index) {
        unit->effect[slot].timer[index] = 0;
        unit->effect[slot].motion_start[index] = 0.0f;
        unit->effect[slot].frame[index] = nullptr;
        unit->effect2[slot].active[index] = 0;
        unit->event_flags[slot][index] = 0;
        unit->sound[slot].id[index] = -1;
        unit->sound[slot].cooldown[index] = 0;
    }
    for (int index = 0; index < 12; ++index) {
        unit->effect3[slot].timer[index] = 0;
        unit->effect3[slot].frame[index] = nullptr;
    }
    unit->effect3[slot].count = 0;
    unit->event[slot].timer = 0;
    unit->event[slot].frame = nullptr;
    unit->event2[slot].timer = 0;
    unit->event2[slot].frame = nullptr;
    unit->sound[slot].sequence_id = -1;
    const bool animation_released = g_mod_animation_plays.erase(handle) != 0;
    AdvanceMonsterHandleGeneration(slot);
    return {ApiCallStatus::Ok,
            {{"id", handle}, {"status", "despawned"}, {"changed", true},
             {"animation_released", animation_released}},
            {}};
}

ApiCallResult GetCombat(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("combat.get takes no arguments");
    CDngStatusData *status = CurrentStatus();
    if (!IsActiveDungeonStatus(status) || NowMonstorUnit == nullptr) {
        return Failed("no active dungeon combat state");
    }

    PrepareMonsterHandleUnit(NowMonstorUnit);
    Json enemies = Json::array();
    for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) {
        const MONSTOR &monster = NowMonstorUnit->monster[slot];
        if (monster.state != 2 || monster.hp <= 0) continue;
        const std::uint32_t generation = EnsureMonsterHandleGeneration(NowMonstorUnit, slot);
        enemies.push_back({{"id", "chronicle:monster-slot:" + std::to_string(slot)},
                           {"entity_id", MonsterHandleId(slot, generation)},
                           {"slot", slot},
                           {"hp", monster.hp},
                           {"max_hp", monster.max_hp},
                           {"family", monster.attachment_kind},
                           {"name_id", monster.name_no},
                           {"poison_frames", monster.poison_timer},
                           {"slow_frames", monster.slow_timer},
                           {"stun_frames", monster.stop_timer},
                           {"revealed", monster.revealed != 0}});
    }
    const std::size_t enemy_count = enemies.size();
    // Living monsters in every slot, awake or dormant (the native alive_count misses PC slots).
    int remaining = 0;
    for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) {
        const MONSTOR &monster = NowMonstorUnit->monster[slot];
        if (monster.state != -1 && monster.hp > 0) ++remaining;
    }
    return {ApiCallStatus::Ok,
            {{"in_combat", enemy_count > 0}, {"enemy_count", enemy_count}, {"alive_count", NowMonstorUnit->alive_count}, {"remaining", remaining},
             {"enemies", std::move(enemies)}},
            {}};
}

// combat.status_add haste: the steps each monster slot's haste has left (counted down as the monster runs).
std::array<int, MONSTOR_SLOT_MAX> g_monster_haste{};
constexpr float kHasteScale = 1.4f;

// combat.strike_player: a mod's own attack (a boss's new move, a trap): one native hit in the game's collision list
// that only the player can be struck by, at {x, y, z} with that reach. The game's own hit test takes it next frame, so
// a guard, a parry, a dodge's invincibility and poise all work on it, and game.player.hit reports it (no entity).
// kind: light (a flinch) or heavy (a knock-back; with guard rules on, a guard must be held). The player's defence comes
// off the damage, as from any hit.
ApiCallResult StrikePlayer(const Json &args) {
    const char *usage = "expected {x, y, z, radius = 2..120, damage = 0..9999, kind = optional light|heavy}";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "x" && field.key() != "y" && field.key() != "z" && field.key() != "radius" && field.key() != "damage" &&
            field.key() != "kind") {
            return Invalid("combat.strike_player received an unsupported field: " + field.key());
        }
    }
    float at[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const char *axes[3] = {"x", "y", "z"};
    for (int axis = 0; axis < 3; ++axis) {
        if (!request.contains(axes[axis]) || !request[axes[axis]].is_number()) return Invalid(usage);
        at[axis] = request[axes[axis]].get<float>();
        if (!std::isfinite(at[axis]) || std::fabs(at[axis]) > 100000.0f) return Invalid(usage);
    }
    if (!request.contains("radius") || !request["radius"].is_number()) return Invalid(usage);
    const float radius = request["radius"].get<float>();
    if (!(radius >= 2.0f && radius <= 120.0f)) return Invalid(usage);
    int damage = 0;
    if (!ReadBoundedInt(request, "damage", 0, 9999, damage)) return Invalid(usage);
    int kind = 2;
    if (request.contains("kind")) {
        if (request["kind"] == "heavy") kind = 3;
        else if (request["kind"] != "light") return Invalid(usage);
    }
    if (mode != GAME_MODE_DUNGEON || NowColData == nullptr || CharaMain.frame == nullptr) return Failed("not in a dungeon");
    const int slot = NowColData->Set(at, damage, 2, radius, 0.0f, 1, kind, 0, 0);
    if (slot < 0) return Failed("the hit list is full");
    return {ApiCallStatus::Ok, {{"struck", true}}, {}};
}

ApiCallResult AddMonsterStatus(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 3 ||
        !args[0].contains("entity") || !args[0].contains("status") || !args[0].contains("duration_frames") ||
        !args[0]["status"].is_string()) {
        return Invalid("expected {entity = generation_checked_handle, status = slow|stop, duration_frames = 1..32767}");
    }
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    int duration = 0;
    if (!ReadId(args[0]["entity"], handle) || !ReadMonsterHandle(handle, slot, generation) ||
        !ReadBoundedInt(args[0], "duration_frames", 1, 32767, duration)) {
        return Invalid("entity must be a Chronicle monster handle and duration_frames must be 1..32767");
    }
    const std::string status_name = args[0]["status"].get<std::string>();
    if (status_name != "slow" && status_name != "stop" && status_name != "haste") {
        return Invalid("Chronicle enemy status must be 'slow', 'stop' or 'haste'");
    }
    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(status) || unit == nullptr) {
        return Failed("combat.status_add requires an active Chronicle dungeon");
    }
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) {
        return Failed("the entity handle is stale because its monster slot was reused");
    }
    MONSTOR &monster = unit->monster[slot];
    if (monster.state != 2 || monster.hp <= 0 || monster.revealed == 0) {
        return Failed("the selected entity is not a live, revealed enemy");
    }
    if (status_name == "haste") {
        // Not a native status: the monster's scripted motions and moves play at kHasteScale until it runs out.
        const int before = g_monster_haste[slot];
        g_monster_haste[slot] = std::max(before, duration);
        const int now = g_monster_haste[slot];
        return {ApiCallStatus::Ok,
                {{"entity", handle}, {"status", status_name}, {"duration_frames", now}, {"previous_frames", before}, {"changed", now != before}},
                {}};
    }
    int &timer = status_name == "slow" ? monster.slow_timer : monster.stop_timer;
    const int before = timer;
    timer = std::max(timer, duration);
    return {ApiCallStatus::Ok,
            {{"entity", handle}, {"status", status_name}, {"duration_frames", timer},
             {"previous_frames", before}, {"changed", timer != before}},
            {}};
}

ApiCallResult DamageCombatEnemy(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("entity") || !args[0].contains("amount")) {
        return Invalid("expected {entity = generation_checked_handle, amount = hit_points}");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "entity" && field.key() != "amount") return Invalid("unsupported combat.damage field");
    }
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    int amount = 0;
    if (!ReadId(args[0]["entity"], handle) || !ReadMonsterHandle(handle, slot, generation) ||
        !ReadBoundedInt(args[0], "amount", 1, 1000000, amount)) {
        return Invalid("entity must be a Chronicle monster handle and amount must be 1-1000000");
    }
    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(status) || unit == nullptr) {
        return Failed("no active dungeon combat state");
    }
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) {
        return Failed("the entity handle is stale because its monster slot was reused");
    }
    MONSTOR &monster = unit->monster[slot];
    if (monster.state != 2 || monster.hp <= 0 || monster.max_hp <= 0 || monster.hp > monster.max_hp) {
        return Failed("the selected entity is not a live, valid enemy");
    }
    if (monster.invincible_timer > 0 || monster.revealed == 0) {
        return {ApiCallStatus::Ok,
                {{"entity", handle}, {"hp_before", monster.hp}, {"hp_after", monster.hp},
                 {"damage", 0}, {"changed", false}, {"blocked", true}},
                {}};
    }

    const int hp_before = monster.hp;
    const int modified_amount = static_cast<int>(std::ceil(
        ApplyChronicleCombatDamageFormula(static_cast<float>(amount), -1, -1, -1, -1, -1)));
    if (modified_amount <= 0) {
        return {ApiCallStatus::Ok,
                {{"entity", handle}, {"hp_before", hp_before}, {"hp_after", hp_before},
                 {"damage", 0}, {"changed", false}, {"blocked", false}, {"death_queued", false}},
                {}};
    }
    const int hp_after = std::max(0, hp_before - modified_amount);
    const int dealt = hp_before - hp_after;
    monster.hp = hp_after;
    monster.last_attacker = -1;
    monster.last_hit_damage = dealt;
    monster.last_hit_id = -1;
    monster.hit_element = WEAPON_ELEMENT_NONE;
    if (hp_after == 0) {
        QueueChronicleMonsterDamageResult(unit, slot, static_cast<int>(MonsterDamageResult::Killed));
    } else {
        QueueChronicleMonsterDamageResult(unit, slot, static_cast<int>(MonsterDamageResult::Hit));
    }
    return {ApiCallStatus::Ok,
            {{"entity", handle}, {"hp_before", hp_before}, {"hp_after", hp_after},
             {"damage", dealt}, {"changed", dealt != 0}, {"blocked", false},
             {"death_queued", hp_after == 0}},
            {}};
}

ApiCallResult HealCombatEnemy(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("entity") || !args[0].contains("amount")) {
        return Invalid("expected {entity = generation_checked_handle, amount = hit_points}");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "entity" && field.key() != "amount") return Invalid("unsupported combat.heal field");
    }
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    int amount = 0;
    if (!ReadId(args[0]["entity"], handle) || !ReadMonsterHandle(handle, slot, generation) ||
        !ReadBoundedInt(args[0], "amount", 1, 1000000, amount)) {
        return Invalid("entity must be a Chronicle monster handle and amount must be 1-1000000");
    }
    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(status) || unit == nullptr) {
        return Failed("no active dungeon combat state");
    }
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) {
        return Failed("the entity handle is stale because its monster slot was reused");
    }
    MONSTOR &monster = unit->monster[slot];
    if (monster.state != 2 || monster.hp <= 0 || monster.max_hp <= 0 || monster.hp > monster.max_hp) {
        return Failed("the selected entity is not a live, valid enemy");
    }
    const int hp_before = monster.hp;
    const int hp_after = static_cast<int>(std::min<std::int64_t>(
        monster.max_hp, static_cast<std::int64_t>(hp_before) + amount));
    monster.hp = hp_after;
    return {ApiCallStatus::Ok,
            {{"entity", handle}, {"hp_before", hp_before}, {"hp_after", hp_after},
             {"restored", hp_after - hp_before}, {"changed", hp_after != hp_before}},
            {}};
}

ApiCallResult QueryEntities(const Json &args) {
    if (!args.is_array() || args.size() > 1 || (args.size() == 1 && !args[0].is_object())) {
        return Invalid("expected no arguments or {type = 'monster'}");
    }
    if (!args.empty()) {
        for (auto field = args[0].begin(); field != args[0].end(); ++field) {
            if (field.key() != "type") return Invalid("unsupported entity query filter");
        }
        const auto type = args[0].find("type");
        if (type != args[0].end() && (!type->is_string() || type->get<std::string>() != "monster")) {
            return Invalid("Chronicle currently supports entities.query only for type 'monster'");
        }
    }

    CDngStatusData *status = CurrentStatus();
    if (!IsActiveDungeonStatus(status) || NowMonstorUnit == nullptr) {
        return Failed("no active dungeon entity state");
    }

    PrepareMonsterHandleUnit(NowMonstorUnit);
    Json entities = Json::array();
    for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) {
        const MONSTOR &monster = NowMonstorUnit->monster[slot];
        if (monster.state != 2 || monster.hp <= 0) continue;
        const std::uint32_t generation = EnsureMonsterHandleGeneration(NowMonstorUnit, slot);
        const ChronicleMonsterHandleRecord &record = g_monster_handle_records[slot];
        Json entity = {{"id", "chronicle:monster-slot:" + std::to_string(slot)},
                       {"entity_id", MonsterHandleId(slot, generation)},
                            {"type", "monster"},
                            {"slot", slot},
                            {"revealed", monster.revealed != 0},
                            {"definition", { {"kind", monster.kind}, {"family", monster.attachment_kind},
                                               {"name_id", monster.name_no}, {"drop_item", monster.drop_item} }},
                            {"health", { {"current", monster.hp}, {"maximum", monster.max_hp} }},
                            {"combat", {{"defense", monster.defense}, {"reward_exp", monster.exp},
                                         {"reward_money", monster.money}}},
                            {"status", { {"poison_frames", monster.poison_timer},
                                          {"slow_frames", monster.slow_timer},
                                          {"stun_frames", monster.stop_timer} }}};
        float position[4]{};
        NowMonstorUnit->chara[slot][0].GetPosition(position);
        entity["position"] = {{"x", position[0]}, {"y", position[1]}, {"z", position[2]}};
        if (monster.base_model >= 0 && monster.base_model < static_cast<int>(g_loaded_monster_model_ids.size())) {
            const int native_model_id = g_loaded_monster_model_ids[monster.base_model];
            if (native_model_id >= 0) entity["native_model_id"] = native_model_id;
        }
        if (record.generation == generation && !record.character_id.empty()) {
            entity["character"] = record.character_id;
            entity["mod_owned"] = true;
        } else {
            entity["mod_owned"] = false;
        }
        entities.push_back(std::move(entity));
    }
    return {ApiCallStatus::Ok,
            {{"type", "monster"}, {"supported_types", Json::array({"monster"})},
             {"count", entities.size()}, {"entities", std::move(entities)}},
            {}};
}

Json ItemDefinition(int item_no, const COM_ITEM_INFO &common, const ITEM_DATA &item) {
    return {{"id", "chronicle:item:" + std::to_string(item_no)},
            {"item_no", item_no},
            {"kind", (item.kind_flags & ITEMKINDF_CONSUMABLE) != 0 ? "consumable" : "item"},
            {"message_id", common.msg},
            {"icon_index", common.icon_index},
            {"sort_key", item.sort_key},
            {"use_flags", item.use_flags},
            {"kind_flags", item.kind_flags},
            {"initial_volume", item.vol},
            {"volume_range", item.vol_range},
            {"stack_kind", item.stack_kind}};
}

bool IsModItemSlot(int item_no) {
    // Persisted assignments can use any item-definition row, but materialization
    // still requires an actually unused native consumable row below the weapon table.
    return item_no >= ITEM_ATTACH_START && item_no < ITEM_WEAPON_START;
}

bool IsUnusedModItemSlot(int item_no) {
    const COM_ITEM_INFO *common = GetCommonItemInfo(item_no);
    const ITEM_DATA *item = GetItemData(item_no);
    return common != nullptr && common->kind == ITEMKIND_ITEM && item != nullptr &&
           item->sort_key == ITEM_SORT_NONE && item->use_flags == 0 && item->kind_flags == 0 && item->vol == 0 &&
           item->vol_range == 0 && item->stack_kind == ITEM_STACK_NEVER;
}

std::vector<int> ChronicleModItemSlotCandidates() {
    std::vector<int> candidates(kChronicleModItemSlots.begin(), kChronicleModItemSlots.end());
    for (int item_no = ITEM_ATTACH_START; item_no < ITEM_WEAPON_START; ++item_no) {
        if (std::ranges::find(kChronicleModItemSlots, item_no) != kChronicleModItemSlots.end()) continue;
        if (IsUnusedModItemSlot(item_no)) candidates.push_back(item_no);
    }
    return candidates;
}

bool ReadBoundedInt(const Json &object, const char *key, int minimum, int maximum, int &out) {
    const auto found = object.find(key);
    if (found == object.end() || !found->is_number_integer()) return false;
    if (found->is_number_unsigned()) {
        const unsigned long long value = found->get<unsigned long long>();
        if (value > static_cast<unsigned long long>(maximum)) return false;
        out = static_cast<int>(value);
    } else {
        const long long value = found->get<long long>();
        if (value < minimum || value > maximum) return false;
        out = static_cast<int>(value);
    }
    return minimum <= out && out <= maximum;
}

bool ReadId(const Json &argument, std::string &out) {
    if (argument.is_string()) {
        out = argument.get<std::string>();
        return !out.empty();
    }
    if (argument.is_object()) {
        const auto found = argument.find("id");
        if (found != argument.end() && found->is_string()) {
            out = found->get<std::string>();
            return !out.empty();
        }
    }
    return false;
}

bool ReadVector3(const Json &value, std::array<float, 4> &out, float limit) {
    const Json *components[3] = {nullptr, nullptr, nullptr};
    if (value.is_array() && value.size() == 3) {
        components[0] = &value[0];
        components[1] = &value[1];
        components[2] = &value[2];
    } else if (value.is_object() && value.size() == 3 && value.contains("x") && value.contains("y") &&
               value.contains("z")) {
        components[0] = &value["x"];
        components[1] = &value["y"];
        components[2] = &value["z"];
    } else {
        return false;
    }
    for (std::size_t axis = 0; axis < 3; ++axis) {
        if (!components[axis]->is_number()) return false;
        const double component = components[axis]->get<double>();
        if (!std::isfinite(component) || component < -limit || component > limit) return false;
        out[axis] = static_cast<float>(component);
    }
    out[3] = 1.0f;
    return true;
}

void RestoreMonsterScale(int slot) {
    if (slot < 0 || slot >= static_cast<int>(g_monster_scale_records.size())) return;
    ChronicleEntityScaleRecord &record = g_monster_scale_records[slot];
    if (record.generation != 0 && record.unit != nullptr &&
        record.generation == g_monster_handle_generations[slot]) {
        record.unit->chara[slot][0].SetScale(record.original[0], record.original[1], record.original[2]);
    }
    record = {};
}

void RestoreMonsterScalesByOwner(std::string_view owner) {
    if (owner.empty()) return;
    for (int slot = 0; slot < static_cast<int>(g_monster_scale_records.size()); ++slot) {
        if (g_monster_scale_records[slot].owner == owner) RestoreMonsterScale(slot);
    }
}

void AdvanceMonsterHandleGeneration(int slot) {
    if (slot < 0 || slot >= static_cast<int>(g_monster_handle_generations.size())) return;
    RestoreMonsterScale(slot);
    MoonModTargetMeterClear(slot); // the meter was the monster's, not the slot's
    if (++g_monster_handle_generations[slot] == 0) ++g_monster_handle_generations[slot];
    g_monster_handle_records[slot] = {};
}

void PrepareMonsterHandleUnit(CMonstorUnit *unit) {
    if (g_monster_handle_unit == unit) return;
    g_monster_handle_unit = unit;
    g_arena_slots.fill({});
    g_monster_damage_scales.fill(1.0f);
    g_monster_hit_reach.fill(0.0f);
    g_monster_hit_share.fill(false);
    g_loaded_monster_model_ids.fill(-1);
    for (int slot = 0; slot < static_cast<int>(g_monster_handle_generations.size()); ++slot) {
        AdvanceMonsterHandleGeneration(slot);
    }
}

std::uint32_t EnsureMonsterHandleGeneration(CMonstorUnit *unit, int slot) {
    PrepareMonsterHandleUnit(unit);
    if (slot < 0 || slot >= static_cast<int>(g_monster_handle_generations.size())) return 0;
    if (g_monster_handle_generations[slot] == 0) AdvanceMonsterHandleGeneration(slot);
    return g_monster_handle_generations[slot];
}

std::string MonsterHandleId(int slot, std::uint32_t generation) {
    return "chronicle:monster:" + std::to_string(slot) + ":" + std::to_string(generation);
}

void DespawnOwnedChronicleActors(std::string_view owner) {
    if (owner.empty() || NowMonstorUnit == nullptr || mode != GAME_MODE_DUNGEON) return;
    PrepareMonsterHandleUnit(NowMonstorUnit);
    for (int slot = 0; slot < static_cast<int>(g_monster_handle_records.size()); ++slot) {
        const ChronicleMonsterHandleRecord &record = g_monster_handle_records[slot];
        if (record.owner != owner || record.generation != g_monster_handle_generations[slot]) continue;
        const std::string handle = MonsterHandleId(slot, record.generation);
        (void)DespawnEntity(owner, Json::array({Json{{"id", handle}}}));
    }
}

bool ReadMonsterHandle(std::string_view handle, int &slot, std::uint32_t &generation) {
    constexpr std::string_view prefix = "chronicle:monster:";
    if (!handle.starts_with(prefix)) return false;
    const std::string_view suffix = handle.substr(prefix.size());
    const std::size_t separator = suffix.find(':');
    if (separator == std::string_view::npos || suffix.find(':', separator + 1) != std::string_view::npos) return false;
    int parsed_slot = -1;
    std::uint32_t parsed_generation = 0;
    const auto slot_parse = std::from_chars(suffix.data(), suffix.data() + separator, parsed_slot);
    const auto generation_parse = std::from_chars(suffix.data() + separator + 1, suffix.data() + suffix.size(),
                                                  parsed_generation);
    if (slot_parse.ec != std::errc{} || slot_parse.ptr != suffix.data() + separator || parsed_slot < 0 ||
        parsed_slot >= MONSTOR_SLOT_MAX || generation_parse.ec != std::errc{} ||
        generation_parse.ptr != suffix.data() + suffix.size() || parsed_generation == 0) {
        return false;
    }
    slot = parsed_slot;
    generation = parsed_generation;
    return true;
}

std::filesystem::path ItemSlotRegistryPath(const std::filesystem::path &save_root) {
    return save_root / "moonmod" / "game-content" / "chronicle-item-slots.json";
}

bool LoadItemSlotRegistry(const std::filesystem::path &save_root, Json &assignments, std::string &error) {
    error.clear();
    assignments = Json::object();
    if (save_root.empty()) {
        error = "Chronicle mod items require the MoonMod save directory to persist their native item slots";
        return false;
    }
    std::filesystem::path path = ItemSlotRegistryPath(save_root);
    if (moonmod::ContainsSymlinkBelow(path, save_root)) {
        error = "Chronicle item slot registry path contains a symbolic link";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) error = "Chronicle item slot registry could not be inspected: " + ec.message();
        if (ec) return false;
        path += ".bak";
        if (moonmod::ContainsSymlinkBelow(path, save_root)) {
            error = "Chronicle item slot registry backup path contains a symbolic link";
            return false;
        }
        if (!std::filesystem::exists(path, ec)) return !ec;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 64 * 1024) {
        error = "Chronicle item slot registry is unreadable or larger than 64 KiB";
        return false;
    }
    try {
        std::ifstream input(path, std::ios::binary);
        Json loaded;
        input >> loaded;
        if (!input.good() && !input.eof()) {
            error = "Chronicle item slot registry could not be read";
            return false;
        }
        if (!loaded.is_object() || loaded.value("format_version", 0) != 1 || !loaded.contains("items") ||
            !loaded["items"].is_object()) {
            error = "Chronicle item slot registry has an unsupported format";
            return false;
        }
        std::map<int, std::string> used_slots;
        for (auto it = loaded["items"].begin(); it != loaded["items"].end(); ++it) {
            if (!it.value().is_number_integer()) {
                error = "Chronicle item slot registry contains an invalid assignment";
                return false;
            }
            const int item_no = it.value().get<int>();
            if (!IsModItemSlot(item_no) || used_slots.contains(item_no)) {
                error = "Chronicle item slot registry contains an out-of-range or duplicate native slot";
                return false;
            }
            used_slots.emplace(item_no, it.key());
        }
        assignments = std::move(loaded["items"]);
        return true;
    } catch (const std::exception &) {
        error = "Chronicle item slot registry is malformed";
        return false;
    }
}

bool SaveItemSlotRegistry(const std::filesystem::path &save_root, const Json &assignments, std::string &error) {
    const std::filesystem::path path = ItemSlotRegistryPath(save_root);
    const std::filesystem::path backup = path.string() + ".bak";
    if (moonmod::ContainsSymlinkBelow(path, save_root)) {
        error = "Chronicle item slot registry path contains a symbolic link";
        return false;
    }
    if (moonmod::ContainsSymlinkBelow(backup, save_root)) {
        error = "Chronicle item slot registry backup path contains a symbolic link";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "could not create Chronicle item slot registry directory: " + ec.message();
        return false;
    }
    const std::filesystem::path temporary = path.string() + ".tmp";
    if (moonmod::ContainsSymlinkBelow(temporary, save_root)) {
        error = "Chronicle item slot registry temporary path contains a symbolic link";
        return false;
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "could not write Chronicle item slot registry";
            return false;
        }
        const std::string document = Json{{"format_version", 1}, {"items", assignments}}.dump(2);
        output.write(document.data(), static_cast<std::streamsize>(document.size()));
        output.flush();
        if (!output.good()) {
            error = "could not finish writing Chronicle item slot registry";
            return false;
        }
    }
    std::filesystem::remove(backup, ec);
    ec.clear();
    const bool had_existing = std::filesystem::exists(path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "could not inspect Chronicle item slot registry before replacement: " + ec.message();
        return false;
    }
    if (had_existing) {
        std::filesystem::rename(path, backup, ec);
        if (ec) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            error = "could not preserve the prior Chronicle item slot registry: " + ec.message();
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        const std::string replace_error = ec.message();
        if (had_existing) {
            std::error_code restore_error;
            std::filesystem::rename(backup, path, restore_error);
            if (restore_error) {
                error = "could not write or restore Chronicle item slot registry: " + replace_error + "; " +
                        restore_error.message();
                return false;
            }
        }
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "could not replace Chronicle item slot registry: " + replace_error;
        return false;
    }
    if (had_existing) {
        std::error_code ignored;
        std::filesystem::remove(backup, ignored);
    }
    return true;
}

bool AllocateModItemSlot(std::string_view id, const std::filesystem::path &save_root, int &item_no,
                         std::string &error) {
    Json assignments;
    if (!LoadItemSlotRegistry(save_root, assignments, error)) return false;
    const auto existing = assignments.find(std::string(id));
    if (existing != assignments.end()) {
        item_no = existing->get<int>();
        return true;
    }
    std::vector<int> occupied;
    occupied.reserve(assignments.size());
    for (auto it = assignments.begin(); it != assignments.end(); ++it) {
        occupied.push_back(it.value().get<int>());
    }
    for (const int candidate : ChronicleModItemSlotCandidates()) {
        if (std::ranges::find(occupied, candidate) != occupied.end() || !IsUnusedModItemSlot(candidate)) continue;
        assignments[std::string(id)] = candidate;
        if (!SaveItemSlotRegistry(save_root, assignments, error)) return false;
        item_no = candidate;
        return true;
    }
    error = "Chronicle has no safe unused native item rows remaining for another mod consumable";
    return false;
}

std::filesystem::path WeaponSlotRegistryPath(const std::filesystem::path &save_root) {
    return save_root / "moonmod" / "game-content" / "chronicle-weapon-slots.json";
}

bool LoadWeaponSlotRegistry(const std::filesystem::path &save_root, Json &assignments, std::string &error) {
    error.clear();
    assignments = Json::object();
    if (save_root.empty()) {
        error = "Chronicle mod weapons require the MoonMod save directory to persist their native weapon slots";
        return false;
    }
    std::filesystem::path path = WeaponSlotRegistryPath(save_root);
    if (moonmod::ContainsSymlinkBelow(path, save_root)) {
        error = "Chronicle weapon slot registry path contains a symbolic link";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) {
            error = "Chronicle weapon slot registry could not be inspected: " + ec.message();
            return false;
        }
        path += ".bak";
        if (moonmod::ContainsSymlinkBelow(path, save_root)) {
            error = "Chronicle weapon slot registry backup path contains a symbolic link";
            return false;
        }
        if (!std::filesystem::exists(path, ec)) return !ec;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 64 * 1024) {
        error = "Chronicle weapon slot registry is unreadable or larger than 64 KiB";
        return false;
    }
    try {
        std::ifstream input(path, std::ios::binary);
        Json loaded;
        input >> loaded;
        if ((!input.good() && !input.eof()) || !loaded.is_object() || loaded.value("format_version", 0) != 1 ||
            !loaded.contains("weapons") || !loaded["weapons"].is_object()) {
            error = "Chronicle weapon slot registry has an unsupported format";
            return false;
        }
        std::map<int, std::string> used_slots;
        for (auto it = loaded["weapons"].begin(); it != loaded["weapons"].end(); ++it) {
            if (!it.value().is_number_integer()) {
                error = "Chronicle weapon slot registry contains an invalid assignment";
                return false;
            }
            const int native_item_no = it.value().get<int>();
            const std::string_view id(it.key());
            const std::size_t separator = id.find(':');
            if (native_item_no < 377 || native_item_no >= 409 || used_slots.contains(native_item_no) ||
                separator == std::string_view::npos || id.find(':', separator + 1) != std::string_view::npos ||
                !InputId(id.substr(0, separator)) || !InputId(id.substr(separator + 1))) {
                error = "Chronicle weapon slot registry contains an out-of-range, duplicate, or invalid assignment";
                return false;
            }
            used_slots.emplace(native_item_no, it.key());
        }
        assignments = std::move(loaded["weapons"]);
        return true;
    } catch (const std::exception &) {
        error = "Chronicle weapon slot registry is malformed";
        return false;
    }
}

bool SaveWeaponSlotRegistry(const std::filesystem::path &save_root, const Json &assignments, std::string &error) {
    const std::filesystem::path path = WeaponSlotRegistryPath(save_root);
    const std::filesystem::path backup = path.string() + ".bak";
    const std::filesystem::path temporary = path.string() + ".tmp";
    if (moonmod::ContainsSymlinkBelow(path, save_root) || moonmod::ContainsSymlinkBelow(backup, save_root) ||
        moonmod::ContainsSymlinkBelow(temporary, save_root)) {
        error = "Chronicle weapon slot registry path contains a symbolic link";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "could not create Chronicle weapon slot registry directory: " + ec.message();
        return false;
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "could not write Chronicle weapon slot registry";
            return false;
        }
        const std::string document = Json{{"format_version", 1}, {"weapons", assignments}}.dump(2);
        output.write(document.data(), static_cast<std::streamsize>(document.size()));
        output.flush();
        if (!output.good()) {
            error = "could not finish writing Chronicle weapon slot registry";
            return false;
        }
    }
    std::filesystem::remove(backup, ec);
    ec.clear();
    const bool had_existing = std::filesystem::exists(path, ec);
    if (ec) {
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "could not inspect Chronicle weapon slot registry before replacement: " + ec.message();
        return false;
    }
    if (had_existing) {
        std::filesystem::rename(path, backup, ec);
        if (ec) {
            std::error_code ignored;
            std::filesystem::remove(temporary, ignored);
            error = "could not preserve the prior Chronicle weapon slot registry: " + ec.message();
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        const std::string replace_error = ec.message();
        if (had_existing) {
            std::error_code restore_error;
            std::filesystem::rename(backup, path, restore_error);
            if (restore_error) {
                error = "could not write or restore Chronicle weapon slot registry: " + replace_error + "; " +
                        restore_error.message();
                return false;
            }
        }
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "could not replace Chronicle weapon slot registry: " + replace_error;
        return false;
    }
    if (had_existing) {
        std::error_code ignored;
        std::filesystem::remove(backup, ignored);
    }
    return true;
}

std::filesystem::path GeoramaPieceSlotRegistryPath(const std::filesystem::path &save_root) {
    return save_root / "moonmod" / "game-content" / "chronicle-georama-piece-slots.json";
}

bool LoadGeoramaPieceSlotRegistry(const std::filesystem::path &save_root, Json &assignments, std::string &error) {
    error.clear();
    assignments = Json::object();
    if (save_root.empty()) {
        error = "Chronicle Georama variants require the MoonMod save directory to persist native piece ids";
        return false;
    }
    const std::filesystem::path path = GeoramaPieceSlotRegistryPath(save_root);
    if (moonmod::ContainsSymlinkBelow(path, save_root)) {
        error = "Chronicle Georama slot registry path contains a symbolic link";
        return false;
    }
    std::error_code ec;
    if (!std::filesystem::exists(path, ec)) {
        if (ec) {
            error = "Chronicle Georama slot registry could not be inspected: " + ec.message();
            return false;
        }
        return true;
    }
    const auto size = std::filesystem::file_size(path, ec);
    if (ec || size == 0 || size > 64 * 1024) {
        error = "Chronicle Georama slot registry is unreadable or larger than 64 KiB";
        return false;
    }
    try {
        std::ifstream input(path, std::ios::binary);
        Json loaded;
        input >> loaded;
        if ((!input.good() && !input.eof()) || !loaded.is_object() || loaded.value("format_version", 0) != 1 ||
            !loaded.contains("pieces") || !loaded["pieces"].is_object()) {
            error = "Chronicle Georama slot registry has an unsupported format";
            return false;
        }
        std::array<bool, CHRONICLE_GEORAMA_MOD_PART_COUNT> used{};
        for (auto it = loaded["pieces"].begin(); it != loaded["pieces"].end(); ++it) {
            const std::string_view id(it.key());
            const std::size_t separator = id.find(':');
            if (!it.value().is_number_integer() || separator == std::string_view::npos ||
                id.find(':', separator + 1) != std::string_view::npos || !InputId(id.substr(0, separator)) ||
                !InputId(id.substr(separator + 1))) {
                error = "Chronicle Georama slot registry contains an invalid piece id or slot";
                return false;
            }
            const int part_id = it.value().get<int>();
            if (part_id < CHRONICLE_GEORAMA_NATIVE_PART_COUNT || part_id >= CHRONICLE_GEORAMA_PART_CAPACITY ||
                used[static_cast<std::size_t>(part_id - CHRONICLE_GEORAMA_NATIVE_PART_COUNT)]) {
                error = "Chronicle Georama slot registry contains an out-of-range or duplicate native slot";
                return false;
            }
            used[static_cast<std::size_t>(part_id - CHRONICLE_GEORAMA_NATIVE_PART_COUNT)] = true;
        }
        assignments = std::move(loaded["pieces"]);
        return true;
    } catch (const std::exception &) {
        error = "Chronicle Georama slot registry is malformed";
        return false;
    }
}

bool SaveGeoramaPieceSlotRegistry(const std::filesystem::path &save_root, const Json &assignments,
                                 std::string &error) {
    const std::filesystem::path path = GeoramaPieceSlotRegistryPath(save_root);
    const std::filesystem::path backup = path.string() + ".bak";
    const std::filesystem::path temporary = path.string() + ".tmp";
    if (moonmod::ContainsSymlinkBelow(path, save_root) || moonmod::ContainsSymlinkBelow(backup, save_root) ||
        moonmod::ContainsSymlinkBelow(temporary, save_root)) {
        error = "Chronicle Georama slot registry path contains a symbolic link";
        return false;
    }
    std::error_code ec;
    std::filesystem::create_directories(path.parent_path(), ec);
    if (ec) {
        error = "could not create Chronicle Georama slot registry directory: " + ec.message();
        return false;
    }
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        if (!output) {
            error = "could not write Chronicle Georama slot registry";
            return false;
        }
        const std::string document = Json{{"format_version", 1}, {"pieces", assignments}}.dump(2);
        output.write(document.data(), static_cast<std::streamsize>(document.size()));
        output.flush();
        if (!output.good()) {
            error = "could not finish writing Chronicle Georama slot registry";
            return false;
        }
    }
    const bool had_existing = std::filesystem::exists(path, ec);
    if (ec) {
        std::filesystem::remove(temporary, ec);
        error = "could not inspect Chronicle Georama slot registry before replacement";
        return false;
    }
    if (had_existing) {
        std::filesystem::remove(backup, ec);
        ec.clear();
        std::filesystem::rename(path, backup, ec);
        if (ec) {
            std::filesystem::remove(temporary, ec);
            error = "could not preserve the prior Chronicle Georama slot registry";
            return false;
        }
    }
    std::filesystem::rename(temporary, path, ec);
    if (ec) {
        if (had_existing) {
            std::error_code restore_error;
            std::filesystem::rename(backup, path, restore_error);
        }
        std::error_code ignored;
        std::filesystem::remove(temporary, ignored);
        error = "could not replace Chronicle Georama slot registry: " + ec.message();
        return false;
    }
    if (had_existing) {
        std::filesystem::remove(backup, ec);
    }
    return true;
}

bool AllocateModGeoramaPieceSlot(std::string_view id, const std::filesystem::path &save_root, int &part_id,
                                std::string &error) {
    Json assignments;
    if (!LoadGeoramaPieceSlotRegistry(save_root, assignments, error)) return false;
    const auto existing = assignments.find(std::string(id));
    if (existing != assignments.end()) {
        part_id = existing->get<int>();
        return true;
    }
    std::array<bool, CHRONICLE_GEORAMA_MOD_PART_COUNT> occupied{};
    for (auto it = assignments.begin(); it != assignments.end(); ++it) {
        const int assigned = it.value().get<int>();
        occupied[static_cast<std::size_t>(assigned - CHRONICLE_GEORAMA_NATIVE_PART_COUNT)] = true;
    }
    for (int candidate = CHRONICLE_GEORAMA_NATIVE_PART_COUNT; candidate < CHRONICLE_GEORAMA_PART_CAPACITY; ++candidate) {
        if (occupied[static_cast<std::size_t>(candidate - CHRONICLE_GEORAMA_NATIVE_PART_COUNT)] ||
            g_mod_georama_piece_by_native_id.contains(candidate)) {
            continue;
        }
        assignments[std::string(id)] = candidate;
        if (!SaveGeoramaPieceSlotRegistry(save_root, assignments, error)) return false;
        part_id = candidate;
        return true;
    }
    error = "Chronicle's reserved Georama range is exhausted; the current adapter supports 32 mod piece variants";
    return false;
}

bool AllocateModWeaponSlot(std::string_view id, const std::filesystem::path &save_root, int &item_no,
                           std::string &error) {
    Json assignments;
    if (!LoadWeaponSlotRegistry(save_root, assignments, error)) return false;
    const auto existing = assignments.find(std::string(id));
    if (existing != assignments.end()) {
        item_no = existing->get<int>();
        return true;
    }
    std::array<bool, 32> occupied{};
    for (auto it = assignments.begin(); it != assignments.end(); ++it) {
        occupied[static_cast<std::size_t>(it.value().get<int>() - 377)] = true;
    }
    for (int candidate = 377; candidate < 409; ++candidate) {
        const COM_ITEM_INFO *common = GetCommonItemInfo(candidate);
        if (occupied[static_cast<std::size_t>(candidate - 377)] || common == nullptr || common->kind != 0 ||
            g_mod_weapon_by_native_id.contains(candidate)) {
            continue;
        }
        assignments[std::string(id)] = candidate;
        if (!SaveWeaponSlotRegistry(save_root, assignments, error)) return false;
        item_no = candidate;
        return true;
    }
    error = "Chronicle's reserved native weapon slots are exhausted; the registered weapon table currently has 32 slots";
    return false;
}

Json ModItemJson(const ModItem &item) {
    const COM_ITEM_INFO *common = GetCommonItemInfo(item.native_item_no);
    const ITEM_DATA *data = GetItemData(item.native_item_no);
    Json result = common != nullptr && data != nullptr ? ItemDefinition(item.native_item_no, *common, *data)
                                                       : Json::object();
    result["id"] = item.id;
    result["name"] = item.name;
    result["kind"] = "consumable";
    result["effect"] = item.effect_kind == "cure_status"
                           ? Json{{"kind", item.effect_kind}, {"status", item.effect_status}}
                       : item.effect_kind == "cure_all" ? Json{{"kind", item.effect_kind}}
                                                         : Json{{"kind", item.effect_kind}, {"amount", item.effect_amount}};
    result["native_item_no"] = item.native_item_no;
    result["template_item"] = item.template_item;
    result["status"] = "registered";
    result["mod_owned"] = true;
    result["assets"] = item.icon.empty() ? Json::object() : Json{{"icon", "icons/" + item.icon + ".png"}};
    return result;
}

std::string ItemStableId(int item_no) {
    const auto found = g_mod_item_by_native_id.find(item_no);
    return found == g_mod_item_by_native_id.end() ? "chronicle:item:" + std::to_string(item_no) : found->second;
}

ApiCallResult RegisterItem(std::string_view mod_id, const std::filesystem::path &save_root, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected one item definition object");
    }
    const Json &definition = args[0];
    const auto id_it = definition.find("id");
    const auto name_it = definition.find("name");
    if (id_it == definition.end() || !id_it->is_string() || name_it == definition.end() || !name_it->is_string()) {
        return Invalid("item definition requires string id and name fields");
    }
    std::string local_id = id_it->get<std::string>();
    std::string qualified_id;
    const std::size_t separator = local_id.find(':');
    if (separator == std::string::npos) {
        if (!InputId(local_id)) return Invalid("item id must be a short identifier using letters, digits, '.', '_' or '-'");
        qualified_id = std::string(mod_id) + ":" + local_id;
    } else {
        if (local_id.find(':', separator + 1) != std::string::npos || local_id.substr(0, separator) != mod_id ||
            !InputId(std::string_view(local_id).substr(separator + 1))) {
            return Invalid("fully qualified item ids must use this mod's namespace");
        }
        qualified_id = std::move(local_id);
    }
    const std::string &display_name = name_it->get_ref<const std::string &>();
    if (display_name.empty() || display_name.size() > 95) return Invalid("item name must contain 1-95 UTF-8 bytes");
    const auto kind = definition.find("kind");
    if (kind != definition.end() && (!kind->is_string() || kind->get<std::string>() != "consumable")) {
        return Invalid("Chronicle currently accepts data-only consumable items");
    }
    const auto assets = definition.find("assets");
    if (assets != definition.end() && ((!assets->is_object() && !assets->is_array()) || !assets->empty())) {
        return Invalid("Chronicle item registration inherits vanilla assets; asset overrides are not accepted");
    }
    std::string icon_stem;
    if (std::string asset_error; !ReadOwnedAssetStem(definition, "icon", mod_id, "icons", "png", icon_stem, asset_error)) {
        return Invalid(std::move(asset_error));
    }
    int template_item_no = ITEM_BREAD;
    const auto template_it = definition.find("template_item");
    if (template_it != definition.end()) {
        if (!template_it->is_number_integer()) return Invalid("template_item must be a built-in item number");
        const std::int64_t value = template_it->get<std::int64_t>();
        if (value < ITEM_DUNGEON_START || value >= ITEM_WEAPON_START) return Invalid("template_item must be a built-in item number");
        template_item_no = static_cast<int>(value);
    }
    const COM_ITEM_INFO *template_common = GetCommonItemInfo(template_item_no);
    const ITEM_DATA *template_data = GetItemData(template_item_no);
    const auto effect = definition.find("effect");
    if (effect == definition.end() || !effect->is_object()) {
        return Invalid("item definition requires a supported single-effect consumable effect");
    }
    const auto effect_kind = effect->find("kind");
    if (effect_kind == effect->end() || !effect_kind->is_string()) {
        return Invalid("effect.kind must be 'heal', 'restore_water', 'cure_status', or 'cure_all'");
    }
    const std::string effect_name = effect_kind->get<std::string>();
    int amount = 0;
    unsigned int effect_flags = 0;
    std::string effect_status;
    if (effect_name == "heal" || effect_name == "restore_water") {
        const bool restores_water = effect_name == "restore_water";
        if (effect->size() != 2 || !ReadBoundedInt(*effect, "amount", 1, 32767, amount)) {
            return Invalid("effect amount must be an integer from 1 to 32767, with no extra fields");
        }
        effect_flags = restores_water ? ITEMUSE_DRINK : ITEMUSE_HEAL_HP;
        if (template_common == nullptr || template_common->kind != ITEMKIND_ITEM || template_data == nullptr ||
            template_data->kind_flags != ITEMKINDF_CONSUMABLE || template_data->use_flags != effect_flags) {
            return Invalid("template_item must be a built-in consumable with matching HP or water behavior");
        }
    } else if (effect_name == "cure_status" || effect_name == "cure_all") {
        const auto status_it = effect->find("status");
        if (effect_name == "cure_all") {
            if (effect->size() != 1 || status_it != effect->end()) {
                return Invalid("cure_all takes no fields");
            }
            effect_flags = kChronicleCureItemFlags;
        } else if (effect->size() != 2 || status_it == effect->end() || !status_it->is_string()) {
            return Invalid("cure_status requires exactly one status field");
        } else {
            effect_status = status_it->get<std::string>();
            effect_flags = ChronicleCureFlagForName(effect_status);
            if (effect_flags == 0) {
                return Invalid("cure_status status must be freeze, poison, curse, or goo");
            }
        }
        const bool safe_cure_template = template_data != nullptr &&
                                        template_data->kind_flags == ITEMKINDF_USE_ON_CHARACTER &&
                                        template_data->use_flags != 0 &&
                                        (template_data->use_flags & ~kChronicleCureItemFlags) == 0;
        if (template_common == nullptr || template_common->kind != ITEMKIND_ITEM || !safe_cure_template) {
            return Invalid("cure_status requires a native single-effect character-cure template such as item 154");
        }
    } else {
        return Invalid("effect.kind must be 'heal', 'restore_water', 'cure_status', or 'cure_all'");
    }
    if (auto found = g_mod_items.find(qualified_id); found != g_mod_items.end() && found->second.owner != mod_id) {
        return Invalid("an item with this stable id is already owned by another mod");
    }
    int native_item_no = -1;
    std::string allocation_error;
    if (!AllocateModItemSlot(qualified_id, save_root, native_item_no, allocation_error)) {
        return Failed(std::move(allocation_error));
    }

    COM_ITEM_INFO *common = GetCommonItemInfo(native_item_no);
    ITEM_DATA *data = GetItemData(native_item_no);
    const auto current_owner = g_mod_item_by_native_id.find(native_item_no);
    if (common == nullptr || data == nullptr ||
        (current_owner != g_mod_item_by_native_id.end() && current_owner->second != qualified_id) ||
        (current_owner == g_mod_item_by_native_id.end() && !IsUnusedModItemSlot(native_item_no))) {
        return Failed("the persisted native item slot cannot be safely materialized");
    }
    if (!g_original_mod_item_slots.contains(native_item_no)) {
        g_original_mod_item_slots.emplace(native_item_no, std::make_pair(*common, *data));
    }
    common->kind = ITEMKIND_ITEM;
    common->icon_index = template_common->icon_index;
    common->msg = template_common->msg;
    data->sort_key = template_data->sort_key;
    data->use_flags = effect_flags;
    data->kind_flags = effect_name == "cure_status" || effect_name == "cure_all"
                           ? ITEMKINDF_USE_ON_CHARACTER
                           : ITEMKINDF_CONSUMABLE;
    data->vol = static_cast<s16>(amount);
    data->vol_range = 0;
    data->stack_kind = template_data->stack_kind;

    SetItemIconOverride(native_item_no, icon_stem.empty() ? "" : ("icons/" + icon_stem + ".png").c_str());
    ModItem item{qualified_id, std::string(mod_id), display_name, template_item_no, effect_name, amount,
                 effect_status, native_item_no, icon_stem};
    g_mod_items[qualified_id] = std::move(item);
    g_mod_item_by_native_id[native_item_no] = qualified_id;
    return {ApiCallStatus::Ok, ModItemJson(g_mod_items.at(qualified_id)), {}};
}

bool ReadItemNumber(const Json &selector, int &item_no) {
    if (selector.is_number_integer()) {
        const std::int64_t value = selector.get<std::int64_t>();
        if (value < ITEM_ATTACH_START || value >= ITEM_ATTACH_START + 296) return false;
        item_no = static_cast<int>(value);
        return true;
    }

    const Json *id = nullptr;
    if (selector.is_string()) {
        id = &selector;
    } else if (selector.is_object()) {
        const auto id_field = selector.find("id");
        if (id_field != selector.end()) {
            id = &*id_field;
        } else {
            const auto number_field = selector.find("item_no");
            return number_field != selector.end() && ReadItemNumber(*number_field, item_no);
        }
    }
    if (id == nullptr || !id->is_string()) return false;

    const std::string &stable_id = id->get_ref<const std::string &>();
    const auto mod_item = g_mod_items.find(stable_id);
    if (mod_item != g_mod_items.end() && mod_item->second.native_item_no >= 0) {
        item_no = mod_item->second.native_item_no;
        return true;
    }

    constexpr std::string_view prefix = "chronicle:item:";
    if (!stable_id.starts_with(prefix) || stable_id.size() == prefix.size()) return false;
    int parsed = 0;
    const char *begin = stable_id.data() + prefix.size();
    const char *end = stable_id.data() + stable_id.size();
    const auto [next, error] = std::from_chars(begin, end, parsed);
    if (error != std::errc{} || next != end || parsed < ITEM_ATTACH_START || parsed >= ITEM_ATTACH_START + 296) {
        return false;
    }
    item_no = parsed;
    return true;
}

ApiCallResult ListItems(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("items.list takes no arguments");
    Json result = Json::array();
    for (int item_no = ITEM_ATTACH_START; item_no < ITEM_ATTACH_START + 296; ++item_no) {
        if (g_mod_item_by_native_id.contains(item_no)) continue;
        const COM_ITEM_INFO *common = GetCommonItemInfo(item_no);
        const ITEM_DATA *item = GetItemData(item_no);
        if (common == nullptr || common->kind != ITEMKIND_ITEM || item == nullptr) continue;
        result.push_back(ItemDefinition(item_no, *common, *item));
    }
    for (const auto &[id, item] : g_mod_items) result.push_back(ModItemJson(item));
    return {ApiCallStatus::Ok, std::move(result), {}};
}

ApiCallResult GetQuickItemSlots(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("items.quick_slots takes no arguments");
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save inventory");
    Json slots = Json::array();
    for (int index = 0; index < 3; ++index) {
        const int item_no = status->item_pack.quick_item_slot[index];
        const int count = std::max<int>(0, status->item_pack.quick_item_qty[index]);
        const COM_ITEM_INFO *common = count > 0 && item_no >= ITEM_ATTACH_START && item_no < ITEM_ATTACH_START + 296
                                          ? GetCommonItemInfo(item_no)
                                          : nullptr;
        if (common == nullptr || common->kind != ITEMKIND_ITEM) {
            slots.push_back({{"slot", index + 1}, {"item_no", nullptr}, {"id", nullptr}, {"count", 0}});
            continue;
        }
        slots.push_back({{"slot", index + 1}, {"item_no", item_no}, {"id", ItemStableId(item_no)}, {"count", count}});
    }
    return {ApiCallStatus::Ok, std::move(slots), {}};
}

// items.charm_slots: each character's charm slots (charm_slots.hpp), as {open, characters = {[id] = {{slot, item_no,
// id, uses}}}} for the slots shown.
Json CharmSlotsState() {
    Json characters = Json::object();
    for (int chara = 0; chara < CharaCount(); ++chara) {
        Json slots = Json::array();
        for (int position = 0; position < CharmSlotsShown(chara); ++position) {
            const int        index = CharmSlotIndex(chara, position);
            const CharmSlot &slot = CharmSlotAt(chara, index);
            const char      *kind = index == CHARM_FEATHER_SLOT ? "feather" : "amulet";
            if (slot.item_no < 0) {
                slots.push_back({{"slot", position + 1}, {"kind", kind}, {"item_no", nullptr}, {"id", nullptr}, {"uses", 0}});
            } else {
                slots.push_back({{"slot", position + 1}, {"kind", kind}, {"item_no", slot.item_no}, {"id", ItemStableId(slot.item_no)},
                                 {"uses", slot.uses}});
            }
        }
        characters[kCharacterIds[chara]] = std::move(slots);
    }
    return {{"open", CharmSlotsOpen()}, {"feather", CharmFeatherOpen()}, {"characters", std::move(characters)}};
}

ApiCallResult GetCharmSlots(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("items.charm_slots takes no arguments");
    return {ApiCallStatus::Ok, CharmSlotsState(), {}};
}

// items.charm_slots_set: open count amulet slots on the item page, and with feather = true the Feather's slot after
// them; none is the game's own page.
ApiCallResult SetCharmSlots(const Json &args) {
    int count = 0;
    const bool has_feather = args.is_array() && args.size() == 1 && args[0].is_object() && args[0].contains("feather");
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != (has_feather ? 2u : 1u) ||
        !ReadBoundedInt(args[0], "count", 0, CHARM_SLOT_MAX, count) || (has_feather && !args[0]["feather"].is_boolean())) {
        return Invalid("expected {count = 0.." + std::to_string(CHARM_SLOT_MAX) + ", feather = optional boolean}");
    }
    SetCharmSlotsOpen(count);
    SetCharmFeatherOpen(has_feather && args[0]["feather"].get<bool>());
    return {ApiCallStatus::Ok, CharmSlotsState(), {}};
}

// items.gear*: the pieces each character wears in their four gear slots (gear.hpp), and the bag of those not worn.
Json GearEffectsJson(const GearEffects &effects) {
    Json out = Json::object();
    Json guard = Json::object();
    for (int g = 0; g < GEAR_GUARD_COUNT; ++g) {
        if (effects.guard[g] > 0) guard[GearGuardName(g)] = effects.guard[g];
    }
    if (!guard.empty()) out["guard"] = std::move(guard);
    if (effects.stamina > 0) out["stamina"] = effects.stamina;
    if (effects.lightness > 0) out["lightness"] = effects.lightness;
    if (effects.defence > 0) out["defence"] = effects.defence;
    if (effects.run_cost > 0) out["run_cost"] = effects.run_cost;
    if (effects.poise > 0) out["poise"] = effects.poise;
    if (effects.bag > 0) out["bag"] = effects.bag;
    return out;
}

Json GearPieceJson(const GearPiece &piece) {
    Json out = {{"uid", piece.uid},     {"kind", GearKindName(piece.kind)}, {"rarity", piece.rarity},
                {"name", piece.name},   {"value", piece.value},             {"effects", GearEffectsJson(piece.effects)},
                {"worn_by", nullptr}};
    if (!piece.worn_by.empty()) out["worn_by"] = piece.worn_by;
    out["icon"] = piece.icon.empty() ? Json(nullptr) : Json(piece.icon);
    out["color"] = {{"r", piece.color >> 16 & 0xFF}, {"g", piece.color >> 8 & 0xFF}, {"b", piece.color & 0xFF}};
    return out;
}

// An icon stem, as items.register takes one: icons/<stem>.png, 1-48 bytes with no path or extension.
bool ReadIconStem(const Json &value, std::string &game_path) {
    if (!value.is_string() || value.get_ref<const std::string &>().empty() || value.get_ref<const std::string &>().size() > 48 ||
        value.get_ref<const std::string &>().find_first_of("/\\.") != std::string::npos) {
        return false;
    }
    game_path = "icons/" + value.get<std::string>() + ".png";
    return true;
}

// {r, g, b}, each 0 to 255, as 0xRRGGBB.
bool ReadRgb(const Json &value, u32 &rgb) {
    if (!value.is_object() || value.size() != 3) return false;
    int r = 0, g = 0, b = 0;
    if (!ReadBoundedInt(value, "r", 0, 255, r) || !ReadBoundedInt(value, "g", 0, 255, g) || !ReadBoundedInt(value, "b", 0, 255, b)) {
        return false;
    }
    rgb = static_cast<u32>(r << 16 | g << 8 | b);
    return true;
}

// The item bag's size after a pouch goes on or comes off (GearSyncBag).
void SyncGearBag() {
    if (CDngStatusData *status = CurrentStatus()) GearSyncBag(status->item_pack);
}

Json GearState() {
    Json pieces = Json::array();
    for (const GearPiece &piece : GearPieces()) pieces.push_back(GearPieceJson(piece));
    Json worn = Json::object();
    for (int chara = 0; chara < CharaCount(); ++chara) {
        Json slots = Json::object();
        for (int kind = 0; kind < GEAR_KIND_COUNT; ++kind) {
            const GearPiece *piece = WornGear(chara, kind);
            slots[GearKindName(kind)] = piece != nullptr ? Json(piece->uid) : Json(nullptr);
        }
        worn[kCharacterIds[chara]] = {{"slots", std::move(slots)}, {"effects", GearEffectsJson(GearWornEffects(chara))}};
    }
    return {{"pieces", std::move(pieces)}, {"bag", GearBagCount()}, {"bag_max", GEAR_BAG_MAX}, {"worn", std::move(worn)},
            {"page", GearPageOpen()}, {"bag_slots", GearBagAdded()}};
}

// "active" (or nothing) is the active character; otherwise a character id.
bool ReadGearCharacter(const Json &object, int &chara) {
    const auto found = object.find("character");
    if (found == object.end() || (found->is_string() && found->get<std::string>() == "active")) {
        if (UserStatus == nullptr) return false;
        chara = UserStatus->cur_chara;
        return chara >= 0 && chara < CharaCount();
    }
    if (!found->is_string()) return false;
    for (int c = 0; c < CharaCount(); ++c) {
        if (found->get<std::string>() == kCharacterIds[c]) {
            chara = c;
            return true;
        }
    }
    return false;
}

bool ReadGearUid(const Json &object, u32 &uid) {
    const auto found = object.find("uid");
    if (found == object.end() || !found->is_number_integer() || found->get<std::int64_t>() < 1 ||
        found->get<std::int64_t>() > 0xFFFFFFFF) {
        return false;
    }
    uid = found->get<u32>();
    return FindGear(uid) != nullptr;
}

ApiCallResult GetGear(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("items.gear takes no arguments");
    return {ApiCallStatus::Ok, GearState(), {}};
}

// items.gear_give({kind, name, rarity?, value?, effects?}): a new piece in the bag.
// A gear piece as items.gear_give takes it (and each of items.gear_stock_set's goods, which may add `price`): false and
// why when it isn't one.
bool ReadGearPiece(const Json &spec, GearPiece &piece, std::string &error, std::string_view op, bool priced) {
    const auto fail = [&error](std::string why) {
        error = std::move(why);
        return false;
    };
    for (const auto &[key, value] : spec.items()) {
        if (key != "kind" && key != "name" && key != "rarity" && key != "value" && key != "effects" && key != "icon" &&
            key != "color" && !(priced && key == "price")) {
            return fail(std::string(op) + " does not take " + key);
        }
    }
    piece.kind = spec.contains("kind") && spec["kind"].is_string() ? GearKindFromName(spec["kind"].get<std::string>()) : -1;
    if (piece.kind < 0) return fail("kind must be hat, tunic, gloves, boots or pouch");
    if (!spec.contains("name") || !spec["name"].is_string() || spec["name"].get<std::string>().empty() ||
        spec["name"].get<std::string>().size() > GEAR_NAME_MAX) {
        return fail("name must be 1 to " + std::to_string(GEAR_NAME_MAX) + " bytes");
    }
    piece.name = spec["name"].get<std::string>();
    if (spec.contains("rarity") && !ReadBoundedInt(spec, "rarity", 0, GEAR_RARITY_COUNT - 1, piece.rarity)) {
        return fail("rarity must be 0 (common) to 4 (legendary)");
    }
    if (spec.contains("value") && !ReadBoundedInt(spec, "value", 0, 65535, piece.value)) {
        return fail("value must be 0 to 65535 gilda");
    }
    if (spec.contains("icon") && !ReadIconStem(spec["icon"], piece.icon)) {
        return fail("icon must be the stem of icons/<stem>.png (1-48 bytes, no path or extension)");
    }
    if (spec.contains("color") && !ReadRgb(spec["color"], piece.color)) return fail("color must be {r, g, b}, each 0 to 255");
    if (spec.contains("effects")) {
        const Json &effects = spec["effects"];
        if (!effects.is_object()) return fail("effects must be {guard = {poison = %}, stamina, lightness, defence, run_cost, poise, bag}");
        struct Field {
            const char *key;
            int        *out;
            int         cap;
        };
        const std::array<Field, 6> fields = {{{"stamina", &piece.effects.stamina, GEAR_STAMINA_CAP},
                                              {"lightness", &piece.effects.lightness, GEAR_LIGHTNESS_CAP},
                                              {"defence", &piece.effects.defence, GEAR_DEFENCE_CAP},
                                              {"run_cost", &piece.effects.run_cost, GEAR_RUN_COST_CAP},
                                              {"poise", &piece.effects.poise, GEAR_POISE_CAP},
                                              {"bag", &piece.effects.bag, GEAR_BAG_SLOTS_CAP}}};
        for (const auto &[key, value] : effects.items()) {
            if (key == "guard") {
                if (!value.is_object()) return fail("effects.guard must be {freeze, poison, curse, goo = percent}");
                for (const auto &[ailment, chance] : value.items()) {
                    const int g = GearGuardFromName(ailment);
                    if (g < 0) return fail("effects.guard names no ailment: " + ailment + " (freeze, poison, curse, goo)");
                    if (!ReadBoundedInt(value, ailment.c_str(), 0, GEAR_GUARD_CAP, piece.effects.guard[g])) {
                        return fail("effects.guard." + ailment + " must be 0 to " + std::to_string(GEAR_GUARD_CAP));
                    }
                }
                continue;
            }
            const auto field = std::ranges::find_if(fields, [&key](const Field &f) { return key == f.key; });
            if (field == fields.end()) return fail("effects does not take " + key);
            if (!ReadBoundedInt(effects, field->key, 0, field->cap, *field->out)) {
                return fail("effects." + key + " must be 0 to " + std::to_string(field->cap));
            }
        }
    }
    return true;
}

ApiCallResult GiveGear(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {kind, name, rarity = 0..4, value = gilda, icon = stem, color = {r, g, b}, effects = {guard = {poison = %}, "
                       "stamina, lightness, defence, run_cost, poise, bag}}");
    }
    if (CurrentStatus() == nullptr) return Failed("the game has no active save");
    GearPiece piece;
    if (std::string error; !ReadGearPiece(args[0], piece, error, "items.gear_give", false)) return Invalid(error);
    const u32 uid = AddGear(std::move(piece));
    if (uid == 0) return Failed("the gear bag is full (" + std::to_string(GEAR_BAG_MAX) + " pieces)");
    return {ApiCallStatus::Ok, GearPieceJson(*FindGear(uid)), {}};
}

// items.gear_stock_set({shop, goods}): the gear a shop sells beside its own goods, each a piece as gear_give takes it
// with a `price` (its value when left out); {shop, goods = {}} takes them away.
ApiCallResult StockGear(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("shop") || !args[0].contains("goods") ||
        args[0].size() != 2) {
        return Invalid("expected {shop = shop number, goods = {{kind, name, rarity, value, icon, color, effects, price?}, ...}}");
    }
    int shop = 0;
    if (!ReadBoundedInt(args[0], "shop", 0, 255, shop)) return Invalid("shop must be a shop's number, 0 to 255");
    const Json &list = args[0]["goods"];
    if (!(list.is_array() || (list.is_object() && list.empty())) || list.size() > GEAR_STOCK_MAX) {
        return Invalid("goods must be a list of at most " + std::to_string(GEAR_STOCK_MAX) + " pieces");
    }
    std::vector<GearGood> goods;
    for (size_t k = 0; k < list.size() && list.is_array(); k++) {
        if (!list[k].is_object()) return Invalid("goods[" + std::to_string(k + 1) + "] must be a piece");
        GearGood good;
        if (std::string error; !ReadGearPiece(list[k], good.piece, error, "items.gear_stock_set", true)) {
            return Invalid("goods[" + std::to_string(k + 1) + "]: " + error);
        }
        good.price = good.piece.value;
        if (list[k].contains("price") && !ReadBoundedInt(list[k], "price", 0, GEAR_PRICE_MAX, good.price)) {
            return Invalid("goods[" + std::to_string(k + 1) + "].price must be 0 to " + std::to_string(GEAR_PRICE_MAX) + " gilda");
        }
        goods.push_back(std::move(good));
    }
    const int count = static_cast<int>(goods.size());
    SetGearStock(shop, std::move(goods));
    return {ApiCallStatus::Ok, {{"shop", shop}, {"goods", count}}, {}};
}

// A list of 1 to 8 frame names of a menu model, each 1 to 31 bytes.
bool ReadFrameNames(const Json &value, std::vector<std::string> &names) {
    if (!value.is_array() || value.empty() || value.size() > 8) return false;
    for (const Json &name : value) {
        if (!name.is_string() || name.get_ref<const std::string &>().empty() || name.get_ref<const std::string &>().size() > 31) {
            return false;
        }
        names.push_back(name.get<std::string>());
    }
    return true;
}

// parts = {<character id> = {<kind> = {frame names} or {draw = {frame names}, fit = {frame names}, bare = {frame names},
// below = {frame names}, turn = degrees, tilt = degrees}}}.
bool ReadGearParts(const Json &value, std::map<std::string, GearCharaParts> &parts, std::string &error) {
    if (!value.is_object()) {
        error = "parts must be {<character id> = {hat, tunic, gloves, boots, pouch = frame names}}";
        return false;
    }
    for (const auto &[id, kinds] : value.items()) {
        if (id.empty() || id.size() > 32 || !kinds.is_object()) {
            error = "parts." + id + " must be {hat, tunic, gloves, boots, pouch = frame names}";
            return false;
        }
        GearCharaParts &chara = parts[id];
        for (const auto &[kind_name, entry] : kinds.items()) {
            const int kind = GearKindFromName(kind_name);
            const std::string where = "parts." + id + "." + kind_name;
            if (kind < 0) {
                error = where + ": the kinds are hat, tunic, gloves, boots and pouch";
                return false;
            }
            GearKindParts &kind_parts = chara[kind];
            const bool split = entry.is_object();
            bool       ok = split ? entry.contains("draw") && ReadFrameNames(entry["draw"], kind_parts.draw)
                                  : ReadFrameNames(entry, kind_parts.draw);
            static const Json no_fields = Json::object();
            for (const auto &[key, field] : (split ? entry : no_fields).items()) {
                if (key == "fit") {
                    ok = ok && ReadFrameNames(field, kind_parts.fit);
                } else if (key == "bare") {
                    ok = ok && ReadFrameNames(field, kind_parts.bare);
                } else if (key == "below") {
                    ok = ok && ReadFrameNames(field, kind_parts.below);
                } else if (key == "turn") {
                    ok = ok && field.is_number() && field.get<double>() >= -180.0 && field.get<double>() <= 180.0;
                    kind_parts.turn = ok ? static_cast<float>(field.get<double>() * 3.14159265358979 / 180.0) : 0.0f;
                } else if (key == "tilt") {
                    ok = ok && field.is_number() && field.get<double>() >= -90.0 && field.get<double>() <= 90.0;
                    kind_parts.tilt = ok ? static_cast<float>(field.get<double>() * 3.14159265358979 / 180.0) : 0.0f;
                } else if (key != "draw") {
                    ok = false;
                }
            }
            if (!ok) {
                error = where + " must be 1 to 8 frame names, or {draw = names, fit = names?, bare = names?, below = names?, turn = -180 to 180 degrees?, tilt = -90 to 90 degrees?}";
                return false;
            }
        }
    }
    return true;
}

// items.gear_page_set({icons = {hat, tunic, gloves, boots, pouch?}, parts?}): opens the item board's Equipment tab and the
// gear slots on the character page, each kind's empty slot showing its icon blacked out; {} closes them. The pouch's slot
// shows only with its icon named. parts names, for each character, the frames of their menu model that show each kind in
// its slot in place of the icon.
// weapons.stat_label_set({attack?, endurance?, speed?, magic?}): each named stat's label on the weapon cards becomes
// icons/<stem>.png; a stat left out has the game's two letters again, and {} gives them all back.
// render.target_bar_set({on}): the locked target's wider bar (moonmod/target_hud.hpp) in place of the game's gauge.
ApiCallResult SetTargetBar(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 || !args[0].contains("on") ||
        !args[0]["on"].is_boolean()) {
        return Invalid("expected {on = true or false}");
    }
    MoonModTargetBarSet(args[0]["on"].get<bool>());
    return {ApiCallStatus::Ok, {{"on", MoonModTargetBarOn()}}, {}};
}

// render.target_meter_set({entity, fill = 0..1, color = {r, g, b}?, seconds?}): the meter under that monster's bar while
// it is the target, gone after seconds without another set (0, the default, keeps it); {entity} alone takes it away.
ApiCallResult SetTargetMeter(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("entity") || !args[0]["entity"].is_string()) {
        return Invalid("expected {entity = monster handle, fill = 0..1, color = {r, g, b}?, seconds?}");
    }
    const Json &spec = args[0];
    for (const auto &[key, value] : spec.items()) {
        if (key != "entity" && key != "fill" && key != "color" && key != "seconds") return Invalid("render.target_meter_set does not take " + key);
    }
    int slot = -1;
    std::uint32_t generation = 0;
    if (!ReadMonsterHandle(spec["entity"].get<std::string>(), slot, generation) || slot < 0 ||
        slot >= static_cast<int>(g_monster_handle_generations.size())) {
        return Invalid("entity must be a monster handle (chronicle:monster:<slot>:<generation>)");
    }
    if (spec.contains("fill") && (!spec["fill"].is_number() || spec["fill"].get<double>() < 0.0 || spec["fill"].get<double>() > 1.0)) {
        return Invalid("fill must be a number from 0 to 1");
    }
    u32 rgb = 0xF0C040;
    if (spec.contains("color") && !ReadRgb(spec["color"], rgb)) return Invalid("color must be {r, g, b}, each 0 to 255");
    double seconds = 0.0;
    if (spec.contains("seconds")) {
        if (!spec["seconds"].is_number() || spec["seconds"].get<double>() < 0.0 || spec["seconds"].get<double>() > 600.0) {
            return Invalid("seconds must be 0 to 600");
        }
        seconds = spec["seconds"].get<double>();
    }
    if (generation != g_monster_handle_generations[slot]) return Failed("that monster is gone");
    if (!spec.contains("fill")) {
        MoonModTargetMeterClear(slot);
        return {ApiCallStatus::Ok, {{"entity", spec["entity"]}, {"meter", false}}, {}};
    }
    MoonModTargetMeterSet(slot, static_cast<float>(spec["fill"].get<double>()), rgb, static_cast<float>(seconds));
    return {ApiCallStatus::Ok, {{"entity", spec["entity"]}, {"meter", true}}, {}};
}

ApiCallResult SetWeaponStatLabels(const Json &args) {
    static constexpr std::array<std::string_view, WEAPON_STAT_COUNT> names = {"attack", "endurance", "speed", "magic"};
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {attack?, endurance?, speed?, magic? = icon stem}");
    }
    std::array<std::string, WEAPON_STAT_COUNT> paths;
    for (const auto &[key, value] : args[0].items()) {
        const auto found = std::ranges::find(names, key);
        if (found == names.end()) return Invalid("weapons.stat_label_set does not take " + key + " (attack, endurance, speed, magic)");
        if (!ReadIconStem(value, paths[found - names.begin()])) return Invalid(key + " must be the stem of icons/<stem>.png");
    }
    Json set = Json::object();
    for (int stat = 0; stat < WEAPON_STAT_COUNT; ++stat) {
        if (!paths[stat].empty()) set[std::string(names[stat])] = paths[stat];
        SetWeaponStatLabel(stat, std::move(paths[stat]));
    }
    return {ApiCallStatus::Ok, {{"labels", std::move(set)}}, {}};
}

ApiCallResult SetGearPage(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() > 2 ||
        (!args[0].empty() && !args[0].contains("icons")) || (args[0].size() == 2 && !args[0].contains("parts"))) {
        return Invalid("expected {icons = {hat, tunic, gloves, boots, pouch? = icon stems}, parts = optional frame names}, or {} to close the page");
    }
    if (args[0].empty()) {
        SetGearPageOpen(false);
        SetGearParts({});
        return {ApiCallStatus::Ok, {{"page", false}}, {}};
    }
    std::map<std::string, GearCharaParts> parts;
    if (std::string error; args[0].contains("parts") && !ReadGearParts(args[0]["parts"], parts, error)) return Invalid(error);
    const Json &icons = args[0]["icons"];
    std::array<std::string, GEAR_KIND_COUNT> paths;
    const bool pouch = icons.is_object() && icons.contains(GearKindName(GEAR_POUCH));
    if (!icons.is_object() || icons.size() != (pouch ? GEAR_KIND_COUNT : GEAR_POUCH)) {
        return Invalid("icons must name hat, tunic, gloves and boots, and may name pouch");
    }
    for (int kind = 0; kind < (pouch ? GEAR_KIND_COUNT : GEAR_POUCH); ++kind) {
        if (!icons.contains(GearKindName(kind)) || !ReadIconStem(icons[GearKindName(kind)], paths[kind])) {
            return Invalid(std::string("icons.") + GearKindName(kind) + " must be the stem of icons/<stem>.png");
        }
    }
    for (int kind = 0; kind < GEAR_KIND_COUNT; ++kind) SetGearKindIcon(kind, std::move(paths[kind]));
    SetGearParts(std::move(parts));
    SetGearPageOpen(true);
    return {ApiCallStatus::Ok, {{"page", true}}, {}};
}

// items.gear_equip({uid, character?}): puts it on, what was in that slot going to the bag.
ApiCallResult EquipGearPiece(const Json &args) {
    u32 uid = 0;
    int chara = -1;
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !ReadGearUid(args[0], uid) ||
        !ReadGearCharacter(args[0], chara)) {
        return Invalid("expected {uid = a piece's uid, character = optional id or \"active\"}");
    }
    EquipGear(chara, uid);
    SyncGearBag();
    return {ApiCallStatus::Ok, GearState(), {}};
}

// items.gear_unequip({kind, character?}): takes the piece in that slot off into the bag.
ApiCallResult UnequipGearPiece(const Json &args) {
    int chara = -1;
    const int kind = args.is_array() && args.size() == 1 && args[0].is_object() && args[0].contains("kind") &&
                             args[0]["kind"].is_string()
                         ? GearKindFromName(args[0]["kind"].get<std::string>())
                         : -1;
    if (kind < 0 || !ReadGearCharacter(args[0], chara)) {
        return Invalid("expected {kind = hat, tunic, gloves, boots or pouch, character = optional id or \"active\"}");
    }
    if (WornGear(chara, kind) == nullptr) return Failed("nothing is worn there");
    if (!UnequipGear(chara, kind)) return Failed("the gear bag is full");
    SyncGearBag();
    return {ApiCallStatus::Ok, GearState(), {}};
}

// items.gear_remove({uid, sell?}): takes the piece away; with sell = true its value goes into the wallet.
ApiCallResult RemoveGearPiece(const Json &args) {
    u32 uid = 0;
    const bool has_sell = args.is_array() && args.size() == 1 && args[0].is_object() && args[0].contains("sell");
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != (has_sell ? 2u : 1u) ||
        !ReadGearUid(args[0], uid) || (has_sell && !args[0]["sell"].is_boolean())) {
        return Invalid("expected {uid = a piece's uid, sell = optional boolean}");
    }
    const bool sell = has_sell && args[0]["sell"].get<bool>();
    CDngStatusData *status = CurrentStatus();
    if (sell && status == nullptr) return Failed("the game has no active save");
    int money = 0;
    if (sell) {
        const int before = status->money;
        status->money = static_cast<u16>(std::min(before + FindGear(uid)->value, 65535));
        money = status->money - before;
    }
    RemoveGear(uid);
    SyncGearBag();
    return {ApiCallStatus::Ok, {{"uid", uid}, {"money", money}}, {}};
}

ApiCallResult SelectQuickItemSlot(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("slot") || !args[0]["slot"].is_number_integer()) {
        return Invalid("expected {slot = 1, 2, or 3}");
    }
    const int slot = args[0]["slot"].get<int>();
    if (slot < 1 || slot > 3) return Invalid("slot must be between 1 and 3");
    CDngStatusData *status = CurrentStatus();
    if (!IsActiveDungeonStatus(status)) return Failed("the game has no active dungeon inventory");
    if (BtGameModeFlag != BT_GAME_MODE_PLAY || gameTask != GAME_TASK_PLAY) {
        return Failed("quick-item selection requires active dungeon play");
    }
    const int item_no = status->item_pack.quick_item_slot[slot - 1];
    const int count = std::max<int>(0, status->item_pack.quick_item_qty[slot - 1]);
    if (item_no < ITEM_DUNGEON_START || item_no >= ITEM_WEAPON_START || count == 0) {
        return Failed("the selected quick-item slot is empty");
    }
    itemNowSel = slot;
    activeItem.now = slot;
    return {ApiCallStatus::Ok,
            {{"slot", slot}, {"item_no", item_no}, {"id", ItemStableId(item_no)}, {"count", count}}, {}};
}

ApiCallResult UseQuickItemSlot(const Json &args) {
    ApiCallResult result = SelectQuickItemSlot(args);
    if (result.status != ApiCallStatus::Ok) return result;
    MoonModRequestQuickItemUse();
    result.value["use_requested"] = true;
    return result;
}

ApiCallResult RequestFirstPersonToggle(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("render.first_person_toggle takes no arguments");
    if (!IsActiveDungeonStatus(CurrentStatus()) || BtGameModeFlag != BT_GAME_MODE_PLAY ||
        (gameTask != GAME_TASK_PLAY && gameTask != GAME_TASK_EYE_CAMERA)) {
        return Failed("first-person view can only be toggled during active dungeon play");
    }
    MoonModRequestFirstPersonToggle();
    return {ApiCallStatus::Ok, {{"requested", true}}, {}};
}

ApiCallResult SetNativeAutosave(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("enabled") || !args[0]["enabled"].is_boolean()) {
        return Invalid("expected {enabled = boolean}");
    }
    const bool enabled = args[0]["enabled"].get<bool>();
    GameSetAutosave(enabled);
    return {ApiCallStatus::Ok, {{"enabled", enabled}}, {}};
}

ApiCallResult GetItem(const Json &args) {
    if (!args.is_array() || args.size() != 1) return Invalid("expected one Chronicle item id or item number");
    int item_no = 0;
    if (!ReadItemNumber(args[0], item_no)) {
        return Invalid("expected a Chronicle item number or stable item id");
    }
    if (const auto mod_id = g_mod_item_by_native_id.find(item_no); mod_id != g_mod_item_by_native_id.end()) {
        const auto mod_item = g_mod_items.find(mod_id->second);
        if (mod_item != g_mod_items.end()) return {ApiCallStatus::Ok, ModItemJson(mod_item->second), {}};
    }
    const COM_ITEM_INFO *common = GetCommonItemInfo(item_no);
    const ITEM_DATA *item = GetItemData(item_no);
    if (common == nullptr || common->kind != ITEMKIND_ITEM || item == nullptr) {
        return Failed("no built-in Chronicle item has that id");
    }
    return {ApiCallStatus::Ok, ItemDefinition(item_no, *common, *item), {}};
}

constexpr int kChronicleWeaponCount = 152;

bool ParseWeaponStableId(const Json &value, int &item_no) {
    if (!value.is_string()) return false;
    const std::string &id = value.get_ref<const std::string &>();
    if (const auto registered = g_mod_weapons.find(id); registered != g_mod_weapons.end()) {
        item_no = registered->second.native_item_no;
        return true;
    }
    constexpr std::string_view prefix = "chronicle:weapon:";
    if (!std::string_view(id).starts_with(prefix)) return false;
    const std::string_view suffix = std::string_view(id).substr(prefix.size());
    int parsed = 0;
    const auto [end, error] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), parsed);
    if (error != std::errc{} || end != suffix.data() + suffix.size() || parsed < ITEM_WEAPON_START ||
        parsed >= ITEM_WEAPON_END) return false;
    item_no = parsed;
    return true;
}

bool ReadWeaponNumber(const Json &selector, int &item_no) {
    if (selector.is_string()) return ParseWeaponStableId(selector, item_no);
    if (selector.is_number_integer()) {
        if (selector.is_number_unsigned()) {
            const unsigned long long value = selector.get<unsigned long long>();
            if (value < ITEM_WEAPON_START || value >= ITEM_WEAPON_START + kChronicleWeaponCount) return false;
            item_no = static_cast<int>(value);
        } else {
            const long long value = selector.get<long long>();
            if (value < ITEM_WEAPON_START || value >= ITEM_WEAPON_START + kChronicleWeaponCount) return false;
            item_no = static_cast<int>(value);
        }
        return true;
    }
    if (!selector.is_object()) return false;
    const auto id = selector.find("id");
    const auto number = selector.find("item_no");
    const auto native_number = selector.find("native_item_no");
    int by_id = -1;
    int by_number = -1;
    if (id != selector.end() && !ParseWeaponStableId(*id, by_id)) return false;
    const Json *number_value = number != selector.end() ? &*number
                                : native_number != selector.end() ? &*native_number
                                                                  : nullptr;
    if (number_value != nullptr) {
        if (!number_value->is_number_integer()) return false;
        if (number_value->is_number_unsigned()) {
            const unsigned long long value = number_value->get<unsigned long long>();
            if (value < ITEM_WEAPON_START || value >= ITEM_WEAPON_START + kChronicleWeaponCount) return false;
            by_number = static_cast<int>(value);
        } else {
            const long long value = number_value->get<long long>();
            if (value < ITEM_WEAPON_START || value >= ITEM_WEAPON_START + kChronicleWeaponCount) return false;
            by_number = static_cast<int>(value);
        }
    }
    if (by_id < 0 && by_number < 0) return false;
    if (by_id >= 0 && by_number >= 0 && by_id != by_number) return false;
    item_no = by_id >= 0 ? by_id : by_number;
    return true;
}

Json WeaponDefinitionJson(int item_no, const COM_ITEM_INFO &common, const WEAPON_DATA &weapon, bool include_state) {
    Json elements = Json::array();
    Json monster_effectiveness = Json::array();
    for (int index = 0; index < 5; ++index) elements.push_back(weapon.elem[index]);
    for (int index = 0; index < 10; ++index) monster_effectiveness.push_back(weapon.vs_monster[index]);
    const bool owner_valid = weapon.owner < kCharacterIds.size();
    Json result = {{"id", "chronicle:weapon:" + std::to_string(item_no)},
                   {"item_no", item_no},
                   {"native_item_no", item_no},
                   {"kind", "weapon"},
                   {"message_id", common.msg},
                   {"icon_index", common.icon_index},
                   {"owner", owner_valid ? Json(kCharacterIds[weapon.owner]) : Json(nullptr)},
                   {"stats", {{"attack", weapon.attack}, {"endurance", weapon.endurance}, {"speed", weapon.speed},
                               {"magic", weapon.magic}, {"durability", weapon.durability}}},
                   {"stat_caps", {{"attack", weapon.attack_max}, {"magic", weapon.magic_max}}},
                   {"elements", std::move(elements)},
                   {"monster_effectiveness", std::move(monster_effectiveness)},
                   {"socket_count", weapon.hole_num},
                   {"experience", {{"base", weapon.exp_base}, {"per_level", weapon.exp_per_level}}},
                   {"chain_position", weapon.chain_pos},
                   {"build_up_masks", {weapon.buildup_mask0, weapon.buildup_mask1}},
                   {"flags", weapon.flags},
                   {"mod_owned", false},
                   {"status", "registered"}};
    if (const std::string name = MoonModWeaponNativeName(item_no); !name.empty()) result["name"] = name;
    if (!include_state) return result;

    Json instances = Json::array();
    Json owned_by = Json::array();
    Json equipped_by = Json::array();
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) {
        result["owned_count"] = nullptr;
        result["owned_by"] = nullptr;
        result["equipped_by"] = nullptr;
        result["instances"] = nullptr;
        return result;
    }
    for (int character_id = 0; character_id < static_cast<int>(kCharacterIds.size()); ++character_id) {
        for (int slot = 0; slot < 11; ++slot) {
            const WEAPON_HAVE &owned = status->chara_weapons[character_id][slot];
            if (owned.item_no != item_no) continue;
            const std::string &character = kCharacterIds[character_id];
            owned_by.push_back(character);
            const bool equipped = status->equipped_weapon_slot[character_id] == slot;
            if (equipped) equipped_by.push_back(character);
            instances.push_back({{"character", character},
                                 {"slot", slot},
                                 {"equipped", equipped},
                                 {"level", owned.level},
                                 {"stats", {{"attack", owned.attack}, {"endurance", owned.endurance},
                                             {"speed", owned.speed}, {"magic", owned.magic},
                                             {"durability", owned.durability},
                                             {"durability_current", owned.durability_f}}},
                                 {"elements", Json::array({static_cast<unsigned char>(owned.elem[0]),
                                                            static_cast<unsigned char>(owned.elem[1]),
                                                            static_cast<unsigned char>(owned.elem[2]),
                                                            static_cast<unsigned char>(owned.elem[3]),
                                                            static_cast<unsigned char>(owned.elem[4])})},
                                 {"vs_monster", Json::array({static_cast<unsigned char>(owned.vs_monster[0]),
                                                             static_cast<unsigned char>(owned.vs_monster[1]),
                                                             static_cast<unsigned char>(owned.vs_monster[2]),
                                                             static_cast<unsigned char>(owned.vs_monster[3]),
                                                             static_cast<unsigned char>(owned.vs_monster[4]),
                                                             static_cast<unsigned char>(owned.vs_monster[5]),
                                                             static_cast<unsigned char>(owned.vs_monster[6]),
                                                             static_cast<unsigned char>(owned.vs_monster[7]),
                                                             static_cast<unsigned char>(owned.vs_monster[8]),
                                                             static_cast<unsigned char>(owned.vs_monster[9])})},
                                 {"flags", static_cast<std::uint16_t>(owned.flags)},
                                 {"experience", owned.experience},
                                 {"synthesis_count", owned.synthesis_count}});
        }
    }
    result["owned_count"] = instances.size();
    result["owned_by"] = std::move(owned_by);
    result["equipped_by"] = std::move(equipped_by);
    result["instances"] = std::move(instances);
    return result;
}

Json ModWeaponJson(const ModWeapon &weapon, bool include_state) {
    const COM_ITEM_INFO *common = GetCommonItemInfo(weapon.native_item_no);
    const WEAPON_DATA *data = GetWeaponData(weapon.native_item_no);
    if (common == nullptr || data == nullptr) return Json::object();
    Json result = WeaponDefinitionJson(weapon.native_item_no, *common, *data, include_state);
    result["id"] = weapon.id;
    result["name"] = weapon.name;
    result["owner"] = data->owner < kCharacterIds.size() ? Json(kCharacterIds[data->owner]) : Json(nullptr);
    if (const int holder = MoonModWeaponHolder(weapon.native_item_no); holder >= 0 && holder < static_cast<int>(kCharacterIds.size())) {
        result["holder"] = kCharacterIds[holder];
    }
    result["native_item_no"] = weapon.native_item_no;
    result["template_weapon"] = "chronicle:weapon:" + std::to_string(weapon.template_weapon);
    if (!weapon.description.empty()) result["description"] = weapon.description;
    if (weapon.menu_scale != 1.0) result["menu_scale"] = weapon.menu_scale;
    result["mod_owned"] = true;
    result["owner_mod"] = weapon.owner_mod;
    result["assets"] = {{"source", weapon.model.empty() ? "native_template" : "package_override"},
                        {"overridden", !weapon.model.empty() || !weapon.icon.empty()},
                        {"model", weapon.model.empty() ? Json(nullptr) : Json("commenu/weapon/" + weapon.model + ".chr")},
                        {"icon", weapon.icon.empty() ? Json(nullptr) : Json("icons/" + weapon.icon + ".png")}};
    result["status"] = "registered";
    return result;
}

ApiCallResult ListWeapons(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("weapons.list takes no arguments");
    Json result = Json::array();
    for (int item_no = ITEM_WEAPON_START; item_no < ITEM_WEAPON_START + kChronicleWeaponCount; ++item_no) {
        if (g_mod_weapon_by_native_id.contains(item_no)) continue;
        const COM_ITEM_INFO *common = GetCommonItemInfo(item_no);
        if (common == nullptr || common->kind != ITEMKIND_WEAPON) continue;
        const WEAPON_DATA *weapon = GetWeaponData(item_no);
        if (weapon == nullptr) return Failed("a native Chronicle weapon definition is unavailable");
        result.push_back(WeaponDefinitionJson(item_no, *common, *weapon, false));
    }
    for (const auto &[id, weapon] : g_mod_weapons) result.push_back(ModWeaponJson(weapon, false));
    return {ApiCallStatus::Ok, std::move(result), {}};
}

ApiCallResult GetWeapon(const Json &args) {
    if (!args.is_array() || args.size() != 1) return Invalid("weapons.get expects one native weapon id or selector");
    int item_no = -1;
    if (!ReadWeaponNumber(args[0], item_no)) {
        return Invalid("expected chronicle:weapon:<item_no>, a weapon item number, or a matching selector object");
    }
    const COM_ITEM_INFO *common = GetCommonItemInfo(item_no);
    const WEAPON_DATA *weapon = GetWeaponData(item_no);
    if (common == nullptr || common->kind != ITEMKIND_WEAPON || weapon == nullptr) {
        return Failed("no built-in Chronicle weapon has that id");
    }
    if (const auto id = g_mod_weapon_by_native_id.find(item_no); id != g_mod_weapon_by_native_id.end()) {
        const auto registered = g_mod_weapons.find(id->second);
        if (registered != g_mod_weapons.end()) return {ApiCallStatus::Ok, ModWeaponJson(registered->second, true), {}};
    }
    return {ApiCallStatus::Ok, WeaponDefinitionJson(item_no, *common, *weapon, true), {}};
}

ApiCallResult RegisterWeapon(std::string_view mod_id, const std::filesystem::path &save_root, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected one weapon definition object");
    }
    const Json &definition = args[0];
    const auto id_it = definition.find("id");
    const auto name_it = definition.find("name");
    const auto template_it = definition.find("template_weapon");
    if (id_it == definition.end() || !id_it->is_string() || name_it == definition.end() || !name_it->is_string() ||
        template_it == definition.end()) {
        return Invalid("weapon definition requires string id and name fields and a template_weapon");
    }
    for (auto it = definition.begin(); it != definition.end(); ++it) {
        const std::string_view key(it.key());
        if (key != "id" && key != "name" && key != "kind" && key != "template_weapon" && key != "owner" &&
            key != "stats" && key != "stat_caps" && key != "assets" && key != "model" && key != "icon" && key != "description" &&
            key != "menu_scale") {
            return Invalid("weapon definition contains an unsupported field: " + it.key());
        }
    }
    std::string local_id = id_it->get<std::string>();
    std::string qualified_id;
    const std::size_t separator = local_id.find(':');
    if (separator == std::string::npos) {
        if (!InputId(local_id)) return Invalid("weapon id must be a short identifier using letters, digits, '.', '_' or '-'");
        qualified_id = std::string(mod_id) + ":" + local_id;
    } else {
        if (local_id.find(':', separator + 1) != std::string::npos || local_id.substr(0, separator) != mod_id ||
            !InputId(std::string_view(local_id).substr(separator + 1))) {
            return Invalid("fully qualified weapon ids must use this mod's namespace");
        }
        qualified_id = std::move(local_id);
    }
    const std::string &display_name = name_it->get_ref<const std::string &>();
    if (display_name.empty() || display_name.size() > 95) return Invalid("weapon name must contain 1-95 UTF-8 bytes");
    const auto kind = definition.find("kind");
    if (kind != definition.end() && (!kind->is_string() || kind->get<std::string>() != "weapon")) {
        return Invalid("Chronicle accepts registered definitions with kind 'weapon'");
    }
    const auto assets = definition.find("assets");
    if (assets != definition.end() && ((!assets->is_object() && !assets->is_array()) || !assets->empty())) {
        return Invalid("Chronicle weapon registration inherits native template assets; asset overrides are not accepted");
    }
    int template_item_no = -1;
    if (!ReadWeaponNumber(*template_it, template_item_no) || template_item_no < ITEM_WEAPON_START ||
        template_item_no >= ITEM_WEAPON_END) {
        return Invalid("template_weapon must identify a built-in Chronicle weapon");
    }
    const COM_ITEM_INFO *template_common = GetCommonItemInfo(template_item_no);
    const WEAPON_DATA *template_data = GetWeaponData(template_item_no);
    if (template_common == nullptr || template_common->kind != ITEMKIND_WEAPON || template_data == nullptr) {
        return Invalid("template_weapon does not identify an available native weapon");
    }

    int owner_character = -1;
    const auto owner = definition.find("owner");
    if (owner != definition.end()) {
        if (!owner->is_string()) return Invalid("owner must be one of Chronicle's six playable character ids");
        for (int index = 0; index < static_cast<int>(kCharacterIds.size()); ++index) {
            if (owner->get_ref<const std::string &>() == kCharacterIds[index]) owner_character = index;
        }
        if (owner_character < 0) return Invalid("owner must be one of Chronicle's playable character ids");
    }
    // A registered character's weapon belongs to the one it fights as, and sits in its own row.
    int holder_character = -1;
    if (owner_character >= 0 && CombatChara(owner_character) != owner_character) {
        holder_character = owner_character;
        owner_character = CombatChara(owner_character);
    }

    int attack = -1;
    int durability = -1;
    int attack_max = -1;
    int magic_max = -1;
    const auto stats = definition.find("stats");
    if (stats != definition.end()) {
        if (!stats->is_object()) return Invalid("stats must be an object");
        for (auto it = stats->begin(); it != stats->end(); ++it) {
            const std::string_view key(it.key());
            if (key != "attack" && key != "endurance" && key != "speed" && key != "magic" && key != "durability") {
                return Invalid("stats contains an unsupported field: " + it.key());
            }
        }
        auto read_stat = [&](const char *key, int &value) {
            const auto found = stats->find(key);
            return found == stats->end() || ReadBoundedInt(*stats, key, 0, 32767, value);
        };
        if (!read_stat("attack", attack) || !read_stat("durability", durability)) {
            return Invalid("stats.attack and stats.durability must be integers from 0 to 32767");
        }
        // The native registration bridge currently exposes attack and durability as writable fields.
        for (const char *key : {"endurance", "speed", "magic"}) {
            if (stats->contains(key)) return Invalid(std::string("stats.") + key + " is inherited from template_weapon and cannot be overridden yet");
        }
    }
    const auto caps = definition.find("stat_caps");
    if (caps != definition.end()) {
        if (!caps->is_object()) return Invalid("stat_caps must be an object");
        for (auto it = caps->begin(); it != caps->end(); ++it) {
            if (it.key() != "attack" && it.key() != "magic") {
                return Invalid("stat_caps accepts only attack and magic");
            }
        }
        const auto attack_cap = caps->find("attack");
        const auto magic_cap = caps->find("magic");
        if ((attack_cap != caps->end() && !ReadBoundedInt(*caps, "attack", 0, 32767, attack_max)) ||
            (magic_cap != caps->end() && !ReadBoundedInt(*caps, "magic", 0, 32767, magic_max))) {
            return Invalid("stat_caps.attack and stat_caps.magic must be integers from 0 to 32767");
        }
    }

    std::string model_stem;
    std::string icon_stem;
    std::string asset_error;
    if (!ReadOwnedAssetStem(definition, "model", mod_id, "commenu/weapon", "chr", model_stem, asset_error) ||
        !ReadOwnedAssetStem(definition, "icon", mod_id, "icons", "png", icon_stem, asset_error)) {
        return Invalid(std::move(asset_error));
    }
    if (!model_stem.empty() && model_stem.size() == 6 && model_stem[0] == 'c' && model_stem[3] == 'w') {
        return Invalid("model must not be a native weapon model name (cNNwNN); give the new model a stem of its own");
    }
    // description: what the weapon menu's card says of it (a string, or up to 4 lines), in place of its template's
    std::vector<std::string> description;
    if (const auto it = definition.find("description"); it != definition.end()) {
        const Json lines = it->is_string() ? Json::array({*it}) : *it;
        if (!lines.is_array() || lines.empty() || lines.size() > 4) return Invalid("description must be a string or up to 4 lines");
        for (const Json &line : lines) {
            if (!line.is_string() || line.get<std::string>().empty() || line.get<std::string>().size() > 64) {
                return Invalid("each description line must be 1-64 bytes");
            }
            description.push_back(line.get<std::string>());
        }
    }

    // menu_scale: how big the weapon menu draws the model, against its holder's own weapons (a long staff shrunk to fit)
    double menu_scale = 1.0;
    if (const auto it = definition.find("menu_scale"); it != definition.end()) {
        if (!it->is_number() || it->get<double>() < 0.1 || it->get<double>() > 4.0) return Invalid("menu_scale must be a number from 0.1 to 4");
        menu_scale = it->get<double>();
    }

    const auto current = g_mod_weapons.find(qualified_id);
    if (current != g_mod_weapons.end()) {
        const ModWeapon &old = current->second;
        if (old.owner_mod != mod_id) return Invalid("a weapon with this stable id is already owned by another mod");
        if (old.name != display_name || old.template_weapon != template_item_no || old.owner_character != owner_character ||
            old.attack != attack || old.durability != durability || old.attack_max != attack_max || old.magic_max != magic_max ||
            old.model != model_stem || old.icon != icon_stem || old.description != description ||
            old.menu_scale != menu_scale) {
            return Invalid("a registered weapon definition cannot be changed until its owning mod is unloaded");
        }
        return {ApiCallStatus::Ok, ModWeaponJson(old, false), {}};
    }

    int native_item_no = -1;
    std::string allocation_error;
    if (!AllocateModWeaponSlot(qualified_id, save_root, native_item_no, allocation_error)) {
        return Failed(std::move(allocation_error));
    }
    const auto slot_owner = g_mod_weapon_by_native_id.find(native_item_no);
    COM_ITEM_INFO *common = GetCommonItemInfo(native_item_no);
    if ((slot_owner != g_mod_weapon_by_native_id.end() && slot_owner->second != qualified_id) || common == nullptr ||
        (slot_owner == g_mod_weapon_by_native_id.end() && common->kind != 0)) {
        return Failed("the persisted native weapon slot cannot be safely materialized");
    }
    char registration_error[192] = {};
    if (!RegisterMoonModWeapon(native_item_no, template_item_no, attack, durability, attack_max, magic_max,
                               owner_character, registration_error, sizeof(registration_error))) {
        return Failed(registration_error[0] == '\0' ? "native weapon registration failed" : registration_error);
    }
    SetMoonModWeaponModel(native_item_no, model_stem.c_str());
    SetMoonModWeaponMenuScale(native_item_no, static_cast<float>(menu_scale));
    SetMoonModWeaponHolder(native_item_no, holder_character);
    {
        std::string about;
        for (const std::string &line : description) about += (about.empty() ? "" : "\n") + line;
        SetMoonModWeaponText(native_item_no, display_name, about);
    }
    SetItemIconOverride(native_item_no, icon_stem.empty() ? "" : ("icons/" + icon_stem + ".png").c_str());
    ModWeapon weapon{qualified_id, std::string(mod_id), display_name, template_item_no, owner_character,
                     attack, durability, attack_max, magic_max, native_item_no, model_stem, icon_stem, description, menu_scale};
    g_mod_weapons.emplace(qualified_id, std::move(weapon));
    g_mod_weapon_by_native_id[native_item_no] = qualified_id;
    return {ApiCallStatus::Ok, ModWeaponJson(g_mod_weapons.at(qualified_id), false), {}};
}

// MoonMod: a weapon fits its owner, and a registered character the weapons of the character he fights as (CombatChara).
bool WeaponFits(const CDngStatusData &status, int item_no, int character_id) {
    const int user = const_cast<CDngStatusData &>(status).CheckWeaponUser(item_no);
    return user >= 0 && (user == character_id || user == CombatChara(character_id));
}

ApiCallResult GiveWeapon(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = stable_weapon_id, character = optional character_id}");
    }
    const Json &query = args[0];
    const bool has_character = query.contains("character");
    if (query.size() != (has_character ? 2u : 1u) || !query.contains("id") || !query["id"].is_string()) {
        return Invalid("weapons.give accepts only a stable weapon id and an optional character");
    }
    int wanted_character = -1;
    if (has_character && !ReadCharacterId(query["character"], wanted_character)) {
        return Invalid("character must be one of Chronicle's playable character ids");
    }
    int item_no = -1;
    if (!ReadWeaponNumber(query["id"], item_no)) return Invalid("expected a stable Chronicle weapon id");
    const COM_ITEM_INFO *common = GetCommonItemInfo(item_no);
    const WEAPON_DATA *definition = GetWeaponData(item_no);
    if (common == nullptr || common->kind != ITEMKIND_WEAPON || definition == nullptr) {
        return Failed("no registered Chronicle weapon has that id");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    const int party_size = status->GetPartySize();
    if (party_size < 0 || party_size > static_cast<int>(kCharacterIds.size())) {
        return Failed("the saved party size is out of range");
    }
    const int character_id = has_character ? wanted_character : status->CheckWeaponUser(item_no);
    if (character_id < 0 || !InParty(*status, character_id)) {
        return Failed("the selected character has not joined the party or cannot use this weapon");
    }
    if (!WeaponFits(*status, item_no, character_id)) {
        return Failed("the weapon is not compatible with the selected character");
    }
    for (int slot = 0; slot < 11; ++slot) {
        WEAPON_HAVE &owned = status->chara_weapons[character_id][slot];
        if (owned.item_no >= ITEM_WEAPON_START) continue;
        WepDataListToHaveCopy(item_no, &owned);
        // MoonMod: one with nothing in hand (a registered character who has just joined) takes it up, as the
        // story's own joins equip a character's first weapon; switching to one with no weapon would not load.
        const bool equipped = status->equipped_weapon_slot[character_id] < 0;
        if (equipped) status->equipped_weapon_slot[character_id] = static_cast<s8>(slot);
        return {ApiCallStatus::Ok,
                {{"id", g_mod_weapon_by_native_id.contains(item_no)
                              ? Json(g_mod_weapon_by_native_id.at(item_no))
                              : Json("chronicle:weapon:" + std::to_string(item_no))},
                 {"native_item_no", item_no}, {"slot", slot}, {"added", true}, {"equipped", equipped}},
                {}};
    }
    return Failed("the selected character's weapon inventory has no free slot");
}

ApiCallResult EquipWeapon(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = weapon_id, character = optional character_id, slot = optional 0-10}");
    }
    const Json &selector = args[0];
    int item_no = -1;
    if (!ReadWeaponNumber(selector, item_no)) {
        return Invalid("expected a Chronicle weapon id, item number, or selector object");
    }
    const COM_ITEM_INFO *common = GetCommonItemInfo(item_no);
    const WEAPON_DATA *definition = GetWeaponData(item_no);
    if (common == nullptr || common->kind != ITEMKIND_WEAPON || definition == nullptr) {
        return Failed("no built-in Chronicle weapon has that id");
    }

    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    const bool in_town = mode == GAME_MODE_EDIT && status->cur_floor == -1 && status->cur_georama >= 0 && status->cur_georama < 6;
    const bool in_dungeon = IsActiveDungeonStatus(status);
    if (!in_town && !in_dungeon) {
        return Failed("weapons.equip works only in an active Georama town or dungeon");
    }

    int character_id = status->cur_chara;
    const auto character_field = selector.find("character");
    if (character_field != selector.end()) {
        if (!character_field->is_string()) {
            return Invalid("character must be one of Chronicle's playable character ids");
        }
        const std::string &name = character_field->get_ref<const std::string &>();
        const auto character = std::ranges::find(kCharacterIds, name);
        if (character == kCharacterIds.end()) {
            return Invalid("character must be one of Chronicle's playable character ids");
        }
        character_id = static_cast<int>(character - kCharacterIds.begin());
    }

    const int party_size = status->GetPartySize();
    if (party_size < 0 || party_size > static_cast<int>(kCharacterIds.size())) {
        return Failed("the saved party size is out of range");
    }
    if (character_id < 0 || !InParty(*status, character_id)) {
        return Failed("the selected character has not joined the party");
    }
    if (!WeaponFits(*status, item_no, character_id)) {
        return Failed("the weapon is not compatible with the selected character");
    }

    int requested_slot = -1;
    const auto slot_field = selector.find("slot");
    if (slot_field != selector.end()) {
        if (!slot_field->is_number_integer()) return Invalid("slot must be an integer from 0 to 10");
        if (slot_field->is_number_unsigned()) {
            const unsigned long long value = slot_field->get<unsigned long long>();
            if (value > 10) return Invalid("slot must be an integer from 0 to 10");
            requested_slot = static_cast<int>(value);
        } else {
            const long long value = slot_field->get<long long>();
            if (value < 0 || value > 10) return Invalid("slot must be an integer from 0 to 10");
            requested_slot = static_cast<int>(value);
        }
    }

    int target_slot = -1;
    for (int slot = 0; slot < 11; ++slot) {
        if (requested_slot >= 0 && slot != requested_slot) continue;
        if (status->chara_weapons[character_id][slot].item_no != item_no) continue;
        if (target_slot >= 0) {
            return Invalid("weapon has multiple owned copies; specify its slot");
        }
        target_slot = slot;
    }
    if (target_slot < 0) return Failed("weapon is not owned by the selected character");

    // -1: nothing in hand yet (a registered character who has just joined a story party, party.join)
    const int previous_slot = status->equipped_weapon_slot[character_id];
    if (previous_slot < -1 || previous_slot >= 11) {
        return Failed("the saved equipped-weapon slot is out of range");
    }
    const int previous_item_no = previous_slot >= 0 ? status->chara_weapons[character_id][previous_slot].item_no : -1;
    const bool changed = previous_slot != target_slot;
    // In a dungeon the active character holds the weapon model: reload it the way a character switch does.
    const bool reload = changed && in_dungeon && character_id == status->cur_chara;
    if (reload) {
        if (BtActStatus.action_on != 0 || BtActStatus.movement_locked != 0) {
            return Failed("the native game currently prevents changing weapons during this action");
        }
        if (g_character_switch_routine == nullptr) return Failed("the native character switch routine is unavailable");
    }
    if (changed) status->equipped_weapon_slot[character_id] = static_cast<s8>(target_slot);
    if (reload) g_character_switch_routine(character_id, 1);

    return {ApiCallStatus::Ok,
            {{"id", "chronicle:weapon:" + std::to_string(item_no)}, {"item_no", item_no},
             {"native_item_no", item_no}, {"character", kCharacterIds[character_id]},
             {"previous_slot", previous_slot}, {"slot", target_slot}, {"previous_item_no", previous_item_no},
             {"equipped", true}, {"changed", changed}},
            {}};
}

ApiCallResult UpgradeWeapon(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = weapon_id, character = optional character_id, slot = optional 0-10}");
    }
    const Json &selector = args[0];
    const ApiCallResult definition = GetWeapon(args);
    if (definition.status != ApiCallStatus::Ok) return definition;
    const int item_no = definition.value.value("native_item_no", 0);
    if (item_no < ITEM_WEAPON_START || item_no >= ITEM_WEAPON_START + kChronicleWeaponCount) {
        return Failed("weapon definition is unavailable in the native weapon table");
    }
    const WEAPON_DATA *weapon_data = GetWeaponData(item_no);
    if (weapon_data == nullptr) return Failed("native weapon progression data is unavailable");
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");

    int requested_character = -1;
    const auto character_field = selector.find("character");
    if (character_field != selector.end()) {
        if (!character_field->is_string()) return Invalid("character must be one of Chronicle's playable character ids");
        const std::string &name = character_field->get_ref<const std::string &>();
        for (int index = 0; index < static_cast<int>(kCharacterIds.size()); ++index) {
            if (name == kCharacterIds[index]) requested_character = index;
        }
        if (requested_character < 0) return Invalid("character must be one of Chronicle's playable character ids");
    }
    int requested_slot = -1;
    const auto slot_field = selector.find("slot");
    if (slot_field != selector.end()) {
        if (!slot_field->is_number_integer()) return Invalid("slot must be an integer from 0 to 10");
        if (slot_field->is_number_unsigned()) {
            const unsigned long long value = slot_field->get<unsigned long long>();
            if (value > 10) return Invalid("slot must be an integer from 0 to 10");
            requested_slot = static_cast<int>(value);
        } else {
            const long long value = slot_field->get<long long>();
            if (value < 0 || value > 10) return Invalid("slot must be an integer from 0 to 10");
            requested_slot = static_cast<int>(value);
        }
    }
    if (requested_slot >= 0 && requested_character < 0) return Invalid("slot requires an explicit character id");

    WEAPON_HAVE *target = nullptr;
    int target_character = -1;
    int target_slot = -1;
    for (int character = 0; character < static_cast<int>(kCharacterIds.size()); ++character) {
        if (requested_character >= 0 && character != requested_character) continue;
        for (int slot = 0; slot < 11; ++slot) {
            if (requested_slot >= 0 && slot != requested_slot) continue;
            WEAPON_HAVE *candidate = &status->chara_weapons[character][slot];
            if (candidate->item_no != item_no) continue;
            if (target != nullptr) return Invalid("weapon has multiple owned copies; specify both character and slot");
            target = candidate;
            target_character = character;
            target_slot = slot;
        }
    }
    if (target == nullptr) return Failed("weapon is not owned by the active save");

    const int previous_level = target->level;
    const int level_experience = GetWeaponMaxExp(target);
    const int experience = target->experience;
    if (previous_level >= 99 || experience < level_experience) {
        return {ApiCallStatus::Ok,
                {{"id", definition.value.value("id", "")}, {"item_no", item_no}, {"native_item_no", item_no},
                 {"character", kCharacterIds[target_character]}, {"slot", target_slot},
                 {"level", previous_level}, {"experience", experience},
                 {"experience_to_level", level_experience}, {"upgraded", false},
                 {"changed", false}, {"ready", false}},
                {}};
    }

    WEAPON_HAVE upgraded{};
    WeaponLevelUpValueCalc(target, &upgraded, 1, 0);
    if (upgraded.item_no != item_no || upgraded.level != previous_level) {
        return Failed("the native weapon calculation did not produce a valid level-up state");
    }
    upgraded.level = static_cast<s16>(previous_level + 1);
    for (ATTACH_LIST &attachment : upgraded.attach) std::memset(&attachment, 0, sizeof(attachment));
    *target = upgraded;

    return {ApiCallStatus::Ok,
            {{"id", definition.value.value("id", "")}, {"item_no", item_no}, {"native_item_no", item_no},
             {"character", kCharacterIds[target_character]}, {"slot", target_slot},
             {"previous_level", previous_level}, {"level", target->level},
             {"experience", target->experience}, {"experience_to_level", GetWeaponMaxExp(target)},
             {"stats", {{"attack", target->attack}, {"endurance", target->endurance},
                         {"speed", target->speed}, {"magic", target->magic},
                         {"durability", target->durability}}},
             {"synthesis_count", target->synthesis_count}, {"upgraded", true},
             {"changed", true}, {"ready", true}},
            {}};
}

ApiCallResult GiveItems(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = Chronicle item id, count = optional integer}");
    }
    const Json &query = args[0];
    int item_no = 0;
    if (!ReadItemNumber(query, item_no)) {
        return Invalid("expected a Chronicle item number or stable item id");
    }
    const COM_ITEM_INFO *common = GetCommonItemInfo(item_no);
    if (common && common->kind == ITEMKIND_ATTACH && GetAttachData(item_no)) {
        int count = 1;
        if (query.contains("count") && !ReadBoundedInt(query, "count", 1, 999, count)) return Invalid("count must be 1..999");
        CDngStatusData *status = CurrentStatus();
        if (!status || status->overflow_flag) return Failed("no active inventory or inventory overflow");
        int occupied = 0;
        for (int i=0; i<43; ++i) if (status->consumable_items[i].id >= ITEM_ATTACH_START) ++occupied;
        int added = 0;
        while (added < count && occupied < 40) {
            if (status->GetItem(item_no, 0) < 0) break;
            ++occupied; ++added;
        }
        return {ApiCallStatus::Ok, {{"id", ItemStableId(item_no)}, {"requested", count}, {"added", added}, {"remaining_capacity", std::max(0,40-occupied)}}, {}};
    }
    const ITEM_DATA *item = GetItemData(item_no);
    if (common == nullptr || common->kind != ITEMKIND_ITEM || item == nullptr) {
        return Failed("no built-in Chronicle item has that id");
    }
    int requested = 1;
    const auto count_field = query.find("count");
    if (count_field != query.end()) {
        if (!count_field->is_number_integer()) return Invalid("count must be an integer from 1 to 999");
        const std::int64_t count = count_field->get<std::int64_t>();
        if (count < 1 || count > 999) return Invalid("count must be an integer from 1 to 999");
        requested = static_cast<int>(count);
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save inventory");
    const int capacity = status->item_pack.num;
    if (capacity < 0 || capacity > 103) return Failed("the saved item-pack capacity is out of range");
    if (status->overflow_flag != 0) return Failed("the saved item pack is already in overflow state");

    int occupied = 0;
    for (int index = 0; index < capacity; ++index) {
        const int held_id = status->item_pack.item[index];
        if (held_id >= ITEM_DUNGEON_START && held_id < ITEM_WEAPON_START) ++occupied;
    }
    for (int index = 0; index < 3; ++index) {
        if (status->item_pack.quick_item_slot[index] >= ITEM_DUNGEON_START &&
            status->item_pack.quick_item_slot[index] < ITEM_WEAPON_START) {
            occupied += std::max<int>(0, status->item_pack.quick_item_qty[index]);
        }
    }

    int added = 0;
    while (added < requested && occupied < capacity) {
        if (status->GetItem(item_no, 0) < 0) break;
        ++added;
        ++occupied;
    }
    return {ApiCallStatus::Ok,
            {{"id", ItemStableId(item_no)}, {"requested", requested}, {"added", added},
             {"remaining_capacity", std::max(0, capacity - occupied)}},
            {}};
}

ApiCallResult RemoveItems(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = Chronicle item id, count = optional integer}");
    }
    const Json &query = args[0];
    int item_no = 0;
    if (!ReadItemNumber(query, item_no)) {
        return Invalid("expected a Chronicle item number or stable item id");
    }
    const COM_ITEM_INFO *common = GetCommonItemInfo(item_no);
    const ITEM_DATA *item = GetItemData(item_no);
    if (common == nullptr || common->kind != ITEMKIND_ITEM || item == nullptr) {
        return Failed("no built-in Chronicle item has that id");
    }
    int requested = 1;
    const auto count_field = query.find("count");
    if (count_field != query.end()) {
        if (!count_field->is_number_integer()) return Invalid("count must be an integer from 1 to 999");
        const std::int64_t count = count_field->get<std::int64_t>();
        if (count < 1 || count > 999) return Invalid("count must be an integer from 1 to 999");
        requested = static_cast<int>(count);
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save inventory");
    const int capacity = status->item_pack.num;
    if (capacity < 0 || capacity > 103) return Failed("the saved item-pack capacity is out of range");

    const int before = GetNowItemNum(static_cast<short>(item_no), &status->item_pack);
    int removed = 0;
    while (removed < requested && GetNowItemNum(static_cast<short>(item_no), &status->item_pack) > 0) {
        DeleteItemAfterUseItem(static_cast<short>(item_no), &status->item_pack);
        ++removed;
    }
    const int remaining = GetNowItemNum(static_cast<short>(item_no), &status->item_pack);
    return {ApiCallStatus::Ok,
            {{"id", ItemStableId(item_no)}, {"requested", requested},
             {"removed", std::min(removed, before)}, {"remaining", remaining}},
            {}};
}

bool g_mod_item_use = false;

ApiCallResult UseItem(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = Chronicle item id, character = optional 'active' or a joined character id}");
    }
    const Json &query = args[0];
    for (auto field = query.begin(); field != query.end(); ++field) {
        if (field.key() != "id" && field.key() != "item_no" && field.key() != "character") {
            return Invalid("items.use accepts only id, item_no, and character fields");
        }
    }

    int item_no = 0;
    if (!ReadItemNumber(query, item_no)) {
        return Invalid("expected a Chronicle item number or stable item id");
    }
    const COM_ITEM_INFO *common = GetCommonItemInfo(item_no);
    const ITEM_DATA *item = GetItemData(item_no);
    if (common == nullptr || common->kind != ITEMKIND_ITEM || item == nullptr) {
        return Failed("no built-in Chronicle item has that id");
    }
    // ItemUseFunc also handles random foods, weapon upgrades, and effects that
    // depend on live menu/audio state. Keep this adapter slice to deterministic
    // HP/water consumables and single-status cures.
    const bool heals_hp = item->use_flags == ITEMUSE_HEAL_HP;
    const bool restores_water = item->use_flags == ITEMUSE_DRINK;
    const bool cures_all = item->kind_flags == ITEMKINDF_USE_ON_CHARACTER && item->use_flags == kChronicleCureItemFlags;
    const std::string_view cured_status = item->kind_flags == ITEMKINDF_USE_ON_CHARACTER && !cures_all
                                              ? ChronicleStatusForCureFlag(item->use_flags)
                                              : std::string_view{};
    const bool cures_status = !cured_status.empty() || cures_all;
    if ((!heals_hp && !restores_water && !cures_status) ||
        ((heals_hp || restores_water) && item->kind_flags != ITEMKINDF_CONSUMABLE)) {
        return {ApiCallStatus::Unsupported, nullptr,
                "Chronicle items.use supports HP/water consumables, single-status cures, and a cure_all item for supported ailments"};
    }

    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save inventory");
    int character_id = status->cur_chara;
    if (!ReadPlayerTarget(query, *status, character_id)) {
        return Invalid("character must be 'active' or a Chronicle playable character id");
    }
    if (character_id < CHARA_TOAN || character_id >= CharaCount()) {
        return Failed("the selected character id is not recognized");
    }
    if (status->party_size < 0 || status->party_size > static_cast<int>(kCharacterIds.size())) {
        return Failed("the native party size is outside the supported range");
    }
    if (!InParty(*status, character_id)) {
        return Failed("the selected character is not in the joined party");
    }
    const int held = GetNowItemNum(static_cast<short>(item_no), &status->item_pack);
    if (held <= 0) return Failed("the active inventory does not contain this item");
    if (heals_hp && (status->hp[character_id] <= 0 || status->hp[character_id] >= status->max_hp[character_id])) {
        return Failed("the selected character cannot receive HP from this item right now");
    }
    if (restores_water && 2.0f + static_cast<int>(status->water_now[character_id]) >= status->water_max[character_id]) {
        return Failed("the selected character cannot receive water from this item right now");
    }
    const int cured_ailment = cures_all ? (AILMENT_FREEZE | AILMENT_POISON | AILMENT_CURSE | AILMENT_GOO)
                                        : cures_status ? ChronicleAilmentForName(cured_status) : 0;
    if (cures_status && (status->hp[character_id] <= 0 || (status->ailments[character_id] & cured_ailment) == 0)) {
        return Failed("the selected character does not have the status this item cures");
    }

    const int hp_before = status->hp[character_id];
    const float water_before = status->water_now[character_id];
    const int ailments_before = status->ailments[character_id];
    g_mod_item_use = true; // a mod's own use is not the item menu's: item_heal_lock leaves it be
    const int used = ItemUseFunc(status, item_no, character_id, ITEM_USE_ON_CHARA, nullptr);
    g_mod_item_use = false;
    if (used != 1) return Failed("the native item rules refused this use");
    DeleteItemAfterUseItem(static_cast<short>(item_no), &status->item_pack);
    const int remaining = GetNowItemNum(static_cast<short>(item_no), &status->item_pack);
    Json result = {{"id", ItemStableId(item_no)},
                   {"character", kCharacterIds[character_id]},
                   {"consumed", std::max(0, held - remaining)},
                   {"remaining", remaining}};
    if (heals_hp) {
        const int hp_target = status->next_hp[character_id];
        result["effect"] = "heal";
        result["hp"] = status->hp[character_id];
        result["hp_target"] = hp_target;
        result["healing"] = std::max(0, hp_target - hp_before);
    } else if (restores_water) {
        result["effect"] = "water";
        result["water"] = status->water_now[character_id];
        result["water_restored"] = std::max(0.0f, status->water_now[character_id] - water_before);
        result["water_max"] = status->water_max[character_id];
    } else if (cures_all) {
        result["effect"] = "cure_all";
        result["removed"] = (ailments_before & cured_ailment & ~status->ailments[character_id]) != 0;
        result["remaining_ailments"] = status->ailments[character_id] & cured_ailment;
    } else {
        result["effect"] = "cure_status";
        result["cured_status"] = cured_status;
        result["removed"] = (ailments_before & cured_ailment) != 0 &&
                             (status->ailments[character_id] & cured_ailment) == 0;
        result["active"] = (status->ailments[character_id] & cured_ailment) != 0;
    }
    return {ApiCallStatus::Ok, std::move(result), {}};
}

ApiCallResult BindInputAction(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = local_action_id, keys = {physical_input, ...}, context = optional local_context_id}");
    }
    const Json &definition = args[0];
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "id" && field.key() != "keys" && field.key() != "context") {
            return Invalid("input.bind received an unsupported field");
        }
    }
    const auto id = definition.find("id");
    const auto keys = definition.find("keys");
    if (id == definition.end() || !id->is_string() || !InputId(id->get_ref<const std::string &>()) ||
        keys == definition.end() || !keys->is_array() || keys->empty() || keys->size() > 8) {
        return Invalid("id must be a local identifier and keys must contain 1-8 physical inputs");
    }
    std::string context = "default";
    if (definition.contains("context")) {
        const Json &context_value = definition["context"];
        if (!context_value.is_string() || !InputId(context_value.get_ref<const std::string &>())) {
            return Invalid("context must be a local identifier");
        }
        if (context_value != "default") context = std::string(mod_id) + ":" + context_value.get<std::string>();
    }
    std::vector<std::string_view> names;
    Json result_names = Json::array();
    for (const Json &key : *keys) {
        if (!key.is_string() || key.get_ref<const std::string &>().empty() || key.get_ref<const std::string &>().size() > 64) {
            return Invalid("input names must be non-empty strings of at most 64 bytes");
        }
        names.push_back(key.get_ref<const std::string &>());
        result_names.push_back(key);
    }
    const std::string qualified = std::string(mod_id) + ":" + id->get<std::string>();
    if (!InputBindModAction(qualified, names)) {
        return Invalid("keys must name supported keyboard, mouse-button, or gamepad-button inputs; Escape is reserved");
    }
    g_input_contexts.BindAction(qualified, context);
    return {ApiCallStatus::Ok, {{"id", qualified}, {"keys", std::move(result_names)}, {"context", context}}, {}};
}

ApiCallResult ReadInputAction(std::string_view mod_id, const Json &args, bool consume) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = local_action_id}");
    }
    const auto id = args[0].find("id");
    if (id == args[0].end() || !id->is_string() || !InputId(id->get_ref<const std::string &>())) {
        return Invalid("expected a local action id");
    }
    const std::string qualified = std::string(mod_id) + ":" + id->get<std::string>();
    if (!InputModActionExists(qualified)) return Invalid("input action is not registered by this mod");
    const std::string context = g_input_contexts.ContextFor(qualified);
    const bool active = g_input_contexts.IsActive(qualified);
    if (consume) {
        bool pressed = false;
        if (active) {
            pressed = InputModActionConsumePress(qualified);
        } else {
            while (InputModActionConsumePress(qualified)) {}
        }
        return {ApiCallStatus::Ok, {{"id", qualified}, {"context", context}, {"active", active}, {"pressed", pressed}}, {}};
    }
    return {ApiCallStatus::Ok,
            {{"id", qualified}, {"context", context}, {"active", active},
             {"down", active && InputModActionHeld(qualified)}}, {}};
}

ApiCallResult PushInputContext(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("id") || !args[0]["id"].is_string()) {
        return Invalid("expected {id = local_context_id}");
    }
    const std::string &local_id = args[0]["id"].get_ref<const std::string &>();
    if (!InputId(local_id) || local_id == "default") return Invalid("id must be a local context identifier other than 'default'");
    std::string token;
    std::string qualified_id;
    if (!g_input_contexts.Push(mod_id, local_id, token, qualified_id)) return Failed("the input context stack is full");
    return {ApiCallStatus::Ok,
            {{"id", qualified_id}, {"token", token}, {"active", g_input_contexts.Active()},
             {"depth", g_input_contexts.stack.size()}}, {}};
}

ApiCallResult PopInputContext(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("token") || !args[0]["token"].is_string()) {
        return Invalid("expected {token = token_returned_by_context_push}");
    }
    const std::string &token = args[0]["token"].get_ref<const std::string &>();
    if (token.empty() || token.size() > 64) return Invalid("token must be a non-empty context token");
    std::string removed_id;
    if (!g_input_contexts.Pop(mod_id, token, removed_id)) return Invalid("context token is missing or owned by another mod");
    return {ApiCallStatus::Ok,
            {{"id", removed_id}, {"active", g_input_contexts.Active()}, {"depth", g_input_contexts.stack.size()}}, {}};
}

ApiCallResult GetInputContext(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("input.context_get takes no arguments");
    Json stack = Json::array();
    for (const InputContextFrame &frame : g_input_contexts.stack) {
        stack.push_back({{"id", frame.id}, {"token", frame.token}, {"owner", frame.owner}});
    }
    return {ApiCallStatus::Ok,
            {{"active", g_input_contexts.Active()}, {"depth", stack.size()}, {"stack", std::move(stack)}}, {}};
}

ApiCallResult SetWheelMode(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {mode = 0|1}, or {} to restore native camera-zoom routing");
    }
    if (args[0].empty()) {
        std::erase_if(g_wheel_mode_requests, [owner](const auto &entry) { return entry.first == owner; });
        InputSetWheelMode(g_wheel_mode_requests.empty() ? 2 : g_wheel_mode_requests.back().second);
        return {ApiCallStatus::Ok, {{"mode", g_wheel_mode_requests.empty() ? 2 : g_wheel_mode_requests.back().second}}, {}};
    }
    if (args[0].size() != 1 || !args[0].contains("mode") || !args[0]["mode"].is_number_integer()) {
        return Invalid("expected only {mode = 0|1}");
    }
    const int requested = args[0]["mode"].get<int>();
    if (requested < 0 || requested > 1) return Invalid("mode must be 0 (Left/Right) or 1 (Up/Down)");
    auto existing = std::ranges::find(g_wheel_mode_requests, owner, &std::pair<std::string, int>::first);
    if (existing == g_wheel_mode_requests.end()) {
        if (g_wheel_mode_requests.size() >= 64) return Failed("the wheel mode owner limit is full");
        g_wheel_mode_requests.push_back({std::string(owner), requested});
    } else {
        existing->second = requested;
    }
    const int effective = RefreshChronicleWheelMode();
    return {ApiCallStatus::Ok, {{"mode", effective}, {"requested_mode", requested}}, {}};
}

ApiCallResult ReadInputAxis(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {axis = left_x|left_y|right_x|right_y|left_trigger|right_trigger, pad = optional 0 or 1}");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "axis" && field.key() != "pad") return Invalid("input.axis received an unsupported field");
    }
    const auto axis = args[0].find("axis");
    if (axis == args[0].end() || !axis->is_string()) {
        return Invalid("axis must name one of the four stick axes or two trigger axes");
    }
    int pad = 0;
    if (args[0].contains("pad") && !ReadBoundedInt(args[0], "pad", 0, kInputPadCount - 1, pad)) {
        return Invalid("pad must be an integer from 0 through 1");
    }
    const std::string &name = axis->get_ref<const std::string &>();
    const InputPadState &state = InputGetPad(pad);
    std::uint8_t value = kInputAxisCentre;
    bool trigger = false;
    if (name == "left_x") value = state.left_x;
    else if (name == "left_y") value = state.left_y;
    else if (name == "right_x") value = state.right_x;
    else if (name == "right_y") value = state.right_y;
    else if (name == "left_trigger") {
        value = state.left_trigger;
        trigger = true;
    } else if (name == "right_trigger") {
        value = state.right_trigger;
        trigger = true;
    } else return Invalid("axis must name one of the four stick axes or two trigger axes");
    const float normalized = trigger ? static_cast<float>(value) / 255.0f
                             : value == kInputAxisCentre ? 0.0f
                             : value < kInputAxisCentre
                                 ? static_cast<float>(static_cast<int>(value) - kInputAxisCentre) / kInputAxisCentre
                                 : static_cast<float>(static_cast<int>(value) - kInputAxisCentre) /
                                       (255 - kInputAxisCentre);
    return {ApiCallStatus::Ok,
            {{"axis", name}, {"pad", pad}, {"connected", state.connected}, {"value", normalized}}, {}};
}

ApiCallResult ReadDungeonCamera(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("render.camera_get takes no arguments");
    if (mode != GAME_MODE_DUNGEON || NowCamera__3 == nullptr) {
        return Failed("there is no active Chronicle dungeon camera");
    }
    float eye[4]{};
    float target[4]{};
    NowCamera__3->GetPos(eye);
    NowCamera__3->GetRef(target);
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(eye[axis]) || !std::isfinite(target[axis])) {
            return Failed("the native camera pose contains a non-finite coordinate");
        }
    }
    const float heading = NowCamera__3->GetAngleH();
    const float pitch = NowCamera__3->angle_v;
    const float roll = NowCamera__3->GetRoll();
    if (!std::isfinite(heading) || !std::isfinite(pitch) || !std::isfinite(roll)) {
        return Failed("the native camera pose contains a non-finite angle");
    }
    return {ApiCallStatus::Ok,
            {{"scene", "dungeon"},
             {"eye", {{"x", eye[0]}, {"y", eye[1]}, {"z", eye[2]}}},
             {"target", {{"x", target[0]}, {"y", target[1]}, {"z", target[2]}}},
             {"heading", heading}, {"pitch", pitch}, {"roll", roll}},
            {}};
}

// The active floor's fog, background and ambient light: what the floor script's FOG, BG_COL and
// AMBIENT lines set (and URA_ for the back side), which the dungeon draw reapplies every frame
// (port/src/dun/gameloop.cpp). Values hold until the next floor reads its own script.
// The floor's three directional lights (LIGHT_C n): direction in the columns of light, colour in
// the rows of color.
Json DungeonLights(const sceVu0FMATRIX light, const sceVu0FMATRIX color) {
    Json lights = Json::array();
    for (int n = 0; n < 3; ++n) {
        lights.push_back({{"x", light[0][n]}, {"y", light[1][n]}, {"z", light[2][n]},
                          {"r", color[n][0]}, {"g", color[n][1]}, {"b", color[n][2]}});
    }
    return lights;
}

Json DungeonLightingSide(const float *fog_rate, const u8 *fog_color, const u8 *background, const float *ambient) {
    return {{"fog",
             {{"near", fog_rate[0]}, {"far", fog_rate[1]}, {"min", fog_rate[2]}, {"max", fog_rate[3]},
              {"r", fog_color[0]}, {"g", fog_color[1]}, {"b", fog_color[2]}}},
            {"background", {{"r", background[0]}, {"g", background[1]}, {"b", background[2]}}},
            {"ambient", {{"r", ambient[0]}, {"g", ambient[1]}, {"b", ambient[2]}}}};
}

ApiCallResult ReadDungeonLighting(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("dungeons.lighting_get takes no arguments");
    if (mode != GAME_MODE_DUNGEON) return Failed("there is no active Chronicle dungeon floor");
    return {ApiCallStatus::Ok,
            {{"side", lightingMode == 0 ? "front" : "back"},
             {"front", DungeonLightingSide(main_fogRate, main_fogColor, main_bgColor, main_ambientlight)},
             {"back", DungeonLightingSide(sub_fogRate, sub_fogColor, sub_bgColor, sub_ambientlight)},
             {"front_lights", DungeonLights(main_light, main_lightcolor)},
             {"back_lights", DungeonLights(sub_light, sub_lightcolor)},
             {"view_scale", NowDngMap != nullptr ? Json(NowDngMap->draw_dist_scale) : Json(nullptr)}},
            {}};
}

ApiCallResult SetDungeonLighting(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].empty()) {
        return Invalid("expected {side?, fog?, background?, ambient?}");
    }
    const Json &definition = args[0];
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "side" && field.key() != "fog" && field.key() != "background" && field.key() != "ambient" &&
            field.key() != "lights" && field.key() != "view_scale") {
            return Invalid("dungeons.lighting_set only accepts side, fog, background, ambient, lights and view_scale");
        }
    }
    if (mode != GAME_MODE_DUNGEON) return Failed("there is no active Chronicle dungeon floor");
    std::string side = "both";
    if (definition.contains("side")) {
        if (!definition["side"].is_string()) return Invalid("side must be \"front\", \"back\" or \"both\"");
        side = definition["side"].get<std::string>();
        if (side != "front" && side != "back" && side != "both") {
            return Invalid("side must be \"front\", \"back\" or \"both\"");
        }
    }
    // every field is checked before anything is written
    struct Field {
        const char *group;
        const char *key;
        float minimum;
        float maximum;
    };
    static constexpr Field kFields[] = {
        {"fog", "near", 0.0f, 100000.0f}, {"fog", "far", 0.0f, 100000.0f}, {"fog", "min", 0.0f, 255.0f},
        {"fog", "max", 0.0f, 255.0f},     {"fog", "r", 0.0f, 255.0f},      {"fog", "g", 0.0f, 255.0f},
        {"fog", "b", 0.0f, 255.0f},       {"background", "r", 0.0f, 255.0f}, {"background", "g", 0.0f, 255.0f},
        {"background", "b", 0.0f, 255.0f}, {"ambient", "r", 0.0f, 255.0f}, {"ambient", "g", 0.0f, 255.0f},
        {"ambient", "b", 0.0f, 255.0f},
    };
    for (const char *group : {"fog", "background", "ambient"}) {
        if (!definition.contains(group)) continue;
        const Json &values = definition[group];
        if (!values.is_object()) return Invalid(std::string(group) + " must be a table");
        for (auto value = values.begin(); value != values.end(); ++value) {
            const Field *known = nullptr;
            for (const Field &field : kFields) {
                if (std::string_view(field.group) == group && value.key() == field.key) known = &field;
            }
            if (known == nullptr) return Invalid(std::string(group) + "." + value.key() + " is not a lighting field");
            if (!value->is_number()) return Invalid(std::string(group) + "." + value.key() + " must be a number");
            const float number = value->get<float>();
            if (!std::isfinite(number) || number < known->minimum || number > known->maximum) {
                return Invalid(std::string(group) + "." + value.key() + " is outside its range");
            }
        }
    }
    // lights: up to three of {light = 1..3, x, y, z (a direction), r, g, b (0..255)}
    if (definition.contains("lights")) {
        const Json &lights = definition["lights"];
        if (!lights.is_array() || lights.size() > 3) return Invalid("lights must be a list of at most 3 lights");
        for (const Json &light : lights) {
            if (!light.is_object() || !light.contains("light") || !light["light"].is_number_integer() ||
                light["light"].get<int>() < 1 || light["light"].get<int>() > 3) {
                return Invalid("each light needs light = 1, 2 or 3");
            }
            for (auto value = light.begin(); value != light.end(); ++value) {
                const std::string &key = value.key();
                if (key == "light") continue;
                const bool direction = key == "x" || key == "y" || key == "z";
                if (!direction && key != "r" && key != "g" && key != "b") return Invalid("light." + key + " is not a light field");
                if (!value->is_number() || !std::isfinite(value->get<float>()) ||
                    (direction ? std::fabs(value->get<float>()) > 1.0e6f : (value->get<float>() < 0.0f || value->get<float>() > 255.0f))) {
                    return Invalid("light." + key + " is outside its range");
                }
            }
        }
    }
    if (definition.contains("view_scale") &&
        (!definition["view_scale"].is_number() || !(definition["view_scale"].get<float>() >= 0.1f) ||
         definition["view_scale"].get<float>() > 10.0f)) {
        return Invalid("view_scale must be 0.1..10 (the floor script's VIEWLEVEL)");
    }
    const auto apply_lights = [&definition](sceVu0FMATRIX light, sceVu0FMATRIX color) {
        if (!definition.contains("lights")) return;
        for (const Json &entry : definition["lights"]) {
            const int n = entry["light"].get<int>() - 1;
            if (entry.contains("x") || entry.contains("y") || entry.contains("z")) {
                float direction[4] = {entry.value("x", light[0][n]), entry.value("y", light[1][n]),
                                      entry.value("z", light[2][n]), 0.0f};
                const float length = std::sqrt(direction[0] * direction[0] + direction[1] * direction[1] +
                                               direction[2] * direction[2]);
                if (length > 1.0e-6f) {
                    for (int axis = 0; axis < 3; ++axis) light[axis][n] = direction[axis] / length;
                }
            }
            const char *channels[3] = {"r", "g", "b"};
            for (int c = 0; c < 3; ++c) {
                if (entry.contains(channels[c])) color[n][c] = entry[channels[c]].get<float>();
            }
        }
    };
    if (side != "back") apply_lights(main_light, main_lightcolor);
    if (side != "front") apply_lights(sub_light, sub_lightcolor);
    if (definition.contains("view_scale") && NowDngMap != nullptr) {
        NowDngMap->draw_dist_scale = definition["view_scale"].get<float>();
    }
    const auto apply = [&definition](float *fog_rate, u8 *fog_color, u8 *background, float *ambient) {
        const auto number = [&definition](const char *group, const char *key) -> std::optional<float> {
            if (!definition.contains(group) || !definition[group].contains(key)) return std::nullopt;
            return definition[group][key].get<float>();
        };
        if (auto v = number("fog", "near")) fog_rate[0] = *v;
        if (auto v = number("fog", "far")) fog_rate[1] = *v;
        if (auto v = number("fog", "min")) fog_rate[2] = *v;
        if (auto v = number("fog", "max")) fog_rate[3] = *v;
        const char *channels[3] = {"r", "g", "b"};
        for (int c = 0; c < 3; ++c) {
            if (auto v = number("fog", channels[c])) fog_color[c] = static_cast<u8>(*v);
            if (auto v = number("background", channels[c])) background[c] = static_cast<u8>(*v);
            if (auto v = number("ambient", channels[c])) ambient[c] = *v;
        }
    };
    if (side != "back") apply(main_fogRate, main_fogColor, main_bgColor, main_ambientlight);
    if (side != "front") apply(sub_fogRate, sub_fogColor, sub_bgColor, sub_ambientlight);
    return ReadDungeonLighting(Json::array());
}

// Script-VM arguments built from Lua: numbers (whole numbers as ints, others as floats), booleans,
// or {out = "int"|"float"} for a value the native writes back, returned in order by Out().
struct NativeArgs {
    std::array<RS_STACKDATA, 17> stack{};
    std::array<RS_STACKDATA, 16> outs{};
    std::array<int, 16>          out_kind{}; // 0 none, 1 int, 2 float
    std::array<std::array<char, 32>, 16> texts{}; // frame names (SET_SHOT's 'dcol0')
    int                          count = 0;

    std::string Read(const Json &request) {
        if (!request.contains("args")) return {};
        const Json &values = request["args"];
        if (!values.is_array() || values.size() > 16) return "args must be a list of at most 16 values";
        for (std::size_t i = 0; i < values.size(); ++i) {
            const Json &value = values[i];
            if (value.is_number_integer()) {
                stack[i].type = RS_INT;
                stack[i].i = value.get<int>();
            } else if (value.is_number()) {
                const float number = value.get<float>();
                if (!std::isfinite(number)) return "args must be finite numbers";
                stack[i].type = RS_FLOAT;
                stack[i].f = number;
            } else if (value.is_boolean()) {
                stack[i].type = RS_INT;
                stack[i].i = value.get<bool>() ? 1 : 0;
            } else if (value.is_string()) {
                // A model frame's name: letters, digits and _, at most 31.
                const std::string &text = value.get_ref<const std::string &>();
                if (text.empty() || text.size() > 31 ||
                    !std::ranges::all_of(text, [](char c) { return std::isalnum(static_cast<unsigned char>(c)) != 0 || c == '_'; })) {
                    return "a text arg is a frame name: 1-31 letters, digits or _";
                }
                std::ranges::copy(text, texts[i].begin());
                texts[i][text.size()] = '\0';
                stack[i].type = RS_STR;
                stack[i].s = texts[i].data();
            } else if (value.is_object() && value.size() == 1 && value.contains("out") && value["out"].is_string() &&
                       (value["out"] == "int" || value["out"] == "float")) {
                out_kind[i] = value["out"] == "int" ? 1 : 2;
                outs[i].type = out_kind[i] == 1 ? RS_INT : RS_FLOAT;
                stack[i].type = RS_PTR;
                stack[i].p = &outs[i];
            } else {
                return "each arg must be a number, a boolean, a frame name or {out = \"int\"|\"float\"}";
            }
        }
        count = static_cast<int>(values.size());
        return {};
    }

    Json Out() const {
        Json out = Json::array();
        for (int i = 0; i < count; ++i) {
            if (out_kind[i] == 1) out.push_back(outs[i].i);
            if (out_kind[i] == 2) out.push_back(outs[i].f);
        }
        return out;
    }
};

// The event-script natives safe to run outside a running event: the getters and checks and the
// game, map and event flag setters. Town natives live in editloop3.cpp, dungeon ones in
// btsysscript.cpp; both fill ext_func__2 for the mode they run in.
struct EventNative {
    const char *name;
    int         op;
    bool        dungeon;
};
constexpr EventNative kEventNatives[] = {
    {"GET_PADON", 0, false},
    {"GET_PADDOWN", 1, false},
    {"GET_PADUP", 2, false},
    {"GET_APAD", 903, false},
    {"GET_RANDOM", 4, false},
    {"GET_OLD_MAPNO", 16, false},
    {"GET_DUNGEON_FLOOR", 22, false},
    {"CHECK_IN_VILLAGER", 58, false},
    {"GET_CHARA_POS", 64, false},
    {"GET_CHARA_TALK_POS", 65, false},
    {"GET_CHARA_ROT", 69, false},
    {"GET_NPC_TALK_POS", 130, false},
    {"GET_NPC_POS", 131, false},
    {"GET_NPC_PARTS_NO", 132, false},
    {"GET_NPC_ROT", 139, false},
    {"GET_NPC_SCALE", 148, false},
    {"GET_NPC_BODY_SIZE", 155, false},
    {"CHECK_MES_COMPLETE", 200, false},
    {"CHECK_MES_WAIT", 201, false},
    {"CHECK_MES", 202, false},
    {"SET_MES_WIN_FLAG", 205, false},
    {"CHECK_COMPLETE_PARTS", 256, false},
    {"GET_EDIT_PARTS_POS", 257, false},
    {"GET_NOW_TIME", 258, false},
    {"GET_CLOCK", 262, false},
    {"CHECK_PLACE", 263, false},
    {"CHECK_VILLAGER", 267, false},
    {"GET_OBJ_POS", 325, false},
    {"GET_OBJ_ROT", 327, false},
    {"GET_OBJ_SCALE", 329, false},
    {"GET_TALKNPC_INFO_ID", 384, false},
    {"GET_TALKNPC_ID", 385, false},
    {"GET_TALKNPC_STATUS", 389, false},
    {"GET_CAMERA_POS", 423, false},
    {"GET_CAMERA_REF", 425, false},
    {"GET_CAMERA_ANGLE", 430, false},
    {"GET_CAMERA_ROTATE", 432, false},
    {"GET_CAMERA_ROLL", 442, false},
    {"CHECK_FADE", 502, false},
    {"SSET_GAME_FLAG", 700, false},
    {"SGET_GAME_FLAG", 701, false},
    {"SSET_GAME_INT_FLAG", 711, false},
    {"SGET_GAME_INT_FLAG", 712, false},
    {"SSET_MAP_FLAG", 703, false},
    {"SGET_MAP_FLAG", 704, false},
    {"SET_WORK_FLAG", 705, false},
    {"GET_WORK_FLAG", 706, false},
    {"SSET_REQUEST_EVENT_FLAG", 713, false},
    {"GET_BGM_VOL", 805, false},
    {"GET_BGM_NO", 806, false},
    {"GET_OUT_BGM_NO", 814, false},
    {"CHECK_BGM_FADE", 819, false},
    {"GET_TV_MODE", 1001, false},
    {"GET_LANG_CODE", 1002, false},
    {"GET_FLOOR_LEVEL", 1010, true},
    {"GET_OLD_FLOOR_LEVEL", 1012, true},
    {"GET_ACTION_MODE", 1020, true},
    {"CHECK_EVENT_FLG", 1023, true},
    {"SET_EVENT_FLG", 1024, true},
    {"GET_OBJHDL", 1030, true},
    {"SET_OBJHDL_DRAW_FLAG", 1033, true},
    {"GET_OBJHDL_POS", 1034, true},
    {"GET_OBJHDL_ROT", 1035, true},
    {"GET_EVENT_POS", 1040, true},
    {"GET_EVENT_ROT", 1041, true},
    {"CHK_ATRA_HAVE", 1057, true},
    {"GET_NOW_USER_ID", 1060, true},
    {"GET_NPC_OBJHDL", 1062, true},
    {"GET_GATEKEY_NO", 1064, true},
    {"SET_DEAD_FLAG", 1069, true},
    {"ALL_DRAW_FLAG", 1072, true},
    {"GET_RUBY_ELEMENT", 1074, true},
    {"GET_ITEM_TRAPID", 1081, true},
    {"CHECK_MARDAN", 1086, true},
    {"CHECK_CHR_HELP", 1089, true},
    {"GET_MAP_CODE", 1095, true},
    {"GET_ITEM_UNIT_NO", 1097, true},
    {"GET_PIERO_ITEM", 1100, true},
};

ApiCallResult ListEventNatives(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("events.natives takes no arguments");
    Json natives = Json::array();
    for (const EventNative &native : kEventNatives) {
        natives.push_back({{"name", native.name}, {"op", native.op}, {"mode", native.dungeon ? "dungeon" : "town"}});
    }
    return {ApiCallStatus::Ok, {{"natives", std::move(natives)}}, {}};
}

ApiCallResult CallEventNative(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("native")) {
        return Invalid("expected {native = name or op, args = {...}?}");
    }
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "native" && field.key() != "args") return Invalid("events.call only accepts native and args");
    }
    const bool dungeon = mode == GAME_MODE_DUNGEON;
    const EventNative *native = nullptr;
    for (const EventNative &candidate : kEventNatives) {
        const Json &which = request["native"];
        const bool match = (which.is_string() && which.get_ref<const std::string &>() == candidate.name) ||
                           (which.is_number_integer() && which.get<int>() == candidate.op);
        if (match && candidate.dungeon == dungeon) native = &candidate;
    }
    if (native == nullptr) {
        return Invalid(std::string("native is not an event native of the current mode (") +
                       (dungeon ? "dungeon" : "town") + "); see game.events.natives()");
    }
    NativeArgs native_args;
    if (std::string error = native_args.Read(request); !error.empty()) return Invalid(error);
    if (mode != GAME_MODE_DUNGEON && mode != GAME_MODE_EDIT) return Failed("events.call needs a town or a dungeon");
    if (ext_func__2[native->op] == nullptr) return Failed("this native is not set up in the current scene");
    const int ok = ext_func__2[native->op](native_args.stack.data(), native_args.count);
    return {ApiCallStatus::Ok, {{"native", native->name}, {"ok", ok != 0}, {"out", native_args.Out()}}, {}};
}

// game.text: message text by the language files' key ("dun.script.d01.d01.12"), set per mod.
std::map<std::string, std::string> g_mod_text_owner; // message key > owning mod

bool ReadTextKey(const Json &args, Json &request, std::initializer_list<std::string_view> allowed, std::string &error) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("key") ||
        !args[0]["key"].is_string()) {
        error = "expected {key = \"<message file key>.<id>\", ...}";
        return false;
    }
    request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (std::ranges::find(allowed, std::string_view(field.key())) == allowed.end()) {
            error = "unsupported field " + field.key();
            return false;
        }
    }
    return true;
}

ApiCallResult GetText(const Json &args) {
    Json request;
    std::string error;
    if (!ReadTextKey(args, request, {"key"}, error)) return Invalid(error);
    const std::string &key = request["key"].get_ref<const std::string &>();
    const std::optional<std::string> text = LocalizeGetText(key);
    const auto owner = g_mod_text_owner.find(key);
    return {ApiCallStatus::Ok,
            {{"key", key},
             {"text", text ? Json(*text) : Json(nullptr)},
             {"source", owner != g_mod_text_owner.end() ? "mod:" + owner->second : (text ? "language file" : "disc")}},
            {}};
}

ApiCallResult SetText(std::string_view mod_id, const Json &args, bool clear) {
    Json request;
    std::string error;
    if (!ReadTextKey(args, request, clear ? std::initializer_list<std::string_view>{"key"}
                                          : std::initializer_list<std::string_view>{"key", "text"}, error)) {
        return Invalid(error);
    }
    const std::string key = request["key"].get<std::string>();
    if (!clear && (!request.contains("text") || !request["text"].is_string() ||
                   request["text"].get_ref<const std::string &>().size() > 4096)) {
        return Invalid("text.set requires text of at most 4096 bytes");
    }
    const auto owner = g_mod_text_owner.find(key);
    if (owner != g_mod_text_owner.end() && owner->second != mod_id) {
        return Failed("another mod (" + owner->second + ") already sets this message");
    }
    if (clear && owner == g_mod_text_owner.end()) return Invalid("this mod does not set that message");
    if (!LocalizeSetModText(key, clear ? std::nullopt : std::optional<std::string>(request["text"].get<std::string>()))) {
        return Invalid("key must be a message file key, a dot and a message id (see lang-export)");
    }
    if (clear) g_mod_text_owner.erase(key);
    else g_mod_text_owner[key] = std::string(mod_id);
    return {ApiCallStatus::Ok, {{"key", key}, {"applies", "when the message file next loads"}}, {}};
}

void UnloadModText(std::string_view mod_id) {
    for (auto entry = g_mod_text_owner.begin(); entry != g_mod_text_owner.end();) {
        if (entry->second == mod_id) {
            LocalizeSetModText(entry->first, std::nullopt);
            entry = g_mod_text_owner.erase(entry);
        } else {
            ++entry;
        }
    }
}

// The retail monster AI natives by the name the .stb scripts call them (leading underscore dropped).
// _RUN_SCRIPT, the global push/pop pair and the reference pair need a running script, so they are left out.
struct MonsterNative {
    const char *name;
    int         op;
};
constexpr MonsterNative kMonsterNatives[] = {
    {"GET_DISTANCE", 10},
    {"GET_POSITION", 11},
    {"SET_ROTATION", 12},
    {"CHK_ROTATION", 13},
    {"CHK_MOVE", 14},
    {"CHK_USER_INNER_PRODUCT", 15},
    {"GET_VECTOR", 30},
    {"GET_DIRECTION", 31},
    {"SET_MOVE", 32},
    {"CHK_MOVE_INFO", 33},
    {"SET_MOVE_CANSEL", 34},
    {"SET_ROT_CANSEL", 35},
    {"SET_POSITION", 36},
    {"STATUS_SET_FALL", 100},
    {"STATUS_SET_MUTEKI", 101},
    {"STATUS_SET_ALPHA", 102},
    {"STATUS_CHK_ALPHA", 103},
    {"STATUS_SET_DEAD", 104},
    {"STATUS_SET_PALLET", 105},
    {"GET_RAND", 180},
    {"GET_RANDF", 181},
    {"SIN_DEG", 182},
    {"COS_DEG", 183},
    {"STATUS_SET_CLIPLEVEL", 106},
    {"STATUS_SET_EVENT", 107},
    {"STATUS_SET_COL_OFF", 108},
    {"STATUS_GET_LIFE_RATE", 109},
    {"STATUS_GET_HEIGHT", 112},
    {"STATUS_GET_HITDMG_VOL", 113},
    {"STATUS_GET_MOTION_ID", 114},
    {"STATUS_GET_DMG_ID", 115},
    {"STATUS_SET_LOCKON_DIST", 116},
    {"STATUS_SET_SHADOW_LEN", 117},
    {"STATUS_SET_LOCKON_TRG", 118},
    {"STATUS_GET_USER_VECTOR", 110},
    {"SET_MOV_COL", 136},
    {"SET_BODY_COL", 130},
    {"SET_BODY_COL_PARA", 134},
    {"SET_DMG_COL", 131},
    {"SET_DMG_PARA", 132},
    {"SET_SHOT", 133},
    {"SET_SHOT2", 229},
    {"SET_SND_FRM", 140},
    {"SET_LOOP_SND", 230},
    {"STOP_LOOP_SND", 231},
    {"DEL_LOOP_SND", 232},
    {"SET_SND_NOW", 141},
    {"SET_MOTION", 200},
    {"CHK_MOTION_FRM", 201},
    {"GET_MOTION_FRM", 202},
    {"SET_MOTION_FRM", 203},
    {"GET_CHR_ID", 210},
    {"GET_COL_HIT_ID", 211},
    {"GET_SCRIPT_ID", 212},
    {"GET_MONSTOR_POS", 213},
    {"GET_MONSTOR_FRM", 214},
    {"SET_MONSTOR_POS", 215},
    {"SET_MONSTOR_MOVE", 216},
    {"SET_MONSTOR_LINK_MOVE", 217},
    {"SET_MONSTOR_MOVE_CANSEL", 218},
    {"SET_MONSTOR_MOTION", 219},
    {"SET_GLOBAL_INT", 220},
    {"GET_GLOBAL_INT", 221},
    {"GET_OBJ_POS", 222},
    {"SET_ROTATION_X", 223},
    {"SET_MOTION_CHANGE_STEP", 224},
    {"GET_MONSTOR_VECTOR", 225},
    {"SET_LOCKON_DIST", 226},
    {"SET_LOCKON_SW", 227},
    {"STOP_SND_NOW", 142},
    {"STATUS_SET_LIFE", 228},
    {"SET_BIN2", 209},
    {"SET_STATUS_CHANGE", 204},
    {"SET_TEX_ANIME_SW", 205},
    {"GET_STATUS_BIN2", 208},
    {"SET_COLLISION_WIDTH", 206},
    {"GET_NEAR_MONSTER", 207},
    {"BOSS_FADE_OUT", 240},
    {"CHEKC_FADE_OUT", 241},
    {"SET_GRAVITY", 242},
    {"SET_GUARD_FRAME", 244},
    {"GUARD_SEARCH", 245},
    {"GET_MOVE_VEC", 246},
    {"GET_USER_STATUS", 249},
    {"LOOKAT", 252},
    {"SET_SHADOW_FLAG", 253},
};

const MonsterNative *FindMonsterNative(const Json &which) {
    for (const MonsterNative &native : kMonsterNatives) {
        if (which.is_string() && which.get_ref<const std::string &>() == native.name) return &native;
        if (which.is_number_integer() && which.get<int>() == native.op) return &native;
    }
    return nullptr;
}

ApiCallResult ListMonsterNatives(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("monsters.natives takes no arguments");
    Json natives = Json::array();
    for (const MonsterNative &native : kMonsterNatives) natives.push_back({{"name", native.name}, {"op", native.op}});
    return {ApiCallStatus::Ok, {{"natives", std::move(natives)}}, {}};
}

// Runs one native as if the monster's own script had called it. Arguments are numbers (whole
// numbers go in as ints, others as floats), booleans, frame names (strings), or {out = "int"|"float"} for a value the
// native writes back; those come back in order in "out".
ApiCallResult CallMonsterNative(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("entity") ||
        !args[0].contains("native")) {
        return Invalid("expected {entity = monster handle, native = name or op, args = {...}?}");
    }
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "entity" && field.key() != "native" && field.key() != "args") {
            return Invalid("monsters.call only accepts entity, native and args");
        }
    }
    const MonsterNative *native = FindMonsterNative(request["native"]);
    if (native == nullptr) return Invalid("native is not a known monster native; see game.monsters.natives()");
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    if (!ReadId(request["entity"], handle) || !ReadMonsterHandle(handle, slot, generation)) {
        return Invalid("entity must be a generation-checked Chronicle monster handle");
    }
    NativeArgs native_args;
    if (std::string error = native_args.Read(request); !error.empty()) return Invalid(error);
    CMonstorUnit *unit = NowMonstorUnit;
    if (mode != GAME_MODE_DUNGEON || unit == nullptr) return Failed("monsters.call requires an active Chronicle dungeon");
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) {
        return Failed("the entity handle is stale because its monster slot was reused");
    }
    if ((unit->monster[slot].state != 1 && unit->monster[slot].state != 2) || unit->monster[slot].hp <= 0) {
        return Failed("monsters.call requires a living monster");
    }
    if (MonsterScriptExtension(native->op) == nullptr) return Failed("the monster natives are not set up yet");
    const int saved = unit->current_monster;
    unit->current_monster = slot;
    const int ok = MonsterScriptExtension(native->op)(native_args.stack.data(), native_args.count);
    unit->current_monster = saved;
    return {ApiCallStatus::Ok, {{"native", native->name}, {"ok", ok != 0}, {"out", native_args.Out()}}, {}};
}

ApiCallResult SetDungeonCamera(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].empty()) {
        return Invalid("expected {distance?, height?, angle?} with at least one camera field");
    }
    const Json &definition = args[0];
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "distance" && field.key() != "height" && field.key() != "angle") {
            return Invalid("render.camera_set only accepts distance, height, and angle");
        }
    }
    if (mode != GAME_MODE_DUNGEON || NowCamera__3 == nullptr) {
        return Failed("there is no active Chronicle dungeon camera");
    }
    CCameraFollow *const camera = NowCamera__3;
    if (camera->follow_on == 0) return Failed("the active Chronicle camera is not in follow mode");

    std::optional<float> distance;
    std::optional<float> height;
    std::optional<float> angle;
    const auto read_bounded = [&](std::string_view key, float minimum, float maximum,
                                  std::optional<float> &value) -> ApiCallResult {
        const auto found = definition.find(std::string(key));
        if (found == definition.end()) return {ApiCallStatus::Ok, nullptr, {}};
        if (!found->is_number()) return Invalid("camera fields must be finite numbers");
        const float parsed = found->get<float>();
        if (!std::isfinite(parsed) || parsed < minimum || parsed > maximum) {
            return Invalid("camera field '" + std::string(key) + "' is outside its safe range");
        }
        value = parsed;
        return {ApiCallStatus::Ok, nullptr, {}};
    };
    ApiCallResult parsed = read_bounded("distance", 8.0f, 300.0f, distance);
    if (parsed.status != ApiCallStatus::Ok) return parsed;
    parsed = read_bounded("height", -80.0f, 150.0f, height);
    if (parsed.status != ApiCallStatus::Ok) return parsed;
    parsed = read_bounded("angle", -1000.0f, 1000.0f, angle);
    if (parsed.status != ApiCallStatus::Ok) return parsed;
    const float current_distance = camera->GetDistance();
    const float current_height = camera->GetHeight();
    const float current_angle = camera->next_angle;
    if (!std::isfinite(current_distance) || !std::isfinite(current_height) || !std::isfinite(current_angle)) {
        return Failed("the native camera orbit contains a non-finite value");
    }
    if (distance) camera->SetDistance(*distance);
    if (height) camera->SetHeight(*height);
    if (angle) camera->SetAngle(static_cast<float>(std::remainder(*angle, 6.2831853071795864769)));
    return {ApiCallStatus::Ok,
            {{"scene", "dungeon"}, {"distance", camera->GetDistance()}, {"height", camera->GetHeight()},
             {"angle", camera->next_angle},
             {"previous", {{"distance", current_distance}, {"height", current_height}, {"angle", current_angle}}}},
            {}};
}

ApiCallResult SetTownCamera(std::string_view owner, const Json &args) {
    if (owner.empty() || !args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("enabled") || !args[0]["enabled"].is_boolean()) {
        return Invalid("expected {enabled = boolean, height = optional 40..500, back = optional -200..200}");
    }
    const Json &definition = args[0];
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "enabled" && field.key() != "height" && field.key() != "back") {
            return Invalid("render.town_camera_set received an unsupported field");
        }
    }
    const bool enabled = definition["enabled"].get<bool>();
    if (!enabled) {
        std::erase_if(g_town_camera_requests, [owner](const auto &entry) { return entry.first == owner; });
        return {ApiCallStatus::Ok, {{"enabled", false}}, {}};
    }

    auto read_offset = [&definition](const char *key, float fallback, float minimum, float maximum,
                                     float &result) -> ApiCallResult {
        const auto value = definition.find(key);
        if (value == definition.end()) { result = fallback; return {ApiCallStatus::Ok, nullptr, {}}; }
        if (!value->is_number()) return Invalid(std::string(key) + " must be a finite number");
        result = value->get<float>();
        if (!std::isfinite(result) || result < minimum || result > maximum) {
            return Invalid(std::string(key) + " is outside its safe range");
        }
        return {ApiCallStatus::Ok, nullptr, {}};
    };
    float height = 90.0f, back = 22.0f;
    ApiCallResult parsed = read_offset("height", 90.0f, 40.0f, 500.0f, height);
    if (parsed.status != ApiCallStatus::Ok) return parsed;
    parsed = read_offset("back", 22.0f, -200.0f, 200.0f, back);
    if (parsed.status != ApiCallStatus::Ok) return parsed;
    const auto existing = std::ranges::find(g_town_camera_requests, owner,
                                             [](const auto &entry) -> std::string_view { return entry.first; });
    if (existing == g_town_camera_requests.end() && g_town_camera_requests.size() >= 64) {
        return Failed("the native town camera already has the maximum number of owners");
    }
    std::erase_if(g_town_camera_requests, [owner](const auto &entry) { return entry.first == owner; });
    g_town_camera_requests.push_back({std::string(owner), {height, back}});
    return {ApiCallStatus::Ok, {{"enabled", true}, {"height", height}, {"back", back}}, {}};
}

constexpr std::array<std::pair<const char *, int>, 5> kTunableAilments = {{
    {"freeze", AILMENT_FREEZE}, {"stamina", AILMENT_STAMINA}, {"poison", AILMENT_POISON}, {"curse", AILMENT_CURSE}, {"goo", AILMENT_GOO}}};

Json StatusImmuneJson(int mask) {
    Json names = Json::array();
    for (const auto &[name, bit] : kTunableAilments) if (mask & bit) names.push_back(name);
    return names;
}

// The characters given their own refill, by id.
Json StaminaJson(const std::array<float, 16> &stamina) {
    Json out = Json::object();
    for (int c = 0; c < CharaCount() && c < static_cast<int>(stamina.size()); ++c) {
        if (stamina[c] > 0.0f) out[CharaIdName(c)] = stamina[c];
    }
    return out;
}

Json PlayerTuningJson(const ChroniclePlayerTuning &tuning) {
    return {{"run_speed", tuning.run_speed},
            {"attack_anim_speed", tuning.attack_anim_speed},
            {"invincible_bonus_frames", tuning.invincible_bonus_frames},
            {"recoil_scale", tuning.recoil_scale},
            {"gauge_refill", tuning.gauge_refill},
            {"thirst_drain", tuning.thirst_drain},
            {"durability_wear", tuning.durability_wear},
            {"map_reveal", tuning.map_reveal},
            {"monster_reveal", tuning.monster_reveal},
            {"map_hidden", tuning.map_hidden},
            {"map_window", tuning.map_window},
            {"status_duration", tuning.status_duration},
            {"status_immune", StatusImmuneJson(tuning.status_immune)},
            {"swap_delay", tuning.swap_delay},
            {"dodge_power", tuning.dodge_power},
            {"dodge_invincible_frames", tuning.dodge_invincible_frames},
            {"dodge_gauge", tuning.dodge_gauge},
            {"dodge_hop", tuning.dodge_hop},
            {"dodge_run", tuning.dodge_run},
            {"lightness", tuning.lightness},
            {"stamina", StaminaJson(tuning.stamina)},
            {"extra_shots", tuning.extra_shots},
            {"shots_pierce", tuning.shots_pierce},
            {"reach_scale", tuning.reach_scale},
            {"first_person", !tuning.no_first_person},
            {"item_heal_lock", tuning.item_heal_lock},
            {"lock_flick", tuning.lock_flick},
            {"lock_sight", tuning.lock_sight},
            {"lock_camera", tuning.lock_camera},
            {"soft_lock", tuning.soft_lock},
            {"input_buffer", tuning.input_buffer},
            {"chain_cancel", tuning.chain_cancel}};
}

// player.tuning_set: one owner-scoped request; fields left out mean retail for that owner.
ApiCallResult SetPlayerTuning(std::string_view owner, const Json &args) {
    if (owner.empty() || !args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected a tuning object: run_speed, attack_anim_speed, recoil_scale, gauge_refill, thirst_drain, "
                       "durability_wear, status_duration (0.25 to 4), swap_delay (0 to 4), invincible_bonus_frames (0 to 120), "
                       "map_reveal, monster_reveal, map_hidden, map_window, dodge_hop (booleans), status_immune (a list of ailment names), "
                       "dodge_power (0 or 0.25 to 4), dodge_invincible_frames (0 to 60), dodge_gauge (0 to 100), dodge_run (0 to 200), lightness (boolean), "
                       "stamina ({character id = gauge a second, 0 to 600}), extra_shots (0 to 2), shots_pierce (boolean), "
                       "reach_scale (0.25 to 4), first_person (boolean: false turns off R2's first-person view), "
                       "item_heal_lock (0 to 400: no HP-healing items within that distance of an enemy), lock_flick, lock_sight, lock_camera "
                       "(booleans), soft_lock (0 to 90 degrees), input_buffer (0 to 20 steps a refused roll or R2 is kept), "
                       "chain_cancel (0 to 30 steps after an attack's last blow when R2 or attack cuts into the next)");
    }
    const Json &definition = args[0];
    ChroniclePlayerTuning tuning;
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        const std::string &key = field.key();
        const Json &value = field.value();
        float *multiplier = nullptr;
        if (key == "run_speed") multiplier = &tuning.run_speed;
        else if (key == "attack_anim_speed") multiplier = &tuning.attack_anim_speed;
        else if (key == "recoil_scale") multiplier = &tuning.recoil_scale;
        else if (key == "gauge_refill") multiplier = &tuning.gauge_refill;
        else if (key == "thirst_drain") multiplier = &tuning.thirst_drain;
        else if (key == "durability_wear") multiplier = &tuning.durability_wear;
        else if (key == "status_duration") multiplier = &tuning.status_duration;
        else if (key == "reach_scale") multiplier = &tuning.reach_scale;
        if (multiplier != nullptr) {
            if (!value.is_number()) return Invalid(key + " must be a number from 0.25 to 4");
            const float scale = value.get<float>();
            if (!std::isfinite(scale) || scale < 0.25f || scale > 4.0f) return Invalid(key + " must be a number from 0.25 to 4");
            *multiplier = scale;
            continue;
        }
        if (key == "invincible_bonus_frames") {
            if (!value.is_number()) return Invalid("invincible_bonus_frames must be an integer from 0 to 120");
            const double frames = value.get<double>();
            if (!std::isfinite(frames) || frames != std::floor(frames) || frames < 0.0 || frames > 120.0) {
                return Invalid("invincible_bonus_frames must be an integer from 0 to 120");
            }
            tuning.invincible_bonus_frames = static_cast<int>(frames);
            continue;
        }
        if (key == "map_reveal" || key == "monster_reveal" || key == "map_hidden" || key == "map_window") {
            if (!value.is_boolean()) return Invalid(key + " must be a boolean");
            bool &flag = key == "map_reveal" ? tuning.map_reveal
                       : key == "monster_reveal" ? tuning.monster_reveal
                       : key == "map_hidden" ? tuning.map_hidden : tuning.map_window;
            flag = value.get<bool>();
            continue;
        }
        if (key == "dodge_power") {
            const float power = value.is_number() ? value.get<float>() : -1.0f;
            if (!std::isfinite(power) || (power != 0.0f && (power < 0.25f || power > 4.0f))) {
                return Invalid("dodge_power must be 0 or a number from 0.25 to 4");
            }
            tuning.dodge_power = power;
            continue;
        }
        if (key == "dodge_run") {
            const float drain = value.is_number() ? value.get<float>() : -1.0f;
            if (!std::isfinite(drain) || drain < 0.0f || drain > 200.0f) {
                return Invalid("dodge_run must be a number from 0 to 200 (gauge a second; 0 = no run)");
            }
            tuning.dodge_run = drain;
            continue;
        }
        if (key == "lightness") {
            if (!value.is_boolean()) return Invalid("lightness must be a boolean");
            tuning.lightness = value.get<bool>();
            continue;
        }
        if (key == "stamina") {
            if (!value.is_object()) return Invalid("stamina must be {character id = gauge a second, 0 to 600}");
            for (auto entry = value.begin(); entry != value.end(); ++entry) {
                int chara = -1;
                for (int c = 0; c < CharaCount() && c < static_cast<int>(tuning.stamina.size()); ++c) {
                    if (entry.key() == CharaIdName(c)) chara = c;
                }
                if (chara < 0) return Invalid("stamina names an unknown character: " + entry.key());
                const float rate = entry.value().is_number() ? entry.value().get<float>() : -1.0f;
                if (!std::isfinite(rate) || rate < 0.0f || rate > 600.0f) {
                    return Invalid("stamina." + entry.key() + " must be a number from 0 to 600 (gauge a second; 0 = the game's)");
                }
                tuning.stamina[chara] = rate;
            }
            continue;
        }
        if (key == "dodge_gauge") {
            int gauge = 0;
            if (!ReadBoundedInt(definition, "dodge_gauge", 0, 100, gauge)) return Invalid("dodge_gauge must be an integer from 0 to 100");
            tuning.dodge_gauge = gauge;
            continue;
        }
        if (key == "dodge_invincible_frames") {
            int frames = 0;
            if (!ReadBoundedInt(definition, "dodge_invincible_frames", 0, 60, frames)) {
                return Invalid("dodge_invincible_frames must be an integer from 0 to 60");
            }
            tuning.dodge_invincible_frames = frames;
            continue;
        }
        if (key == "lock_camera") {
            if (!value.is_boolean()) return Invalid("lock_camera must be a boolean");
            tuning.lock_camera = value.get<bool>();
            continue;
        }
        if (key == "lock_flick" || key == "lock_sight") {
            if (!value.is_boolean()) return Invalid(key + " must be a boolean");
            (key == "lock_flick" ? tuning.lock_flick : tuning.lock_sight) = value.get<bool>();
            continue;
        }
        if (key == "chain_cancel") {
            int steps = 0;
            if (!ReadBoundedInt(definition, "chain_cancel", 0, 30, steps)) return Invalid("chain_cancel must be an integer from 0 to 30 steps");
            tuning.chain_cancel = steps;
            continue;
        }
        if (key == "input_buffer") {
            int frames = 0;
            if (!ReadBoundedInt(definition, "input_buffer", 0, 20, frames)) return Invalid("input_buffer must be an integer from 0 to 20 steps");
            tuning.input_buffer = frames;
            continue;
        }
        if (key == "soft_lock") {
            int degrees = 0;
            if (!ReadBoundedInt(definition, "soft_lock", 0, 90, degrees)) return Invalid("soft_lock must be an integer from 0 to 90 degrees");
            tuning.soft_lock = degrees;
            continue;
        }
        if (key == "item_heal_lock") {
            int distance = 0;
            if (!ReadBoundedInt(definition, "item_heal_lock", 0, 400, distance)) return Invalid("item_heal_lock must be an integer from 0 to 400");
            tuning.item_heal_lock = distance;
            continue;
        }
        if (key == "extra_shots") {
            int shots = 0;
            if (!ReadBoundedInt(definition, "extra_shots", 0, 2, shots)) return Invalid("extra_shots must be an integer from 0 to 2");
            tuning.extra_shots = shots;
            continue;
        }
        if (key == "first_person") {
            if (!value.is_boolean()) return Invalid("first_person must be a boolean");
            tuning.no_first_person = !value.get<bool>();
            continue;
        }
        if (key == "shots_pierce") {
            if (!value.is_boolean()) return Invalid("shots_pierce must be a boolean");
            tuning.shots_pierce = value.get<bool>();
            continue;
        }
        if (key == "dodge_hop") {
            if (!value.is_boolean()) return Invalid("dodge_hop must be a boolean");
            tuning.dodge_hop = value.get<bool>();
            continue;
        }
        if (key == "swap_delay") {
            const float scale = value.is_number() ? value.get<float>() : -1.0f;
            if (!std::isfinite(scale) || scale < 0.0f || scale > 4.0f) return Invalid("swap_delay must be a number from 0 to 4");
            tuning.swap_delay = scale;
            continue;
        }
        if (key == "status_immune") {
            if (!value.is_array() || value.size() > kTunableAilments.size()) return Invalid("status_immune must be a list of ailment names");
            for (const Json &name : value) {
                const auto found = std::ranges::find_if(kTunableAilments, [&name](const auto &entry) {
                    return name.is_string() && name.get_ref<const std::string &>() == entry.first;
                });
                if (found == kTunableAilments.end()) return Invalid("status_immune names freeze, stamina, poison, curse or goo");
                tuning.status_immune |= found->second;
            }
            continue;
        }
        return Invalid("player.tuning_set received an unsupported field: " + key);
    }
    const auto existing = std::ranges::find(g_player_tuning_requests, owner,
                                             [](const auto &entry) -> std::string_view { return entry.first; });
    if (existing == g_player_tuning_requests.end() && g_player_tuning_requests.size() >= 64) {
        return Failed("player tuning already has the maximum number of owners");
    }
    std::erase_if(g_player_tuning_requests, [owner](const auto &entry) { return entry.first == owner; });
    g_player_tuning_requests.push_back({std::string(owner), tuning});
    RefreshPlayerTuning();
    return {ApiCallStatus::Ok,
            {{"owner", std::string(owner)}, {"status", "set"}, {"tuning", PlayerTuningJson(tuning)},
             {"effective", PlayerTuningJson(g_player_tuning)}},
            {}};
}

ApiCallResult ClearPlayerTuning(std::string_view owner, const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("player.tuning_clear takes no arguments");
    const bool cleared = std::erase_if(g_player_tuning_requests, [owner](const auto &entry) { return entry.first == owner; }) != 0;
    RefreshPlayerTuning();
    return {ApiCallStatus::Ok, {{"cleared", cleared}, {"effective", PlayerTuningJson(g_player_tuning)}}, {}};
}

// player.invincible: one-shot invincibility frames on the active player, never shortening a longer run.
std::array<int, CHARA_MAX> g_fatal_guard{}; // player.fatal_guard charges per character

// party.companion_set: the Gauntlet's companion. {character = id or nil, stance = passive|target|aggressive,
// damage_scale = 0..10}; the weapon is the character's equipped one.
std::string g_companion_owner;
ApiCallResult SetCompanion(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {character = optional id, stance = optional passive|target|aggressive, damage_scale = optional 0..10}");
    }
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "character" && field.key() != "stance" && field.key() != "damage_scale" && field.key() != "shot" &&
            field.key() != "hostile") {
            return Invalid("party.companion_set received an unsupported field: " + field.key());
        }
    }
    if (!g_companion_owner.empty() && g_companion_owner != owner && dc::game::MoonModCompanionLoaded() >= 0) {
        return Failed("another mod's companion is out");
    }
    int character = -1;
    if (request.contains("character") && !request["character"].is_null()) {
        const Json &who = request["character"];
        const auto found = who.is_string() ? std::ranges::find(kCharacterIds, who.get<std::string>()) : kCharacterIds.end();
        if (found == kCharacterIds.end()) return Invalid("character must be a playable character id");
        character = static_cast<int>(found - kCharacterIds.begin());
    }
    int stance = 1;
    if (request.contains("stance")) {
        const Json &value = request["stance"];
        if (value == "passive") stance = 0;
        else if (value == "target") stance = 1;
        else if (value == "aggressive") stance = 2;
        else return Invalid("stance must be passive, target or aggressive");
    }
    float scale = 1.0f;
    if (request.contains("damage_scale")) {
        if (!request["damage_scale"].is_number()) return Invalid("damage_scale must be a number from 0 to 10");
        scale = request["damage_scale"].get<float>();
        if (!std::isfinite(scale) || scale < 0.0f || scale > 10.0f) return Invalid("damage_scale must be a number from 0 to 10");
    }
    // shot: the companion fights from range, throwing this basefx01 cell at its target (Opar's glob)
    int shot_cell = -1;
    float shot_size = 1.0f;
    if (request.contains("shot")) {
        const Json &shot = request["shot"];
        if (!shot.is_object() || !ReadBoundedInt(shot, "cell", 0, 15, shot_cell)) return Invalid("shot is {cell = 0..15, size = 0.3..4}");
        if (shot.contains("size")) {
            if (!shot["size"].is_number()) return Invalid("shot is {cell = 0..15, size = 0.3..4}");
            shot_size = shot["size"].get<float>();
            if (!(shot_size >= 0.3f && shot_size <= 4.0f)) return Invalid("shot is {cell = 0..15, size = 0.3..4}");
        }
    }
    int weapon = -1;
    if (character >= 0 && UserStatus != nullptr) {
        const int slot = UserStatus->equipped_weapon_slot[character];
        if (slot >= 0 && slot < 11) weapon = UserStatus->chara_weapons[character][slot].item_no;
    }
    // hostile: an invader. {hp = 1..9999, damage = 1..999, defense = optional 0..999, tint = optional {r, g, b}}
    int   hostile_hp = 0, hostile_damage = 10, hostile_defense = 0;
    float tint[3] = {-1.0f, -1.0f, -1.0f};
    if (request.contains("hostile") && !request["hostile"].is_null()) {
        const Json &hostile = request["hostile"];
        const char *shape = "hostile is {hp = 1..9999, damage = 1..999, defense = optional 0..999, tint = optional {r, g, b}}";
        if (!hostile.is_object() || !ReadBoundedInt(hostile, "hp", 1, 9999, hostile_hp) ||
            !ReadBoundedInt(hostile, "damage", 1, 999, hostile_damage)) {
            return Invalid(shape);
        }
        if (hostile.contains("defense") && !ReadBoundedInt(hostile, "defense", 0, 999, hostile_defense)) return Invalid(shape);
        if (hostile.contains("tint")) {
            u32 rgb = 0;
            if (!ReadRgb(hostile["tint"], rgb)) return Invalid(shape);
            tint[0] = static_cast<float>((rgb >> 16) & 0xFF) / 2.0f; // the ambient runs 0..128
            tint[1] = static_cast<float>((rgb >> 8) & 0xFF) / 2.0f;
            tint[2] = static_cast<float>(rgb & 0xFF) / 2.0f;
        }
        if (character < 0) return Invalid("a hostile companion needs a character");
    }
    dc::game::MoonModCompanionHostile(character >= 0 ? hostile_hp : 0, hostile_damage, hostile_defense, tint);
    dc::game::MoonModCompanionRequest(character, weapon, stance, scale, shot_cell, shot_size);
    g_companion_owner = character >= 0 ? std::string(owner) : std::string();
    return {ApiCallStatus::Ok,
            {{"character", character >= 0 ? Json(kCharacterIds[character]) : Json(nullptr)},
             {"stance", stance == 0 ? "passive" : (stance == 1 ? "target" : "aggressive")}, {"damage_scale", scale}},
            {}};
}

// items.icon_set: a built-in item's menu icon from a mod's png (icons/<stem>.png), by owner; nil puts the native back.
std::map<int, std::string> g_builtin_icon_owners;
ApiCallResult SetBuiltinItemIcon(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("id")) {
        return Invalid("expected {id = built-in item id, icon = optional icon stem}");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "id" && field.key() != "icon") return Invalid("items.icon_set received an unsupported field: " + field.key());
    }
    const Json &id = args[0]["id"];
    int item_no = -1;
    if (id.is_number_integer()) item_no = id.get<int>();
    else if (id.is_string()) {
        const std::string &text = id.get_ref<const std::string &>();
        constexpr std::string_view prefix = "chronicle:item:";
        if (text.starts_with(prefix)) item_no = std::atoi(text.c_str() + prefix.size());
    }
    if (item_no < ITEM_DUNGEON_START || item_no >= ITEM_WEAPON_START) return Invalid("id must be a built-in Chronicle item");
    const auto taken = g_builtin_icon_owners.find(item_no);
    if (taken != g_builtin_icon_owners.end() && taken->second != owner) return Failed("another mod already set this item's icon");
    if (!args[0].contains("icon") || args[0]["icon"].is_null()) {
        SetItemIconOverride(item_no, "");
        g_builtin_icon_owners.erase(item_no);
        return {ApiCallStatus::Ok, {{"id", ItemStableId(item_no)}, {"icon", nullptr}}, {}};
    }
    const Json &icon = args[0]["icon"];
    if (!icon.is_string() || icon.get_ref<const std::string &>().empty() || icon.get_ref<const std::string &>().size() > 48 ||
        icon.get_ref<const std::string &>().find_first_of("/\\.") != std::string::npos) {
        return Invalid("icon must be the stem of icons/<stem>.png (1-48 bytes, no path or extension)");
    }
    const std::string game_path = "icons/" + icon.get<std::string>() + ".png";
    SetItemIconOverride(item_no, game_path.c_str());
    g_builtin_icon_owners[item_no] = std::string(owner);
    return {ApiCallStatus::Ok, {{"id", ItemStableId(item_no)}, {"icon", game_path}}, {}};
}

// dungeons.set_shortcuts: shortcut doors per floor, by owner (the most any owner asks for is used).
std::map<std::string, int, std::less<>> g_shortcut_requests;
std::map<std::string, int, std::less<>> g_shortcut_open; // owner -> the open keys' bits
std::map<std::string, std::pair<int, int>, std::less<>> g_illusory_requests; // owner -> (walls, open walls' bits)
void RefreshShortcutRequests() {
    int most = 0;
    for (const auto &[owner, count] : g_shortcut_requests) most = std::max(most, count);
    DngShortcutDoors = most;
    int open = 0;
    for (const auto &[owner, keys] : g_shortcut_open) open |= keys;
    DngShortcutOpenKeys = open;
    int walls = 0, open_walls = 0;
    for (const auto &[owner, request] : g_illusory_requests) {
        walls = std::max(walls, request.first);
        open_walls |= request.second;
    }
    DngIllusoryWalls = walls;
    DngIllusoryOpen = open_walls;
}
ApiCallResult SetShortcutDoors(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {count = 0..7, open?}");
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "count" && field.key() != "open" && field.key() != "illusory" && field.key() != "open_walls") {
            return Invalid("dungeons.set_shortcuts received an unsupported field: " + field.key());
        }
    }
    int count = 0;
    if (!ReadBoundedInt(args[0], "count", 0, DNG_SHORTCUT_MAX, count)) return Invalid("count must be an integer from 0 to 7");
    // open: the key items (216-222) of doors a floor built again from its seed builds open, as zone gates' open does.
    int open = 0;
    if (args[0].contains("open")) {
        const Json &keys = args[0]["open"];
        if (!keys.is_array() || keys.size() > DNG_SHORTCUT_MAX) return Invalid("open must be a list of up to 7 key items (216-222)");
        for (const Json &key : keys) {
            if (!key.is_number_integer() || key.get<int>() < ITEM_BONE_KEY || key.get<int>() >= ITEM_BONE_KEY + DNG_SHORTCUT_MAX) {
                return Invalid("open must be a list of up to 7 key items (216-222)");
            }
            open |= 1 << (key.get<int>() - ITEM_BONE_KEY);
        }
    }
    // illusory: up to 3 more doorways drawn as plain wall, opened by a blow; open_walls: their numbers (0-6) built open.
    int illusory = 0, open_walls = 0;
    if (args[0].contains("illusory") && !ReadBoundedInt(args[0], "illusory", 0, 3, illusory)) {
        return Invalid("illusory must be an integer from 0 to 3");
    }
    if (args[0].contains("open_walls")) {
        const Json &walls = args[0]["open_walls"];
        if (!walls.is_array() || walls.size() > DNG_SHORTCUT_MAX) return Invalid("open_walls must be a list of up to 7 wall numbers (0-6)");
        for (const Json &wall : walls) {
            if (!wall.is_number_integer() || wall.get<int>() < 0 || wall.get<int>() >= DNG_SHORTCUT_MAX) {
                return Invalid("open_walls must be a list of up to 7 wall numbers (0-6)");
            }
            open_walls |= 1 << wall.get<int>();
        }
    }
    if (count == 0) {
        g_shortcut_requests.erase(std::string(owner));
        g_shortcut_open.erase(std::string(owner));
    } else {
        g_shortcut_requests[std::string(owner)] = count;
        g_shortcut_open[std::string(owner)] = open;
    }
    if (illusory == 0) g_illusory_requests.erase(std::string(owner));
    else g_illusory_requests[std::string(owner)] = {illusory, open_walls};
    RefreshShortcutRequests();
    return {ApiCallStatus::Ok, {{"count", count}, {"effective", DngShortcutDoors}, {"illusory", DngIllusoryWalls}}, {}};
}

// ui.choice_open: a choice panel that pauses the dungeon until the player picks (moonmod/choice_panel.hpp).
ApiCallResult OpenChoicePanel(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id, title, subtitle?, cancel?, cursor?, town?, options = {{title, subtitle?, lines?}, ...}}");
    }
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        const std::string &key = field.key();
        if (key != "id" && key != "title" && key != "subtitle" && key != "cancel" && key != "cursor" && key != "town" &&
            key != "options") {
            return Invalid("ui.choice_open received an unsupported field: " + key);
        }
    }
    const auto text = [&](const Json &object, const char *key, std::size_t most, bool required, std::string &out) {
        if (!object.contains(key)) return !required;
        if (!object[key].is_string()) return false;
        out = object[key].get<std::string>();
        return out.size() <= most && (!required || !out.empty());
    };
    dc::game::MoonModChoice choice;
    choice.owner = std::string(owner);
    if (!text(request, "id", 48, true, choice.id)) return Invalid("id must be a string of 1-48 bytes");
    if (!text(request, "title", 64, true, choice.title)) return Invalid("title must be a string of 1-64 bytes");
    if (!text(request, "subtitle", 96, false, choice.subtitle)) return Invalid("subtitle must be a string of up to 96 bytes");
    if (request.contains("cancel")) {
        if (!request["cancel"].is_boolean()) return Invalid("cancel must be a boolean");
        choice.cancel = request["cancel"].get<bool>();
    }
    if (!request.contains("options") || !request["options"].is_array() || request["options"].empty() || request["options"].size() > 6) {
        return Invalid("options must list 1-6 options");
    }
    for (const Json &entry : request["options"]) {
        if (!entry.is_object()) return Invalid("each option must be {title, subtitle?, lines?}");
        dc::game::MoonModChoiceOption option;
        if (!text(entry, "title", 48, true, option.title)) return Invalid("an option's title must be a string of 1-48 bytes");
        if (!text(entry, "subtitle", 64, false, option.subtitle)) return Invalid("an option's subtitle must be up to 64 bytes");
        if (entry.contains("lines")) {
            if (!entry["lines"].is_array() || entry["lines"].size() > 10) return Invalid("an option's lines must list up to 10 lines");
            for (const Json &line : entry["lines"]) {
                dc::game::MoonModChoiceLine out;
                if (line.is_string()) {
                    out.text = line.get<std::string>();
                } else if (line.is_object() && line.contains("text") && line["text"].is_string()) {
                    out.text = line["text"].get<std::string>();
                    const std::string tone = line.value("tone", std::string());
                    static constexpr std::array<std::string_view, 5> kTones = {"", "good", "bad", "dim", "gold"};
                    const auto found = std::ranges::find(kTones, tone);
                    if (found == kTones.end()) return Invalid("a line's tone is good, bad, dim or gold");
                    out.tone = static_cast<int>(found - kTones.begin());
                } else {
                    return Invalid("a line is a string or {text, tone?}");
                }
                if (out.text.size() > 96) return Invalid("a line is up to 96 bytes");
                option.lines.push_back(std::move(out));
            }
        }
        choice.options.push_back(std::move(option));
    }
    if (request.contains("cursor")) {
        int cursor = 1;
        if (!ReadBoundedInt(request, "cursor", 1, static_cast<int>(choice.options.size()), cursor)) return Invalid("cursor is an option's number from 1");
        choice.cursor = cursor - 1;
    }
    // a town's walk only when the mod asks for it ({town = true}): a mod that retries until its panel opens keeps waiting
    // for the dungeon, as before
    if (request.contains("town") && !request["town"].is_boolean()) return Invalid("town must be a boolean");
    const bool town_ok = request.value("town", false);
    const bool town_walk = town_ok && mode == GAME_MODE_EDIT && GameMode == ED_MODE_WALK && IsSettlementOrRegisteredLocationMap(MapNo);
    if (mode != GAME_MODE_DUNGEON && !town_walk) {
        return Failed(town_ok ? "a choice panel opens during dungeon play or a town walk" : "a choice panel opens during dungeon play");
    }
    const std::string id = choice.id;
    if (!dc::game::MoonModChoiceOpen(std::move(choice))) return Failed("a choice panel is already open");
    return {ApiCallStatus::Ok, {{"id", id}}, {}};
}

// A floor's map as a mod keeps it (dungeons.map_snapshot): each cell a letter, `a` for nothing (or not seen yet) and
// `b` on for 32 + n of DngMapCellShapes, a run of one letter written once with its count after it.
constexpr std::string_view kMapLetters = "abcdefghijklmnopqrstuvwxyzABCDEFG";

int MapLetter(const unsigned char *shapes, const unsigned char *seen, int cell) {
    return seen[cell] != 0 && shapes[cell] >= 32 ? shapes[cell] - 31 : 0;
}

std::string EncodeMapCells(const unsigned char *shapes, const unsigned char *seen) {
    std::string out;
    for (int cell = 0; cell < DNG_CELLS;) {
        const int value = MapLetter(shapes, seen, cell);
        int run = 1;
        while (cell + run < DNG_CELLS && MapLetter(shapes, seen, cell + run) == value) ++run;
        out += kMapLetters[value];
        if (run > 1) out += std::to_string(run);
        cell += run;
    }
    return out;
}

// The shapes of a kept map, or false when it is not one.
bool DecodeMapCells(std::string_view text, std::vector<unsigned char> &shapes) {
    shapes.assign(DNG_CELLS, 0);
    int cell = 0;
    for (std::size_t i = 0; i < text.size();) {
        const std::size_t letter = kMapLetters.find(text[i++]);
        if (letter == std::string_view::npos) return false;
        int run = 0;
        while (i < text.size() && text[i] >= '0' && text[i] <= '9') {
            run = run * 10 + (text[i++] - '0');
            if (run > DNG_CELLS) return false;
        }
        if (run == 0) run = 1;
        if (cell + run > DNG_CELLS) return false;
        std::fill_n(shapes.begin() + cell, run, static_cast<unsigned char>(letter == 0 ? 0 : letter + 31));
        cell += run;
    }
    return cell == DNG_CELLS;
}

bool LiveFloor() { return mode == GAME_MODE_DUNGEON && NowDngMap == &MainDungeonMap && MainDungeonMap.map_type == 1; }

// dungeons.map_snapshot: the floor the player is on as far as they have seen it, for a mod to keep and show later.
ApiCallResult MapSnapshot() {
    if (!LiveFloor()) return Failed("the player is not on a dungeon floor");
    std::vector<unsigned char> shapes(DNG_CELLS), seen(DNG_CELLS);
    dc::game::DngMapCellShapes(MainDungeonMap, shapes.data());
    int count = 0;
    for (int cell = 0; cell < DNG_CELLS; ++cell) {
        seen[cell] = MainDungeonMap.mask[cell] != 0 ? 1 : 0;
        count += seen[cell] != 0 && shapes[cell] != 0;
    }
    return {ApiCallStatus::Ok,
            {{"seed", MainDungeonMap.map_seed}, {"width", DNG_GRID}, {"cells", EncodeMapCells(shapes.data(), seen.data())}, {"seen", count}},
            {}};
}

// dungeons.set_seen: the cells of a kept map counted as seen again on the floor the player is on (when it was built
// from that seed), so the mini map and the map screen show them.
ApiCallResult SetSeen(const Json &args) {
    const std::string usage = "expected {seed, cells} as dungeons.map_snapshot gave them";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
    const Json &request = args[0];
    if (!request.contains("seed") || !request["seed"].is_number_integer() || !request.contains("cells") || !request["cells"].is_string()) {
        return Invalid(usage);
    }
    std::vector<unsigned char> shapes;
    if (!DecodeMapCells(request["cells"].get<std::string>(), shapes)) return Invalid("cells is not a map dungeons.map_snapshot gave");
    if (!LiveFloor()) return Failed("the player is not on a dungeon floor");
    if (request["seed"].get<long long>() != MainDungeonMap.map_seed) return {ApiCallStatus::Ok, {{"applied", false}}, {}};
    int count = 0;
    for (int cell = 0; cell < DNG_CELLS; ++cell) {
        if (shapes[cell] == 0 || MainDungeonMap.cells[cell].parts_no == MAP_PARTS_NONE) continue;
        MainDungeonMap.mask[cell] = 1;
        ++count;
    }
    return {ApiCallStatus::Ok, {{"applied", true}, {"cells", count}}, {}};
}

// dungeons.set_waypoint: the red flag on the mini map, on the floor the player is on; {} takes it away.
ApiCallResult SetWaypoint(const Json &args) {
    const std::string usage = "expected {x = column, y = row}, or {}";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
    if (args[0].empty()) {
        DngWaypointX = DngWaypointY = DngWaypointSeed = -1;
        return {ApiCallStatus::Ok, {{"waypoint", false}}, {}};
    }
    int x = 0, y = 0;
    if (args[0].size() != 2 || !ReadBoundedInt(args[0], "x", 0, DNG_GRID - 1, x) || !ReadBoundedInt(args[0], "y", 0, DNG_GRID - 1, y)) {
        return Invalid(usage);
    }
    if (!LiveFloor()) return Failed("the player is not on a dungeon floor");
    DngWaypointX = x;
    DngWaypointY = y;
    DngWaypointSeed = MainDungeonMap.map_seed;
    return {ApiCallStatus::Ok, {{"waypoint", true}, {"x", x}, {"y", y}}, {}};
}

// ui.sky_open: a node sky over the paused dungeon (moonmod/sky_screen.hpp), read as title.mode_set reads its sky.
ApiCallResult OpenSkyScreen(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {id, title, hint?, sky}");
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        const std::string &key = field.key();
        if (key != "id" && key != "title" && key != "hint" && key != "sky") return Invalid("ui.sky_open received an unsupported field: " + key);
    }
    if (!request.contains("id") || !request["id"].is_string() || request["id"].get<std::string>().empty() ||
        request["id"].get<std::string>().size() > 64) {
        return Invalid("id must be 1-64 bytes");
    }
    if (!request.contains("title") || !request["title"].is_string() || request["title"].get<std::string>().empty() ||
        request["title"].get<std::string>().size() > 32) {
        return Invalid("title must be 1-32 bytes");
    }
    if (request.contains("hint") && (!request["hint"].is_string() || request["hint"].get<std::string>().size() > 160)) {
        return Invalid("hint must be at most 160 bytes");
    }
    if (!request.contains("sky")) return Invalid("expected a sky");
    if (mode != GAME_MODE_DUNGEON) return Failed("a sky page opens during dungeon play");
    moonmod::TitleMode page;
    page.owner = std::string(owner);
    page.local_id = request["id"].get<std::string>();
    page.id = page.owner + ":" + page.local_id;
    page.title = request["title"].get<std::string>();
    if (request.contains("hint")) page.hint = request["hint"].get<std::string>();
    const moonmod::TitleMode *open = dc::game::MoonModSkyPage();
    std::optional<moonmod::TitleSky> previous;
    if (open != nullptr && open->owner == owner && open->local_id == page.local_id) previous = open->sky;
    if (ApiCallResult read = moonmod::ReadTitleSky(request["sky"], previous, page.sky); read.status != ApiCallStatus::Ok) return read;
    const std::size_t nodes = page.sky ? page.sky->nodes.size() : 0;
    if (!dc::game::MoonModSkyOpen(std::move(page))) return Failed("another mod's sky page is open");
    return {ApiCallStatus::Ok, {{"id", request["id"]}, {"nodes", nodes}}, {}};
}

// ui.map_open: the map screen (moonmod/map_screen.hpp), of the floor the player is on or of one the mod kept.
ApiCallResult OpenMapScreen(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id, title, subtitle?, cells?, marks?, cursor?, waypoints?, hint?, pages?}");
    }
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        const std::string &key = field.key();
        if (key != "id" && key != "title" && key != "subtitle" && key != "cells" && key != "marks" && key != "cursor" && key != "waypoints" &&
            key != "hint" && key != "pages") {
            return Invalid("ui.map_open received an unsupported field: " + key);
        }
    }
    const auto text = [&](const Json &object, const char *key, std::size_t most, bool required, std::string &out) {
        if (!object.contains(key)) return !required;
        if (!object[key].is_string()) return false;
        out = object[key].get<std::string>();
        return out.size() <= most && (!required || !out.empty());
    };
    dc::game::MoonModMapView view;
    view.owner = std::string(owner);
    if (!text(request, "id", 48, true, view.id)) return Invalid("id must be a string of 1-48 bytes");
    if (!text(request, "title", 64, true, view.title)) return Invalid("title must be a string of 1-64 bytes");
    if (!text(request, "subtitle", 96, false, view.subtitle)) return Invalid("subtitle must be a string of up to 96 bytes");
    if (!text(request, "hint", 24, false, view.hint)) return Invalid("hint must be a string of up to 24 bytes");
    if (request.contains("waypoints")) {
        if (!request["waypoints"].is_boolean()) return Invalid("waypoints must be a boolean");
        view.waypoints = request["waypoints"].get<bool>();
    }
    if (request.contains("pages")) {
        if (!request["pages"].is_boolean()) return Invalid("pages must be a boolean");
        view.pages = request["pages"].get<bool>();
    }
    if (request.contains("cells")) {
        if (!request["cells"].is_string() || !DecodeMapCells(request["cells"].get<std::string>(), view.shapes)) {
            return Invalid("cells is a map dungeons.map_snapshot gave");
        }
        view.live = false;
        view.seen.assign(DNG_CELLS, 1);
    }
    static constexpr std::array<std::string_view, 6> kKinds = {"shrine", "shrine_lit", "waypoint", "gate", "stair", "mark"};
    // An empty Lua table arrives as an empty object: no marks.
    if (request.contains("marks") && !(request["marks"].is_object() && request["marks"].empty())) {
        if (!request["marks"].is_array() || request["marks"].size() > 64) return Invalid("marks must list up to 64 marks");
        for (const Json &entry : request["marks"]) {
            if (!entry.is_object()) return Invalid("a mark is {x, y, kind?, label?, pick?}");
            dc::game::MoonModMapMark mark;
            if (!ReadBoundedInt(entry, "x", 0, DNG_GRID - 1, mark.x) || !ReadBoundedInt(entry, "y", 0, DNG_GRID - 1, mark.y)) {
                return Invalid("a mark's x and y are a cell of the map");
            }
            const std::string kind = entry.value("kind", std::string("mark"));
            const auto        found = std::ranges::find(kKinds, kind);
            if (found == kKinds.end()) return Invalid("a mark's kind is shrine, shrine_lit, waypoint, gate, stair or mark");
            mark.kind = static_cast<int>(found - kKinds.begin());
            if (!text(entry, "label", 48, false, mark.label)) return Invalid("a mark's label is up to 48 bytes");
            if (entry.contains("pick")) {
                if (!entry["pick"].is_boolean()) return Invalid("a mark's pick must be a boolean");
                mark.pick = entry["pick"].get<bool>();
            }
            view.marks.push_back(std::move(mark));
        }
    }
    if (request.contains("cursor")) {
        const Json &cursor = request["cursor"];
        if (!cursor.is_object() || !ReadBoundedInt(cursor, "x", 0, DNG_GRID - 1, view.cursor_x) ||
            !ReadBoundedInt(cursor, "y", 0, DNG_GRID - 1, view.cursor_y)) {
            return Invalid("cursor is {x, y}, a cell of the map");
        }
    }
    if (view.live && !LiveFloor()) return Failed("the map of the floor the player is on opens during dungeon play (or pass cells)");
    if (mode != GAME_MODE_DUNGEON && mode != GAME_MODE_EDIT) return Failed("a map opens in a dungeon or a town");
    if (dc::game::MoonModChoiceUp() || dc::game::MoonModMenuUp()) return Failed("a panel is already open");
    const std::string id = view.id;
    if (!dc::game::MoonModMapOpen(std::move(view))) return Failed("another mod's map is open");
    return {ApiCallStatus::Ok, {{"id", id}}, {}};
}

// ui.run_menu_set: a mod's run menu, opened from Start in a dungeon (moonmod/run_menu.hpp).
ApiCallResult SetRunMenu(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {title, tabs = {{id, title, lines?, rows?}, ...}}");
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "title" && field.key() != "tabs") return Invalid("ui.run_menu_set received an unsupported field: " + field.key());
    }
    const auto text = [](const Json &object, const char *key, std::size_t most, bool required, std::string &out) {
        if (!object.contains(key)) return !required;
        if (!object[key].is_string()) return false;
        out = object[key].get<std::string>();
        return out.size() <= most && (!required || !out.empty());
    };
    dc::game::MoonModMenu menu;
    menu.owner = std::string(owner);
    if (!text(request, "title", 48, true, menu.title)) return Invalid("title must be a string of 1-48 bytes");
    if (!request.contains("tabs") || !request["tabs"].is_array() || request["tabs"].empty() || request["tabs"].size() > 6) {
        return Invalid("tabs must list 1-6 tabs");
    }
    static constexpr std::array<std::string_view, 5> kTones = {"", "good", "bad", "dim", "gold"};
    for (const Json &entry : request["tabs"]) {
        if (!entry.is_object()) return Invalid("each tab is {id, title, lines?, rows?}");
        dc::game::MoonModMenuTab tab;
        if (!text(entry, "id", 32, true, tab.id)) return Invalid("a tab's id must be a string of 1-32 bytes");
        if (!text(entry, "title", 24, true, tab.title)) return Invalid("a tab's title must be a string of 1-24 bytes");
        // An empty Lua table arrives as an empty object: no lines, no rows.
        const auto empty = [&](const char *key) { return entry.contains(key) && entry[key].is_object() && entry[key].empty(); };
        if (entry.contains("lines") && !empty("lines")) {
            if (!entry["lines"].is_array() || entry["lines"].size() > 8) return Invalid("a tab's lines must list up to 8 strings");
            for (const Json &line : entry["lines"]) {
                if (!line.is_string() || line.get_ref<const std::string &>().size() > 96) return Invalid("a tab line is a string of up to 96 bytes");
                tab.lines.push_back(line.get<std::string>());
            }
        }
        if (entry.contains("rows") && !empty("rows")) {
            if (!entry["rows"].is_array() || entry["rows"].size() > 64) return Invalid("a tab's rows must list up to 64 rows");
            for (const Json &item : entry["rows"]) {
                if (!item.is_object()) return Invalid("a row is {id, label, value?, detail?, tone?, enabled?}");
                dc::game::MoonModMenuRow row;
                if (!text(item, "id", 32, true, row.id)) return Invalid("a row's id must be a string of 1-32 bytes");
                if (!text(item, "label", 48, true, row.label)) return Invalid("a row's label must be a string of 1-48 bytes");
                if (!text(item, "value", 24, false, row.value)) return Invalid("a row's value is up to 24 bytes");
                if (!text(item, "detail", 240, false, row.detail)) return Invalid("a row's detail is up to 240 bytes");
                if (item.contains("tone")) {
                    const auto found = item["tone"].is_string() ? std::ranges::find(kTones, item["tone"].get<std::string>()) : kTones.end();
                    if (found == kTones.end()) return Invalid("a row's tone is good, bad, dim or gold");
                    row.tone = static_cast<int>(found - kTones.begin());
                }
                if (item.contains("enabled")) {
                    if (!item["enabled"].is_boolean()) return Invalid("a row's enabled is a boolean");
                    row.enabled = item["enabled"].get<bool>();
                }
                tab.rows.push_back(std::move(row));
            }
        }
        menu.tabs.push_back(std::move(tab));
    }
    if (!dc::game::MoonModMenuSet(std::move(menu))) return Failed("another mod has the run menu");
    return {ApiCallStatus::Ok, {{"set", true}}, {}};
}

// dungeons.set_backrooms: the back floor's stairs ask for no key; a panel asks whether to go down instead.
std::string g_backroom_owner, g_backroom_title, g_backroom_text;
// set_backrooms stay: the panel's answer is only reported (the owner moves the run on itself); lines: what Proceed says.
bool        g_backroom_stay = false;
std::vector<std::string> g_backroom_lines;
int         g_backroom_key = 0, g_backroom_key_count = 0, g_backroom_key_frames = 0;
// Set when the player chose to go down from the entrance's panel; the next frame of play starts the swap, and once
// below the party is stood at the back floor's own way in (the entrance's script, which would place it, never runs).
bool        g_backroom_descend = false, g_backroom_place = false;
ApiCallResult SetBackrooms(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {free = boolean, title?, text?}");
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "free" && field.key() != "title" && field.key() != "text" && field.key() != "stay" && field.key() != "lines") {
            return Invalid("dungeons.set_backrooms received an unsupported field: " + field.key());
        }
    }
    if (!g_backroom_owner.empty() && g_backroom_owner != owner) return Failed("another mod already sets the back floors");
    if (!args[0].contains("free") || !args[0]["free"].is_boolean()) return Invalid("free must be a boolean");
    const std::string title = args[0].value("title", std::string("ENTER THE BACKROOM?"));
    const std::string text = args[0].value("text", std::string("Enter at your own risk."));
    if (title.empty() || title.size() > 64 || text.size() > 96) return Invalid("title is 1-64 bytes, text up to 96");
    if (args[0].contains("stay") && !args[0]["stay"].is_boolean()) return Invalid("stay must be a boolean");
    std::vector<std::string> lines;
    if (args[0].contains("lines")) {
        const Json &list = args[0]["lines"];
        if (!list.is_array() || list.size() > 2) return Invalid("lines lists up to two strings");
        for (const Json &line : list) {
            if (!line.is_string() || line.get<std::string>().empty() || line.get<std::string>().size() > 64) return Invalid("each line is 1-64 bytes");
            lines.push_back(line.get<std::string>());
        }
    }
    if (args[0]["free"].get<bool>()) {
        g_backroom_owner = std::string(owner);
        g_backroom_title = title;
        g_backroom_text = text;
        g_backroom_stay = args[0].value("stay", false);
        g_backroom_lines = std::move(lines);
    } else {
        g_backroom_owner.clear();
    }
    return {ApiCallStatus::Ok, {{"free", !g_backroom_owner.empty()}, {"stay", !g_backroom_owner.empty() && g_backroom_stay}}, {}};
}

// dungeons.set_story_exits: on a floor of the game's own design (not generated), pressing at the gate that wants the
// floor's key asks the owner's question instead (game.dungeon.story_exit); the gate stays shut and the owner takes the
// player on.
std::string g_story_exit_owner, g_story_exit_title, g_story_exit_text;
ApiCallResult SetStoryExits(std::string_view owner, const Json &args) {
    const std::string usage = "expected {title?, text?} to ask at a fixed floor's gate, or {} to take it back";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "title" && field.key() != "text") return Invalid("dungeons.set_story_exits received an unsupported field: " + field.key());
    }
    if (!g_story_exit_owner.empty() && g_story_exit_owner != owner) return Failed("another mod already sets the story exits");
    if (args[0].empty()) {
        g_story_exit_owner.clear();
        return {ApiCallStatus::Ok, {{"set", false}}, {}};
    }
    if ((args[0].contains("title") && !args[0]["title"].is_string()) || (args[0].contains("text") && !args[0]["text"].is_string())) return Invalid(usage);
    const std::string title = args[0].value("title", std::string("LEAVE THIS FLOOR?"));
    const std::string text = args[0].value("text", std::string("The way on opens."));
    if (title.empty() || title.size() > 64 || text.size() > 96) return Invalid("title is 1-64 bytes, text up to 96");
    g_story_exit_owner = std::string(owner);
    g_story_exit_title = title;
    g_story_exit_text = text;
    return {ApiCallStatus::Ok, {{"set", true}}, {}};
}

// dungeons.set_npc_models: character packs a dungeon's map loads into free NPC slots (from its next map load).
std::string g_npc_models_owner;
ApiCallResult SetNpcModels(std::string_view owner, const Json &args) {
    const std::string usage = "expected {models = {{dungeon_id = n, npc = 1-3, pack = \"dun/.../<stem>.chr\" or \"gedit/.../<stem>.chr\", "
                              "cfg = \"<name>.cfg\", motion = n}, ...}} (up to 3), or {}";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
    if (!g_npc_models_owner.empty() && g_npc_models_owner != owner) return Failed("another mod already loads the dungeons' NPC models");
    const Json &request = args[0];
    DngNpcModel models[DNG_NPC_MODEL_MAX] = {};
    int count = 0;
    if (!request.empty()) {
        if (request.size() != 1 || !request.contains("models") || !request["models"].is_array() ||
            request["models"].size() > DNG_NPC_MODEL_MAX) {
            return Invalid(usage);
        }
        for (const Json &model : request["models"]) {
            DngNpcModel m{};
            if (!model.is_object() || !ReadBoundedInt(model, "dungeon_id", 0, 6, m.dungeon) || !ReadBoundedInt(model, "npc", 1, 3, m.npc) ||
                !model.contains("pack") || !model["pack"].is_string()) {
                return Invalid(usage);
            }
            const auto name_char = [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; };
            const auto is_name = [&](std::string_view s) { return !s.empty() && s.size() <= 12 && std::ranges::all_of(s, name_char); };
            // a game pack: dun/... or gedit/..., folders of names, a <stem>.chr
            const std::string pack = model["pack"].get<std::string>();
            if ((!pack.starts_with("dun/") && !pack.starts_with("gedit/")) || !pack.ends_with(".chr") || pack.size() >= sizeof(m.pack)) {
                return Invalid(usage);
            }
            const std::string_view path(pack.data(), pack.size() - 4);
            for (size_t at = 0; at <= path.size();) {
                const size_t slash = std::min(path.find('/', at), path.size());
                if (!is_name(path.substr(at, slash - at))) return Invalid(usage);
                at = slash + 1;
            }
            const std::string_view stem = std::string_view(pack).substr(pack.rfind('/') + 1);
            std::copy(pack.begin(), pack.end(), m.pack);
            std::string cfg = pack.starts_with("dun/mainchara/") ? "base.cfg" : std::string(stem.substr(0, stem.size() - 4)) + ".cfg";
            if (model.contains("cfg")) {
                if (!model["cfg"].is_string()) return Invalid(usage);
                cfg = model["cfg"].get<std::string>();
                if (!cfg.ends_with(".cfg") || !is_name(std::string_view(cfg).substr(0, cfg.size() - 4))) return Invalid(usage);
            }
            if (cfg.size() >= sizeof(m.cfg)) return Invalid(usage);
            std::copy(cfg.begin(), cfg.end(), m.cfg);
            m.motion = 0;
            if (model.contains("motion") && !ReadBoundedInt(model, "motion", -1, 255, m.motion)) return Invalid(usage);
            for (int i = 0; i < count; i++) {
                if (models[i].dungeon == m.dungeon && models[i].npc == m.npc) return Invalid("two models for one NPC slot");
            }
            models[count++] = m;
        }
    }
    std::copy(models, models + count, DngNpcModels);
    DngNpcModelCount = count;
    g_npc_models_owner = count > 0 ? std::string(owner) : std::string();
    return {ApiCallStatus::Ok, {{"models", count}}, {}};
}

// dungeons.set_npc_marks: stands the floor's own NPCs (loaded with its parts) at cells of the floor the player is on.
std::string g_npc_marks_owner;
ApiCallResult SetNpcMarks(std::string_view owner, const Json &args) {
    const std::string usage = "expected {marks = {{npc = 0, x = column, y = row, facing = 0-3}, ...}} (up to 16), or {}";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
    if (!g_npc_marks_owner.empty() && g_npc_marks_owner != owner) return Failed("another mod already stands the floor's NPCs");
    const Json &request = args[0];
    if (request.empty()) {
        g_npc_marks_owner.clear();
        DngNpcMarkCount = 0;
        return {ApiCallStatus::Ok, {{"marks", 0}}, {}};
    }
    if (request.size() != 1 || !request.contains("marks") || !request["marks"].is_array() || request["marks"].size() > DNG_NPC_MARK_MAX) {
        return Invalid(usage);
    }
    DngNpcMark marks[DNG_NPC_MARK_MAX] = {};
    int count = 0;
    for (const Json &mark : request["marks"]) {
        DngNpcMark m{};
        if (!mark.is_object() || !ReadBoundedInt(mark, "npc", 0, 3, m.npc) || !ReadBoundedInt(mark, "x", 0, DNG_GRID - 1, m.x) ||
            !ReadBoundedInt(mark, "y", 0, DNG_GRID - 1, m.y)) {
            return Invalid(usage);
        }
        m.facing = 0;
        if (mark.contains("facing") && !ReadBoundedInt(mark, "facing", 0, 3, m.facing)) return Invalid(usage);
        marks[count++] = m;
    }
    if (mode != GAME_MODE_DUNGEON || NowDngMap != &MainDungeonMap) return Failed("not on a dungeon floor");
    for (int i = 0; i < count; i++) {
        if (MainDungeonMap.npc[marks[i].npc].used == 0 || MainDungeonMap.npc[marks[i].npc].chara.frame == nullptr) {
            return Failed("this floor has no NPC " + std::to_string(marks[i].npc));
        }
    }
    std::copy(marks, marks + count, DngNpcMarks);
    DngNpcMarkCount = count;
    DngNpcMarkSeed = MainDungeonMap.map_seed;
    g_npc_marks_owner = count > 0 ? std::string(owner) : std::string();
    return {ApiCallStatus::Ok, {{"marks", count}}, {}};
}

// dungeons.play_event: starts a program of the dungeon's event script (a scene) on the floor the player is on, the way a
// monster's script asks for one; game.dungeon.event_ended tells the mod when it is over (the floor's monsters are gone
// after a scene, so the mod builds the floor again).
std::string g_play_event_owner;
int         g_play_event_program = -1;
ApiCallResult PlayEvent(std::string_view owner, const Json &args) {
    const std::string usage = "expected {program = n}";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1) return Invalid(usage);
    int program = -1;
    if (!ReadBoundedInt(args[0], "program", 0, 9999, program)) return Invalid(usage);
    if (mode != GAME_MODE_DUNGEON || NowDngMap != &MainDungeonMap || UserStatus == nullptr || NowMonstorUnit == nullptr ||
        BtUraDongeon != 0 || dc::game::MoonModChoiceUp()) {
        return Failed("not on a dungeon floor where a scene can start");
    }
    if (!BtSystemScriptHasProgram(program)) return Failed("the dungeon's event script has no such program");
    if (NowMonstorUnit->requested_event != -1 || g_play_event_program != -1) return Failed("a scene is already starting");
    NowMonstorUnit->requested_event = program;
    g_play_event_owner = std::string(owner);
    g_play_event_program = program;
    return {ApiCallStatus::Ok, {{"program", program}}, {}};
}

// dungeons.set_quick_passage: with it on, going between a town and a dungeon shows no loading card (the area's
// title held for seconds), only the fade out and in.
std::string g_quick_passage_owner;
ApiCallResult SetQuickPassage(std::string_view owner, const Json &args) {
    const std::string usage = "expected {enabled = true or false}";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 || !args[0].contains("enabled") ||
        !args[0]["enabled"].is_boolean()) {
        return Invalid(usage);
    }
    if (!g_quick_passage_owner.empty() && g_quick_passage_owner != owner) return Failed("another mod already set the passage");
    if (args[0]["enabled"].get<bool>()) {
        g_quick_passage_owner = std::string(owner);
    } else {
        g_quick_passage_owner.clear();
        dc::game::DoorwayReset();
    }
    return {ApiCallStatus::Ok, {{"enabled", !g_quick_passage_owner.empty()}}, {}};
}

// world.set_roads: with it on, walking into a town's gate is game.town.road for the mod to take (the next town, say,
// by world.transition), not the world map.
std::string g_roads_owner;
ApiCallResult SetRoads(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 || !args[0].contains("enabled") ||
        !args[0]["enabled"].is_boolean()) {
        return Invalid("expected {enabled = true or false}");
    }
    if (!g_roads_owner.empty() && g_roads_owner != owner) return Failed("another mod already has the roads");
    if (args[0]["enabled"].get<bool>()) {
        g_roads_owner = std::string(owner);
    } else {
        g_roads_owner.clear();
    }
    return {ApiCallStatus::Ok, {{"enabled", !g_roads_owner.empty()}}, {}};
}

// dungeons.set_floor_select: with the floor menu off, the dungeon's entrance takes the player straight in, to its first
// floor (a mod then starts them where it likes, such as dungeons.set_arrival); a resumed floor still wins.
std::string g_floor_select_owner;
ApiCallResult SetFloorSelect(std::string_view owner, const Json &args) {
    const std::string usage = "expected {enabled = true or false}";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 || !args[0].contains("enabled") ||
        !args[0]["enabled"].is_boolean()) {
        return Invalid(usage);
    }
    if (!g_floor_select_owner.empty() && g_floor_select_owner != owner) return Failed("another mod already turned the floor menu off");
    if (args[0]["enabled"].get<bool>()) {
        g_floor_select_owner.clear();
    } else {
        g_floor_select_owner = std::string(owner);
    }
    return {ApiCallStatus::Ok, {{"enabled", g_floor_select_owner.empty()}}, {}};
}

// The Proceed option's two lines: the owner's, else the back floor's warning.
std::vector<dc::game::MoonModChoiceLine> BackroomLines() {
    if (g_backroom_lines.empty()) return {{"The monsters below are far stronger.", 2}, {"The rewards are far greater.", 1}};
    std::vector<dc::game::MoonModChoiceLine> out;
    for (std::size_t i = 0; i < g_backroom_lines.size(); ++i) out.push_back({g_backroom_lines[i], i == 0 ? 2 : 1});
    return out;
}

// dungeons.set_rest_room: one mod builds the floors made while it asks as rest floors (moonmod/dungeonmap_layout.hpp).
std::string g_rest_room_owner;
Json RestRoomJson() {
    Json result = {{"enabled", DngRestRoomEnabled != 0}, {"state", "none"}};
    if (DngRestRoomSeed >= 0 && NowDngMap != nullptr && DngRestRoomSeed == NowDngMap->map_seed && BtUraDongeon == 0) {
        const auto point = [](float col, float row) { return Json{{"x", 160.0f * col}, {"y", 0.0f}, {"z", 160.0f * row}}; };
        result["state"] = "rest";
        result["spring"] = point(static_cast<float>(DngRestSpringX), static_cast<float>(DngRestSpringY));
        result["stairs"] = point(static_cast<float>(DngRestStairsX), static_cast<float>(DngRestStairsY));
        result["center"] = point(DngRestHallX + 0.5f * (DngRestHallW - 1), DngRestHallY + 0.5f * (DngRestHallH - 1));
        result["size"] = {{"x", 160.0f * DngRestHallW}, {"z", 160.0f * DngRestHallH}};
    }
    return result;
}

ApiCallResult SetRestRoom(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {enabled = boolean}");
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "enabled") return Invalid("dungeons.set_rest_room received an unsupported field: " + field.key());
    }
    if (!g_rest_room_owner.empty() && g_rest_room_owner != owner) return Failed("another mod already builds rest floors");
    if (!args[0].contains("enabled") || !args[0]["enabled"].is_boolean()) return Invalid("enabled must be a boolean");
    const bool enabled = args[0]["enabled"].get<bool>();
    DngRestRoomEnabled = enabled ? 1 : 0;
    if (enabled) g_rest_room_owner = std::string(owner);
    else g_rest_room_owner.clear();
    return {ApiCallStatus::Ok, RestRoomJson(), {}};
}

// dungeons.set_warden_room: one mod seals the Warden room on the floors built while it asks.
std::string g_warden_room_owner;
Json WardenRoomJson() {
    static const char *const kStates[] = {"none", "sealed", "open", "shut", "released"};
    Json result = {{"enabled", DngWardenRoomEnabled != 0}, {"lock_in", DngWardenRoomLockIn != 0},
                   {"state", kStates[std::clamp(DngWardenRoomState, 0, 4)]}};
    if (DngWardenRoomState != DNG_WARDEN_NONE && NowDngMap != nullptr && DngWardenRoomSeed == NowDngMap->map_seed) {
        result["center"] = {{"x", 160.0f * (DngWardenRoomX + 0.5f * (DngWardenRoomW - 1))}, {"y", 0.0f},
                            {"z", 160.0f * (DngWardenRoomY + 0.5f * (DngWardenRoomH - 1))}};
        result["size"] = {{"x", 160.0f * DngWardenRoomW}, {"z", 160.0f * DngWardenRoomH}};
        result["doors"] = DngWardenDoorCount;
    } else {
        result["state"] = "none";
    }
    return result;
}

void SetWardenDoors(bool shut) {
    if (NowDngMap == nullptr) return;
    for (int d = 0; d < DngWardenDoorCount; ++d) {
        const DNG_WARDEN_DOOR &door = DngWardenDoorTable[d];
        MAP_CELL &cell = MainDungeonMap.cells[door.x + door.y * DNG_GRID];
        cell.parts_no = shut ? door.gate_part : door.open_part;
        cell.direction = shut ? 0 : door.open_direction;
    }
}

// weapons.style_set: a mod's name and colour for one owned weapon, and the glow of its swing trail.
struct WeaponStyle {
    std::string owner;
    int item_no = -1;
    std::string name;
    std::array<unsigned char, 3> rgb{255, 255, 255};
    bool has_glow = false;
    std::array<unsigned char, 3> glow{};
    struct CardLine {
        std::string text;
        std::array<unsigned char, 3> rgb{235, 235, 235};
        float scale = 1.0f;
    };
    std::vector<CardLine> badge, details; // the weapon menu's card in place of the powder and description boxes
};
std::map<int, WeaponStyle> g_weapon_styles; // chara * 16 + slot

bool ReadRgb(const Json &value, std::array<unsigned char, 3> &out) {
    if (!value.is_object()) return false;
    for (const char *key : {"r", "g", "b"}) {
        if (!value.contains(key) || !value[key].is_number_integer() || value[key].get<int>() < 0 || value[key].get<int>() > 255) return false;
    }
    out = {static_cast<unsigned char>(value["r"].get<int>()), static_cast<unsigned char>(value["g"].get<int>()),
           static_cast<unsigned char>(value["b"].get<int>())};
    return true;
}

int CharacterIndex(const Json &value) {
    const CharacterIdView &kNames = kCharacterIds;
    if (value.is_number_integer() && value.get<int>() >= 0 && value.get<int>() < CharaCount()) return value.get<int>();
    if (!value.is_string()) return -1;
    for (int c = 0; c < CharaCount(); ++c) {
        if (value.get<std::string>() == kNames[c] || value.get<std::string>() == std::string("chronicle:character:") + kNames[c]) return c;
    }
    return -1;
}

void RefreshWeaponGlow() {
    if (mode == GAME_MODE_DUNGEON && NowWeaponHave != nullptr && CharaMain.frame != nullptr) SetWeaponColor();
}

// A card's lines: each a string or {text, color = {r, g, b}?, scale?}, up to `most` of them, 64 bytes each.
bool ReadCardLines(const Json &value, size_t most, std::vector<WeaponStyle::CardLine> &out) {
    if (!value.is_array() || value.size() > most) return false;
    for (const Json &entry : value) {
        WeaponStyle::CardLine line;
        const Json *text = &entry;
        if (entry.is_object()) {
            for (auto field = entry.begin(); field != entry.end(); ++field) {
                if (field.key() != "text" && field.key() != "color" && field.key() != "scale") return false;
            }
            if (!entry.contains("text")) return false;
            text = &entry["text"];
            if (entry.contains("color") && !ReadRgb(entry["color"], line.rgb)) return false;
            if (entry.contains("scale")) {
                if (!entry["scale"].is_number() || entry["scale"].get<double>() < 0.5 || entry["scale"].get<double>() > 1.6) return false;
                line.scale = entry["scale"].get<float>();
            }
        }
        if (!text->is_string() || text->get<std::string>().size() > 64) return false;
        line.text = text->get<std::string>();
        out.push_back(std::move(line));
    }
    return true;
}

ApiCallResult SetWeaponStyle(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {character, slot, name?, color = {r, g, b}?, glow = {r, g, b}?, badge?, details?}");
    }
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "character" && field.key() != "slot" && field.key() != "name" && field.key() != "color" && field.key() != "glow" &&
            field.key() != "badge" && field.key() != "details") {
            return Invalid("weapons.style_set received an unsupported field: " + field.key());
        }
    }
    const int chara = request.contains("character") ? CharacterIndex(request["character"]) : -1;
    int slot = -1;
    if (chara < 0 || !ReadBoundedInt(request, "slot", 0, 10, slot)) return Invalid("character names one of the six and slot is 0..10");
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("no game is loaded");
    const int item_no = status->chara_weapons[chara][slot].item_no;
    if (item_no <= 0) return Failed("that slot holds no weapon");
    WeaponStyle style;
    style.owner = std::string(owner);
    style.item_no = item_no;
    if (request.contains("name")) {
        if (!request["name"].is_string() || request["name"].get<std::string>().size() > 40) return Invalid("name is a string of up to 40 bytes");
        style.name = request["name"].get<std::string>();
    }
    if (request.contains("color") && !ReadRgb(request["color"], style.rgb)) return Invalid("color is {r, g, b}, each 0..255");
    if (request.contains("glow")) {
        if (!ReadRgb(request["glow"], style.glow)) return Invalid("glow is {r, g, b}, each 0..255");
        style.has_glow = true;
    }
    if (request.contains("badge") && !ReadCardLines(request["badge"], 3, style.badge)) {
        return Invalid("badge is up to 3 lines, each a string or {text, color?, scale = 0.5..1.6?}, 64 bytes each");
    }
    if (request.contains("details") && !ReadCardLines(request["details"], 5, style.details)) {
        return Invalid("details is up to 5 lines, each a string or {text, color?, scale = 0.5..1.6?}, 64 bytes each");
    }
    const auto existing = g_weapon_styles.find(chara * 16 + slot);
    if (existing != g_weapon_styles.end() && existing->second.owner != owner) return Failed("another mod styles this weapon");
    g_weapon_styles[chara * 16 + slot] = style;
    RefreshWeaponGlow();
    return {ApiCallStatus::Ok, {{"character", chara}, {"slot", slot}, {"item_no", item_no}}, {}};
}

// weapons.style_clear: one weapon's style, or (with no slot) all this mod's.
ApiCallResult ClearWeaponStyle(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() > 1 || (args.size() == 1 && !args[0].is_object())) return Invalid("expected {character?, slot?}");
    int key = -1;
    if (args.size() == 1 && args[0].contains("character")) {
        const int chara = CharacterIndex(args[0]["character"]);
        int slot = -1;
        if (chara < 0 || !ReadBoundedInt(args[0], "slot", 0, 10, slot)) return Invalid("character names one of the six and slot is 0..10");
        key = chara * 16 + slot;
    }
    std::erase_if(g_weapon_styles, [&](const auto &entry) { return entry.second.owner == owner && (key < 0 || entry.first == key); });
    RefreshWeaponGlow();
    return {ApiCallStatus::Ok, {{"cleared", true}}, {}};
}

// effects.burst: one of the game's own effects at a point (an explosion, healing sparkles, hit sparks, an impact).
ApiCallResult BurstEffect(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {kind, position = {x, y, z}, scale?}");
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "kind" && field.key() != "position" && field.key() != "scale") return Invalid("effects.burst received an unsupported field: " + field.key());
    }
    if (mode != GAME_MODE_DUNGEON || CharaMain.frame == nullptr) return Failed("effects.burst needs an active dungeon");
    std::array<float, 4> at{};
    if (!request.contains("position") || !ReadVector3(request["position"], at, 100000.0f)) return Invalid("position is {x, y, z}");
    at[3] = 1.0f;
    const std::string kind = request.value("kind", std::string());
    float scale = 1.0f;
    if (request.contains("scale")) {
        if (!request["scale"].is_number() || request["scale"].get<float>() < 0.1f || request["scale"].get<float>() > 4.0f) return Invalid("scale is 0.1..4");
        scale = request["scale"].get<float>();
    }
    if (kind == "explosion") {
        if (NowBombEffect == nullptr) return Failed("no explosion effect is loaded");
        NowBombEffect->SetBomb(at.data(), scale);
    } else if (kind == "heal") {
        HealEffect.Set(at.data());
    } else if (kind == "sparks" || kind == "guard") {
        static int next = 0;
        float up[4] = {0.0f, 1.1f, 0.0f, 1.0f};
        HitMark[next].Set(at.data(), up, kind == "guard" ? HIT_MARK_GUARD : HIT_MARK_HIT, 0.5f * scale, 0.01f, 0.02f, scale, 32, at[1]);
        next = (next + 1) % 16;
    } else if (kind == "impact") {
        OzumondShotEffect.Set(at.data());
    } else if (kind == "smoke") {
        // a bomb effect slot that is free (the last if none is), so a bomb's blast going off isn't cut short
        if (NowBombEffect == nullptr) return Failed("no explosion effect is loaded");
        CItemBombEffect *slot = &NowBombEffect[2];
        for (int bomb = 0; bomb < 3; bomb++) {
            if (NowBombEffect[bomb].CheckBomb() == 0) {
                slot = &NowBombEffect[bomb];
                break;
            }
        }
        slot->SetSmoke(at.data(), scale);
    } else {
        return Invalid("kind is explosion, heal, sparks, guard, impact or smoke");
    }
    return {ApiCallStatus::Ok, {{"kind", kind}}, {}};
}

// entities.tint: an enemy drawn in a colour for a while (a poisoned one green). entities.knockback: a shove.
struct MonsterTint {
    std::uint32_t generation = 0;
    int frames = 0;
    std::array<unsigned char, 3> rgb{};
};
std::array<MonsterTint, MONSTOR_SLOT_MAX> g_monster_tints{};

bool LiveMonster(const Json &entity, int &slot, std::string &error) {
    std::string handle;
    std::uint32_t generation = 0;
    if (!ReadId(entity, handle) || !ReadMonsterHandle(handle, slot, generation)) { error = "entity must be a Chronicle monster handle"; return false; }
    CMonstorUnit *unit = NowMonstorUnit;
    if (!IsActiveDungeonStatus(CurrentStatus()) || unit == nullptr) { error = "needs an active Chronicle dungeon"; return false; }
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) { error = "the entity handle is stale"; return false; }
    const MONSTOR &monster = unit->monster[slot];
    if (monster.state == -1 || monster.hp <= 0) { error = "the selected entity is not a live enemy"; return false; }
    return true;
}

ApiCallResult TintMonster(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {entity, color = {r, g, b}, frames = 1..3600}");
    int slot = -1;
    std::string error;
    if (!args[0].contains("entity") || !LiveMonster(args[0]["entity"], slot, error)) return Failed(error.empty() ? "entity is required" : error);
    MonsterTint tint;
    if (!args[0].contains("color") || !ReadRgb(args[0]["color"], tint.rgb)) return Invalid("color is {r, g, b}, each 0..255");
    if (!ReadBoundedInt(args[0], "frames", 0, 3600, tint.frames)) return Invalid("frames is 0..3600 (0 takes the tint off)");
    tint.generation = g_monster_handle_generations[slot];
    g_monster_tints[slot] = tint;
    return {ApiCallStatus::Ok, {{"frames", tint.frames}}, {}};
}

ApiCallResult KnockbackMonster(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {entity, direction = {x, z}, speed = 0.5..40}");
    int slot = -1;
    std::string error;
    if (!args[0].contains("entity") || !LiveMonster(args[0]["entity"], slot, error)) return Failed(error.empty() ? "entity is required" : error);
    const Json &direction = args[0].value("direction", Json::object());
    if (!direction.is_object() || !direction.contains("x") || !direction.contains("z") || !direction["x"].is_number() || !direction["z"].is_number()) {
        return Invalid("direction is {x, z}");
    }
    if (!args[0].contains("speed") || !args[0]["speed"].is_number()) return Invalid("speed is 0.5..40");
    const float speed = std::clamp(args[0]["speed"].get<float>(), 0.5f, 40.0f);
    float dir[4] = {direction["x"].get<float>(), 0.0f, direction["z"].get<float>(), 1.0f};
    const float length = std::hypot(dir[0], dir[2]);
    if (!(length > 0.0001f)) return Invalid("direction must not be zero");
    dir[0] /= length;
    dir[2] /= length;
    MONSTOR &monster = NowMonstorUnit->monster[slot];
    sceVu0CopyVector(monster.knockback_direction, dir);
    monster.knockback[0] = speed;
    monster.knockback[1] = speed / 10.0f;
    return {ApiCallStatus::Ok, {{"speed", speed}}, {}};
}

// animations.player_register: a mod's own motion for a playable character, read from a JSON file in its package
// ("moonmod-motion" v1: per bone, rotation keys as the game stores them (w, x, y, z) and translation/scale keys
// (x, y, z), at frames 0..length). When the character's model loads in a dungeon, every registered motion of that
// character is appended after the last of its native frames, on the model and its shadow, and the first motion set's
// table grows by one entry for each, so the motion plays like any other by its new number.
struct MotionTrack {
    std::string bone;
    int type = 0; // MOTION_KEY_ROTATION, _SCALE or _TRANSLATION
    std::vector<std::pair<int, std::array<float, 4>>> keys;
};
struct PlayerMotion {
    std::string owner, id;
    int chara = 0;
    int length = 1;
    float speed = 0.5f;
    std::vector<MotionTrack> tracks;
};
std::vector<PlayerMotion> g_player_motions;

// What one application changed on the loaded character, to put back before the next.
struct AppliedMotions {
    CCharacter *character = nullptr;
    tagMOTION_TYPE *motion = nullptr, *shadow = nullptr;
    MOTION_INFO *native_table = nullptr;
    s32 native_end = 0;
    struct Backup {
        Mot_List *list;
        Mot_Key *keys;
        u32 count;
    };
    std::vector<Backup> backups;
    std::vector<std::vector<Mot_Key>> keys;
    std::vector<MOTION_INFO> table;
    std::map<std::string, int> numbers; // "mod:id" -> motion number
    int chara = -1;
};
AppliedMotions g_applied_motions;

std::string FrameBaseName(const CFrame *frame) {
    std::string name(frame->name, strnlen(frame->name, sizeof(frame->name)));
    if (const auto attr = name.find("__"); attr != std::string::npos) name.resize(attr);
    return name;
}

// The loaded set still the one we changed (the model was not reloaded behind our back).
bool AppliedMotionsLive() {
    const AppliedMotions &a = g_applied_motions;
    return a.motion != nullptr && mode == GAME_MODE_DUNGEON && a.character == &CharaMain && CharaMain.motion[0] == a.motion &&
           a.motion->motion_info == a.table.data();
}

void RestoreAppliedMotions() {
    AppliedMotions &a = g_applied_motions;
    if (AppliedMotionsLive()) {
        for (const AppliedMotions::Backup &b : a.backups) {
            b.list->keys = b.keys;
            b.list->key_count = b.count;
        }
        a.motion->motion_info = a.native_table;
        if (a.shadow != nullptr) a.shadow->motion_info = a.native_table;
        CharaMain.motion_end[0] = a.native_end;
    }
    a = AppliedMotions{};
}

// The value a channel holds at a frame on its own keys (the key at or before it).
std::array<float, 4> HeldValue(const Mot_List *list, u32 frame) {
    std::array<float, 4> value{};
    for (u32 k = 0; k < list->key_count; ++k) {
        if (list->keys[k].frame > frame && k > 0) break;
        std::copy(list->keys[k].value, list->keys[k].value + 4, value.begin());
    }
    return value;
}

// The new keys go into store; with backups, each list's native keys are kept there to put back.
void AppendToMotionLists(tagMOTION_TYPE *motion, CFrame *frames, const std::vector<const PlayerMotion *> &motions, const std::vector<int> &starts,
                         std::vector<std::vector<Mot_Key>> &store, std::vector<AppliedMotions::Backup> *backups) {
    for (Mot_List *list = motion->proc_list; list != nullptr; list = list->next) {
        if ((list->type != MOTION_KEY_ROTATION && list->type != MOTION_KEY_TRANSLATION && list->type != MOTION_KEY_SCALE) || list->key_count == 0) continue;
        const std::string bone = FrameBaseName(&static_cast<CFrameVu1 *>(frames)[list->frame]);
        std::vector<Mot_Key> keys(list->keys, list->keys + list->key_count);
        const std::array<float, 4> held = HeldValue(list, 10);
        for (size_t m = 0; m < motions.size(); ++m) {
            const PlayerMotion &pm = *motions[m];
            const MotionTrack *track = nullptr;
            for (const MotionTrack &t : pm.tracks) {
                if (t.type == list->type && t.bone == bone) { track = &t; break; }
            }
            auto push = [&](int frame, const std::array<float, 4> &value) {
                Mot_Key key{};
                key.frame = static_cast<u32>(starts[m] + frame);
                std::copy(value.begin(), value.end(), key.value);
                if (keys.empty() || keys.back().frame < key.frame) keys.push_back(key);
            };
            if (track == nullptr || track->keys.empty()) {
                push(0, held);
                push(pm.length, held);
                continue;
            }
            if (track->keys.front().first > 0) push(0, track->keys.front().second);
            for (const auto &[frame, value] : track->keys) push(frame, value);
            if (track->keys.back().first < pm.length) push(pm.length, track->keys.back().second);
        }
        // The game reads keys[next_frame - 1] while blending, so a list holds at least as many keys as its last frame.
        const u32 count = static_cast<u32>(keys.size());
        const size_t padded = std::max<size_t>(keys.size(), keys.back().frame + 2);
        keys.resize(padded, keys.back());
        if (backups != nullptr) backups->push_back({list, list->keys, list->key_count});
        store.push_back(std::move(keys));
        list->keys = store.back().data();
        list->key_count = count;
    }
}

void ApplyPlayerMotions(int chara) {
    RestoreAppliedMotions();
    if (mode != GAME_MODE_DUNGEON || chara < 0 || chara >= CharaCount() || CharaMain.frame == nullptr) return;
    if (dc::game::ActiveCharaForm(chara) != nullptr) return; // a form's model has its own skeleton and motions
    tagMOTION_TYPE *motion = CharaMain.motion[0];
    if (motion == nullptr || motion->motion_info == nullptr || motion->proc_list == nullptr) return;
    std::vector<const PlayerMotion *> motions;
    for (const PlayerMotion &pm : g_player_motions) {
        if (pm.chara == chara) motions.push_back(&pm);
    }
    if (motions.empty()) return;

    AppliedMotions &a = g_applied_motions;
    a.character = &CharaMain;
    a.motion = motion;
    a.shadow = CharaMain.shadow_motion[0] != nullptr && CharaMain.shadow_motion[0]->proc_list != nullptr ? CharaMain.shadow_motion[0] : nullptr;
    a.native_table = motion->motion_info;
    a.native_end = CharaMain.motion_end[0];
    a.chara = chara;
    a.keys.reserve(256);

    // New motions start past the last native key of any channel, ten frames apart.
    u32 last = 0;
    for (Mot_List *list = motion->proc_list; list != nullptr; list = list->next) {
        if (list->key_count > 0) last = std::max(last, list->keys[list->key_count - 1].frame);
    }
    const int native_count = CharaMain.motion_end[0] - CharaMain.motion_start[0];
    for (int n = 0; n < native_count; ++n) last = std::max(last, static_cast<u32>(std::max(0, motion->motion_info[n].end)));
    std::vector<int> starts;
    int next = static_cast<int>(last) + 10;
    for (const PlayerMotion *pm : motions) {
        starts.push_back(next);
        next += pm->length + 10;
    }

    AppendToMotionLists(motion, CharaMain.frame, motions, starts, a.keys, &a.backups);
    if (a.shadow != nullptr && CharaMain.shadow_frame != nullptr) AppendToMotionLists(a.shadow, CharaMain.shadow_frame, motions, starts, a.keys, &a.backups);

    a.table.assign(motion->motion_info, motion->motion_info + native_count);
    for (size_t m = 0; m < motions.size(); ++m) {
        a.numbers[motions[m]->owner + ":" + motions[m]->id] = native_count + static_cast<int>(m);
        a.table.push_back({starts[m], starts[m] + motions[m]->length, motions[m]->speed, 0});
    }
    a.table.push_back({-1, -1, -1.0f, 0});
    motion->motion_info = a.table.data();
    if (a.shadow != nullptr) a.shadow->motion_info = a.table.data();
    CharaMain.motion_end[0] = CharaMain.motion_start[0] + native_count + static_cast<int>(motions.size());
}

int PlayerMotionNumber(const std::string &qualified) {
    if (!AppliedMotionsLive()) return -1;
    const auto found = g_applied_motions.numbers.find(qualified);
    return found == g_applied_motions.numbers.end() ? -1 : found->second;
}

std::string ReadMotionBody(const Json &doc, PlayerMotion &pm);

std::string ReadPlayerMotion(const Json &doc, PlayerMotion &pm) {
    if (!doc.is_object() || doc.value("format", std::string()) != "moonmod-motion" || doc.value("version", 0) != 1) {
        return "the file is not a moonmod-motion version 1 document";
    }
    pm.chara = doc.contains("character") ? CharacterIndex(doc["character"]) : -1;
    if (pm.chara < 0) return "character names one of the six";
    return ReadMotionBody(doc, pm);
}

// A motion file's length, speed and tracks.
std::string ReadMotionBody(const Json &doc, PlayerMotion &pm) {
    if (!doc.contains("length") || !doc["length"].is_number_integer() || doc["length"].get<int>() < 1 || doc["length"].get<int>() > 600) {
        return "length is 1..600 frames";
    }
    pm.length = doc["length"].get<int>();
    if (doc.contains("speed")) {
        if (!doc["speed"].is_number() || doc["speed"].get<double>() <= 0.0 || doc["speed"].get<double>() > 4.0) return "speed is above 0, up to 4";
        pm.speed = doc["speed"].get<float>();
    }
    if (!doc.contains("tracks") || !doc["tracks"].is_array() || doc["tracks"].empty() || doc["tracks"].size() > 128) return "tracks is 1..128 tracks";
    for (const Json &t : doc["tracks"]) {
        MotionTrack track;
        if (!t.is_object() || !t.contains("bone") || !t["bone"].is_string() || t["bone"].get<std::string>().empty() ||
            t["bone"].get<std::string>().size() > 31) {
            return "each track names its bone";
        }
        track.bone = t["bone"].get<std::string>();
        if (const auto attr = track.bone.find("__"); attr != std::string::npos) track.bone.resize(attr); // knife__m: a frame's attribute
        if (track.bone.empty()) return "each track names its bone";
        const std::string type = t.value("type", std::string());
        track.type = type == "rotation" ? MOTION_KEY_ROTATION : type == "translation" ? MOTION_KEY_TRANSLATION : type == "scale" ? MOTION_KEY_SCALE : -1;
        if (track.type < 0) return track.bone + ": type is rotation, translation or scale";
        const size_t width = track.type == MOTION_KEY_ROTATION ? 5 : 4;
        if (!t.contains("keys") || !t["keys"].is_array() || t["keys"].empty() || t["keys"].size() > 601) return track.bone + ": keys is 1..601 keys";
        int previous = -1;
        for (const Json &k : t["keys"]) {
            if (!k.is_array() || k.size() != width || !k[0].is_number_integer()) {
                return track.bone + ": each key is [frame, w, x, y, z] for a rotation, [frame, x, y, z] otherwise";
            }
            const int frame = k[0].get<int>();
            if (frame <= previous || frame < 0 || frame > pm.length) return track.bone + ": key frames rise from 0 to length";
            previous = frame;
            std::array<float, 4> value{};
            for (size_t c = 1; c < width; ++c) {
                if (!k[c].is_number() || !std::isfinite(k[c].get<double>())) return track.bone + ": key values are finite numbers";
                value[c - 1] = k[c].get<float>();
            }
            track.keys.emplace_back(frame, value);
        }
        pm.tracks.push_back(std::move(track));
    }
    return {};
}

std::optional<ApiCallResult> ReadModMotionFile(std::string_view owner, const Json &request, Json &doc);

ApiCallResult RegisterPlayerMotion(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {id, file}");
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "id" && field.key() != "file") return Invalid("animations.player_register received an unsupported field: " + field.key());
    }
    Json doc;
    if (const std::optional<ApiCallResult> refused = ReadModMotionFile(owner, request, doc)) return *refused;
    PlayerMotion pm;
    pm.owner = std::string(owner);
    pm.id = request["id"].get<std::string>();
    if (const std::string why = ReadPlayerMotion(doc, pm); !why.empty()) return Invalid(why);
    const auto mine = std::count_if(g_player_motions.begin(), g_player_motions.end(), [&](const PlayerMotion &m) { return m.owner == owner && m.id != pm.id; });
    if (mine >= 64) return Failed("a mod registers up to 64 motions");
    std::erase_if(g_player_motions, [&](const PlayerMotion &m) { return m.owner == owner && m.id == pm.id; });
    const int chara = pm.chara;
    const int length = pm.length;
    const size_t tracks = pm.tracks.size();
    g_player_motions.push_back(std::move(pm));
    if (UserStatus != nullptr && mode == GAME_MODE_DUNGEON && UserStatus->cur_chara == chara && BtActStatus.action_on == 0) ApplyPlayerMotions(chara);
    return {ApiCallStatus::Ok, {{"id", request["id"]}, {"length", length}, {"tracks", tracks}}, {}};
}

// A register request's id and its motion file, parsed into doc; a refusal otherwise.
std::optional<ApiCallResult> ReadModMotionFile(std::string_view owner, const Json &request, Json &doc) {
    if (!request.contains("id") || !request["id"].is_string() || request["id"].get<std::string>().empty() ||
        request["id"].get<std::string>().size() > 48 || request["id"].get<std::string>().find(':') != std::string::npos) {
        return Invalid("id is 1 to 48 bytes, without ':'");
    }
    if (!request.contains("file") || !request["file"].is_string()) return Invalid("file is a JSON file under the package's resources/");
    const std::filesystem::path relative = std::filesystem::path(request["file"].get<std::string>()).lexically_normal();
    if (relative.is_absolute() || relative.has_root_name() || relative.empty() || *relative.begin() != "resources" ||
        std::ranges::any_of(relative, [](const std::filesystem::path &part) { return part == ".."; }) || relative.extension() != ".json") {
        return Invalid("file is a .json file under the package's resources/");
    }
    std::filesystem::path package_root;
    for (const ModRecord &record : SharedRuntime().Mods()) {
        if (record.id == owner) package_root = record.package_root;
    }
    if (package_root.empty()) return Failed("the mod's package was not found");
    std::error_code error;
    const std::filesystem::path path = package_root / relative;
    if (std::filesystem::is_symlink(path, error) || !std::filesystem::is_regular_file(path, error)) return Failed("the motion file was not found");
    if (std::filesystem::file_size(path, error) > 4u * 1024u * 1024u || error) return Failed("a motion file is at most 4 MiB");
    std::ifstream in(path, std::ios::binary);
    doc = Json::parse(in, nullptr, false);
    if (doc.is_discarded()) return Invalid("the motion file is not valid JSON");
    return std::nullopt;
}

ApiCallResult UnregisterPlayerMotion(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("id") || !args[0]["id"].is_string()) return Invalid("expected {id}");
    const auto removed = std::erase_if(g_player_motions, [&](const PlayerMotion &m) { return m.owner == owner && m.id == args[0]["id"].get<std::string>(); });
    if (removed != 0 && UserStatus != nullptr && mode == GAME_MODE_DUNGEON && BtActStatus.action_on == 0) ApplyPlayerMotions(UserStatus->cur_chara);
    return {ApiCallStatus::Ok, {{"removed", removed != 0}}, {}};
}

ApiCallResult ListPlayerMotions(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() > 1) return Invalid("expected no arguments");
    const CharacterIdView &kNames = kCharacterIds;
    Json list = Json::array();
    for (const PlayerMotion &m : g_player_motions) {
        if (m.owner != owner) continue;
        const int number = PlayerMotionNumber(m.owner + ":" + m.id);
        list.push_back({{"id", m.id}, {"character", kNames[m.chara]}, {"length", m.length}, {"tracks", m.tracks.size()},
                        {"motion", number >= 0 ? Json(number) : Json(nullptr)}});
    }
    return {ApiCallStatus::Ok, {{"motions", list}}, {}};
}

// animations.monster_register: the same kind of motion file for a native monster model (its "character" is the model's
// file name, as e03a). Whenever a floor loads that model, its registered motions are appended after its native frames,
// on the model and each part it carries, and its first motion set's table grows by one entry for each. A motion's hits
// become damage spheres of every monster of the model as it spawns, live only while that motion plays: each takes the
// kind of hit of the monster's first native blow, and that blow's damage times the hit's damage.
struct MonsterMotionHit {
    std::string frame;
    float radius = 2.0f;
    int from = 0, to = 1;
    float damage = 1.0f;
};
struct MonsterMotion {
    PlayerMotion motion; // chara unused
    int model = -1;
    std::vector<MonsterMotionHit> hits;
};
std::vector<MonsterMotion> g_monster_motions;

// What the motions added to one loaded model slot: kept until the slot loads again, as its monsters play from them.
struct AppliedMonsterMotions {
    int model = -1;
    std::vector<std::vector<Mot_Key>> keys;
    std::array<std::vector<MOTION_INFO>, 3> tables;
    struct Entry {
        std::string owner, id;
        int number = 0, start = 0, length = 0;
        std::vector<MonsterMotionHit> hits;
    };
    std::vector<Entry> entries;
};
std::array<AppliedMonsterMotions, MONSTOR_MODEL_MAX> g_applied_monster_motions;

std::string MonsterModelStem(int model) {
    const MONSTOR_MODEL *definition = GetMonstorTable(model);
    if (definition == nullptr) return {};
    const char *name = definition->model_name[0];
    return std::string(name, strnlen(name, sizeof(definition->model_name[0])));
}

void ApplyMonsterMotions(CMonstorUnit *unit, int slot, int model) {
    AppliedMonsterMotions &a = g_applied_monster_motions[slot];
    a = AppliedMonsterMotions{}; // the slot's model was just loaded afresh: nothing points at the old keys
    std::vector<const PlayerMotion *> motions;
    std::vector<const MonsterMotion *> sources;
    for (const MonsterMotion &mm : g_monster_motions) {
        if (mm.model != model) continue;
        motions.push_back(&mm.motion);
        sources.push_back(&mm);
    }
    if (motions.empty()) return;
    CCharacter *parts = unit->base_chara[slot];
    auto usable = [&](int part) {
        if (part > 0 && unit->model[slot].model_name[part][0] == 0) return false;
        const CCharacter &c = parts[part];
        return c.frame != nullptr && c.motion[0] != nullptr && c.motion[0]->motion_info != nullptr && c.motion[0]->proc_list != nullptr;
    };
    if (!usable(0)) return;

    // New motions start past the last native key of the model and its parts, ten frames apart.
    const int native_count = parts[0].motion_end[0] - parts[0].motion_start[0];
    u32 last = 0;
    for (int part = 0; part < 3; ++part) {
        if (!usable(part)) continue;
        const tagMOTION_TYPE *motion = parts[part].motion[0];
        for (Mot_List *list = motion->proc_list; list != nullptr; list = list->next) {
            if (list->key_count > 0) last = std::max(last, list->keys[list->key_count - 1].frame);
        }
        const int count = parts[part].motion_end[0] - parts[part].motion_start[0];
        for (int n = 0; n < count; ++n) last = std::max(last, static_cast<u32>(std::max(0, motion->motion_info[n].end)));
    }
    std::vector<int> starts;
    int next = static_cast<int>(last) + 10;
    for (const PlayerMotion *pm : motions) {
        starts.push_back(next);
        next += pm->length + 10;
    }

    a.model = model;
    a.keys.reserve(512);
    for (int part = 0; part < 3; ++part) {
        if (!usable(part)) continue;
        CCharacter &c = parts[part];
        tagMOTION_TYPE *motion = c.motion[0];
        tagMOTION_TYPE *shadow = c.shadow_motion[0] != nullptr && c.shadow_motion[0]->proc_list != nullptr && c.shadow_frame != nullptr ? c.shadow_motion[0] : nullptr;
        AppendToMotionLists(motion, c.frame, motions, starts, a.keys, nullptr);
        if (shadow != nullptr) AppendToMotionLists(shadow, c.shadow_frame, motions, starts, a.keys, nullptr);
        // A part plays the model's motion numbers, so its table runs as long as the model's before the new entries.
        std::vector<MOTION_INFO> &table = a.tables[part];
        const int count = c.motion_end[0] - c.motion_start[0];
        table.assign(motion->motion_info, motion->motion_info + std::min(count, native_count));
        table.resize(native_count, motion->motion_info[0]);
        for (size_t m = 0; m < motions.size(); ++m) table.push_back({starts[m], starts[m] + motions[m]->length, motions[m]->speed, 0});
        table.push_back({-1, -1, -1.0f, 0});
        motion->motion_info = table.data();
        if (shadow != nullptr) shadow->motion_info = table.data();
        c.motion_end[0] = c.motion_start[0] + native_count + static_cast<int>(motions.size());
    }
    for (size_t m = 0; m < motions.size(); ++m) {
        a.entries.push_back({motions[m]->owner, motions[m]->id, native_count + static_cast<int>(m), starts[m], motions[m]->length, sources[m]->hits});
    }
}

// The registered motions' hit spheres on a monster that has just spawned, once its spawn script has set its own (a
// sphere already there, from an earlier spawn in the slot, is left as it is).
void SetMonsterMotionHits(CMonstorUnit *unit, int slot) {
    const int model_slot = unit->monster[slot].base_model;
    if (model_slot < 0 || model_slot >= MONSTOR_MODEL_MAX || unit->chara[slot][0].frame == nullptr) return;
    const AppliedMonsterMotions &a = g_applied_monster_motions[model_slot];
    MONSTOR_EFFECT_STATE2 &fx = unit->effect2[slot];
    int blow = -1;
    for (int i = 0; i < 16 && blow < 0; ++i) {
        if (fx.active[i] != 0 && fx.damage[i] > 0) blow = i;
    }
    for (const AppliedMonsterMotions::Entry &entry : a.entries) {
        for (const MonsterMotionHit &hit : entry.hits) {
            int free = -1;
            for (int i = 0; i < 16 && free < 0; ++i) {
                if (fx.active[i] == 0) free = i;
            }
            if (free < 0) return;
            std::string name = hit.frame;
            CFrame *frame = unit->chara[slot][0].frame->SearchFrame(name.data());
            if (frame == nullptr) continue;
            const float from = static_cast<float>(entry.start + hit.from), to = static_cast<float>(entry.start + hit.to);
            bool there = false;
            for (int i = 0; i < 16 && !there; ++i) {
                there = fx.active[i] != 0 && fx.frame[i] == frame && fx.motion_start[i] == from && fx.motion_end[i] == to;
            }
            if (there) continue;
            fx.active[free] = 1;
            fx.frame[free] = frame;
            fx.radius[free] = hit.radius;
            fx.motion_start[free] = from;
            fx.motion_end[free] = to;
            fx.damage[free] = std::max(1, static_cast<int>(std::lround((blow >= 0 ? fx.damage[blow] : 1) * hit.damage)));
            fx.flags[free] = blow >= 0 ? fx.flags[blow] : 0;
            fx.kind[free] = blow >= 0 ? fx.kind[blow] : 0;
            fx.angle[free] = blow >= 0 ? fx.angle[blow] : 0.0f;
        }
    }
}

ApiCallResult RegisterMonsterMotion(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {id, file, model, hits}");
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "id" && field.key() != "file" && field.key() != "model" && field.key() != "hits") {
            return Invalid("animations.monster_register received an unsupported field: " + field.key());
        }
    }
    MonsterMotion mm;
    if (!ReadBoundedInt(request, "model", 0, 4095, mm.model)) return Invalid("model is a native monster model id");
    const std::string stem = MonsterModelStem(mm.model);
    if (stem.empty()) return Invalid("model is a native monster model id");
    Json doc;
    if (const std::optional<ApiCallResult> refused = ReadModMotionFile(owner, request, doc)) return *refused;
    if (!doc.is_object() || doc.value("format", std::string()) != "moonmod-motion" || doc.value("version", 0) != 1) {
        return Invalid("the file is not a moonmod-motion version 1 document");
    }
    if (doc.value("character", std::string()) != stem) return Invalid("character names the model's file, " + stem);
    mm.motion.owner = std::string(owner);
    mm.motion.id = request["id"].get<std::string>();
    if (const std::string why = ReadMotionBody(doc, mm.motion); !why.empty()) return Invalid(why);
    if (request.contains("hits")) {
        const Json &hits = request["hits"];
        if (!hits.is_array() || hits.size() > 4) return Invalid("hits is a list of up to 4 {bone, radius, from, to, damage}");
        for (const Json &h : hits) {
            MonsterMotionHit hit;
            if (!h.is_object() || !h.contains("bone") || !h["bone"].is_string() || h["bone"].get<std::string>().empty() || h["bone"].get<std::string>().size() > 31) {
                return Invalid("each hit names its bone");
            }
            for (auto field = h.begin(); field != h.end(); ++field) {
                if (field.key() != "bone" && field.key() != "radius" && field.key() != "from" && field.key() != "to" && field.key() != "damage") {
                    return Invalid("a hit received an unsupported field: " + field.key());
                }
            }
            hit.frame = h["bone"].get<std::string>();
            if (!ReadBoundedInt(h, "from", 0, mm.motion.length, hit.from) || !ReadBoundedInt(h, "to", hit.from + 1, mm.motion.length, hit.to)) {
                return Invalid(hit.frame + ": from and to are frames of the motion, from before to");
            }
            if (h.contains("radius")) {
                if (!h["radius"].is_number() || h["radius"].get<double>() <= 0.0 || h["radius"].get<double>() > 40.0) return Invalid(hit.frame + ": radius is above 0, up to 40");
                hit.radius = h["radius"].get<float>();
            }
            if (h.contains("damage")) {
                if (!h["damage"].is_number() || h["damage"].get<double>() <= 0.0 || h["damage"].get<double>() > 10.0) return Invalid(hit.frame + ": damage is above 0, up to 10");
                hit.damage = h["damage"].get<float>();
            }
            mm.hits.push_back(std::move(hit));
        }
    }
    const auto mine = std::count_if(g_monster_motions.begin(), g_monster_motions.end(), [&](const MonsterMotion &m) { return m.motion.owner == owner && m.motion.id != mm.motion.id; });
    if (mine >= 64) return Failed("a mod registers up to 64 monster motions");
    std::erase_if(g_monster_motions, [&](const MonsterMotion &m) { return m.motion.owner == owner && m.motion.id == mm.motion.id; });
    const int length = mm.motion.length;
    const size_t tracks = mm.motion.tracks.size();
    g_monster_motions.push_back(std::move(mm));
    return {ApiCallStatus::Ok, {{"id", request["id"]}, {"length", length}, {"tracks", tracks}}, {}};
}

ApiCallResult UnregisterMonsterMotion(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("id") || !args[0]["id"].is_string()) return Invalid("expected {id}");
    const auto removed = std::erase_if(g_monster_motions, [&](const MonsterMotion &m) { return m.motion.owner == owner && m.motion.id == args[0]["id"].get<std::string>(); });
    return {ApiCallStatus::Ok, {{"removed", removed != 0}}, {}};
}

// The mod's monster motions, with the motion number and first frame each has where a loaded floor model carries it.
ApiCallResult ListMonsterMotions(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() > 1) return Invalid("expected no arguments");
    Json list = Json::array();
    for (const MonsterMotion &m : g_monster_motions) {
        if (m.motion.owner != owner) continue;
        Json motion = nullptr, start = nullptr;
        for (const AppliedMonsterMotions &a : g_applied_monster_motions) {
            if (a.model != m.model) continue;
            for (const AppliedMonsterMotions::Entry &entry : a.entries) {
                if (entry.owner == m.motion.owner && entry.id == m.motion.id) {
                    motion = entry.number;
                    start = entry.start;
                }
            }
        }
        list.push_back({{"id", m.motion.id}, {"model", m.model}, {"length", m.motion.length}, {"tracks", m.motion.tracks.size()},
                        {"hits", m.hits.size()}, {"motion", motion}, {"start", start}});
    }
    return {ApiCallStatus::Ok, {{"motions", list}}, {}};
}

// player.moves_set: a mod's own attacks, built from a character's native motions. Each is started by a trigger,
// replays a stretch of one motion at its own speed, and carries its own lunge, spin, hits, bursts, sounds and trail.
struct MoveHit {
    float at = 0.5f, radius = 8.0f, reach = 6.0f, height = 6.0f, damage = 1.0f, knockback = 2.0f, burst_scale = 1.0f;
    int kind = 3;
    bool charged = false;
    // shot: a projectile (a slingshot shot) instead of a strike, `count` of them fanned `spread` degrees apart.
    bool shot = false;
    int count = 1;
    float spread = 10.0f, speed = 5.0f;
    std::string burst;
    // a shot's own start and look: from a frame of the character's model (empty: the weapon's tip), moved ahead the
    // way the move faces and up; drawn as basefx01 cell `cell` (-1: the native one) at `size`. A burst on a shot plays
    // where each one lands.
    std::string from;
    float from_ahead = 0.0f, from_rise = 0.0f, size = 1.0f;
    int cell = -1;
};
struct MoveCue {
    float at = 0.0f;
    int sound = -1, trail = -1;
};
// Another animation for a move, played instead when the move starts while the player is partway through a native
// motion: `motion` between the fractions `from` and `to` of it (Toan's run, say, with his left foot leading).
struct MoveVariant {
    std::string animation;
    int motion = 0;
    float from = 0.0f, to = 1.0f;
};

struct PlayerMove {
    std::string owner, id;
    std::string animation; // a registered mod motion ("id" of the same mod, or "mod:id"), in place of motion
    std::vector<MoveVariant> variants; // the first that matches how the player is moving replaces animation
    int chara = 0;
    // 0 run_attack, 1 charge_release, 2 guard_attack, 3 dodge (stick pushed), 4 dodge_back (stick at rest), 5 heavy (R2),
    // 6 combo_finisher (R2 after `chain` blows of the attack combo), 7 lock_attack (attack, locked on, stick `direction`),
    // 8 evade_attack (attack late in an evade or just after it)
    int trigger = 0;
    int chain = 0;     // combo_finisher: how many blows of the combo come first
    // lock_attack: 0 toward the target, 1 away from it. dodge: -1 any way; locked on, 0 toward, 1 away, 2 left or 3 right
    // of the target picks it (a side roll keeps him facing the target).
    int direction = 0;
    int blend_in = 0; // frames to fade from his pose into the move's first; 0 keeps the game's own fade (10)
    int camera = 0;   // steps the follow camera takes to close on him while the move plays; 0 keeps the game's (8, 14 locked on)
    int run_frames = 24;
    float min_charge = 0.0f, max_charge = 99.0f;
    int motion = 0;
    float from = 0.0f, to = 1.0f, speed = 1.0f;
    float lunge = 0.0f, lunge_decay = 0.1f, lunge_angle = 0.0f, spin = 0.0f, spin_from = 0.0f, spin_to = 1.0f;
    bool invincible = false;
    float gauge = 0.0f, wear = 1.0f;
    int cooldown = 0;
    // An evade (dodge, dodge_back) can also cut short an attack's recovery: cancel_after steps after its last blow.
    bool cancel = true;
    int cancel_after = 2;
    std::vector<MoveHit> hits;
    std::vector<MoveCue> cues;
    // critical (trigger 9): 1 a backstab (the attack button behind an enemy, within `critical_arc` degrees of its
    // back), 2 a riposte (on the enemy player.critical_set armed). He is drawn in to `critical_gap` of a target within
    // `critical_reach`, facing it; the target is turned (to him for a riposte, away for a backstab), plays its pair's
    // motion and is held `critical_hold` steps (0: the length of that motion).
    int critical = 0;
    // art (trigger 11): R2 while guarding, with one of `weapons` (native item numbers; empty: any) equipped at
    // `min_level` or higher. The first of the character's arts that fits plays.
    std::vector<int> weapons;
    int min_level = 1;
    float critical_reach = 24.0f, critical_gap = 12.0f, critical_arc = 90.0f;
    int critical_hold = 0;
    struct CriticalPair {
        int model = 0;
        std::string owner, id; // the monster motion (animations.monster_register)
        float speed = 0.5f;
    };
    std::vector<CriticalPair> pairs;
};
std::vector<PlayerMove> g_player_moves;

bool MoveNumber(const Json &object, const char *key, float minimum, float maximum, float &out) {
    if (!object.contains(key)) return true;
    if (!object[key].is_number()) return false;
    const float value = object[key].get<float>();
    if (!(value >= minimum && value <= maximum)) return false;
    out = value;
    return true;
}

int MoveSound(const std::string &name) {
    if (name == "swing") return 0x190;
    if (name == "swing_heavy") return 0x191;
    if (name == "voice") return 0x1A4;
    if (name == "voice_2") return 0x1A5;
    if (name == "voice_3") return 0x1A6;
    if (name == "shout") return SE_CHARA_SHOUT_4;
    if (name == "shout_2") return SE_CHARA_SHOUT_3;
    if (name == "action") return SE_CHARA_ACTION;
    return -1;
}

int MoveTrail(const std::string &name) {
    static const char *const kTrails[] = {"light", "wide", "heavy", "wave", "slam"};
    for (int n = 0; n < 5; ++n) {
        if (name == kTrails[n]) return n;
    }
    return -1;
}

std::string ReadMove(const Json &entry, PlayerMove &move) {
    if (!entry.is_object()) return "each move is a table";
    static const char *const kFields[] = {"id", "character", "trigger", "run_frames", "min_charge", "max_charge", "motion", "from", "to",
                                          "speed", "lunge", "lunge_decay", "lunge_angle", "spin", "spin_from", "spin_to", "invincible", "gauge", "wear", "cooldown", "hits", "cues", "animation", "cancel", "cancel_after",
                                          "chain", "direction", "variants", "blend_in", "camera", "critical", "reach", "gap", "arc", "hold", "pairs", "weapons", "min_level"};
    for (auto field = entry.begin(); field != entry.end(); ++field) {
        if (std::find_if(std::begin(kFields), std::end(kFields), [&](const char *k) { return field.key() == k; }) == std::end(kFields)) {
            return "a move received an unsupported field: " + field.key();
        }
    }
    if (!entry.contains("id") || !entry["id"].is_string() || entry["id"].get<std::string>().empty() || entry["id"].get<std::string>().size() > 40) {
        return "id is a string of 1 to 40 bytes";
    }
    move.id = entry["id"].get<std::string>();
    move.chara = entry.contains("character") ? CharacterIndex(entry["character"]) : -1;
    if (move.chara < 0) return move.id + ": character names one of the six";
    const std::string trigger = entry.value("trigger", std::string());
    if (trigger == "run_attack") move.trigger = 0;
    else if (trigger == "charge_release" && CombatChara(move.chara) == CHARA_TOAN) move.trigger = 1;
    else if (trigger == "guard_attack") move.trigger = 2;
    else if (trigger == "dodge") move.trigger = 3;
    else if (trigger == "dodge_back") move.trigger = 4;
    else if (trigger == "heavy") move.trigger = 5;
    else if (trigger == "combo_finisher") move.trigger = 6;
    else if (trigger == "lock_attack") move.trigger = 7;
    else if (trigger == "evade_attack") move.trigger = 8;
    else if (trigger == "critical") move.trigger = 9;
    else if (trigger == "played") move.trigger = 10; // no button: only player.move_play starts it
    else if (trigger == "art") move.trigger = 11;
    else {
        return move.id + ": trigger is run_attack, guard_attack, dodge, dodge_back, heavy, combo_finisher, lock_attack, evade_attack, critical, played, art, or charge_release (Toan)";
    }
    if (move.trigger == 6) {
        if (!entry.contains("chain") || !entry["chain"].is_number_integer() || entry["chain"].get<int>() < 1 || entry["chain"].get<int>() > 6) {
            return move.id + ": a combo_finisher's chain is the number of combo blows before it, 1..6";
        }
        move.chain = entry["chain"].get<int>();
    } else if (entry.contains("chain")) {
        return move.id + ": chain is for a combo_finisher";
    }
    if (move.trigger == 7) {
        const std::string direction = entry.value("direction", std::string());
        if (direction == "toward") move.direction = 0;
        else if (direction == "away") move.direction = 1;
        else return move.id + ": a lock_attack's direction is toward or away (the stick, from the target)";
    } else if (move.trigger == 3) {
        const std::string direction = entry.value("direction", std::string("any"));
        if (direction == "any") move.direction = -1;
        else if (direction == "toward") move.direction = 0;
        else if (direction == "away") move.direction = 1;
        else if (direction == "left") move.direction = 2;
        else if (direction == "right") move.direction = 3;
        else return move.id + ": a dodge's direction is any, toward, away, left or right (the stick, from the target, locked on)";
    } else if (entry.contains("direction")) {
        return move.id + ": direction is for a lock_attack or a dodge";
    }
    if (move.trigger == 9) {
        const std::string critical = entry.value("critical", std::string());
        if (critical == "backstab") move.critical = 1;
        else if (critical == "riposte") move.critical = 2;
        else return move.id + ": a critical is a backstab (behind an enemy) or a riposte (on the enemy player.critical_set armed)";
        float hold = 0.0f;
        if (!MoveNumber(entry, "reach", 4, 60, move.critical_reach) || !MoveNumber(entry, "gap", 2, 40, move.critical_gap) ||
            !MoveNumber(entry, "arc", 20, 180, move.critical_arc) || !MoveNumber(entry, "hold", 0, 600, hold)) {
            return move.id + ": a critical has reach 4..60, gap 2..40, arc 20..180 degrees and hold 0..600 steps";
        }
        move.critical_hold = static_cast<int>(hold);
        if (entry.contains("pairs") && !(entry["pairs"].is_object() && entry["pairs"].empty())) {
            if (!entry["pairs"].is_array() || entry["pairs"].size() > 32) return move.id + ": pairs is up to 32 {model, animation, speed?}";
            for (const Json &p : entry["pairs"]) {
                PlayerMove::CriticalPair pair;
                if (!p.is_object() || !p.contains("model") || !p["model"].is_number_integer() || p["model"].get<int>() < 0 ||
                    p["model"].get<int>() > 255 || !p.contains("animation") || !p["animation"].is_string() ||
                    p["animation"].get<std::string>().empty() || p["animation"].get<std::string>().size() > 104) {
                    return move.id + ": a pair is {model = a native model id, animation = a monster motion (animations.monster_register), speed?}";
                }
                for (auto field = p.begin(); field != p.end(); ++field) {
                    if (field.key() != "model" && field.key() != "animation" && field.key() != "speed") return move.id + ": a pair received an unsupported field: " + field.key();
                }
                pair.model = p["model"].get<int>();
                const std::string animation = p["animation"].get<std::string>();
                const std::size_t colon = animation.find(':');
                pair.owner = colon == std::string::npos ? move.owner : animation.substr(0, colon);
                pair.id = colon == std::string::npos ? animation : animation.substr(colon + 1);
                if (!MoveNumber(p, "speed", 0.05f, 4, pair.speed)) return move.id + ": a pair's speed is 0.05..4";
                move.pairs.push_back(std::move(pair));
            }
        }
    } else {
        for (const char *key : {"critical", "reach", "gap", "arc", "hold", "pairs"}) {
            if (entry.contains(key)) return move.id + ": critical, reach, gap, arc, hold and pairs are for a critical";
        }
    }
    if (move.trigger == 11) {
        if (entry.contains("weapons") && !(entry["weapons"].is_object() && entry["weapons"].empty())) {
            if (!entry["weapons"].is_array() || entry["weapons"].size() > 64) return move.id + ": weapons is up to 64 native weapon item numbers";
            for (const Json &w : entry["weapons"]) {
                if (!w.is_number_integer() || w.get<int>() < ITEM_WEAPON_SLOT_EMPTY || w.get<int>() > 511) {
                    return move.id + ": weapons is up to 64 native weapon item numbers (257 and up)";
                }
                move.weapons.push_back(w.get<int>());
            }
        }
        float level = 1.0f;
        if (!MoveNumber(entry, "min_level", 1, 99, level)) return move.id + ": min_level is 1..99";
        move.min_level = static_cast<int>(level);
    } else if (entry.contains("weapons") || entry.contains("min_level")) {
        return move.id + ": weapons and min_level are for an art";
    }
    float blend_in = 0.0f;
    if (!MoveNumber(entry, "blend_in", 1, 30, blend_in)) return move.id + ": blend_in is 1..30 frames";
    move.blend_in = static_cast<int>(blend_in);
    float camera = 0.0f;
    if (!MoveNumber(entry, "camera", 1, 14, camera)) return move.id + ": camera is 1..14 steps";
    move.camera = static_cast<int>(camera);
    float run_frames = static_cast<float>(move.run_frames), cooldown = 0.0f;
    if (!MoveNumber(entry, "run_frames", 1, 300, run_frames)) return move.id + ": run_frames is 1..300";
    move.run_frames = static_cast<int>(run_frames);
    if (!MoveNumber(entry, "min_charge", 0, 10, move.min_charge) || !MoveNumber(entry, "max_charge", 0, 99, move.max_charge) ||
        move.min_charge >= move.max_charge) {
        return move.id + ": min_charge and max_charge are seconds held, 0..10 and above min_charge";
    }
    if (entry.contains("animation")) {
        if (!entry["animation"].is_string() || entry["animation"].get<std::string>().empty() || entry["animation"].get<std::string>().size() > 104) {
            return move.id + ": animation names a registered mod motion";
        }
        move.animation = entry["animation"].get<std::string>();
        if (move.animation.find(':') == std::string::npos) move.animation = move.owner + ":" + move.animation;
    } else {
        if (!entry.contains("motion") || !entry["motion"].is_number_integer() || entry["motion"].get<int>() < 0 || entry["motion"].get<int>() > 63) {
            return move.id + ": motion is a native motion number 0..63, or animation names a mod motion";
        }
        move.motion = entry["motion"].get<int>();
    }
    if (!MoveNumber(entry, "from", 0, 1, move.from) || !MoveNumber(entry, "to", 0, 1, move.to) || move.from >= move.to) {
        return move.id + ": from and to are fractions of the motion, from below to";
    }
    if (!MoveNumber(entry, "speed", 0.2f, 4, move.speed)) return move.id + ": speed is 0.2..4";
    if (!MoveNumber(entry, "lunge", 0, 6, move.lunge) || !MoveNumber(entry, "lunge_decay", 0.01f, 2, move.lunge_decay)) {
        return move.id + ": lunge is 0..6 and lunge_decay 0.01..2";
    }
    if (!MoveNumber(entry, "lunge_angle", -180, 180, move.lunge_angle)) return move.id + ": lunge_angle is -180..180 degrees from the way he faces";
    if (!MoveNumber(entry, "spin", -4, 4, move.spin)) return move.id + ": spin is -4..4 turns";
    if (!MoveNumber(entry, "spin_from", 0, 1, move.spin_from) || !MoveNumber(entry, "spin_to", 0, 1, move.spin_to) || move.spin_from >= move.spin_to) {
        return move.id + ": spin_from and spin_to are fractions of the move, from below to";
    }
    if (entry.contains("invincible")) {
        if (!entry["invincible"].is_boolean()) return move.id + ": invincible is a boolean";
        move.invincible = entry["invincible"].get<bool>();
    }
    if (!MoveNumber(entry, "gauge", 0, 100, move.gauge) || !MoveNumber(entry, "wear", 0, 5, move.wear) ||
        !MoveNumber(entry, "cooldown", 0, 600, cooldown)) {
        return move.id + ": gauge is 0..100, wear 0..5 and cooldown 0..600 frames";
    }
    move.cooldown = static_cast<int>(cooldown);
    if (entry.contains("cancel") || entry.contains("cancel_after")) {
        if (move.trigger != 3 && move.trigger != 4) return move.id + ": cancel and cancel_after are for evades (dodge, dodge_back)";
        if (entry.contains("cancel")) {
            if (!entry["cancel"].is_boolean()) return move.id + ": cancel is a boolean";
            move.cancel = entry["cancel"].get<bool>();
        }
        float after = static_cast<float>(move.cancel_after);
        if (!MoveNumber(entry, "cancel_after", 0, 60, after)) return move.id + ": cancel_after is 0..60 steps after the attack's last blow";
        move.cancel_after = static_cast<int>(after);
    }
    if (entry.contains("hits") && !(entry["hits"].is_object() && entry["hits"].empty())) {
        if (!entry["hits"].is_array() || entry["hits"].size() > 12) return move.id + ": hits is up to 12 hits";
        for (const Json &h : entry["hits"]) {
            MoveHit hit;
            if (!h.is_object()) return move.id + ": each hit is a table";
            for (auto field = h.begin(); field != h.end(); ++field) {
                static const char *const kHit[] = {"at", "radius", "reach", "height", "damage", "knockback", "heavy", "charged", "burst", "burst_scale",
                                                   "shot", "count", "spread", "speed", "from", "from_ahead", "from_rise", "cell", "size"};
                if (std::find_if(std::begin(kHit), std::end(kHit), [&](const char *k) { return field.key() == k; }) == std::end(kHit)) {
                    return move.id + ": a hit received an unsupported field: " + field.key();
                }
            }
            if (!MoveNumber(h, "at", 0, 1, hit.at) || !MoveNumber(h, "radius", 1, 40, hit.radius) || !MoveNumber(h, "reach", -20, 40, hit.reach) ||
                !MoveNumber(h, "height", -10, 30, hit.height) || !MoveNumber(h, "damage", 0, 10, hit.damage) ||
                !MoveNumber(h, "knockback", 0, 10, hit.knockback) || !MoveNumber(h, "burst_scale", 0.1f, 4, hit.burst_scale)) {
                return move.id + ": a hit has at 0..1, radius 1..40, reach -20..40, height -10..30, damage 0..10, knockback 0..10";
            }
            if (h.contains("heavy") && h["heavy"].is_boolean() && h["heavy"].get<bool>()) hit.kind = 4;
            if (h.contains("shot")) {
                if (!h["shot"].is_boolean()) return move.id + ": a hit's shot is a boolean";
                hit.shot = h["shot"].get<bool>();
            }
            float count = static_cast<float>(hit.count);
            if (!MoveNumber(h, "count", 1, 5, count) || !MoveNumber(h, "spread", 0, 45, hit.spread) || !MoveNumber(h, "speed", 1, 10, hit.speed)) {
                return move.id + ": a shot has count 1..5, spread 0..45 degrees and speed 1..10";
            }
            hit.count = static_cast<int>(count);
            if (!hit.shot && (h.contains("count") || h.contains("spread") || h.contains("speed"))) return move.id + ": count, spread and speed are for a shot";
            if (!hit.shot && (h.contains("from") || h.contains("from_ahead") || h.contains("from_rise") || h.contains("cell") || h.contains("size"))) {
                return move.id + ": from, from_ahead, from_rise, cell and size are for a shot";
            }
            if (h.contains("from")) {
                if (!h["from"].is_string() || h["from"].get<std::string>().empty() || h["from"].get<std::string>().size() > 15) {
                    return move.id + ": from names a frame of the character's model (up to 15 letters)";
                }
                hit.from = h["from"].get<std::string>();
            }
            float cell = static_cast<float>(hit.cell);
            if (!MoveNumber(h, "from_ahead", -10, 10, hit.from_ahead) || !MoveNumber(h, "from_rise", -10, 10, hit.from_rise) ||
                !MoveNumber(h, "cell", 0, 15, cell) || !MoveNumber(h, "size", 0.3f, 4, hit.size)) {
                return move.id + ": a shot has from_ahead and from_rise -10..10, cell 0..15 and size 0.3..4";
            }
            hit.cell = static_cast<int>(cell);
            if (h.contains("charged")) {
                if (!h["charged"].is_boolean()) return move.id + ": charged is a boolean";
                hit.charged = h["charged"].get<bool>();
            }
            if (h.contains("burst")) {
                hit.burst = h["burst"].is_string() ? h["burst"].get<std::string>() : std::string("?");
                if (hit.burst != "explosion" && hit.burst != "heal" && hit.burst != "sparks" && hit.burst != "guard" && hit.burst != "impact" && hit.burst != "smoke") {
                    return move.id + ": burst is explosion, heal, sparks, guard or impact";
                }
            }
            move.hits.push_back(std::move(hit));
        }
    }
    if (entry.contains("cues") && !(entry["cues"].is_object() && entry["cues"].empty())) {
        if (!entry["cues"].is_array() || entry["cues"].size() > 12) return move.id + ": cues is up to 12 cues";
        for (const Json &c : entry["cues"]) {
            MoveCue cue;
            if (!c.is_object() || !MoveNumber(c, "at", 0, 1, cue.at)) return move.id + ": each cue is {at = 0..1, sound?, trail?}";
            for (auto field = c.begin(); field != c.end(); ++field) {
                if (field.key() != "at" && field.key() != "sound" && field.key() != "trail") return move.id + ": a cue received an unsupported field: " + field.key();
            }
            if (c.contains("sound") && (!c["sound"].is_string() || (cue.sound = MoveSound(c["sound"].get<std::string>())) < 0)) {
                return move.id + ": sound is swing, swing_heavy, voice, voice_2, voice_3, shout, shout_2 or action";
            }
            if (c.contains("trail") && (!c["trail"].is_string() || (cue.trail = MoveTrail(c["trail"].get<std::string>())) < 0)) {
                return move.id + ": trail is light, wide, heavy, wave or slam";
            }
            move.cues.push_back(cue);
        }
    }
    if (entry.contains("variants") && !(entry["variants"].is_object() && entry["variants"].empty())) {
        if (move.animation.empty()) return move.id + ": variants are for a move that plays an animation";
        if (!entry["variants"].is_array() || entry["variants"].size() > 4) return move.id + ": variants is up to 4 variants";
        for (const Json &v : entry["variants"]) {
            MoveVariant variant;
            const char *const shape = ": each variant is {animation, motion = native motion 0..63, from = 0..1, to = 0..1}";
            if (!v.is_object() || !v.contains("animation") || !v["animation"].is_string() || v["animation"].get<std::string>().empty() ||
                v["animation"].get<std::string>().size() > 104 || !v.contains("motion") || !v["motion"].is_number_integer() ||
                v["motion"].get<int>() < 0 || v["motion"].get<int>() > 63) {
                return move.id + shape;
            }
            for (auto field = v.begin(); field != v.end(); ++field) {
                if (field.key() != "animation" && field.key() != "motion" && field.key() != "from" && field.key() != "to") {
                    return move.id + ": a variant received an unsupported field: " + field.key();
                }
            }
            if (!MoveNumber(v, "from", 0, 1, variant.from) || !MoveNumber(v, "to", 0, 1, variant.to) || variant.from >= variant.to) {
                return move.id + ": a variant's from and to are fractions of its motion, from below to";
            }
            variant.animation = v["animation"].get<std::string>();
            if (variant.animation.find(':') == std::string::npos) variant.animation = move.owner + ":" + variant.animation;
            variant.motion = v["motion"].get<int>();
            move.variants.push_back(std::move(variant));
        }
    }
    return {};
}

ApiCallResult SetPlayerMoves(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("moves") || !args[0]["moves"].is_array()) {
        return Invalid("expected {moves = {...}}");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "moves") return Invalid("player.moves_set received an unsupported field: " + field.key());
    }
    const Json &list = args[0]["moves"];
    if (list.size() > 160) return Invalid("a mod sets up to 160 moves");
    std::vector<PlayerMove> moves;
    for (const Json &entry : list) {
        PlayerMove move;
        move.owner = std::string(owner);
        if (const std::string error = ReadMove(entry, move); !error.empty()) return Invalid(error);
        for (const PlayerMove &other : g_player_moves) {
            if (move.trigger != 10 && move.trigger != 11 && other.owner != owner && other.chara == move.chara && other.trigger == move.trigger && other.chain == move.chain &&
                other.direction == move.direction && other.critical == move.critical) {
                return Failed(move.id + ": another mod already gives this character a move on that trigger");
            }
        }
        moves.push_back(std::move(move));
    }
    std::erase_if(g_player_moves, [&](const PlayerMove &m) { return m.owner == owner; });
    for (PlayerMove &move : moves) g_player_moves.push_back(std::move(move));
    return {ApiCallStatus::Ok, {{"moves", list.size()}}, {}};
}

ApiCallResult PlayPlayerMove(std::string_view owner, const Json &args); // after StartPlayerMove, below

ApiCallResult ClearPlayerMoves(std::string_view owner, const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("player.moves_clear takes no arguments");
    std::erase_if(g_player_moves, [&](const PlayerMove &m) { return m.owner == owner; });
    return {ApiCallStatus::Ok, Json::object(), {}};
}

// player.form_set: the active character becomes another model for a while (a mod's pack: e.g. Opar's monster form).
// His model reloads from the form's pack where he stands, with the frames the player code looks up by name found
// under their stand-ins; his action, weapon stats and moves stay his. player.form_clear gives his own model back.
// Either waits, like a character switch, for the game to let the model change (Failed: try again shortly).
std::string FormReloadBlocked() {
    if (UserStatus == nullptr) return "the game has no active save data";
    if (!IsActiveDungeonStatus(CurrentStatus())) return "a form needs an active dungeon scene";
    if (BtActStatus.action_on != 0 || BtActStatus.movement_locked != 0) return "the native game is in an action";
    if (gameTask != GAME_TASK_PLAY) return "a scene is playing";
    if (g_character_switch_routine == nullptr) return "the native character switch routine is unavailable";
    return {};
}

bool IsFrameName(const std::string &name) {
    return !name.empty() && name.size() <= 15 &&
           std::ranges::all_of(name, [](unsigned char c) { return std::isalnum(c) != 0 || c == '_'; });
}

ApiCallResult SetPlayerForm(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {pack, frames?, hide_weapon?, scale?}");
    const Json &a = args[0];
    for (auto field = a.begin(); field != a.end(); ++field) {
        if (field.key() != "pack" && field.key() != "frames" && field.key() != "hide_weapon" && field.key() != "scale") {
            return Invalid("player.form_set received an unsupported field: " + field.key());
        }
    }
    dc::game::CharaForm form;
    form.owner = std::string(owner);
    if (!a.contains("pack") || !a["pack"].is_string()) return Invalid("pack must be dun/mainchara/<stem>.chr");
    form.pack = a["pack"].get<std::string>();
    if (!form.pack.starts_with("dun/mainchara/") || !form.pack.ends_with(".chr") || form.pack.size() > 40 ||
        !IsFrameName(form.pack.substr(14, form.pack.size() - 18))) {
        return Invalid("pack must be dun/mainchara/<stem>.chr");
    }
    if (a.contains("frames")) {
        if (!a["frames"].is_object() || a["frames"].size() > 16) return Invalid("frames must map up to 16 frame names to stand-ins");
        for (auto it = a["frames"].begin(); it != a["frames"].end(); ++it) {
            if (!it.value().is_string() || !IsFrameName(it.key()) || !IsFrameName(it.value().get<std::string>())) {
                return Invalid("frames must map frame names to frame names (letters, digits, _; up to 15)");
            }
            form.frames.emplace_back(it.key(), it.value().get<std::string>());
        }
    }
    if (a.contains("hide_weapon")) {
        if (!a["hide_weapon"].is_boolean()) return Invalid("hide_weapon must be a boolean");
        form.hide_weapon = a["hide_weapon"].get<bool>();
    }
    if (a.contains("scale")) {
        if (!a["scale"].is_number() || !(a["scale"].get<double>() >= 0.1 && a["scale"].get<double>() <= 4.0)) {
            return Invalid("scale must be 0.1-4");
        }
        form.scale = static_cast<float>(a["scale"].get<double>());
    }
    if (const std::string blocked = FormReloadBlocked(); !blocked.empty()) return Failed(blocked);
    const int chara = UserStatus->cur_chara;
    const dc::game::CharaForm *held = dc::game::ActiveCharaForm(chara);
    if (held != nullptr && held->owner != owner) return Failed("another mod's form holds this character");
    dc::game::SetCharaForm(chara, std::move(form));
    g_character_switch_routine(chara, 1);
    return {ApiCallStatus::Ok, {{"character", kCharacterIds[chara]}, {"pack", dc::game::CharaPackPath(chara)}}, {}};
}

ApiCallResult ClearPlayerForm(std::string_view owner, const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("player.form_clear takes no arguments");
    if (UserStatus == nullptr) return Failed("the game has no active save data");
    const int chara = UserStatus->cur_chara;
    const dc::game::CharaForm *held = dc::game::ActiveCharaForm(chara);
    if (held == nullptr || held->owner != owner || !IsActiveDungeonStatus(CurrentStatus())) {
        const bool changed = dc::game::ClearCharaForm(-1, owner); // out of a dungeon, or benched: it ends quietly
        return {ApiCallStatus::Ok, {{"changed", changed}}, {}};
    }
    if (const std::string blocked = FormReloadBlocked(); !blocked.empty()) return Failed(blocked);
    dc::game::ClearCharaForm(-1, owner);
    g_character_switch_routine(chara, 1);
    return {ApiCallStatus::Ok, {{"changed", true}}, {}};
}

// dungeons.set_character_blocks: only the named characters' own doors and parts go on new floors.
std::string g_chara_block_owner;
ApiCallResult SetCharacterBlocks(std::string_view owner, const Json &args) {
    const CharacterIdView &kNames = kCharacterIds;
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {characters = {\"toan\", \"xiao\", ...}}, or {} for the retail rule");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "characters") return Invalid("dungeons.set_character_blocks received an unsupported field: " + field.key());
    }
    if (!g_chara_block_owner.empty() && g_chara_block_owner != owner) return Failed("another mod already sets the character blocks");
    if (!args[0].contains("characters")) {
        g_chara_block_owner.clear();
        DngCharaBlockMask = -1;
        return {ApiCallStatus::Ok, {{"characters", nullptr}}, {}};
    }
    const Json &list = args[0]["characters"];
    if (!list.is_array() || list.size() > kCharacterIds.size()) return Invalid("characters must list up to one id per playable character");
    int mask = 0;
    Json names = Json::array();
    for (const Json &entry : list) {
        int found = -1;
        for (int c = 0; c < CharaCount(); ++c) {
            if (entry.is_string() && (entry.get<std::string>() == kNames[c] || entry.get<std::string>() == std::string("chronicle:character:") + kNames[c])) found = c;
        }
        if (found < 0) return Invalid("characters are toan, xiao, goro, ruby, ungaga, osmond or a registered playable character");
        mask |= 1 << found;
        names.push_back(kNames[found]);
    }
    g_chara_block_owner = std::string(owner);
    DngCharaBlockMask = mask;
    return {ApiCallStatus::Ok, {{"characters", names}}, {}};
}

ApiCallResult SetWardenRoom(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {enabled = boolean, lock_in = optional boolean}");
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "enabled" && field.key() != "lock_in") return Invalid("dungeons.set_warden_room received an unsupported field: " + field.key());
    }
    if (!g_warden_room_owner.empty() && g_warden_room_owner != owner) return Failed("another mod already seals the Warden room");
    if (!args[0].contains("enabled") || !args[0]["enabled"].is_boolean()) return Invalid("enabled must be a boolean");
    if (args[0].contains("lock_in") && !args[0]["lock_in"].is_boolean()) return Invalid("lock_in must be a boolean");
    const bool enabled = args[0]["enabled"].get<bool>();
    DngWardenRoomEnabled = enabled ? 1 : 0;
    DngWardenRoomLockIn = enabled && args[0].value("lock_in", false) ? 1 : 0;
    if (enabled) {
        g_warden_room_owner = std::string(owner);
    } else {
        g_warden_room_owner.clear();
    }
    return {ApiCallStatus::Ok, WardenRoomJson(), {}};
}

// dungeons.set_zones (Vigil World): floors built from now on are a chain of zones joined by locked gates
// (moonmod/dungeonmap_layout.hpp). One mod at a time; {} or unloading it turns zones off.
std::string g_zones_owner;
Json ZonesRequestJson() {
    Json rooms = Json::array(), open = Json::array(), sources = Json::array(), dungeons = Json::array();
    for (int z = 0; z < DngZoneCount; ++z) {
        rooms.push_back(DngZoneRooms[z]);
        sources.push_back(DngZoneSourceFloor[z]);
        dungeons.push_back(DngZoneSourceDungeon[z]);
        if (z > 0 && (DngZoneOpenMask & (1 << z)) != 0) open.push_back(z);
    }
    Json quiet = Json::array(), heal = Json::array(), arena = Json::array();
    for (int z = 0; z < DngZoneCount; ++z) {
        if ((DngZoneQuietMask & (1 << z)) != 0) quiet.push_back(z);
        if ((DngZoneHealMask & (1 << z)) != 0) heal.push_back(z);
        if ((DngZoneArenaMask & (1 << z)) != 0) arena.push_back(z);
    }
    Json smoke = Json::array();
    for (int z = 1; z < DngZoneCount; ++z) {
        if ((DngZoneSmokeMask & (1 << z)) != 0) smoke.push_back(z);
    }
    return {{"zones", DngZoneCount}, {"rooms", rooms}, {"open", open}, {"shrines", DngZoneShrines != 0}, {"source_floors", sources}, {"source_dungeons", dungeons},
            {"exit_floor", DngZoneExitFloor}, {"smoke", smoke}, {"shrine_heal", DngZoneShrineHeal != 0},
            {"sanctuary", DngZoneSanctuary != 0}, {"descent", DngZoneDescent}, {"quiet", quiet}, {"heal", heal}, {"arena", arena}, {"vaults", DngZoneVaults != 0}};
}
ApiCallResult SetZones(std::string_view owner, const Json &args) {
    const std::string usage = "expected {zones = {{rooms = 5}, {rooms = 5}}, open = {1}} with 2-" + std::to_string(DNG_ZONE_MAX) +
                              " zones of 2-12 rooms (open: gates already opened, 1 = the gate into the second zone; shrines = true: "
                              "a spring in each zone's shrine room; shrine_heal = false: those springs refill water only; sanctuary = true: "
                              "no monster steps near a shrine; smoke: gates that are smoke, opened by no key; descent: units each zone lies below "
                              "the one before, a ramp down outside each gate; per zone, quiet = true: none of the floor's monsters, "
                              "heal = true: its spring heals, arena = true: 6x6 rooms; vaults = false: no way down to the back side), or {}";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
    if (!g_zones_owner.empty() && g_zones_owner != owner) return Failed("another mod already lays floors out in zones");
    const Json &request = args[0];
    if (request.empty()) {
        g_zones_owner.clear();
        DngZoneCount = 0;
        DngZoneOpenMask = 0;
        DngZoneShrines = 0;
        DngZoneShrineHeal = 1;
        DngZoneSanctuary = 0;
        DngZoneSmokeMask = 0;
        DngZoneDescent = 0;
        DngZoneExitFloor = -1;
        DngZoneQuietMask = DngZoneHealMask = DngZoneArenaMask = 0;
        DngZoneVaults = 1;
        std::fill(std::begin(DngZoneSourceFloor), std::end(DngZoneSourceFloor), -1);
        std::fill(std::begin(DngZoneSourceDungeon), std::end(DngZoneSourceDungeon), -1);
        return {ApiCallStatus::Ok, ZonesRequestJson(), {}};
    }
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "zones" && field.key() != "open" && field.key() != "shrines" && field.key() != "exit_floor" &&
            field.key() != "smoke" && field.key() != "shrine_heal" && field.key() != "sanctuary" && field.key() != "descent" &&
            field.key() != "vaults") {
            return Invalid("dungeons.set_zones received an unsupported field: " + field.key());
        }
    }
    if (request.contains("shrines") && !request["shrines"].is_boolean()) return Invalid("shrines is true or false");
    if (request.contains("shrine_heal") && !request["shrine_heal"].is_boolean()) return Invalid("shrine_heal is true or false");
    if (request.contains("sanctuary") && !request["sanctuary"].is_boolean()) return Invalid("sanctuary is true or false");
    if (request.contains("vaults") && !request["vaults"].is_boolean()) return Invalid("vaults is true or false");
    int descent = 0;
    if (request.contains("descent") && !ReadBoundedInt(request, "descent", 0, 120, descent)) {
        return Invalid("descent is how far (0-120 units) each zone lies below the one before; 0 keeps the floor level");
    }
    int exit_floor = -1;
    if (request.contains("exit_floor") && !ReadBoundedInt(request, "exit_floor", 0, 99, exit_floor)) {
        return Invalid("exit_floor is the floor (from 0) the exit gate leads to");
    }
    if (!request.contains("zones") || !request["zones"].is_array() || request["zones"].size() < 2 || request["zones"].size() > DNG_ZONE_MAX) {
        return Invalid(usage);
    }
    int rooms[DNG_ZONE_MAX] = {};
    int sources[DNG_ZONE_MAX];
    int source_dungeons[DNG_ZONE_MAX];
    std::fill(std::begin(sources), std::end(sources), -1);
    std::fill(std::begin(source_dungeons), std::end(source_dungeons), -1);
    int total = 0, quiet = 0, heal = 0, arena = 0;
    const int count = static_cast<int>(request["zones"].size());
    for (int z = 0; z < count; ++z) {
        const Json &zone = request["zones"][z];
        if (!zone.is_object() || !ReadBoundedInt(zone, "rooms", 2, 12, rooms[z])) return Invalid(usage);
        for (auto field = zone.begin(); field != zone.end(); ++field) {
            if (field.key() != "rooms" && field.key() != "source_floor" && field.key() != "source_dungeon" && field.key() != "quiet" &&
                field.key() != "heal" && field.key() != "arena") {
                return Invalid("a zone takes rooms, source_floor, source_dungeon, quiet, heal and arena, not " + field.key());
            }
        }
        for (const char *flag : {"quiet", "heal", "arena"}) {
            if (zone.contains(flag) && !zone[flag].is_boolean()) return Invalid(std::string(flag) + " is true or false");
        }
        if (zone.value("quiet", false)) quiet |= 1 << z;
        if (zone.value("heal", false)) heal |= 1 << z;
        if (zone.value("arena", false)) arena |= 1 << z;
        if (zone.contains("source_floor") && !ReadBoundedInt(zone, "source_floor", 0, 99, sources[z])) {
            return Invalid("source_floor is a floor of the dungeon (from 0) whose monsters, atla and chest items the zone takes");
        }
        if (zone.contains("source_dungeon") && !ReadBoundedInt(zone, "source_dungeon", 0, 6, source_dungeons[z])) {
            return Invalid("source_dungeon is a dungeon (0-6) whose rooms, corridors and monsters the zone is built from");
        }
        total += rooms[z];
    }
    if (total > DNG_ROOM_MAX - 1) return Invalid("the zones hold more rooms than a floor can (" + std::to_string(DNG_ROOM_MAX - 1) + ")");
    int open = 0;
    if (request.contains("open")) {
        const Json &list = request["open"];
        if (!(list.is_array() || (list.is_object() && list.empty()))) return Invalid(usage);
        for (const Json &gate : list) {
            if (!gate.is_number_integer() || gate.get<int>() < 1 || gate.get<int>() >= count) return Invalid("open lists gates 1 to zones - 1");
            open |= 1 << gate.get<int>();
        }
    }
    int smoke = 0;
    if (request.contains("smoke")) {
        const Json &list = request["smoke"];
        if (!(list.is_array() || (list.is_object() && list.empty()))) return Invalid(usage);
        for (const Json &gate : list) {
            if (!gate.is_number_integer() || gate.get<int>() < 1 || gate.get<int>() >= count) return Invalid("smoke lists gates 1 to zones - 1");
            smoke |= 1 << gate.get<int>();
        }
    }
    g_zones_owner = std::string(owner);
    DngZoneCount = count;
    for (int z = 0; z < DNG_ZONE_MAX; ++z) {
        DngZoneRooms[z] = z < count ? rooms[z] : 0;
        DngZoneSourceFloor[z] = z < count ? sources[z] : -1;
        DngZoneSourceDungeon[z] = z < count ? source_dungeons[z] : -1;
    }
    DngZoneOpenMask = open;
    DngZoneQuietMask = quiet;
    DngZoneHealMask = heal;
    DngZoneArenaMask = arena;
    DngZoneVaults = request.value("vaults", true) ? 1 : 0;
    DngZoneShrines = request.value("shrines", false) ? 1 : 0;
    DngZoneShrineHeal = request.value("shrine_heal", true) ? 1 : 0;
    DngZoneSanctuary = request.value("sanctuary", false) ? 1 : 0;
    DngZoneSmokeMask = smoke;
    DngZoneDescent = descent;
    DngZoneExitFloor = exit_floor;
    return {ApiCallStatus::Ok, ZonesRequestJson(), {}};
}
// The zone of the cell under a world position, or -1.
int ZoneAt(const float *at) {
    const int col = static_cast<int>((at[0] + 80.0f) / 160.0f), row = static_cast<int>((at[2] + 80.0f) / 160.0f);
    if (col < 0 || row < 0 || col >= DNG_GRID || row >= DNG_GRID) return -1;
    const int zone = DngZoneOfCell[col + row * DNG_GRID];
    return zone == DNG_ZONE_NONE ? -1 : zone;
}
bool ZonedFloorShown() {
    return mode == GAME_MODE_DUNGEON && NowDngMap == &MainDungeonMap && DngZoneFloorCount > 0 && DngZoneFloorSeed == NowDngMap->map_seed;
}
// dungeons.zone_gate_open: the zones' mod opens the gate into a zone (a Warden's way on, once it falls), keyed or smoke.
ApiCallResult OpenZoneGate(std::string_view owner, const Json &args) {
    int zone = -1;
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !ReadBoundedInt(args[0], "zone", 1, DNG_ZONE_MAX - 1, zone)) {
        return Invalid("dungeons.zone_gate_open takes {zone = 1-" + std::to_string(DNG_ZONE_MAX - 1) + "}: the zone the gate leads into");
    }
    // (the floor keeps its zones after its mod lets the request go: only another mod's request stands in the way)
    if (!g_zones_owner.empty() && g_zones_owner != owner) return Failed("another mod lays floors out in zones");
    if (!ZonedFloorShown()) return Failed("the player is not on a floor built in zones");
    for (int g = 0; g < DngZoneGateCount; ++g) {
        DNG_ZONE_GATE &gate = DngZoneGateTable[g];
        if (gate.zone != zone) continue;
        if (!gate.locked) return {ApiCallStatus::Ok, {{"opened", false}}, {}};
        gate.locked = 0;
        MAP_CELL &cell = NowDngMap->cells[gate.x + gate.y * DNG_GRID];
        cell.parts_no = gate.open_part;
        cell.direction = gate.open_direction;
        SndSePlay(SE_ITEM_GET, -1, 0);
        SharedRuntime().QueueEvent("game.dungeon.zone_gate_opened", {{"zone", gate.zone}, {"key", nullptr}, {"x", gate.x}, {"y", gate.y},
                                                                     {"frame", SharedRuntime().Frame()}});
        return {ApiCallStatus::Ok, {{"opened", true}, {"x", gate.x}, {"y", gate.y}}, {}};
    }
    return Failed("no gate leads into that zone");
}
// dungeons.zones_get: the zones of the floor the player is on, its gates, and the zone the player stands in.
ApiCallResult GetZones() {
    Json result = ZonesRequestJson();
    result["floor_zones"] = ZonedFloorShown() ? DngZoneFloorCount : 0;
    Json gates = Json::array();
    if (ZonedFloorShown()) {
        for (int g = 0; g < DngZoneGateCount; ++g) {
            const DNG_ZONE_GATE &gate = DngZoneGateTable[g];
            Json entry = {{"zone", gate.zone}, {"x", gate.x}, {"y", gate.y}, {"locked", gate.locked != 0}, {"smoke", gate.key_item < 0}};
            if (gate.key_item >= 0) {
                entry["key"] = ItemStableId(gate.key_item);
                entry["key_item"] = gate.key_item;
            }
            // the monster carrying its key, while it lives
            if (gate.locked && gate.key_item >= 0 && NowMonstorUnit != nullptr) {
                PrepareMonsterHandleUnit(NowMonstorUnit);
                for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) {
                    const MONSTOR &monster = NowMonstorUnit->monster[slot];
                    if (monster.state == -1 || monster.hp <= 0 || monster.drop_item != gate.key_item) continue; // asleep counts
                    entry["carrier"] = MonsterHandleId(slot, EnsureMonsterHandleGeneration(NowMonstorUnit, slot));
                }
            }
            gates.push_back(entry);
        }
        Json shrines = Json::array();
        for (int z = 0; z < DngZoneFloorCount; ++z) {
            if (DngZoneShrineX[z] >= 0) shrines.push_back({{"zone", z}, {"x", DngZoneShrineX[z]}, {"y", DngZoneShrineY[z]}});
        }
        result["shrines"] = shrines;
        Json floors = Json::array();
        for (int z = 0; z < DngZoneFloorCount; ++z) floors.push_back(DngZoneFloorSource[z] >= 0 ? DngZoneFloorSource[z] : UserStatus->cur_floor);
        result["zone_floors"] = floors;
        Json zone_dungeons = Json::array();
        for (int z = 0; z < DngZoneFloorCount; ++z) {
            zone_dungeons.push_back(DngZoneFloorDungeon[z] >= 0 && DngZoneDungeonsShown(NowDngMap->map_seed) ? DngZoneFloorDungeon[z] : selectMapNo);
        }
        result["zone_dungeons"] = zone_dungeons;
        // Height steps (set_zones descent): each zone's floor height and the ramp cells down into it.
        if (DngHeightsOn(NowDngMap)) {
            Json heights = Json::array(), ramps = Json::array();
            for (int z = 0; z < DngZoneFloorCount; ++z) {
                float y = 0.0f;
                for (int cell = 0; cell < DNG_CELLS; ++cell) {
                    if (DngZoneOfCell[cell] == z && DngCellRamp[cell] == 0) {
                        y = DngCellY0[cell];
                        break;
                    }
                }
                heights.push_back(y);
            }
            for (int cell = 0; cell < DNG_CELLS; ++cell) {
                if (DngCellRamp[cell] != 0) ramps.push_back({{"x", cell % DNG_GRID}, {"y", cell / DNG_GRID}, {"from", DngCellY0[cell]}, {"to", DngCellY1[cell]}});
            }
            result["heights"] = heights;
            result["ramps"] = ramps;
        }
        if (CharaMain.frame != nullptr) {
            float at[4];
            CharaMain.GetPosition(at);
            const int zone = ZoneAt(at);
            if (zone >= 0) result["current"] = zone;
        }
    }
    result["gates"] = gates;
    return {ApiCallStatus::Ok, result, {}};
}

// dungeons.set_exits (Vigil World): {town = "shrines"} keeps the party from leaving a dungeon for town by the Escape
// menu or the exit gate's continue/leave window (it answers continue); the mod's shrines are the way out. One mod at
// a time; {town = "any"}, {} or unloading it gives the native exits back.
std::string g_exits_owner;
ApiCallResult SetExits(std::string_view owner, const Json &args) {
    const std::string usage = "expected {town = \"shrines\"} or {town = \"any\"}";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
    if (!g_exits_owner.empty() && g_exits_owner != owner) return Failed("another mod already holds the dungeon exits");
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "town") return Invalid("dungeons.set_exits received an unsupported field: " + field.key());
    }
    const std::string town = request.contains("town") && request["town"].is_string() ? request["town"].get<std::string>() : "any";
    if (request.contains("town") && town != "shrines" && town != "any") return Invalid(usage);
    if (town == "shrines") {
        g_exits_owner = std::string(owner);
    } else {
        g_exits_owner.clear();
    }
    return {ApiCallStatus::Ok, {{"town", town}}, {}};
}

// Where dungeons.enter_floor puts the player once the floor it asked for is built and shown.
struct FloorArrival {
    int dungeon = -1, floor = -1, x = -1, y = -1;
    bool built = false;
    int ticks = 0;
    int zone = -1; // dungeons.set_arrival: in front of this zone's shrine (or at its gate), on whichever floor is built next
    bool teleport = false; // no stairs walk-in: the player appears in the escape light
};
std::optional<FloorArrival> g_arrival;
// The shrine the player was put on: no game.dungeon.shrine_reached for it until they walk away.
bool g_shrine_quiet = false;

// A floor cell the player can stand on.
bool StandableCell(int x, int y) {
    if (NowDngMap == nullptr || x < 0 || y < 0 || x >= DNG_GRID || y >= DNG_GRID) return false;
    return NowDngMap->cells[x + y * DNG_GRID].parts_no != MAP_PARTS_NONE;
}

// Puts the player in the middle of a floor cell facing north (where a room's spring stands, against its top wall); the
// camera and the map's draw distances follow.
void PlaceOnCell(int x, int y) {
    TeleportPlayer(160.0f * static_cast<float>(x), 160.0f * static_cast<float>(y));
    CharaMain.SetRotation(0.0f, PI, 0.0f);
    CharaMain.frame->SetRotation(0.0f, PI, 0.0f);
    if (NowCamera__3 != nullptr) {
        NowCamera__3->SetAngleSoon(0.0f);
        NowCamera__3->Step(-1);
    }
    for (int row = 0; row < DNG_GRID; ++row) {
        for (int col = 0; col < DNG_GRID; ++col) {
            NowDngMap->cells[col + row * DNG_GRID].camera_dist = std::hypot(160.0f * static_cast<float>(col - x), 160.0f * static_cast<float>(row - y));
        }
    }
    g_shrine_quiet = true;
}

// dungeons.warp: the player to a cell of the floor they are on, at once (teleport = true: in the escape light).
ApiCallResult WarpPlayer(const Json &args) {
    const std::string usage = "expected {x = column, y = row, teleport = true or not} of a floor cell";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
    const bool has_teleport = args[0].contains("teleport");
    if (args[0].size() != (has_teleport ? 3u : 2u) || (has_teleport && !args[0]["teleport"].is_boolean())) return Invalid(usage);
    int x = 0, y = 0;
    if (!ReadBoundedInt(args[0], "x", 0, DNG_GRID - 1, x) || !ReadBoundedInt(args[0], "y", 0, DNG_GRID - 1, y)) return Invalid(usage);
    if (mode != GAME_MODE_DUNGEON || NowDngMap == nullptr || CharaMain.frame == nullptr) return Failed("the player is not on a dungeon floor");
    if (!StandableCell(x, y)) return Failed("nothing to stand on at that cell");
    PlaceOnCell(x, y);
    if (has_teleport && args[0]["teleport"].get<bool>() && g_teleport_in == -1) StartTeleportLight();
    return {ApiCallStatus::Ok, {{"x", x}, {"y", y}}, {}};
}

// dungeons.drop_gilda: a pile of gold on the floor the player is on, at a world position, as a monster that stole
// Gilda drops it: the player takes it by walking within about half a cell of it (it shows on the corner map). Gone when the floor is built
// again; up to 32 things lie on a floor at once.
ApiCallResult DropGilda(const Json &args) {
    const std::string usage = "expected {x = number, y = number, z = number, amount = 1-9999999} (a world position, 160 to a cell)";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 4) return Invalid(usage);
    const Json &request = args[0];
    float at[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    const char *axes[3] = {"x", "y", "z"};
    for (int i = 0; i < 3; ++i) {
        if (!request.contains(axes[i]) || !request[axes[i]].is_number()) return Invalid(usage);
        at[i] = request[axes[i]].get<float>();
        if (!std::isfinite(at[i])) return Invalid(usage);
    }
    int amount = 0;
    if (!ReadBoundedInt(request, "amount", 1, 9999999, amount)) return Invalid(usage);
    if (mode != GAME_MODE_DUNGEON || RandomItem == nullptr || NowDngMap == nullptr) return Failed("the player is not on a dungeon floor");
    if (RandomItem->CheckID() < 0) return Failed("the floor already holds as many dropped things as it can");
    RandomItem->Set(at, 2, amount, -1); // id 2: taken from a step away (CRandomItem::CheckPosition)
    return {ApiCallStatus::Ok, {{"amount", amount}}, {}};
}

// dungeons.enter_floor: the dungeon's floor floor_id built again from the start (the floor the player is on too: its
// monsters come back), the player arriving at cell x, y when given. The floor is built as any other: a mod that keeps
// floors (dungeons.set_seed, set_opened_chests, set_zones open) gives it back as it was left.
ApiCallResult EnterFloor(const Json &args) {
    const std::string usage = "expected {floor_id = n, x = column, y = row, dungeon_id = n, teleport = true} (x and y optional: where the player arrives; teleport: "
                              "with x and y, in the escape light instead of down the stairs; dungeon_id: "
                              "from a town, the dungeon to enter)";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "floor_id" && field.key() != "x" && field.key() != "y" && field.key() != "dungeon_id" && field.key() != "teleport") {
            return Invalid("dungeons.enter_floor received an unsupported field: " + field.key());
        }
    }
    int floor = 0, x = -1, y = -1;
    if (request.contains("x") != request.contains("y") || !request.contains("floor_id")) return Invalid(usage);
    if (request.contains("teleport") && (!request["teleport"].is_boolean() || !request.contains("x"))) return Invalid(usage);
    const bool teleport = request.contains("teleport") && request["teleport"].get<bool>();
    if (request.contains("x") && (!ReadBoundedInt(request, "x", 0, DNG_GRID - 1, x) || !ReadBoundedInt(request, "y", 0, DNG_GRID - 1, y))) {
        return Invalid(usage);
    }
    int dungeon = mode == GAME_MODE_DUNGEON ? selectMapNo : -1;
    if (request.contains("dungeon_id") && !ReadBoundedInt(request, "dungeon_id", 0, DUNGEON_COUNT - 1, dungeon)) return Invalid(usage);
    const bool in_town = mode == GAME_MODE_EDIT && IsSettlementOrRegisteredLocationMap(MapNo);
    if ((mode != GAME_MODE_DUNGEON && !in_town) || UserStatus == nullptr) return Failed("the player is not in a dungeon or a town");
    if (dungeon < 0 || dungeon >= DUNGEON_COUNT) return Failed(in_town ? "from a town, dungeon_id names the dungeon" : "the player is not in a dungeon");
    if (NextMapNo >= 0) return Failed("a native map transition is already pending");
    if (!ReadBoundedInt(request, "floor_id", 0, maxFloorTbl__3[dungeon] - 1, floor)) return Invalid(usage);
    g_pending_dungeon_floor_entry = PendingDungeonFloorEntry{dungeon, floor};
    MapJump(dungeon + 200, -1);
    g_arrival.reset();
    if (x >= 0) {
        g_arrival = FloorArrival{dungeon, floor, x, y};
        g_arrival->teleport = teleport;
    }
    return {ApiCallStatus::Ok, {{"dungeon_id", dungeon}, {"floor_id", floor}, {"queued", true}}, {}};
}

// dungeons.set_arrival: the next main floor built starts the player at cell x, y, or in front of zone `zone`'s shrine
// (just inside its gate on a floor without shrines). Called from game.dungeon.generating; {} takes it back.
ApiCallResult SetArrival(const Json &args) {
    const std::string usage = "expected {zone = 0-" + std::to_string(DNG_ZONE_MAX - 1) +
                              "} or {x = column, y = row} (each with teleport = true or not), or {}";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
    const Json &request = args[0];
    if (request.empty()) {
        g_arrival.reset();
        return {ApiCallStatus::Ok, {{"arrival", false}}, {}};
    }
    FloorArrival arrival;
    std::size_t fields = request.size();
    if (request.contains("teleport")) {
        if (!request["teleport"].is_boolean()) return Invalid(usage);
        arrival.teleport = request["teleport"].get<bool>();
        --fields;
    }
    if (fields == 1 && ReadBoundedInt(request, "zone", 0, DNG_ZONE_MAX - 1, arrival.zone)) {
        // resolved once the floor is built
    } else if (fields != 2 || !ReadBoundedInt(request, "x", 0, DNG_GRID - 1, arrival.x) || !ReadBoundedInt(request, "y", 0, DNG_GRID - 1, arrival.y)) {
        return Invalid(usage);
    }
    g_arrival = arrival;
    return {ApiCallStatus::Ok, {{"arrival", true}}, {}};
}

// dungeons.room_seal: the room the player stands in (past its edge cells, so no doorway is under them) has its
// doorways shut by gates; dungeons.room_release opens them. One shut room at a time; a new floor forgets it.
std::string g_sealed_owner;
int         g_sealed_room = -1, g_sealed_seed = -1;
void SetSealedDoors(int room, bool shut) {
    for (int d = 0; d < DngSealDoorCount; ++d) {
        const DNG_SEAL_DOOR &door = DngSealDoorTable[d];
        if (door.room != room) continue;
        MAP_CELL &cell = MainDungeonMap.cells[door.x + door.y * DNG_GRID];
        if (shut && cell.parts_no == door.open_part) {
            cell.parts_no = door.gate_part;
            cell.direction = 0;
        } else if (!shut && cell.parts_no == door.gate_part) {
            cell.parts_no = door.open_part;
            cell.direction = door.open_direction;
        }
    }
}
ApiCallResult SealRoom(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() > 1 || (args.size() == 1 && !(args[0].is_object() && args[0].empty()))) {
        return Invalid("dungeons.room_seal takes {}");
    }
    if (mode != GAME_MODE_DUNGEON || NowDngMap != &MainDungeonMap || CharaMain.frame == nullptr || BtUraDongeon != 0 ||
        DngSealRoomSeed != MainDungeonMap.map_seed) {
        return Failed("not on a built dungeon floor");
    }
    if (g_sealed_room >= 0 && g_sealed_seed == MainDungeonMap.map_seed) return Failed("a room is already shut");
    float at[4];
    CharaMain.GetPosition(at);
    const float col = at[0] / 160.0f, row = at[2] / 160.0f;
    int room = -1;
    for (int r = 0; r < DngSealRoomCount && room < 0; ++r) {
        const DNG_SEAL_ROOM &info = DngSealRoomTable[r];
        if (DngWardenRoomState != DNG_WARDEN_NONE && info.x == DngWardenRoomX && info.y == DngWardenRoomY) continue;
        const float margin = (info.w > 2 && info.h > 2) ? 0.6f : 0.2f;
        if (col >= info.x - 0.5f + margin && col <= info.x + info.w - 0.5f - margin && row >= info.y - 0.5f + margin &&
            row <= info.y + info.h - 0.5f - margin) {
            room = r;
        }
    }
    if (room < 0) return {ApiCallStatus::Ok, {{"sealed", false}}, {}};
    int doors = 0;
    for (int d = 0; d < DngSealDoorCount; ++d) doors += DngSealDoorTable[d].room == room ? 1 : 0;
    if (doors == 0) return {ApiCallStatus::Ok, {{"sealed", false}}, {}};
    SetSealedDoors(room, true);
    g_sealed_owner = std::string(owner);
    g_sealed_room = room;
    g_sealed_seed = MainDungeonMap.map_seed;
    SndSePlay(SE_ITEM_GET, -1, 0);
    const DNG_SEAL_ROOM &info = DngSealRoomTable[room];
    return {ApiCallStatus::Ok,
            {{"sealed", true}, {"doors", doors}, {"room", {{"x", info.x}, {"y", info.y}, {"w", info.w}, {"h", info.h}}}},
            {}};
}
ApiCallResult ReleaseRoom(std::string_view owner) {
    if (g_sealed_room < 0 || g_sealed_owner != owner) return {ApiCallStatus::Ok, {{"released", false}}, {}};
    if (g_sealed_seed == MainDungeonMap.map_seed && NowDngMap == &MainDungeonMap) {
        SetSealedDoors(g_sealed_room, false);
        SndSePlay(SE_ITEM_GET, -1, 0);
    }
    g_sealed_room = -1;
    g_sealed_owner.clear();
    return {ApiCallStatus::Ok, {{"released", true}}, {}};
}

// dungeons.warden_room_release: the doors that closed behind the player open again (the Warden is down).
ApiCallResult ReleaseWardenRoom(std::string_view owner) {
    if (g_warden_room_owner != owner) return Failed("this mod does not seal the Warden room");
    if (DngWardenRoomState == DNG_WARDEN_SHUT || DngWardenRoomState == DNG_WARDEN_SEALED) {
        DngWardenRoomState = DNG_WARDEN_RELEASED;
        SetWardenDoors(false);
        SndSePlay(SE_ITEM_GET, -1, 0);
    }
    return {ApiCallStatus::Ok, WardenRoomJson(), {}};
}

// dungeons.set_weapon_drops: one mod's chest weapon rules at a time; unloading it gives the retail rules back.
std::string g_weapon_drops_owner;
ApiCallResult SetWeaponDrops(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {unique = optional boolean, rare = 0..100, large_chests = 0..100}, or {} for the retail rules");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "unique" && field.key() != "rare" && field.key() != "large_chests") {
            return Invalid("dungeons.set_weapon_drops received an unsupported field: " + field.key());
        }
    }
    if (!g_weapon_drops_owner.empty() && g_weapon_drops_owner != owner) return Failed("another mod already sets the chest weapon rules");
    if (args[0].contains("unique") && !args[0]["unique"].is_boolean()) return Invalid("unique must be a boolean");
    int rare = 0, large = -1;
    if (args[0].contains("rare") && !ReadBoundedInt(args[0], "rare", 0, 100, rare)) return Invalid("rare must be an integer from 0 to 100");
    if (args[0].contains("large_chests") && !ReadBoundedInt(args[0], "large_chests", 0, 100, large)) {
        return Invalid("large_chests must be an integer from 0 to 100");
    }
    DngWeaponUnique = args[0].value("unique", false) ? 1 : 0;
    DngWeaponRarePercent = rare;
    DngLargeChestPercent = large;
    if (args[0].empty()) g_weapon_drops_owner.clear();
    else g_weapon_drops_owner = std::string(owner);
    return {ApiCallStatus::Ok, {{"unique", DngWeaponUnique != 0}, {"rare", DngWeaponRarePercent},
                                {"large_chests", DngLargeChestPercent >= 0 ? DngLargeChestPercent : 12}}, {}};
}

// dungeons.chest_weapons_add: a weapon this mod lets chests hold, on dungeon (-1 any) from floor_from on, percent of the
// time a chest there holds a weapon. dungeons.chest_weapons_clear takes this mod's back, as unloading it does.
ApiCallResult AddChestWeapon(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("item_id")) {
        return Invalid("expected {item_id = weapon, dungeon_id = optional -1..6, floor_from = optional 0..99, percent = optional 1..100}");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "item_id" && field.key() != "dungeon_id" && field.key() != "floor_from" && field.key() != "percent") {
            return Invalid("dungeons.chest_weapons_add received an unsupported field: " + field.key());
        }
    }
    int item_no = -1;
    if (!ReadWeaponNumber(args[0]["item_id"], item_no)) return Invalid("item_id must identify a Chronicle weapon");
    int dungeon = -1, floor_from = 0, percent = 10;
    if (args[0].contains("dungeon_id") && !ReadBoundedInt(args[0], "dungeon_id", -1, 6, dungeon)) {
        return Invalid("dungeon_id must be an integer from -1 (any) to 6");
    }
    if (args[0].contains("floor_from") && !ReadBoundedInt(args[0], "floor_from", 0, 99, floor_from)) {
        return Invalid("floor_from must be an integer from 0 to 99");
    }
    if (args[0].contains("percent") && !ReadBoundedInt(args[0], "percent", 1, 100, percent)) {
        return Invalid("percent must be an integer from 1 to 100");
    }
    AddMoonModChestWeapon(owner, item_no, dungeon, floor_from, percent);
    return {ApiCallStatus::Ok, {{"item_no", item_no}, {"dungeon_id", dungeon}, {"floor_from", floor_from}, {"percent", percent}}, {}};
}

// dungeons.floor_redirect: (dungeon, floor) -> {owner, floor the exit gate leads to instead}.
std::map<std::pair<int, int>, std::pair<std::string, int>> g_floor_redirects;

ApiCallResult SetFloorRedirect(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {dungeon_id = 0..4, floor_id = registered floor, to_floor_id = optional registered floor}");
    }
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "dungeon_id" && field.key() != "floor_id" && field.key() != "to_floor_id") {
            return Invalid("dungeons.floor_redirect received an unsupported field: " + field.key());
        }
    }
    int dungeon = -1, from = -1, to = -1;
    if (!ReadBoundedInt(request, "dungeon_id", 0, 4, dungeon)) return Invalid("dungeon_id must be a procedural dungeon from 0 through 4");
    const int first_added = kNativeDungeonFloorCounts[dungeon], last = maxFloorTbl__3[dungeon] - 1;
    if (!ReadBoundedInt(request, "floor_id", first_added, std::max(first_added, last), from) || from > last) {
        return Invalid("floor_id must be a floor added by dungeons.register");
    }
    const auto key = std::make_pair(dungeon, from);
    const auto existing = g_floor_redirects.find(key);
    if (existing != g_floor_redirects.end() && existing->second.first != owner) {
        return Failed("another mod already redirects this floor");
    }
    if (!request.contains("to_floor_id") || request["to_floor_id"].is_null()) {
        const bool removed = existing != g_floor_redirects.end();
        if (removed) g_floor_redirects.erase(existing);
        return {ApiCallStatus::Ok, {{"dungeon_id", dungeon}, {"floor_id", from}, {"removed", removed}}, {}};
    }
    if (!ReadBoundedInt(request, "to_floor_id", first_added, std::max(first_added, last), to) || to > last) {
        return Invalid("to_floor_id must be a floor added by dungeons.register");
    }
    g_floor_redirects[key] = {std::string(owner), to};
    return {ApiCallStatus::Ok, {{"dungeon_id", dungeon}, {"floor_id", from}, {"to_floor_id", to}}, {}};
}

// player.fatal_guard: set the charges that leave a character on 1 HP instead of a fatal hit.
ApiCallResult SetPlayerFatalGuard(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {charges = 0..99, character = optional id}");
    const Json &definition = args[0];
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "charges" && field.key() != "character") return Invalid("player.fatal_guard received an unsupported field: " + field.key());
    }
    int charges = 0;
    if (!ReadBoundedInt(definition, "charges", 0, 99, charges)) return Invalid("charges must be an integer from 0 to 99");
    int first = 0, last = CharaCount() - 1;
    if (definition.contains("character")) {
        const Json &who = definition["character"];
        const auto found = who.is_string() ? std::ranges::find(kCharacterIds, who.get<std::string>()) : kCharacterIds.end();
        if (found == kCharacterIds.end()) return Invalid("character must be a playable character id");
        first = last = static_cast<int>(found - kCharacterIds.begin());
    }
    for (int c = first; c <= last; ++c) g_fatal_guard[c] = charges;
    Json out = Json::object();
    for (int c = 0; c < CharaCount(); ++c) out[kCharacterIds[c]] = g_fatal_guard[c];
    return {ApiCallStatus::Ok, {{"charges", std::move(out)}}, {}};
}

ApiCallResult SetPlayerInvincible(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {frames = 1..600}");
    const Json &definition = args[0];
    for (auto field = definition.begin(); field != definition.end(); ++field) {
        if (field.key() != "frames") return Invalid("player.invincible received an unsupported field: " + field.key());
    }
    const auto frames_field = definition.find("frames");
    if (frames_field == definition.end() || !frames_field->is_number()) return Invalid("frames must be an integer from 1 to 600");
    const double frames = frames_field->get<double>();
    if (!std::isfinite(frames) || frames != std::floor(frames) || frames < 1.0 || frames > 600.0) {
        return Invalid("frames must be an integer from 1 to 600");
    }
    if (mode != GAME_MODE_DUNGEON) return Failed("player.invincible needs an active dungeon");
    const int previous = BtActStatus.invincible_frames;
    if (BtActStatus.invincible_frames < static_cast<int>(frames)) BtActStatus.invincible_frames = static_cast<int>(frames);
    return {ApiCallStatus::Ok, {{"frames", BtActStatus.invincible_frames}, {"previous", previous}}, {}};
}

// player.guard_rules_set / gauge_rules_set: one owner each, the game's rules again when it clears them or unloads.
std::string    g_guard_rules_owner;
GuardRules     g_guard_rules;
std::string    g_gauge_rules_owner;
GaugeRules     g_gauge_rules;
GaugeDelay     g_gauge_delay;
int            g_guard_frames = 0; // frames the guard has been up (action_on 6)
PoiseState     g_poise;            // the worn gear's poise spent by recent hits
int            g_poise_clock = 0;  // dungeon ticks, for when poise is whole again

// A number field of a rules table within [low, high], or its default when left out. False with the message set.
bool ReadRuleNumber(const Json &table, const char *key, float low, float high, float &out, std::string &error) {
    const auto field = table.find(key);
    if (field == table.end()) return true;
    const float value = field->is_number() ? field->get<float>() : std::nanf("");
    if (!std::isfinite(value) || value < low || value > high) {
        std::ostringstream text;
        text << key << " must be a number from " << low << " to " << high;
        error = text.str();
        return false;
    }
    out = value;
    return true;
}

Json GuardRulesJson() {
    if (!g_guard_rules.on) return {{"on", false}};
    return {{"on", true}, {"owner", g_guard_rules_owner}, {"arc", g_guard_rules.arc}, {"cost_base", g_guard_rules.cost_base},
            {"cost_per_damage", g_guard_rules.cost_per_damage}, {"chip", g_guard_rules.chip},
            {"break_recoil", g_guard_rules.break_recoil}, {"parry_frames", g_guard_rules.parry_frames},
            {"without_lock", g_guard_rules.without_lock}};
}

Json GaugeRulesJson() {
    if (!g_gauge_rules.on) return {{"on", false}};
    return {{"on", true}, {"owner", g_gauge_rules_owner}, {"hit_refill", g_gauge_rules.hit_refill},
            {"first_swing_cost", g_gauge_rules.first_swing_cost}, {"min_to_swing", g_gauge_rules.min_to_swing},
            {"swing_cost_scale", g_gauge_rules.swing_cost_scale}, {"refill_delay", g_gauge_rules.refill_delay}};
}

ApiCallResult SetGuardRules(std::string_view owner, const Json &args) {
    if (owner.empty() || !args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {arc = 0..360, cost_base = 0..100, cost_per_damage = 0..10, chip = 0..1, "
                       "break_recoil = 0..600, parry_frames = 0..60, without_lock = boolean}");
    }
    const Json &table = args[0];
    for (auto field = table.begin(); field != table.end(); ++field) {
        static constexpr std::array<std::string_view, 7> kFields{"arc", "cost_base", "cost_per_damage", "chip",
                                                                 "break_recoil", "parry_frames", "without_lock"};
        if (std::ranges::find(kFields, field.key()) == kFields.end()) {
            return Invalid("player.guard_rules_set received an unsupported field: " + field.key());
        }
    }
    if (!g_guard_rules_owner.empty() && g_guard_rules_owner != owner) {
        return Failed("another mod already sets the guard's rules: " + g_guard_rules_owner);
    }
    GuardRules  rules;
    std::string error;
    if (!ReadRuleNumber(table, "arc", 0.0f, 360.0f, rules.arc, error) ||
        !ReadRuleNumber(table, "cost_base", 0.0f, 100.0f, rules.cost_base, error) ||
        !ReadRuleNumber(table, "cost_per_damage", 0.0f, 10.0f, rules.cost_per_damage, error) ||
        !ReadRuleNumber(table, "chip", 0.0f, 1.0f, rules.chip, error)) {
        return Invalid(error);
    }
    if (table.contains("break_recoil") && !ReadBoundedInt(table, "break_recoil", 0, 600, rules.break_recoil)) {
        return Invalid("break_recoil must be an integer from 0 to 600");
    }
    if (table.contains("parry_frames") && !ReadBoundedInt(table, "parry_frames", 0, 60, rules.parry_frames)) {
        return Invalid("parry_frames must be an integer from 0 to 60");
    }
    if (table.contains("without_lock")) {
        if (!table["without_lock"].is_boolean()) return Invalid("without_lock must be a boolean");
        rules.without_lock = table["without_lock"].get<bool>();
    }
    rules.on = true;
    g_guard_rules = rules;
    g_guard_rules_owner = std::string(owner);
    return {ApiCallStatus::Ok, GuardRulesJson(), {}};
}

ApiCallResult SetGaugeRules(std::string_view owner, const Json &args) {
    if (owner.empty() || !args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {hit_refill = -100..100, first_swing_cost = 0..100, min_to_swing = 0..100, "
                       "swing_cost_scale = 0..4, refill_delay = 0..600}");
    }
    const Json &table = args[0];
    for (auto field = table.begin(); field != table.end(); ++field) {
        static constexpr std::array<std::string_view, 5> kFields{"hit_refill", "first_swing_cost", "min_to_swing",
                                                                 "swing_cost_scale", "refill_delay"};
        if (std::ranges::find(kFields, field.key()) == kFields.end()) {
            return Invalid("player.gauge_rules_set received an unsupported field: " + field.key());
        }
    }
    if (!g_gauge_rules_owner.empty() && g_gauge_rules_owner != owner) {
        return Failed("another mod already sets the gauge's rules: " + g_gauge_rules_owner);
    }
    GaugeRules  rules;
    std::string error;
    if (!ReadRuleNumber(table, "hit_refill", -100.0f, 100.0f, rules.hit_refill, error) ||
        !ReadRuleNumber(table, "first_swing_cost", 0.0f, 100.0f, rules.first_swing_cost, error) ||
        !ReadRuleNumber(table, "min_to_swing", 0.0f, 100.0f, rules.min_to_swing, error) ||
        !ReadRuleNumber(table, "swing_cost_scale", 0.0f, 4.0f, rules.swing_cost_scale, error)) {
        return Invalid(error);
    }
    if (table.contains("refill_delay") && !ReadBoundedInt(table, "refill_delay", 0, 600, rules.refill_delay)) {
        return Invalid("refill_delay must be an integer from 0 to 600");
    }
    rules.on = true;
    g_gauge_rules = rules;
    g_gauge_rules_owner = std::string(owner);
    return {ApiCallStatus::Ok, GaugeRulesJson(), {}};
}

ApiCallResult SetCriticalTarget(std::string_view owner, const Json &args);
ApiCallResult ClearCriticalTarget(std::string_view owner, const Json &args);
void ReleaseCriticals(std::string_view owner);

// player.guard_rules_clear / gauge_rules_clear: the caller's rules off; another mod's are left alone.
ApiCallResult ClearPlayerRules(std::string_view owner, const Json &args, bool guard) {
    if (!args.is_array() || !args.empty()) return Invalid(guard ? "player.guard_rules_clear takes no arguments"
                                                                : "player.gauge_rules_clear takes no arguments");
    std::string &rules_owner = guard ? g_guard_rules_owner : g_gauge_rules_owner;
    const bool   cleared = !rules_owner.empty() && rules_owner == owner;
    if (cleared) {
        rules_owner.clear();
        if (guard) g_guard_rules = {};
        else g_gauge_rules = {};
    }
    return {ApiCallStatus::Ok, {{"cleared", cleared}, {"rules", guard ? GuardRulesJson() : GaugeRulesJson()}}, {}};
}

ApiCallResult GetPlayerGauge(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("player.gauge_get takes no arguments");
    if (mode != GAME_MODE_DUNGEON) return Failed("player.gauge_get needs an active dungeon");
    return {ApiCallStatus::Ok,
            {{"gauge", BtActStatus.action_gauge}, {"max", 100}, {"exhausted", BtActStatus.gauge_exhausted != 0},
             {"guard_rules", GuardRulesJson()}, {"gauge_rules", GaugeRulesJson()}},
            {}};
}

// player.gauge_set: {gauge = 0..100} sets it, {add = -100..100} adds to it.
ApiCallResult SetPlayerGauge(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {gauge = 0..100} or {add = -100..100}");
    const Json &table = args[0];
    for (auto field = table.begin(); field != table.end(); ++field) {
        if (field.key() != "gauge" && field.key() != "add") return Invalid("player.gauge_set received an unsupported field: " + field.key());
    }
    if (table.contains("gauge") == table.contains("add")) return Invalid("give one of gauge (0..100) or add (-100..100)");
    float       value = 0.0f;
    std::string error;
    const bool  absolute = table.contains("gauge");
    if (!ReadRuleNumber(table, absolute ? "gauge" : "add", absolute ? 0.0f : -100.0f, 100.0f, value, error)) return Invalid(error);
    if (mode != GAME_MODE_DUNGEON) return Failed("player.gauge_set needs an active dungeon");
    const float previous = BtActStatus.action_gauge;
    BtActStatus.action_gauge = std::clamp(absolute ? value : previous + value, 0.0f, 100.0f);
    if (static_cast<int>(BtActStatus.action_gauge) >= 100) BtActStatus.gauge_exhausted = 0;
    return {ApiCallStatus::Ok, {{"gauge", BtActStatus.action_gauge}, {"previous", previous}}, {}};
}

// player.state: what the active player is doing this frame, for a HUD or a mod's rules. {active = false} away from a
// dungeon.
ApiCallResult GetPlayerState(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("player.state takes no arguments");
    if (mode != GAME_MODE_DUNGEON || UserStatus == nullptr) return {ApiCallStatus::Ok, {{"active", false}}, {}};
    const int chara = UserStatus->cur_chara;
    const bool guarding = BtActStatus.action_on == 6;
    Json state = {{"active", true},
                  {"character", chara >= 0 && chara < CharaCount() ? Json(CharaIdName(chara)) : Json()},
                  {"hp", UserStatus->hp[chara]},
                  {"max_hp", UserStatus->max_hp[chara]},
                  {"action_on", BtActStatus.action_on},
                  {"action_no", BtActStatus.action_no},
                  {"motion", BtActStatus.motion_no},
                  {"attacking", BtActStatus.action_on == 1},
                  {"guarding", guarding},
                  {"guard_held", guarding && BtActStatus.action_no == 9},
                  {"guard_frames", guarding ? g_guard_frames : 0},
                  {"locked_on", lockOnTargetFlag != 0},
                  {"recoil_frames", BtActStatus.recoil_frames},
                  {"invincible_frames", BtActStatus.invincible_frames},
                  {"running", PlayerDodgeRun.running},
                  {"in_water", BtActStatus.in_water != 0},
                  {"gauge", BtActStatus.action_gauge},
                  {"exhausted", BtActStatus.gauge_exhausted != 0}};
    if (lockOnTargetFlag != 0 && lockOnTargetNo >= 0 && lockOnTargetNo < static_cast<int>(g_monster_handle_generations.size())) {
        state["target"] = MonsterHandleId(lockOnTargetNo, g_monster_handle_generations[lockOnTargetNo]);
    }
    return {ApiCallStatus::Ok, state, {}};
}

ApiCallResult UnbindInputAction(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = local_action_id}");
    }
    const auto id = args[0].find("id");
    if (id == args[0].end() || !id->is_string() || !InputId(id->get_ref<const std::string &>())) {
        return Invalid("expected a local action id");
    }
    const std::string qualified = std::string(mod_id) + ":" + id->get<std::string>();
    if (!InputUnbindModAction(qualified)) return Invalid("input action is not registered by this mod");
    g_input_contexts.UnbindAction(qualified);
    return {ApiCallStatus::Ok, {{"id", qualified}, {"removed", true}}, {}};
}

ApiCallResult ResolveModel(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1) {
        return Invalid("expected {monster_id = native monster model id} or {id = registered model id}");
    }
    if (args[0].contains("id")) {
        if (!args[0]["id"].is_string()) return Invalid("id must be a registered model stable id");
        const auto model = g_mod_models.find(args[0]["id"].get<std::string>());
        if (model == g_mod_models.end()) return Invalid("id does not identify a registered custom model");
        return {ApiCallStatus::Ok, ModModelJson(model->second), {}};
    }
    int model_id = -1;
    if (!ReadBoundedInt(args[0], "monster_id", 0, 32767, model_id)) {
        return Invalid("monster_id must be an integer id from the native monster model table");
    }
    MONSTOR_MODEL *definition = GetMonstorTable(model_id);
    if (definition == nullptr) return Invalid("monster_id does not identify a loaded native model definition");

    Json assets = Json::array();
    for (int index = 0; index < 4; ++index) {
        const char *name_begin = definition->model_name[index];
        const char *name_end = std::find(name_begin, name_begin + sizeof(definition->model_name[index]), '\0');
        if (name_end == name_begin + sizeof(definition->model_name[index])) {
            return Failed("native model table contains an unterminated model filename");
        }
        const std::string model_name(name_begin, name_end);
        if (model_name.empty()) continue;
        if (!InputId(model_name)) return Failed("native model table contains an unsafe model filename");
        const std::string game_path = NativeMonsterAssetPath(model_name, "chr");
        assets.push_back({{"slot", index}, {"model", model_name}, {"game_path", game_path},
                          {"overridden", FindNativeResourceOverride(game_path).has_value()}});
    }
    if (assets.empty()) return Failed("native model definition has no usable model resources");

    const char *script_begin = definition->script_name;
    const char *script_end = std::find(script_begin, script_begin + sizeof(definition->script_name), '\0');
    if (script_end == script_begin + sizeof(definition->script_name)) {
        return Failed("native model table contains an unterminated script filename");
    }
    Json result = {{"monster_id", model_id}, {"model", assets[0]["model"]},
                   {"script", std::string(script_begin, script_end)},
                   {"source", assets[0]["overridden"].get<bool>() ? "package_override" : "native_table"},
                   {"assets", std::move(assets)}};
    bool loaded_on_current_floor = false;
    CDngStatusData *status = CurrentStatus();
    CMonstorUnit *unit = NowMonstorUnit;
    if (IsActiveDungeonStatus(status) && unit != nullptr) {
        PrepareMonsterHandleUnit(unit);
        const int loaded_count = std::clamp(unit->model_count, 0, static_cast<int>(g_loaded_monster_model_ids.size()));
        for (int index = 0; index < loaded_count; ++index) {
            if (g_loaded_monster_model_ids[index] == model_id) {
                loaded_on_current_floor = true;
                break;
            }
        }
    }
    result["loaded_on_current_floor"] = loaded_on_current_floor;
    const auto mod_character = std::ranges::find_if(g_mod_characters, [model_id](const auto &entry) {
        return entry.second.native_model_no == model_id;
    });
    if (mod_character != g_mod_characters.end()) {
        result["character_id"] = mod_character->first;
        result["owner"] = mod_character->second.owner;
        result["template_monster_id"] = mod_character->second.template_model_no;
        if (!mod_character->second.model_id.empty()) result["model_id"] = mod_character->second.model_id;
    }
    return {ApiCallStatus::Ok, std::move(result), {}};
}

bool IsDebugOverlayText(std::string_view text) {
    if (text.empty() || text.size() > kMaximumDebugOverlayText) return false;
    constexpr std::string_view allowed_punctuation = " .,:;/()+-_%'[]?~";
    return std::ranges::all_of(text, [allowed_punctuation](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') ||
               allowed_punctuation.find(static_cast<char>(c)) != std::string_view::npos;
    });
}

ApiCallResult RegisterDebugOverlay(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 2 ||
        !args[0].contains("id") || !args[0]["id"].is_string() ||
        !args[0].contains("text") || !args[0]["text"].is_string()) {
        return Invalid("expected {id = local_id, text = one_line_text}");
    }
    std::string id;
    if (!QualifyModContentId(mod_id, args[0]["id"].get_ref<const std::string &>(), id, "debug overlay")) {
        return Invalid("id must be a local identifier owned by this mod");
    }
    const std::string text = args[0]["text"].get<std::string>();
    if (!IsDebugOverlayText(text)) {
        return Invalid("text must use supported overlay characters and contain 1 to 48 characters");
    }
    if (!g_debug_overlays.contains(id) && g_debug_overlays.size() >= kMaximumDebugOverlays) {
        return Failed("the game supports at most four mod diagnostic overlays at once");
    }
    const auto existing = g_debug_overlays.find(id);
    if (existing != g_debug_overlays.end() && existing->second.render_layer) {
        return Invalid("id is already used by a render layer");
    }
    g_debug_overlays[id] = {id, std::string(mod_id), text, false};
    return {ApiCallStatus::Ok, {{"id", id}, {"text", text}, {"status", "registered"}}, {}};
}

ApiCallResult RegisterRenderLayer(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 2 ||
        !args[0].contains("id") || !args[0]["id"].is_string() ||
        !args[0].contains("text") || !args[0]["text"].is_string()) {
        return Invalid("expected {id = local_id, text = one_line_text}");
    }
    std::string id;
    if (!QualifyModContentId(mod_id, args[0]["id"].get_ref<const std::string &>(), id, "render layer")) {
        return Invalid("id must be a local identifier owned by this mod");
    }
    const std::string text = args[0]["text"].get<std::string>();
    if (!IsDebugOverlayText(text)) {
        return Invalid("text must use supported overlay characters and contain 1 to 48 characters");
    }
    const auto existing = g_debug_overlays.find(id);
    if (existing != g_debug_overlays.end() && !existing->second.render_layer) {
        return Invalid("id is already used by a diagnostic overlay");
    }
    if (existing == g_debug_overlays.end() && g_debug_overlays.size() >= kMaximumDebugOverlays) {
        return Failed("the game supports at most four combined diagnostic overlays and HUD text layers");
    }
    g_debug_overlays[id] = {id, std::string(mod_id), text, true};
    return {ApiCallStatus::Ok, {{"id", id}, {"text", text}, {"status", "registered"}}, {}};
}

ApiCallResult RemoveRenderLayer(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("id") || !args[0]["id"].is_string()) {
        return Invalid("expected {id = local_layer_id}");
    }
    std::string id;
    if (!QualifyModContentId(mod_id, args[0]["id"].get_ref<const std::string &>(), id, "render layer")) {
        return Invalid("id must be a local identifier owned by this mod");
    }
    const auto existing = g_debug_overlays.find(id);
    if (existing == g_debug_overlays.end() || !existing->second.render_layer || existing->second.owner != mod_id) {
        return Invalid("render layer is missing or owned by another mod");
    }
    g_debug_overlays.erase(existing);
    return {ApiCallStatus::Ok, {{"id", id}, {"removed", true}}, {}};
}

ApiCallResult RegisterRenderBar(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("id") || !args[0]["id"].is_string() ||
        !args[0].contains("label") || !args[0]["label"].is_string() ||
        !args[0].contains("current") || !args[0]["current"].is_number() ||
        !args[0].contains("max") || !args[0]["max"].is_number()) {
        return Invalid("expected {id = local_id, label = bar_label, current = number, max = positive_number}");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "id" && field.key() != "label" && field.key() != "current" && field.key() != "max" && field.key() != "boss") {
            return Invalid("render.bar_register accepts only id, label, current, max, and boss");
        }
    }
    std::string id;
    if (!QualifyModContentId(mod_id, args[0]["id"].get_ref<const std::string &>(), id, "render bar")) {
        return Invalid("id must be a local identifier owned by this mod");
    }
    const std::string label = args[0]["label"].get<std::string>();
    if (label.empty() || label.size() > 24 || !IsDebugOverlayText(label)) {
        return Invalid("label must use supported overlay characters and contain 1 to 24 characters");
    }
    const double current = args[0]["current"].get<double>();
    const double maximum = args[0]["max"].get<double>();
    if (!std::isfinite(current) || !std::isfinite(maximum) || maximum <= 0.0 || current < 0.0 || current > maximum) {
        return Invalid("current must be in [0, max] and max must be a finite positive number");
    }
    const auto existing = g_debug_overlays.find(id);
    if (existing != g_debug_overlays.end() && !existing->second.render_bar) {
        return Invalid("id is already used by a diagnostic overlay or text layer");
    }
    if (existing == g_debug_overlays.end() && g_debug_overlays.size() >= kMaximumDebugOverlays) {
        return Failed("the game supports at most four combined diagnostic overlays, HUD text layers, and HUD bars");
    }
    if (args[0].contains("boss") && !args[0]["boss"].is_boolean()) return Invalid("boss must be a boolean");
    const bool boss = args[0].value("boss", false);
    const int percent = static_cast<int>(std::clamp(std::lround((current / maximum) * 100.0), 0L, 100L));
    const std::string text = label + " [" + std::to_string(percent) + "%]";
    g_debug_overlays[id] = {id, std::string(mod_id), text, false, true, boss, label, current, maximum};
    return {ApiCallStatus::Ok,
            {{"id", id}, {"label", label}, {"current", current}, {"max", maximum},
             {"percent", percent}, {"text", text}, {"status", "registered"}},
            {}};
}

// HUD icons and the floor plate's label (render.hud_icon_set, render.floor_label_set): drawn by the dungeon HUD beside
// the key and floor plate (topStatusInfo), in the game's own art.
constexpr std::size_t                     kMaximumHudIcons = 6;
constexpr std::size_t                     kMaximumFloorLabel = 6;
constexpr std::array<std::string_view, 5> kNativeHudIcons = {"freeze", "stamina", "poison", "curse", "goo"};

struct HudIconRecord {
    std::string owner;
    int         native = -1;
    std::string image;
    bool        blink = false;
};
std::map<std::string, HudIconRecord, std::less<>> g_hud_icons;
std::string                                       g_floor_label_owner;
std::string                                       g_floor_label;
int                                               g_floor_number = 0; // 0: the native number

ApiCallResult SetHudIcon(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("id") ||
        !args[0]["id"].is_string()) {
        return Invalid("expected {id = local_id, icon = native icon name | image = png game path, blink = optional boolean}");
    }
    const Json &request = args[0];
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "id" && field.key() != "icon" && field.key() != "image" && field.key() != "blink") {
            return Invalid("render.hud_icon_set accepts only id, icon, image, and blink");
        }
    }
    std::string id;
    if (!QualifyModContentId(mod_id, request["id"].get_ref<const std::string &>(), id, "hud icon")) {
        return Invalid("id must be a local identifier owned by this mod");
    }
    const bool has_icon = request.contains("icon");
    const bool has_image = request.contains("image");
    if (has_icon == has_image) return Invalid("give exactly one of icon (a native icon name) or image (a png game path)");
    HudIconRecord record{std::string(mod_id)};
    if (has_icon) {
        const auto found = request["icon"].is_string()
                               ? std::ranges::find(kNativeHudIcons, request["icon"].get_ref<const std::string &>())
                               : kNativeHudIcons.end();
        if (found == kNativeHudIcons.end()) return Invalid("icon must be freeze, stamina, poison, curse, or goo");
        record.native = static_cast<int>(found - kNativeHudIcons.begin());
    } else {
        if (!request["image"].is_string()) return Invalid("image must be a png game path");
        const std::string &image = request["image"].get_ref<const std::string &>();
        if (image.size() < 5 || image.size() > 240 || !image.ends_with(".png")) return Invalid("image must be a png game path");
        if (!NativeResourceOverrideOwnedBy(mod_id, image)) {
            return Failed("image must be a game path this mod registered with resources.override");
        }
        record.image = image;
    }
    if (request.contains("blink")) {
        if (!request["blink"].is_boolean()) return Invalid("blink must be a boolean");
        record.blink = request["blink"].get<bool>();
    }
    const auto existing = g_hud_icons.find(id);
    if (existing != g_hud_icons.end() && existing->second.owner != mod_id) return Invalid("hud icon is owned by another mod");
    if (existing == g_hud_icons.end() && g_hud_icons.size() >= kMaximumHudIcons) {
        return Failed("the HUD holds at most six mod icons");
    }
    g_hud_icons[id] = std::move(record);
    return {ApiCallStatus::Ok, {{"id", id}, {"status", "set"}}, {}};
}

ApiCallResult RemoveHudIcon(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 || !args[0].contains("id") ||
        !args[0]["id"].is_string()) {
        return Invalid("expected {id = local_id}");
    }
    std::string id;
    if (!QualifyModContentId(mod_id, args[0]["id"].get_ref<const std::string &>(), id, "hud icon")) {
        return Invalid("id must be a local identifier owned by this mod");
    }
    const auto existing = g_hud_icons.find(id);
    const bool removed = existing != g_hud_icons.end() && existing->second.owner == mod_id;
    if (removed) g_hud_icons.erase(existing);
    return {ApiCallStatus::Ok, {{"id", id}, {"removed", removed}}, {}};
}

ApiCallResult SetFloorLabel(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("text") ||
        !args[0]["text"].is_string() || args[0].size() > (args[0].contains("number") ? 2u : 1u)) {
        return Invalid("expected {text = up to six characters, number = optional floor number 1-99}");
    }
    int number = 0;
    if (args[0].contains("number") && !ReadBoundedInt(args[0], "number", 1, 99, number)) {
        return Invalid("number must be an integer from 1 to 99");
    }
    const std::string text = args[0]["text"].get<std::string>();
    if (text.empty() || text.size() > kMaximumFloorLabel || !IsDebugOverlayText(text)) {
        return Invalid("text must be 1 to 6 supported characters");
    }
    if (!g_floor_label_owner.empty() && g_floor_label_owner != mod_id) return Failed("another mod labels the floor plate");
    g_floor_label_owner = std::string(mod_id);
    g_floor_label = text;
    g_floor_number = number;
    return {ApiCallStatus::Ok, {{"text", text}, {"number", number}}, {}};
}

ApiCallResult ClearFloorLabel(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("render.floor_label_clear takes no arguments");
    const bool owned = g_floor_label_owner == mod_id;
    if (owned) {
        g_floor_label_owner.clear();
        g_floor_label.clear();
        g_floor_number = 0;
    }
    return {ApiCallStatus::Ok, {{"cleared", owned}}, {}};
}

ApiCallResult RemoveRenderBar(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 ||
        !args[0].contains("id") || !args[0]["id"].is_string()) {
        return Invalid("expected {id = local_bar_id}");
    }
    std::string id;
    if (!QualifyModContentId(mod_id, args[0]["id"].get_ref<const std::string &>(), id, "render bar")) {
        return Invalid("id must be a local identifier owned by this mod");
    }
    const auto existing = g_debug_overlays.find(id);
    if (existing == g_debug_overlays.end() || !existing->second.render_bar || existing->second.owner != mod_id) {
        return Invalid("render bar is missing or owned by another mod");
    }
    g_debug_overlays.erase(existing);
    return {ApiCallStatus::Ok, {{"id", id}, {"removed", true}}, {}};
}

bool FindOwnedChronicleWeapon(const Json &selector, CDngStatusData *&status, WEAPON_HAVE *&target,
                              int &item_no, int &target_character, int &target_slot, ApiCallResult &error) {
    if (!ReadWeaponNumber(selector, item_no)) {
        error = Invalid("expected a Chronicle weapon id, item number, or selector object");
        return false;
    }
    const COM_ITEM_INFO *common = GetCommonItemInfo(item_no);
    const WEAPON_DATA *weapon_data = GetWeaponData(item_no);
    if (common == nullptr || common->kind != ITEMKIND_WEAPON || weapon_data == nullptr) {
        error = Failed("no built-in or registered Chronicle weapon has that id");
        return false;
    }
    status = CurrentStatus();
    if (status == nullptr) {
        error = Failed("the game has no active save data");
        return false;
    }
    int requested_character = -1;
    const auto character_field = selector.find("character");
    if (character_field != selector.end()) {
        if (!character_field->is_string()) {
            error = Invalid("character must be one of Chronicle's playable character ids");
            return false;
        }
        const std::string &name = character_field->get_ref<const std::string &>();
        for (int index = 0; index < static_cast<int>(kCharacterIds.size()); ++index) {
            if (name == kCharacterIds[index]) requested_character = index;
        }
        if (requested_character < 0) {
            error = Invalid("character must be one of Chronicle's playable character ids");
            return false;
        }
    }
    int requested_slot = -1;
    const auto slot_field = selector.find("slot");
    if (slot_field != selector.end()) {
        if (!ReadBoundedInt(selector, "slot", 0, 10, requested_slot)) {
            error = Invalid("slot must be an integer from 0 to 10");
            return false;
        }
    }
    if (requested_slot >= 0 && requested_character < 0) {
        error = Invalid("slot requires an explicit character id");
        return false;
    }
    target = nullptr;
    target_character = -1;
    target_slot = -1;
    for (int character = 0; character < static_cast<int>(kCharacterIds.size()); ++character) {
        if (requested_character >= 0 && character != requested_character) continue;
        for (int slot = 0; slot < 11; ++slot) {
            if (requested_slot >= 0 && slot != requested_slot) continue;
            WEAPON_HAVE *candidate = &status->chara_weapons[character][slot];
            if (candidate->item_no != item_no) continue;
            if (target != nullptr) {
                error = Invalid("weapon has multiple owned copies; specify both character and slot");
                return false;
            }
            target = candidate;
            target_character = character;
            target_slot = slot;
        }
    }
    if (target == nullptr) {
        error = Failed("weapon is not owned by the active save");
        return false;
    }
    return true;
}

std::string WeaponStableId(int item_no) {
    const auto found = g_mod_weapon_by_native_id.find(item_no);
    return found == g_mod_weapon_by_native_id.end() ? "chronicle:weapon:" + std::to_string(item_no) : found->second;
}

ApiCallResult GetWeaponAbs(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = weapon_id, character = optional character_id, slot = optional 0-10}");
    }
    CDngStatusData *status = nullptr;
    WEAPON_HAVE *target = nullptr;
    int item_no = -1;
    int character_id = -1;
    int slot = -1;
    ApiCallResult error;
    if (!FindOwnedChronicleWeapon(args[0], status, target, item_no, character_id, slot, error)) return error;
    const int max_exp = GetWeaponMaxExp(target);
    return {ApiCallStatus::Ok,
            {{"id", WeaponStableId(item_no)}, {"native_item_no", item_no},
             {"character", kCharacterIds[character_id]}, {"slot", slot},
             {"level", target->level}, {"abs", target->experience}, {"abs_max", max_exp},
             {"ready_to_upgrade", target->level < 99 && target->experience >= max_exp},
             {"synthesis_count", target->synthesis_count}},
            {}};
}

ApiCallResult RemoveWeapon(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = weapon_id, character = optional character_id, slot = optional 0-10}");
    }
    CDngStatusData *status = nullptr;
    WEAPON_HAVE *target = nullptr;
    int item_no = -1;
    int character_id = -1;
    int slot = -1;
    ApiCallResult error;
    if (!FindOwnedChronicleWeapon(args[0], status, target, item_no, character_id, slot, error)) return error;
    if (status->equipped_weapon_slot[character_id] == slot) {
        return Failed("the equipped weapon cannot be removed; equip another first");
    }
    *target = WEAPON_HAVE{};
    return {ApiCallStatus::Ok,
            {{"id", WeaponStableId(item_no)}, {"native_item_no", item_no},
             {"character", kCharacterIds[character_id]}, {"slot", slot}, {"removed", true}},
            {}};
}

ApiCallResult SetWeaponDurability(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("durability") || !args[0]["durability"].is_number()) {
        return Invalid("expected {id = weapon_id, durability = 0..maximum, character = optional, slot = optional}");
    }
    const Json &selector = args[0];
    const double requested = selector["durability"].get<double>();
    if (!std::isfinite(requested) || requested < 0.0 || requested > 32767.0) {
        return Invalid("durability must be a finite number from 0 through the weapon's native maximum");
    }
    CDngStatusData *status = nullptr;
    WEAPON_HAVE *target = nullptr;
    int item_no = -1;
    int character_id = -1;
    int slot = -1;
    ApiCallResult error;
    if (!FindOwnedChronicleWeapon(selector, status, target, item_no, character_id, slot, error)) return error;
    const float maximum = static_cast<float>(std::max<s16>(0, target->durability));
    if (requested > maximum) {
        return Invalid("durability cannot exceed this weapon instance's native maximum");
    }
    const float previous = target->durability_f;
    const float next = static_cast<float>(requested);
    target->durability_f = next;
    return {ApiCallStatus::Ok,
            {{"id", WeaponStableId(item_no)}, {"native_item_no", item_no},
             {"character", kCharacterIds[character_id]}, {"slot", slot},
             {"previous", previous}, {"durability", target->durability_f},
             {"maximum", maximum}, {"changed", previous != target->durability_f}},
            {}};
}

ApiCallResult SetWeaponStats(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = weapon_id, character = optional character_id, slot = optional 0-10, attack?, endurance?, speed?, magic?, durability?, elem?, vs_monster?, flags?}");
    }
    const Json &selector = args[0];
    static const std::array<std::string_view, 11> fields = {
        "id", "character", "slot", "attack", "endurance", "speed", "magic", "durability", "elem", "vs_monster", "flags"};
    for (auto field = selector.begin(); field != selector.end(); ++field) {
        if (std::find(fields.begin(), fields.end(), field.key()) == fields.end()) {
            return Invalid("unsupported weapon stat field '" + field.key() + "'");
        }
    }
    const std::array<std::string_view, 8> stat_fields = {
        "attack", "endurance", "speed", "magic", "durability", "elem", "vs_monster", "flags"};
    if (std::none_of(stat_fields.begin(), stat_fields.end(), [&selector](std::string_view name) {
            return selector.contains(name);
        })) {
        return Invalid("at least one weapon stat must be supplied");
    }

    CDngStatusData *status = nullptr;
    WEAPON_HAVE *target = nullptr;
    int item_no = -1;
    int character_id = -1;
    int slot = -1;
    ApiCallResult error;
    if (!FindOwnedChronicleWeapon(selector, status, target, item_no, character_id, slot, error)) return error;

    const std::array<const char *, 5> scalar_names = {"attack", "endurance", "speed", "magic", "durability"};
    std::array<int, 5> next_scalar = {target->attack, target->endurance, target->speed, target->magic, target->durability};
    for (std::size_t index = 0; index < scalar_names.size(); ++index) {
        const char *name = scalar_names[index];
        if (!selector.contains(name)) continue;
        if (!ReadBoundedInt(selector, name, 0, 32767, next_scalar[index])) {
            return Invalid(std::string(name) + " must be an integer from 0 to 32767");
        }
    }
    std::array<char, 5> next_elem{};
    std::array<char, 10> next_vs_monster{};
    std::copy_n(target->elem, next_elem.size(), next_elem.begin());
    std::copy_n(target->vs_monster, next_vs_monster.size(), next_vs_monster.begin());
    auto parse_byte_array = [&selector](const char *name, auto &destination) -> bool {
        if (!selector.contains(name)) return true;
        const Json &values = selector[name];
        if (!values.is_array() || values.size() != destination.size()) return false;
        for (std::size_t index = 0; index < destination.size(); ++index) {
            if (!values[index].is_number_integer()) return false;
            const std::int64_t value = values[index].get<std::int64_t>();
            if (value < 0 || value > 127) return false;
            destination[index] = static_cast<char>(static_cast<unsigned char>(value));
        }
        return true;
    };
    if (!parse_byte_array("elem", next_elem)) {
        return Invalid("elem must be an array of five integers from 0 to 127");
    }
    if (!parse_byte_array("vs_monster", next_vs_monster)) {
        return Invalid("vs_monster must be an array of ten integers from 0 to 127");
    }
    int next_flags = static_cast<std::uint16_t>(target->flags);
    if (selector.contains("flags")) {
        if (!ReadBoundedInt(selector, "flags", 0, 65535, next_flags)) {
            return Invalid("flags must be an integer from 0 to 65535");
        }
    }

    Json previous = {{"attack", target->attack}, {"endurance", target->endurance}, {"speed", target->speed},
                     {"magic", target->magic}, {"durability", target->durability},
                     {"flags", static_cast<std::uint16_t>(target->flags)}};
    previous["elem"] = Json::array();
    previous["vs_monster"] = Json::array();
    for (int index = 0; index < 5; ++index) previous["elem"].push_back(static_cast<unsigned char>(target->elem[index]));
    for (int index = 0; index < 10; ++index) previous["vs_monster"].push_back(static_cast<unsigned char>(target->vs_monster[index]));

    const float previous_max = static_cast<float>(std::max<s16>(0, target->durability));
    const float durability_fraction = previous_max > 0.0f
        ? std::clamp(target->durability_f / previous_max, 0.0f, 1.0f) : 0.0f;
    target->attack = static_cast<s16>(next_scalar[0]);
    target->endurance = static_cast<s16>(next_scalar[1]);
    target->speed = static_cast<s16>(next_scalar[2]);
    target->magic = static_cast<s16>(next_scalar[3]);
    target->durability = static_cast<s16>(next_scalar[4]);
    if (selector.contains("durability")) {
        target->durability_f = durability_fraction * static_cast<float>(next_scalar[4]);
    }
    std::copy(next_elem.begin(), next_elem.end(), target->elem);
    std::copy(next_vs_monster.begin(), next_vs_monster.end(), target->vs_monster);
    target->flags = static_cast<s16>(static_cast<std::uint16_t>(next_flags));

    target->best_elem = -1;
    int best_element_value = 0;
    for (int index = 0; index < 5; ++index) {
        const int value = static_cast<unsigned char>(target->elem[index]);
        if (value > best_element_value) {
            best_element_value = value;
            target->best_elem = static_cast<s8>(index);
        }
    }
    Json current = {{"attack", target->attack}, {"endurance", target->endurance}, {"speed", target->speed},
                    {"magic", target->magic}, {"durability", target->durability},
                    {"durability_current", target->durability_f}, {"flags", static_cast<std::uint16_t>(target->flags)},
                    {"best_elem", target->best_elem}};
    current["elem"] = Json::array();
    current["vs_monster"] = Json::array();
    for (int index = 0; index < 5; ++index) current["elem"].push_back(static_cast<unsigned char>(target->elem[index]));
    for (int index = 0; index < 10; ++index) current["vs_monster"].push_back(static_cast<unsigned char>(target->vs_monster[index]));
    const bool changed = previous["attack"] != current["attack"] || previous["endurance"] != current["endurance"] ||
        previous["speed"] != current["speed"] || previous["magic"] != current["magic"] ||
        previous["durability"] != current["durability"] || previous["elem"] != current["elem"] ||
        previous["vs_monster"] != current["vs_monster"] || previous["flags"] != current["flags"];
    return {ApiCallStatus::Ok,
            {{"id", WeaponStableId(item_no)}, {"native_item_no", item_no},
             {"character", kCharacterIds[character_id]}, {"slot", slot},
             {"previous", std::move(previous)}, {"stats", std::move(current)}, {"changed", changed}},
            {}};
}

void RestoreWeaponDefinitionsByOwner(std::string_view owner = {}) {
    for (auto it = g_weapon_definition_overrides.begin(); it != g_weapon_definition_overrides.end();) {
        if (!owner.empty() && it->second.owner != owner) {
            ++it;
            continue;
        }
        if (WEAPON_DATA *weapon = GetWeaponData(it->first); weapon != nullptr) *weapon = it->second.original;
        it = g_weapon_definition_overrides.erase(it);
    }
}

ApiCallResult SetWeaponDefinition(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("id")) {
        return Invalid("expected {id = native_weapon_id, attack?, endurance?, speed?, magic?, durability?, owner?, hole?, hole_num?, elem?, vs_monster?, exp_base?, exp_per_level?, flags?, buildup_mask0?, buildup_mask1?, attack_max?, magic_max?, chain_pos?}");
    }
    const Json &selector = args[0];
    int item_no = -1;
    if (!ReadWeaponNumber(selector["id"], item_no) || item_no < ITEM_WEAPON_START || item_no >= ITEM_WEAPON_START + 120) {
        return Invalid("id must identify one of Chronicle's 120 built-in weapons");
    }
    WEAPON_DATA *weapon = GetWeaponData(item_no);
    const COM_ITEM_INFO *common = GetCommonItemInfo(item_no);
    if (weapon == nullptr || common == nullptr || common->kind != ITEMKIND_WEAPON) {
        return Failed("native weapon definition is unavailable");
    }
    static const std::array<std::string_view, 19> fields = {
        "id", "attack", "endurance", "speed", "magic", "durability", "owner", "hole", "hole_num",
        "elem", "vs_monster", "exp_base", "exp_per_level", "flags", "buildup_mask0", "buildup_mask1",
        "attack_max", "magic_max", "chain_pos"};
    for (auto field = selector.begin(); field != selector.end(); ++field) {
        if (std::find(fields.begin(), fields.end(), field.key()) == fields.end()) {
            return Invalid("unsupported native weapon definition field '" + field.key() + "'");
        }
    }
    if (selector.size() < 2) return Invalid("at least one native weapon definition field must be supplied");

    auto existing = g_weapon_definition_overrides.find(item_no);
    if (existing != g_weapon_definition_overrides.end() && existing->second.owner != owner) {
        return Failed("this native weapon definition is already overridden by another mod");
    }
    WEAPON_DATA next = *weapon;
    const std::array<std::pair<const char *, s16 *>, 11> scalar_fields = {{
        {"attack", &next.attack}, {"endurance", &next.endurance}, {"speed", &next.speed},
        {"magic", &next.magic}, {"durability", &next.durability}, {"exp_base", &next.exp_base},
        {"exp_per_level", &next.exp_per_level}, {"flags", &next.flags}, {"attack_max", &next.attack_max},
        {"magic_max", &next.magic_max}, {"chain_pos", &next.chain_pos}}};
    for (const auto &[name, target] : scalar_fields) {
        if (!selector.contains(name)) continue;
        int value = 0;
        if (!ReadBoundedInt(selector, name, 0, 32767, value)) {
            return Invalid(std::string(name) + " must be an integer from 0 to 32767");
        }
        *target = static_cast<s16>(value);
    }
    if (selector.contains("hole_num")) {
        int value = 0;
        if (!ReadBoundedInt(selector, "hole_num", 0, 6, value)) return Invalid("hole_num must be an integer from 0 to 6");
        next.hole_num = static_cast<u8>(value);
    }
    if (selector.contains("owner")) {
        int value = 0;
        if (!ReadBoundedInt(selector, "owner", 0, 5, value)) return Invalid("owner must be a playable character index from 0 to 5");
        next.owner = static_cast<u8>(value);
    }
    auto parse_array = [&selector](const char *name, auto &target, int maximum) -> bool {
        if (!selector.contains(name)) return true;
        const Json &values = selector[name];
        if (!values.is_array() || values.size() != std::size(target)) return false;
        for (std::size_t i = 0; i < std::size(target); ++i) {
            if (!values[i].is_number_integer()) return false;
            const auto value = values[i].get<std::int64_t>();
            if (value < 0 || value > maximum) return false;
            target[i] = static_cast<typename std::remove_reference_t<decltype(target[i])>>(value);
        }
        return true;
    };
    if (!parse_array("hole", next.hole, 127)) return Invalid("hole must contain six integer socket values from 0 to 127");
    if (selector.contains("hole") && !selector.contains("hole_num")) {
        next.hole_num = static_cast<u8>(std::count_if(std::begin(next.hole), std::end(next.hole), [](s8 value) { return value != 0; }));
    }
    if (!parse_array("elem", next.elem, 32767)) return Invalid("elem must contain five integers from 0 to 32767");
    if (!parse_array("vs_monster", next.vs_monster, 32767)) return Invalid("vs_monster must contain ten integers from 0 to 32767");
    for (const char *name : {"buildup_mask0", "buildup_mask1"}) {
        if (!selector.contains(name)) continue;
        if (!selector[name].is_number_unsigned() && (!selector[name].is_number_integer() || selector[name].get<std::int64_t>() < 0)) {
            return Invalid(std::string(name) + " must be an unsigned 32-bit integer");
        }
        const auto value = selector[name].get<std::uint64_t>();
        if (value > std::numeric_limits<u32>::max()) return Invalid(std::string(name) + " exceeds the unsigned 32-bit range");
        if (std::string_view(name) == "buildup_mask0") next.buildup_mask0 = static_cast<u32>(value);
        else next.buildup_mask1 = static_cast<u32>(value);
    }
    if (existing == g_weapon_definition_overrides.end()) {
        existing = g_weapon_definition_overrides.emplace(item_no, ChronicleWeaponDefinitionOverride{std::string(owner), *weapon}).first;
    }
    *weapon = next;
    return {ApiCallStatus::Ok, { {"item_no", item_no}, {"changed", true}, {"owner", std::string(owner)} }, {}};
}

ApiCallResult AddWeaponAbs(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("amount") || !args[0]["amount"].is_number_integer()) {
        return Invalid("expected {id = weapon_id, amount = nonzero_integer, character = optional, slot = optional}");
    }
    // A negative amount takes unspent ABS away (down to none): a weapon's level is never taken.
    int amount = 0;
    if (!ReadBoundedInt(args[0], "amount", -32767, 32767, amount) || amount == 0) {
        return Invalid("amount must be an integer from 1 to 32767, or from -1 to -32767 to take ABS away");
    }
    CDngStatusData *status = nullptr;
    WEAPON_HAVE *target = nullptr;
    int item_no = -1;
    int character_id = -1;
    int slot = -1;
    ApiCallResult error;
    if (!FindOwnedChronicleWeapon(args[0], status, target, item_no, character_id, slot, error)) return error;
    const int previous_abs = target->experience;
    const int max_exp = std::max(0, GetWeaponMaxExp(target));
    const int next_abs = std::clamp(previous_abs + amount, 0, std::max(max_exp, 32767));
    const bool changed = next_abs != previous_abs;
    target->experience = static_cast<s16>(next_abs);
    return {ApiCallStatus::Ok,
            {{"id", WeaponStableId(item_no)}, {"native_item_no", item_no},
             {"character", kCharacterIds[character_id]}, {"slot", slot},
             {"level", target->level}, {"previous_abs", previous_abs},
             {"abs", target->experience}, {"abs_max", max_exp},
             {"added", target->experience - previous_abs}, {"changed", changed}},
            {}};
}

ApiCallResult GetWeaponAttachments(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = weapon_id, character = optional character_id, slot = optional 0-10}");
    }
    CDngStatusData *status = nullptr;
    WEAPON_HAVE *target = nullptr;
    int item_no = -1;
    int character_id = -1;
    int slot = -1;
    ApiCallResult error;
    if (!FindOwnedChronicleWeapon(args[0], status, target, item_no, character_id, slot, error)) return error;
    const WEAPON_DATA *weapon_data = GetWeaponData(item_no);
    const int hole_count = weapon_data != nullptr ? std::clamp<int>(weapon_data->hole_num, 0, 6) : 6;
    Json slots = Json::array();
    int occupied = 0;
    for (int index = 0; index < hole_count; ++index) {
        const ATTACH_LIST &att = target->attach[index];
        const bool filled = att.item_no >= ITEM_ATTACH_START;
        if (filled) ++occupied;
        slots.push_back({{"slot", index}, {"occupied", filled},
                         {"item_no", filled ? Json(att.item_no) : Json(nullptr)},
                         {"id", filled ? Json(ItemStableId(att.item_no)) : Json(nullptr)},
                         {"attack", att.status[0]}, {"endurance", att.status[1]},
                         {"speed", att.status[2]}, {"magic", att.status[3]},
                         {"sphere_weapon_no", att.sphere_weapon_no}, {"sphere_level", att.sphere_level}});
    }
    return {ApiCallStatus::Ok,
            {{"id", WeaponStableId(item_no)}, {"native_item_no", item_no},
             {"character", kCharacterIds[character_id]}, {"slot", slot},
             {"socket_count", hole_count}, {"occupied_count", occupied},
             {"attachments", std::move(slots)}},
            {}};
}

ApiCallResult InsertWeaponAttachment(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("attachment_id")) {
        return Invalid("expected {id = weapon_id, attachment_id = item_id_or_no, attachment_slot = optional 0-5}");
    }
    int attachment_no = -1;
    if (!ReadItemNumber(args[0]["attachment_id"], attachment_no) ||
        attachment_no < ITEM_ATTACH_START || attachment_no >= ITEM_DUNGEON_START) {
        return Invalid("attachment_id must identify a Chronicle attachment item (90..149)");
    }
    CDngStatusData *status = nullptr;
    WEAPON_HAVE *target = nullptr;
    int item_no = -1;
    int character_id = -1;
    int slot = -1;
    ApiCallResult error;
    if (!FindOwnedChronicleWeapon(args[0], status, target, item_no, character_id, slot, error)) return error;
    const WEAPON_DATA *weapon_data = GetWeaponData(item_no);
    const int hole_count = weapon_data != nullptr ? std::clamp<int>(weapon_data->hole_num, 0, 6) : 6;
    if (hole_count <= 0) return Failed("weapon has no attachment slots");
    int target_hole = -1;
    if (args[0].contains("attachment_slot")) {
        if (!ReadBoundedInt(args[0], "attachment_slot", 0, hole_count - 1, target_hole)) {
            return Invalid("attachment_slot must be an integer within the weapon's socket_count");
        }
    } else {
        for (int index = 0; index < hole_count; ++index) {
            if (target->attach[index].item_no < ITEM_ATTACH_START) {
                target_hole = index;
                break;
            }
        }
        if (target_hole < 0) return Failed("all attachment slots on this weapon are full");
    }
    ATTACH_LIST &dest = target->attach[target_hole];
    std::memset(&dest, 0, sizeof(dest));
    dest.item_no = static_cast<s16>(attachment_no);
    if (const ATTACH_DATA *attach_data = GetAttachData(attachment_no); attach_data != nullptr) {
        dest.status[0] = attach_data->attack;
        dest.status[1] = attach_data->endurance;
        dest.status[2] = attach_data->speed;
        dest.status[3] = attach_data->magic;
        for (int index = 0; index < 5; ++index) dest.elem[index] = static_cast<s8>(attach_data->elem[index]);
        for (int index = 0; index < 10; ++index) dest.vs_monster[index] = static_cast<s8>(attach_data->vs_monster[index]);
    } else {
        dest.status[0] = 3;
    }
    target->attach_kind[target_hole] = 1;
    return {ApiCallStatus::Ok,
            {{"id", WeaponStableId(item_no)}, {"native_item_no", item_no},
             {"character", kCharacterIds[character_id]}, {"slot", slot},
             {"attachment_slot", target_hole}, {"attachment_item_no", attachment_no},
             {"inserted", true}},
            {}};
}

ApiCallResult RemoveWeaponAttachment(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("attachment_slot")) {
        return Invalid("expected {id = weapon_id, attachment_slot = 0..5, character = optional, slot = optional}");
    }
    CDngStatusData *status = nullptr;
    WEAPON_HAVE *target = nullptr;
    int item_no = -1;
    int character_id = -1;
    int slot = -1;
    ApiCallResult error;
    if (!FindOwnedChronicleWeapon(args[0], status, target, item_no, character_id, slot, error)) return error;
    const WEAPON_DATA *weapon_data = GetWeaponData(item_no);
    const int hole_count = weapon_data != nullptr ? std::clamp<int>(weapon_data->hole_num, 0, 6) : 6;
    int target_hole = -1;
    if (!ReadBoundedInt(args[0], "attachment_slot", 0, std::max(0, hole_count - 1), target_hole)) {
        return Invalid("attachment_slot must be an integer within the weapon's socket_count");
    }
    ATTACH_LIST &src = target->attach[target_hole];
    if (src.item_no < ITEM_ATTACH_START) {
        return Failed("the selected weapon attachment slot is empty");
    }
    int bag_slot = -1;
    ATTACH_LIST *bag = reinterpret_cast<ATTACH_LIST *>(status->consumable_items);
    for (int index = 0; index < 40; ++index) {
        if (bag[index].item_no < ITEM_ATTACH_START) {
            bag_slot = index;
            break;
        }
    }
    if (bag_slot < 0) return Failed("the attachment bag has no free slot");
    const int removed_item_no = src.item_no;
    bag[bag_slot] = src;
    std::memset(&src, 0, sizeof(src));
    target->attach_kind[target_hole] = 0;
    return {ApiCallStatus::Ok,
            {{"id", WeaponStableId(item_no)}, {"native_item_no", item_no},
             {"character", kCharacterIds[character_id]}, {"slot", slot},
             {"attachment_slot", target_hole}, {"attachment_item_no", removed_item_no},
             {"bag_slot", bag_slot}, {"removed", true}},
            {}};
}

ApiCallResult ListWeaponBuildUps(const Json &args) {
    if (!args.is_array() || args.size() != 1) return Invalid("expected one weapon id or selector");
    int item_no = -1;
    if (!ReadWeaponNumber(args[0], item_no)) return Invalid("expected a Chronicle weapon id or item number");
    const WEAPON_DATA *weapon = GetWeaponData(item_no);
    if (weapon == nullptr) return Failed("weapon definition is unavailable");
    Json targets = Json::array();
    for (int candidate = ITEM_WEAPON_START; candidate < ITEM_WEAPON_START + kChronicleWeaponCount; ++candidate) {
        const WEAPON_DATA *target = GetWeaponData(candidate);
        if (target == nullptr || target->owner != weapon->owner || target->chain_pos < 2) continue;
        const int pos = target->chain_pos;
        const bool linked = pos <= 20
                                ? ((weapon->buildup_mask0 & (1u << (pos - 2))) != 0)
                                : ((weapon->buildup_mask1 & (1u << (pos - 21))) != 0);
        if (linked) {
            targets.push_back({{"id", WeaponStableId(candidate)}, {"native_item_no", candidate},
                               {"chain_position", pos}, {"source", "native"}});
        }
    }
    if (const auto custom = g_mod_weapon_buildups.find(item_no); custom != g_mod_weapon_buildups.end()) {
        for (int target_no : custom->second) {
            const bool already = std::ranges::any_of(targets, [target_no](const Json &entry) {
                return entry.value("native_item_no", -1) == target_no;
            });
            if (!already) {
                targets.push_back({{"id", WeaponStableId(target_no)}, {"native_item_no", target_no},
                                   {"source", "mod"}});
            }
        }
    }
    return {ApiCallStatus::Ok,
            {{"id", WeaponStableId(item_no)}, {"native_item_no", item_no},
             {"targets", std::move(targets)}},
            {}};
}

ApiCallResult RegisterWeaponBuildUp(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("from") || !args[0].contains("to")) {
        return Invalid("expected {from = source_weapon_id, to = target_weapon_id}");
    }
    int from_no = -1;
    int to_no = -1;
    if (!ReadWeaponNumber(args[0]["from"], from_no) || !ReadWeaponNumber(args[0]["to"], to_no) ||
        from_no == to_no) {
        return Invalid("from and to must identify two distinct Chronicle weapons");
    }
    WEAPON_DATA *from_data = GetWeaponData(from_no);
    const WEAPON_DATA *to_data = GetWeaponData(to_no);
    if (from_data == nullptr || to_data == nullptr) return Failed("weapon definition is unavailable");
    if (from_data->owner != to_data->owner) {
        return Invalid("from and to weapons must belong to the same playable character");
    }
    if (!g_original_weapon_buildup_masks.contains(from_no)) {
        g_original_weapon_buildup_masks.emplace(from_no, std::make_pair(from_data->buildup_mask0, from_data->buildup_mask1));
    }
    if (to_data->chain_pos >= 2 && to_data->chain_pos <= 20) {
        from_data->buildup_mask0 |= (1u << (to_data->chain_pos - 2));
    } else if (to_data->chain_pos > 20 && to_data->chain_pos <= 52) {
        from_data->buildup_mask1 |= (1u << (to_data->chain_pos - 21));
    }
    auto &list = g_mod_weapon_buildups[from_no];
    if (std::ranges::find(list, to_no) == list.end()) list.push_back(to_no);
    // a registered target, which the native chains (numbered from the owner's first weapon) cannot reach
    if (MoonModWeaponTemplate(to_no) >= 0) AddMoonModBuildUp(from_no, to_no);
    return {ApiCallStatus::Ok,
            {{"from", WeaponStableId(from_no)}, {"to", WeaponStableId(to_no)},
             {"from_item_no", from_no}, {"to_item_no", to_no}, {"registered", true}},
            {}};
}

ApiCallResult SynthesizeWeapon(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {id = weapon_id, character = optional character_id, slot = optional 0-10}");
    }
    CDngStatusData *status = nullptr;
    WEAPON_HAVE *target = nullptr;
    int item_no = -1;
    int character_id = -1;
    int slot = -1;
    ApiCallResult error;
    if (!FindOwnedChronicleWeapon(args[0], status, target, item_no, character_id, slot, error)) return error;
    if (status->equipped_weapon_slot[character_id] == slot) {
        return Failed("cannot status-break the currently equipped weapon");
    }
    if (WeaponStatusBreakEnable(target) == 0) {
        return Failed("weapon must be at least level 5 (+5) to status-break into a SynthSphere");
    }
    int bag_slot = -1;
    ATTACH_LIST *bag = reinterpret_cast<ATTACH_LIST *>(status->consumable_items);
    for (int index = 0; index < 40; ++index) {
        if (bag[index].item_no < ITEM_ATTACH_START) {
            bag_slot = index;
            break;
        }
    }
    if (bag_slot < 0) return Failed("the attachment bag has no free slot for the SynthSphere");
    ATTACH_LIST sphere{};
    sphere.item_no = ITEM_ATTACH_SYNTHESIS_SPHERE;
    sphere.sphere_weapon_no = target->item_no;
    sphere.sphere_flags = target->flags;
    sphere.sphere_level = target->level;
    sphere.status[0] = static_cast<s16>((target->attack * 6) / 10);
    sphere.status[1] = static_cast<s16>((target->endurance * 6) / 10);
    sphere.status[2] = static_cast<s16>((target->speed * 6) / 10);
    sphere.status[3] = static_cast<s16>((target->magic * 6) / 10);
    for (int index = 0; index < 5; ++index) sphere.elem[index] = static_cast<s8>((target->elem[index] * 6) / 10);
    for (int index = 0; index < 10; ++index) {
        sphere.vs_monster[index] = static_cast<s8>((target->vs_monster[index] * 6) / 10);
    }
    bag[bag_slot] = sphere;
    std::memset(target, 0, sizeof(WEAPON_HAVE));
    target->item_no = -1;
    return {ApiCallStatus::Ok,
            {{"id", WeaponStableId(item_no)}, {"native_item_no", item_no},
             {"character", kCharacterIds[character_id]}, {"slot", slot},
             {"sphere_item_no", ITEM_ATTACH_SYNTHESIS_SPHERE}, {"bag_slot", bag_slot},
             {"synthesized", true}, {"changed", true}},
            {}};
}

bool ReadShopItemOrWeaponNo(const Json &selector, int &item_no) {
    return ReadItemNumber(selector, item_no) || ReadWeaponNumber(selector, item_no);
}

void EnsureChronicleShopBackup(int shop_no) {
    if (g_original_shop_lists.contains(shop_no)) return;
    s16 *list = GetItemShopList(shop_no);
    if (list == nullptr) return;
    ChronicleShopBackup backup;
    backup.shop_no = shop_no;
    for (int index = 0; index < 20; ++index) backup.goods[index] = list[index];
    g_original_shop_lists.emplace(shop_no, backup);
}

bool ClaimChronicleShopList(int shop_no, std::string_view owner) {
    const auto found = g_shop_list_owners.find(shop_no);
    if (found != g_shop_list_owners.end() && found->second != owner) return false;
    EnsureChronicleShopBackup(shop_no);
    if (!g_original_shop_lists.contains(shop_no)) return false;
    g_shop_list_owners[shop_no] = std::string(owner);
    return true;
}

// Puts the goods mods added that a shop's list lacks at its end, while there is room.
void AppendShopAdditions(int shop_no) {
    s16 *list = GetItemShopList(shop_no);
    const auto found = g_shop_additions.find(shop_no);
    if (list == nullptr || found == g_shop_additions.end()) return;
    for (const ChronicleShopAddition &addition : found->second) {
        int  count = 0;
        bool listed = false;
        while (count < 20 && list[count] >= ITEM_ATTACH_START) {
            if (list[count] == addition.item_no) listed = true;
            ++count;
        }
        if (listed || count >= 20) continue;
        list[count] = static_cast<s16>(addition.item_no);
        if (count < 19) list[count + 1] = -1;
    }
}

void RemoveFromShopList(int shop_no, int item_no) {
    s16 *list = GetItemShopList(shop_no);
    if (list == nullptr) return;
    int to = 0;
    for (int from = 0; from < 20 && list[from] >= ITEM_ATTACH_START; ++from) {
        if (list[from] != item_no) list[to++] = list[from];
    }
    for (; to < 20; ++to) list[to] = -1;
}

// A shop no mod holds any more forgets its retail list (it is back in place).
void DropShopBackupIfUnused(int shop_no) {
    if (g_shop_list_owners.contains(shop_no) || g_shop_additions.contains(shop_no)) return;
    g_original_shop_lists.erase(shop_no);
}

void RestoreShopListFromBackup(int shop_no) {
    if (const auto backup = g_original_shop_lists.find(shop_no); backup != g_original_shop_lists.end()) {
        if (s16 *list = GetItemShopList(shop_no); list != nullptr) {
            for (int index = 0; index < 20; ++index) list[index] = backup->second.goods[index];
        }
    }
}

void RestoreShopOverridesByOwner(std::string_view owner) {
    for (auto it = g_shop_additions.begin(); it != g_shop_additions.end();) {
        const int shop_no = it->first;
        std::vector<int> removed;
        for (const ChronicleShopAddition &addition : it->second) {
            if (addition.owner == owner) removed.push_back(addition.item_no);
        }
        std::erase_if(it->second, [owner](const ChronicleShopAddition &addition) { return addition.owner == owner; });
        it = it->second.empty() ? g_shop_additions.erase(it) : std::next(it);
        if (removed.empty()) continue;
        if (g_shop_list_owners.contains(shop_no)) {
            for (const int item_no : removed) RemoveFromShopList(shop_no, item_no);
        } else {
            RestoreShopListFromBackup(shop_no);
            AppendShopAdditions(shop_no);
            DropShopBackupIfUnused(shop_no);
        }
    }
    for (auto it = g_shop_list_owners.begin(); it != g_shop_list_owners.end();) {
        if (it->second != owner) { ++it; continue; }
        const int shop_no = it->first;
        RestoreShopListFromBackup(shop_no);
        it = g_shop_list_owners.erase(it);
        AppendShopAdditions(shop_no);
        DropShopBackupIfUnused(shop_no);
    }
    for (auto it = g_shop_price_owners.begin(); it != g_shop_price_owners.end();) {
        if (it->second != owner) { ++it; continue; }
        const int price_idx = it->first;
        if (const auto backup = g_original_shop_prices.find(price_idx); backup != g_original_shop_prices.end()) {
            if (price_idx >= 0 && price_idx < static_cast<int>(std::size(PriceList))) PriceList[price_idx] = backup->second;
            g_original_shop_prices.erase(backup);
        }
        it = g_shop_price_owners.erase(it);
    }
}

Json ChronicleShopJson(int shop_no) {
    s16 *list = GetItemShopList(shop_no);
    Json items = Json::array();
    if (list != nullptr) {
        for (int index = 0; index < 20; ++index) {
            const int item_no = list[index];
            if (item_no < ITEM_ATTACH_START) break;
            const int price_idx = item_no - ITEM_ATTACH_START;
            const int buy_price = (price_idx >= 0 && price_idx < static_cast<int>(std::size(PriceList))) ? PriceList[price_idx].buy_price : 0;
            const int sell_price = (price_idx >= 0 && price_idx < static_cast<int>(std::size(PriceList))) ? PriceList[price_idx].sell_price : 0;
            items.push_back({{"slot", index},
                             {"item_no", item_no},
                             {"id", item_no >= ITEM_WEAPON_START ? WeaponStableId(item_no) : ItemStableId(item_no)},
                             {"buy_price", buy_price},
                             {"sell_price", sell_price}});
        }
    }
    return {{"id", "chronicle:shop:" + std::to_string(shop_no)},
            {"shop_no", shop_no},
            {"currency", "gilda"},
            {"item_count", items.size()},
            {"items", std::move(items)}};
}

bool ReadChronicleShopNo(const Json &selector, int &shop_no) {
    if (selector.is_number_integer()) {
        const auto val = selector.get<std::int64_t>();
        if (val < 0 || val >= 18) return false;
        shop_no = static_cast<int>(val);
        return true;
    }
    if (selector.is_string()) {
        const std::string &id = selector.get_ref<const std::string &>();
        constexpr std::string_view prefix = "chronicle:shop:";
        if (!std::string_view(id).starts_with(prefix)) return false;
        const std::string_view suffix = std::string_view(id).substr(prefix.size());
        int parsed = -1;
        const auto [end, ec] = std::from_chars(suffix.data(), suffix.data() + suffix.size(), parsed);
        if (ec != std::errc{} || end != suffix.data() + suffix.size() || parsed < 0 || parsed >= 18) return false;
        shop_no = parsed;
        return true;
    }
    if (selector.is_object()) {
        if (selector.contains("shop_no")) return ReadBoundedInt(selector, "shop_no", 0, 17, shop_no);
        if (selector.contains("id")) return ReadChronicleShopNo(selector["id"], shop_no);
    }
    return false;
}

ApiCallResult ListShops(const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("shops.list takes no arguments");
    Json shops = Json::array();
    for (int shop_no = 0; shop_no < 18; ++shop_no) shops.push_back(ChronicleShopJson(shop_no));
    return {ApiCallStatus::Ok, {{"shops", std::move(shops)}}, {}};
}

ApiCallResult GetShop(const Json &args) {
    if (!args.is_array() || args.size() != 1) return Invalid("expected one shop id or shop_no (0..17)");
    int shop_no = -1;
    if (!ReadChronicleShopNo(args[0], shop_no)) return Invalid("expected chronicle:shop:<0..17> or shop_no 0..17");
    return {ApiCallStatus::Ok, ChronicleShopJson(shop_no), {}};
}

ApiCallResult RegisterShopItem(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("item_id")) {
        return Invalid("expected {shop_no|id, item_id = item_or_weapon_id}");
    }
    int shop_no = -1;
    if (!ReadChronicleShopNo(args[0], shop_no)) return Invalid("expected valid shop_no (0..17) or id");
    int item_no = -1;
    if (!ReadShopItemOrWeaponNo(args[0]["item_id"], item_no)) {
        return Invalid("item_id must identify a valid Chronicle item, attachment, or weapon");
    }
    s16 *list = GetItemShopList(shop_no);
    if (list == nullptr) return Failed("native shop goods list is unavailable");
    const auto slot_of = [list, item_no] {
        for (int index = 0; index < 20 && list[index] >= ITEM_ATTACH_START; ++index) {
            if (list[index] == item_no) return index;
        }
        return -1;
    };
    const bool was_listed = slot_of() >= 0;
    // No claim: the addition is kept apart and put back after any mod's set_items.
    EnsureChronicleShopBackup(shop_no);
    auto &additions = g_shop_additions[shop_no];
    const bool known = std::ranges::any_of(additions, [item_no](const ChronicleShopAddition &a) { return a.item_no == item_no; });
    if (!known) additions.push_back({std::string(owner), item_no});
    AppendShopAdditions(shop_no);
    const int slot = slot_of();
    if (slot < 0) {
        if (!known) additions.pop_back();
        if (additions.empty()) g_shop_additions.erase(shop_no);
        DropShopBackupIfUnused(shop_no);
        return Failed("native Chronicle shop already holds the maximum number of goods");
    }
    return {ApiCallStatus::Ok,
            {{"id", "chronicle:shop:" + std::to_string(shop_no)}, {"shop_no", shop_no},
             {"item_no", item_no}, {"slot", slot}, {"added", !was_listed}, {"changed", !was_listed}},
            {}};
}

ApiCallResult RemoveShopItem(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("item_id")) {
        return Invalid("expected {shop_no|id, item_id = item_or_weapon_id}");
    }
    int shop_no = -1;
    if (!ReadChronicleShopNo(args[0], shop_no)) return Invalid("expected valid shop_no (0..17) or id");
    int item_no = -1;
    if (!ReadShopItemOrWeaponNo(args[0]["item_id"], item_no)) {
        return Invalid("item_id must identify a valid Chronicle item, attachment, or weapon");
    }
    s16 *list = GetItemShopList(shop_no);
    if (list == nullptr) return Failed("native shop goods list is unavailable");
    int found_slot = -1;
    for (int index = 0; index < 20 && list[index] >= ITEM_ATTACH_START; ++index) {
        if (list[index] == item_no) {
            found_slot = index;
            break;
        }
    }
    if (found_slot < 0) return Failed("item is not listed in the specified shop");
    // The caller's own addition goes without a claim.
    if (auto additions = g_shop_additions.find(shop_no);
        additions != g_shop_additions.end() &&
        std::erase_if(additions->second, [owner, item_no](const ChronicleShopAddition &a) {
            return a.owner == owner && a.item_no == item_no;
        }) != 0) {
        if (additions->second.empty()) g_shop_additions.erase(additions);
        RemoveFromShopList(shop_no, item_no);
        DropShopBackupIfUnused(shop_no);
        return {ApiCallStatus::Ok,
                {{"id", "chronicle:shop:" + std::to_string(shop_no)}, {"shop_no", shop_no},
                 {"item_no", item_no}, {"removed", true}, {"changed", true}},
                {}};
    }
    if (!ClaimChronicleShopList(shop_no, owner)) return Failed("this native shop list is already owned by another mod");
    for (int index = found_slot; index < 19; ++index) {
        list[index] = list[index + 1];
    }
    list[19] = -1;
    return {ApiCallStatus::Ok,
            {{"id", "chronicle:shop:" + std::to_string(shop_no)}, {"shop_no", shop_no},
             {"item_no", item_no}, {"removed", true}, {"changed", true}},
            {}};
}

ApiCallResult SetShopItems(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("items") ||
        !args[0]["items"].is_array()) {
        return Invalid("expected {shop_no|id, items = array of at most 20 item or weapon ids}");
    }
    int shop_no = -1;
    if (!ReadChronicleShopNo(args[0], shop_no)) return Invalid("expected valid shop_no (0..17) or id");
    const Json &items = args[0]["items"];
    if (items.size() > 20) return Invalid("items must contain no more than 20 goods");
    std::array<s16, 20> next{};
    next.fill(-1);
    for (std::size_t index = 0; index < items.size(); ++index) {
        int item_no = -1;
        if (!ReadShopItemOrWeaponNo(items[index], item_no)) return Invalid("each item must identify a native Chronicle item or weapon");
        // Native shop lists intentionally repeat goods to preserve stock weighting.
        next[index] = static_cast<s16>(item_no);
    }
    s16 *list = GetItemShopList(shop_no);
    if (list == nullptr) return Failed("native shop goods list is unavailable");
    const bool changed = !std::equal(next.begin(), next.end(), list);
    if (!ClaimChronicleShopList(shop_no, owner)) return Failed("this native shop list is already owned by another mod");
    std::copy(next.begin(), next.end(), list);
    AppendShopAdditions(shop_no);
    return {ApiCallStatus::Ok,
            {{"id", "chronicle:shop:" + std::to_string(shop_no)}, {"shop_no", shop_no},
             {"item_count", items.size()}, {"changed", changed}}, {}};
}

ApiCallResult SetShopPrice(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("item_id") ||
        (!args[0].contains("buy_price") && !args[0].contains("sell_price"))) {
        return Invalid("expected {item_id, buy_price = optional 0..32767, sell_price = optional 0..32767}");
    }
    int item_no = -1;
    if (!ReadShopItemOrWeaponNo(args[0]["item_id"], item_no)) {
        return Invalid("item_id must identify a valid Chronicle item, attachment, or weapon");
    }
    const int price_idx = item_no - ITEM_ATTACH_START;
    if (price_idx < 0 || price_idx >= static_cast<int>(std::size(PriceList))) return Invalid("item_id is outside Chronicle's price list");
    const auto price_owner = g_shop_price_owners.find(price_idx);
    if (price_owner != g_shop_price_owners.end() && price_owner->second != owner) {
        return Failed("this native item price is already owned by another mod");
    }
    int buy_price = PriceList[price_idx].buy_price;
    int sell_price = PriceList[price_idx].sell_price;
    if (args[0].contains("buy_price") && !ReadBoundedInt(args[0], "buy_price", 0, 32767, buy_price)) {
        return Invalid("buy_price must be an integer from 0 to 32767");
    }
    if (args[0].contains("sell_price") && !ReadBoundedInt(args[0], "sell_price", 0, 32767, sell_price)) {
        return Invalid("sell_price must be an integer from 0 to 32767");
    }
    if (!g_original_shop_prices.contains(price_idx)) {
        g_original_shop_prices.emplace(price_idx, PriceList[price_idx]);
    }
    g_shop_price_owners[price_idx] = std::string(owner);
    PriceList[price_idx].buy_price = static_cast<s16>(buy_price);
    PriceList[price_idx].sell_price = static_cast<s16>(sell_price);
    return {ApiCallStatus::Ok,
            {{"item_no", item_no}, {"buy_price", buy_price}, {"sell_price", sell_price}, {"changed", true}},
            {}};
}

std::string_view LimitZoneName(int zone_id) {
    switch (zone_id) {
        case RES_LIMIT_ZONE_NONE: return "none";
        case RES_LIMIT_ZONE_TOAN: return "toan_only";
        case RES_LIMIT_ZONE_XIAO: return "xiao_only";
        case RES_LIMIT_ZONE_GORO: return "goro_only";
        case RES_LIMIT_ZONE_RUBY: return "ruby_only";
        case RES_LIMIT_ZONE_UNGAGA: return "ungaga_only";
        case RES_LIMIT_ZONE_OSMOND: return "osmond_only";
        case RES_LIMIT_ZONE_NO_WEAPON_CHANGE: return "abs_down";
        case RES_LIMIT_ZONE_THIRST: return "thirst";
        default: return "custom";
    }
}

ApiCallResult GetDungeonFloorRules(const Json &args) {
    if (!args.is_array() || args.size() > 1 || (args.size() == 1 && !args[0].is_object())) {
        return Invalid("expected optional {dungeon_id = 0..5, floor_id = 0..24}");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    int dungeon_id = (selectMapNo >= 0 && selectMapNo < 6) ? selectMapNo : 0;
    int floor_id = (status->cur_floor >= 0 && status->cur_floor < 25) ? status->cur_floor : 0;
    if (args.size() == 1) {
        if (args[0].contains("dungeon_id") && !ReadBoundedInt(args[0], "dungeon_id", 0, 5, dungeon_id)) {
            return Invalid("dungeon_id must be an integer from 0 to 5");
        }
        if (args[0].contains("floor_id") && !ReadBoundedInt(args[0], "floor_id", 0, 24, floor_id)) {
            return Invalid("floor_id must be an integer from 0 to 24");
        }
    }
    const int table_zone = status->res_limit_zone_id[dungeon_id][floor_id];
    const int active_zone = status->res_limit_zone_current;
    return {ApiCallStatus::Ok,
            {{"dungeon_id", dungeon_id}, {"floor_id", floor_id},
             {"limit_zone", table_zone}, {"limit_zone_name", LimitZoneName(table_zone)},
             {"active_limit_zone", active_zone}, {"active_limit_zone_name", LimitZoneName(active_zone)}},
            {}};
}

ApiCallResult SetDungeonFloorRules(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("limit_zone")) {
        return Invalid("expected {limit_zone = -1..11, dungeon_id = optional 0..5, floor_id = optional 0..24}");
    }
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) return Failed("the game has no active save data");
    int limit_zone = RES_LIMIT_ZONE_NONE;
    if (!ReadBoundedInt(args[0], "limit_zone", -1, 11, limit_zone)) {
        return Invalid("limit_zone must be an integer from -1 to 11");
    }
    int dungeon_id = (selectMapNo >= 0 && selectMapNo < 6) ? selectMapNo : 0;
    int floor_id = (status->cur_floor >= 0 && status->cur_floor < 25) ? status->cur_floor : 0;
    if (args[0].contains("dungeon_id") && !ReadBoundedInt(args[0], "dungeon_id", 0, 5, dungeon_id)) {
        return Invalid("dungeon_id must be an integer from 0 to 5");
    }
    if (args[0].contains("floor_id") && !ReadBoundedInt(args[0], "floor_id", 0, 24, floor_id)) {
        return Invalid("floor_id must be an integer from 0 to 24");
    }
    const int previous = status->res_limit_zone_id[dungeon_id][floor_id];
    const bool changed = previous != limit_zone || status->res_limit_zone_current != limit_zone;
    status->res_limit_zone_id[dungeon_id][floor_id] = limit_zone;
    status->res_limit_zone_current = limit_zone;
    return {ApiCallStatus::Ok,
            {{"dungeon_id", dungeon_id}, {"floor_id", floor_id},
             {"previous_limit_zone", previous}, {"limit_zone", limit_zone},
             {"limit_zone_name", LimitZoneName(limit_zone)}, {"changed", changed}},
            {}};
}

ApiCallResult GetDungeonLoot(const Json &args) {
    if (!args.is_array() || args.size() > 1 || (args.size() == 1 && !args[0].is_object())) {
        return Invalid("expected optional {monster_id = 0..32767}");
    }
    int monster_id = 0;
    if (args.size() == 1 && args[0].contains("monster_id")) {
        if (!ReadBoundedInt(args[0], "monster_id", 0, 32767, monster_id)) {
            return Invalid("monster_id must be a valid native monster model id");
        }
    }
    MONSTOR_MODEL *model = GetMonstorTable(monster_id);
    if (model == nullptr) return Invalid("monster_id does not identify a loaded native monster definition");
    return {ApiCallStatus::Ok,
            {{"monster_id", monster_id},
             {"rare_item", model->rare_item},
             {"steal_item", model->steal_item},
             {"attachment_kind", model->attachment_kind},
             {"drops_items", model->drops_items != 0}},
            {}};
}

ApiCallResult OverrideDungeonLoot(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() ||
        !args[0].contains("monster_id")) {
        return Invalid("expected {monster_id = 0..32767, rare_item = optional, steal_item = optional}");
    }
    int monster_id = -1;
    if (!ReadBoundedInt(args[0], "monster_id", 0, 32767, monster_id)) {
        return Invalid("monster_id must be a valid native monster model id");
    }
    MONSTOR_MODEL *model = GetMonstorTable(monster_id);
    if (model == nullptr) return Invalid("monster_id does not identify a loaded native monster definition");
    int rare_item = model->rare_item;
    int steal_item = model->steal_item;
    if (args[0].contains("rare_item") && !ReadBoundedInt(args[0], "rare_item", -1, 450, rare_item)) {
        return Invalid("rare_item must be -1 or a valid Chronicle item number");
    }
    if (args[0].contains("steal_item") && !ReadBoundedInt(args[0], "steal_item", -1, 450, steal_item)) {
        return Invalid("steal_item must be -1 or a valid Chronicle item number");
    }
    if (!g_original_monster_drops.contains(monster_id)) {
        g_original_monster_drops.emplace(monster_id,
                                         ChronicleMonsterDropBackup{monster_id, model->rare_item,
                                                                    model->steal_item, model->attachment_kind});
    }
    const bool changed = model->rare_item != rare_item || model->steal_item != steal_item;
    model->rare_item = static_cast<s16>(rare_item);
    model->steal_item = static_cast<s16>(steal_item);
    return {ApiCallStatus::Ok,
            {{"monster_id", monster_id}, {"rare_item", model->rare_item},
             {"steal_item", model->steal_item}, {"changed", changed}},
            {}};
}

// Sandbox sessions (title modes): session.start from the mod's open title mode screen, session.finish in play.
// session.start {visit = {world, player?, map}}: a co-op visit to the host's world (moonmod/coop_save.hpp).
ApiCallResult StartVisitSession(std::string_view mod_id, const Json &visit) {
    if (!visit.is_object()) return Invalid("visit must be {world, player?, map}");
    for (auto field = visit.begin(); field != visit.end(); ++field) {
        if (field.key() != "world" && field.key() != "player" && field.key() != "map") return Invalid("visit accepts only world, player and map");
    }
    if (mode != GAME_MODE_TITLE) return Failed("a visit starts from the title screen");
    const TitleMode *open = OpenTitleModeScreenMode();
    if (open == nullptr || open->owner != mod_id) return Failed("start the visit from this mod's open title mode screen");
    if (SandboxRequestPending()) return Failed("a session is already starting");
    if (!visit.contains("world") || !visit["world"].is_string() || !visit["world"].get_ref<const std::string &>().starts_with("DCW1")) {
        return Invalid("world must be the host's save.world_export text");
    }
    if (visit.contains("player") && (!visit["player"].is_string() || !visit["player"].get_ref<const std::string &>().starts_with("DCP1"))) {
        return Invalid("player must be save.player_export text, or left out for a new account");
    }
    int map = 0;
    if (!visit.contains("map") || !ReadBoundedInt(visit, "map", 0, TOWN_COUNT - 1, map)) {
        return Invalid("map must be the town map to start in, 0-" + std::to_string(TOWN_COUNT - 1));
    }
    SandboxRequest queued;
    queued.owner = std::string(mod_id);
    queued.mode_id = open->id;
    queued.visit = true;
    queued.map = map;
    queued.world = visit["world"].get<std::string>();
    if (visit.contains("player")) queued.player = visit["player"].get<std::string>();
    QueueSandboxRequest(std::move(queued));
    return {ApiCallStatus::Ok, {{"queued", true}, {"visit", true}, {"map", map}, {"new_account", !visit.contains("player")}}, {}};
}

ApiCallResult StartSandboxSession(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) {
        return Invalid("expected {floor = registered floor id, party_size = 1..7, character = optional id}, or {visit = ...}");
    }
    const Json &request = args[0];
    if (request.contains("visit")) {
        if (request.size() != 1) return Invalid("a visit takes only visit = {world, player?, map}");
        return StartVisitSession(mod_id, request["visit"]);
    }
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "floor" && field.key() != "party_size" && field.key() != "character") {
            return Invalid("session.start accepts only floor, party_size and character");
        }
    }
    if (mode != GAME_MODE_TITLE) return Failed("a sandbox session starts from the title screen");
    const TitleMode *open = OpenTitleModeScreenMode();
    if (open == nullptr || open->owner != mod_id) return Failed("start the session from this mod's open title mode screen");
    if (SandboxRequestPending()) return Failed("a sandbox session is already starting");
    if (!request.contains("floor") || !request["floor"].is_string()) {
        return Invalid("floor must be the stable id of a floor registered with dungeons.register");
    }
    const std::string &floor_id = request["floor"].get_ref<const std::string &>();
    const ModDungeon *dungeon = nullptr;
    const ModDungeonFloor *floor = nullptr;
    for (const auto &[id, candidate] : g_mod_dungeons) {
        (void)id;
        const auto found = std::ranges::find(candidate.floors, floor_id, &ModDungeonFloor::id);
        if (found != candidate.floors.end()) {
            dungeon = &candidate;
            floor = &*found;
            break;
        }
    }
    if (dungeon == nullptr || floor == nullptr) return Failed("no registered dungeon floor has that id");
    int party_size = 1;
    if (request.contains("party_size") && !ReadBoundedInt(request, "party_size", 1, CharaCount(), party_size)) {
        return Invalid("party_size must be an integer from 1 to the number of playable characters (6, and one more per registered one)");
    }
    int character = 0;
    if (request.contains("character")) {
        if (!request["character"].is_string()) return Invalid("character must be a playable character id");
        const auto found = std::ranges::find(kCharacterIds, request["character"].get<std::string>());
        if (found == kCharacterIds.end()) return Invalid("character must be a playable character id");
        character = static_cast<int>(found - kCharacterIds.begin());
    }
    if (character >= party_size) return Invalid("the starting character must be within party_size (Toan, Xiao, Goro, Ruby, Ungaga, Osmond)");
    SandboxRequest queued;
    queued.owner = std::string(mod_id);
    queued.mode_id = open->id;
    queued.floor_id = floor->id;
    queued.dungeon_id = dungeon->native_dungeon_id;
    queued.floor = floor->native_floor_id;
    queued.party_size = party_size;
    queued.character = character;
    QueueSandboxRequest(queued);
    return {ApiCallStatus::Ok,
            {{"queued", true}, {"floor", floor->id}, {"native_dungeon_id", dungeon->native_dungeon_id},
             {"native_floor_id", floor->native_floor_id}, {"party_size", party_size},
             {"character", kCharacterIds[character]}},
            {}};
}

ApiCallResult EndSandboxSessionApi(std::string_view mod_id, const Json &args) {
    if (!args.is_array() || args.size() > 1 || (args.size() == 1 && (!args[0].is_object() || !args[0].empty()))) {
        return Invalid("session.finish takes no arguments");
    }
    const SandboxSession *session = ActiveSandbox();
    if (session == nullptr) return Failed("no sandbox session is running");
    if (session->owner != mod_id) return Failed("only the mod that started the sandbox session can end it");
    if (!session->visit && mode != GAME_MODE_DUNGEON) return Failed("a sandbox session ends from its dungeon");
    if (session->visit && mode != GAME_MODE_DUNGEON && mode != GAME_MODE_EDIT) return Failed("a visit ends from a town or a dungeon");
    if (NextMapNo >= 0) return Failed("a native map transition is already pending");
    NoteSandboxEnding("ended");
    MapJump(800, -1);
    return {ApiCallStatus::Ok, {{"queued", true}, {"mode", session->mode_id}}, {}};
}

// net.*: one online session for the game, owned by the mod that opened it (moonmod/net_session.hpp).
Json NetStatusJson() {
    moonmod::net::Session &session = moonmod::net::Shared();
    Json peers = Json::array();
    for (const moonmod::net::Peer &peer : session.peers()) peers.push_back({{"id", peer.id}, {"name", peer.name}});
    Json out = {{"state", std::string(moonmod::net::StateName(session.state()))}, {"available", session.available()},
                {"peers", std::move(peers)}};
    if (session.id() >= 0) out["id"] = session.id();
    if (!session.code().empty()) out["code"] = session.code();
    if (!session.owner().empty()) out["owner"] = session.owner();
    return out;
}

ApiCallResult NetCall(std::string_view mod_id, const ApiOperationSpec &operation, const Json &args) {
    moonmod::net::Session &session = moonmod::net::Shared();
    const bool            no_args = args.is_array() && (args.empty() || (args.size() == 1 && args[0].is_object() && args[0].empty()));
    const Json            empty = Json::object();
    const Json           &request = args.is_array() && args.size() == 1 && args[0].is_object() ? args[0] : empty;
    if (!args.is_array() || args.size() > 1 || (args.size() == 1 && !args[0].is_object())) return Invalid("expected one table, or none");
    const auto text_of = [&request](const char *key) {
        return request.contains(key) && request[key].is_string() ? request[key].get<std::string>() : std::string();
    };
    if (operation.path == "net.status") {
        if (!no_args) return Invalid("net.status takes no arguments");
        session.Pump();
        return {ApiCallStatus::Ok, NetStatusJson(), {}};
    }
    const bool mine = session.owner() == mod_id;
    if (!session.owner().empty() && !mine) return Failed("another mod's online session is open (" + session.owner() + ")");
    if (operation.path == "net.host" || operation.path == "net.join") {
        for (auto field = request.begin(); field != request.end(); ++field) {
            const std::string &key = field.key();
            if (key != "name" && key != "relay" && key != "version" && key != "code") return Invalid("unknown field: " + key);
        }
        const bool  join = operation.path == "net.join";
        std::string code = text_of("code");
        if (join) {
            std::ranges::transform(code, code.begin(), [](unsigned char c) { return static_cast<char>(std::toupper(c)); });
            if (code.size() != 5 || !std::ranges::all_of(code, [](unsigned char c) { return std::isalnum(c) != 0; })) {
                return Invalid("code must be the room's five letters and digits");
            }
        } else if (request.contains("code")) {
            return Invalid("net.host takes no code: the relay gives the room one");
        }
        if (!request.contains("version") || !request["version"].is_number_integer() || request["version"].get<int>() < 1 ||
            request["version"].get<int>() > 9999) {
            return Invalid("version must be the mod's message protocol number, 1-9999: players on other numbers can't join");
        }
        std::string name = text_of("name");
        if (name.size() > 24 || !moonmod::net::ValidUtf8(name)) return Invalid("name must be text of at most 24 bytes");
        const std::string relay = moonmod::net::RelayAddress(text_of("relay"));
        std::string       error;
        if (!session.Start(mod_id, relay, join ? code : std::string(), name, "chronicle", request["version"].get<int>(), error)) {
            return session.available() ? Failed(error) : ApiCallResult{ApiCallStatus::Unsupported, {}, error};
        }
        return {ApiCallStatus::Ok, NetStatusJson(), {}};
    }
    if (operation.path == "net.leave") {
        if (!no_args) return Invalid("net.leave takes no arguments");
        const bool was_open = session.state() != moonmod::net::State::Idle;
        if (mine) session.Leave();
        return {ApiCallStatus::Ok, {{"left", was_open && mine}}, {}};
    }
    if (operation.path == "net.send") {
        for (auto field = request.begin(); field != request.end(); ++field) {
            if (field.key() != "to" && field.key() != "data") return Invalid("unknown field: " + field.key());
        }
        if (!request.contains("data") || !request["data"].is_string()) return Invalid("data must be text");
        int to = -1;
        if (request.contains("to") && request["to"].is_string() && request["to"].get<std::string>() == "all") {
            to = moonmod::net::kBroadcast;
        } else if (request.contains("to") && request["to"].is_number_integer()) {
            to = request["to"].get<int>();
        } else {
            return Invalid("to must be a player's id or \"all\"");
        }
        if (!mine) return Failed("not in an online session");
        std::string error;
        if (!session.Send(to, request["data"].get_ref<const std::string &>(), error)) return Failed(error);
        return {ApiCallStatus::Ok, {{"sent", true}}, {}};
    }
    if (operation.path == "net.receive") {
        for (auto field = request.begin(); field != request.end(); ++field) {
            if (field.key() != "max") return Invalid("unknown field: " + field.key());
        }
        std::size_t max = 256;
        if (request.contains("max")) {
            if (!request["max"].is_number_integer() || request["max"].get<int>() < 1 || request["max"].get<int>() > 1024) {
                return Invalid("max must be 1-1024");
            }
            max = static_cast<std::size_t>(request["max"].get<int>());
        }
        Json events = Json::array();
        if (mine) {
            for (moonmod::net::Event &event : session.Receive(max)) {
                Json entry = {{"kind", event.kind}};
                if (event.peer >= 0) entry["peer"] = event.peer;
                if (event.kind == "message") entry["data"] = std::move(event.data);
                if (event.kind == "connected") entry["code"] = std::move(event.data);
                if (event.kind == "closed") entry["reason"] = std::move(event.data);
                if (!event.name.empty()) entry["name"] = std::move(event.name);
                events.push_back(std::move(entry));
            }
        }
        return {ApiCallStatus::Ok, {{"events", std::move(events)}}, {}};
    }
    return Invalid("unknown net operation");
}

ApiCallResult Call(std::string_view mod_id, const std::filesystem::path &save_root,
                   const ApiOperationSpec &operation, const Json &args) {
    if (operation.path == "save.slot_id") {
        if (!args.is_array() || !args.empty()) return Invalid("save.slot_id takes no arguments");
        // A sandbox session belongs to no save slot, whichever slot was loaded last.
        const int file_no = SandboxActive() ? -1 : SaveSlotCurrentNativeFileNo();
        if (file_no < 0) return {ApiCallStatus::Ok, {{"slot_id", nullptr}, {"slot_generation", nullptr}}, {}};
        const std::uint64_t generation = SaveSlotGeneration(file_no);
        if (generation == 0) return {ApiCallStatus::Failed, {}, "Could not persist native save-slot identity"};
        return {ApiCallStatus::Ok,
                {{"slot_id", file_no + 1}, {"slot_generation", generation}},
                {}};
    }
    if (operation.path == "content.list") return ListContent(args);
    if (operation.path == "content.register") return RegisterContent(mod_id, save_root, args);
    if (operation.path == "content.unregister") return UnregisterContent(mod_id, args);
    if (operation.path == "animations.register") return RegisterAnimation(mod_id, args);
    if (operation.path == "animations.play") return PlayAnimation(mod_id, args);
    if (operation.path == "animations.stop") return StopAnimation(mod_id, args);
    if (operation.path == "characters.register") return RegisterCharacter(mod_id, args);
    if (operation.path == "characters.register_playable") return RegisterPlayable(mod_id, args);
    if (operation.path == "models.register") return RegisterModel(mod_id, args);
    if (operation.path == "models.unregister") return UnregisterModel(mod_id, args);
    if (operation.path == "models.replace") return ReplaceNativeModel(mod_id, args);
    if (operation.path == "models.remove") return RemoveNativeModelReplacement(mod_id, args);
    if (operation.path == "models.resolve") return ResolveModel(args);
    if (operation.path == "characters.spawn" || operation.path == "entities.spawn") return SpawnCharacter(mod_id, args);
    if (operation.path == "combat.get") return GetCombat(args);
    if (operation.path == "combat.status_add") return AddMonsterStatus(args);
    if (operation.path == "combat.strike_player") return StrikePlayer(args);
    if (operation.path == "combat.feel_set") return SetHitFeel(mod_id, args);
    if (operation.path == "combat.feel_clear") return ClearHitFeel(mod_id);
    if (operation.path == "combat.register_player_formula") return g_player_damage_formulas.Register(mod_id, args);
    if (operation.path == "combat.register_formula") return g_combat_damage_formulas.Register(mod_id, args);
    if (operation.path == "combat.damage") return DamageCombatEnemy(args);
    if (operation.path == "combat.heal") return HealCombatEnemy(args);
    if (operation.path == "entities.get") return GetEntity(args);
    if (operation.path == "entities.set_stats") return SetEntityStats(args);
    if (operation.path == "entities.scale_set") return SetEntityScale(mod_id, args);
    if (operation.path == "entities.script_int_set") return SetEntityScriptInt(args);
    if (operation.path == "physics.query") return QueryPhysics(args);
    if (operation.path == "physics.velocity_get") return GetEntityVelocity(args);
    if (operation.path == "physics.velocity_set") return SetEntityVelocity(args);
    if (operation.path == "entities.query") return QueryEntities(args);
    if (operation.path == "entities.despawn") return DespawnEntity(mod_id, args);
    if (operation.path == "entities.set_active_limit") return SetActiveMonsterLimit(args);
    if (operation.path == "dungeons.selection") {
        if (DEnterMenu.state != DUN_ENTER_SELECT || !DEnterMenu.textures_ready) return {ApiCallStatus::Ok, nullptr, {}};
        return {ApiCallStatus::Ok, {{"dungeon_id", DEnterMenu.dungeon}, {"floor_id", DEnterMenu.selected_floor}}, {}};
    }
    if (operation.path == "dungeons.set_badges") {
        if (!args.is_array() || args.size()!=1 || !args[0].is_object() || args[0].size()>701) return Invalid("expected bounded tier badge object");
        for (auto field=args[0].begin();field!=args[0].end();++field) {
            if (!field.value().is_number_integer() || field.value()<0 || field.value()>5 || field.key().size()>12) return Invalid("badge tiers must be 0..5");
        }
        g_floor_badges[std::string(mod_id)]=args[0]; return {ApiCallStatus::Ok, {{"updated",true}}, {}};
    }
    if (operation.path == "dungeons.set_modifiers") return SetFloorModifiers(mod_id, args);
    if (operation.path == "dungeons.set_population") return SetPopulationScaling(mod_id, args);
    if (operation.path == "dungeons.set_enemy_survival") return SetEnemySurvivalChance(mod_id, args);
    if (operation.path == "dungeons.set_layout") return SetDungeonLayout(mod_id, args);
    if (operation.path == "dungeons.set_seed") return SetDungeonSeed(args);
    if (operation.path == "dungeons.set_opened_chests") return SetOpenedChests(args);
    if (operation.path == "dungeons.chests_get") return GetChests(args);
    if (operation.path == "dungeons.set_exit_room") return SetExitRoom(args);
    if (operation.path == "dungeons.floor_rules_get") return GetDungeonFloorRules(args);
    if (operation.path == "dungeons.floor_rules_set") return SetDungeonFloorRules(args);
    if (operation.path == "dungeons.floor_redirect") return SetFloorRedirect(mod_id, args);
    if (operation.path == "dungeons.set_shortcuts") return SetShortcutDoors(mod_id, args);
    if (operation.path == "dungeons.set_weapon_drops") return SetWeaponDrops(mod_id, args);
    if (operation.path == "dungeons.chest_weapons_add") return AddChestWeapon(mod_id, args);
    if (operation.path == "dungeons.chest_weapons_clear") {
        ClearMoonModChestWeapons(mod_id);
        return {ApiCallStatus::Ok, {{"cleared", true}}, {}};
    }
    if (operation.path == "dungeons.set_backrooms") return SetBackrooms(mod_id, args);
    if (operation.path == "dungeons.set_story_exits") return SetStoryExits(mod_id, args);
    if (operation.path == "dungeons.set_floor_select") return SetFloorSelect(mod_id, args);
    if (operation.path == "dungeons.set_quick_passage") return SetQuickPassage(mod_id, args);
    if (operation.path == "world.set_roads") return SetRoads(mod_id, args);
    if (operation.path == "dungeons.play_event") return PlayEvent(mod_id, args);
    if (operation.path == "dungeons.set_npc_marks") return SetNpcMarks(mod_id, args);
    if (operation.path == "dungeons.set_npc_models") return SetNpcModels(mod_id, args);
    if (operation.path == "dungeons.set_warden_room") return SetWardenRoom(mod_id, args);
    if (operation.path == "dungeons.set_zones") return SetZones(mod_id, args);
    if (operation.path == "dungeons.zones_get") return GetZones();
    if (operation.path == "dungeons.set_exits") return SetExits(mod_id, args);
    if (operation.path == "dungeons.warp") return WarpPlayer(args);
    if (operation.path == "dungeons.drop_gilda") return DropGilda(args);
    if (operation.path == "dungeons.enter_floor") return EnterFloor(args);
    if (operation.path == "dungeons.set_arrival") return SetArrival(args);
    if (operation.path == "dungeons.set_character_blocks") return SetCharacterBlocks(mod_id, args);
    if (operation.path == "weapons.style_set") return SetWeaponStyle(mod_id, args);
    if (operation.path == "weapons.style_clear") return ClearWeaponStyle(mod_id, args);
    if (operation.path == "effects.burst") return BurstEffect(args);
    if (operation.path == "entities.tint") return TintMonster(args);
    if (operation.path == "entities.knockback") return KnockbackMonster(args);
    if (operation.path == "dungeons.warden_room_get") return {ApiCallStatus::Ok, WardenRoomJson(), {}};
    if (operation.path == "dungeons.set_rest_room") return SetRestRoom(mod_id, args);
    if (operation.path == "dungeons.rest_room_get") return {ApiCallStatus::Ok, RestRoomJson(), {}};
    if (operation.path == "dungeons.warden_room_release") return ReleaseWardenRoom(mod_id);
    if (operation.path == "dungeons.room_seal") return SealRoom(mod_id, args);
    if (operation.path == "dungeons.room_release") return ReleaseRoom(mod_id);
    if (operation.path == "dungeons.zone_gate_open") return OpenZoneGate(mod_id, args);
    if (operation.path == "ui.choice_open") return OpenChoicePanel(mod_id, args);
    if (operation.path == "ui.map_open") return OpenMapScreen(mod_id, args);
    if (operation.path == "ui.sky_open") return OpenSkyScreen(mod_id, args);
    if (operation.path == "ui.sky_close") {
        dc::game::MoonModSkyCloseOwner(mod_id);
        return {ApiCallStatus::Ok, {{"closed", true}}, {}};
    }
    if (operation.path == "ui.map_close") {
        dc::game::MoonModMapCloseOwner(mod_id);
        return {ApiCallStatus::Ok, {{"closed", true}}, {}};
    }
    if (operation.path == "dungeons.map_snapshot") return MapSnapshot();
    if (operation.path == "dungeons.set_seen") return SetSeen(args);
    if (operation.path == "dungeons.set_waypoint") return SetWaypoint(args);
    if (operation.path == "ui.boss_card") {
        if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {name, kicker?, epithet?, frames?}");
        for (auto field = args[0].begin(); field != args[0].end(); ++field) {
            if (field.key() != "name" && field.key() != "kicker" && field.key() != "epithet" && field.key() != "frames") {
                return Invalid("ui.boss_card received an unsupported field: " + field.key());
            }
        }
        const std::string name = args[0].value("name", std::string()), kicker = args[0].value("kicker", std::string()),
                          epithet = args[0].value("epithet", std::string());
        if (name.empty() || name.size() > 40 || kicker.size() > 48 || epithet.size() > 64) return Invalid("name is 1-40 bytes, kicker up to 48, epithet up to 64");
        int frames = 180;
        if (args[0].contains("frames") && !ReadBoundedInt(args[0], "frames", 30, 600, frames)) return Invalid("frames is 30-600");
        if (mode != GAME_MODE_DUNGEON) return Failed("a boss card shows during dungeon play");
        dc::game::MoonModBossCard(kicker, name, epithet, frames);
        return {ApiCallStatus::Ok, {{"frames", frames}}, {}};
    }
    if (operation.path == "ui.run_menu_set") return SetRunMenu(mod_id, args);
    if (operation.path == "world.dungeon_npc_set") {
        if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected {id, model = game path to a .chr, place = \"exit\" or \"player\"}");
        const Json &request = args[0];
        for (auto field = request.begin(); field != request.end(); ++field) {
            if (field.key() != "id" && field.key() != "model" && field.key() != "place") return Invalid("world.dungeon_npc_set received an unsupported field: " + field.key());
        }
        const std::string id = request.value("id", std::string()), model = request.value("model", std::string()), place = request.value("place", std::string("exit"));
        if (id.empty() || id.size() > 32) return Invalid("id must be a string of 1-32 bytes");
        if (model.size() < 5 || model.size() > 96 || !model.ends_with(".chr") || model.find("..") != std::string::npos || model.find('\\') != std::string::npos) {
            return Invalid("model must be a game path to a .chr, such as gedit/e01/chara/p02a.chr");
        }
        if (place != "exit" && place != "player") return Invalid("place is \"exit\" or \"player\"");
        if (mode != GAME_MODE_DUNGEON) return Failed("a figure stands on a dungeon floor");
        if (!dc::game::MoonModNpcSet(std::string(mod_id), id, model, place == "exit" ? 0 : 1)) return Failed("another mod has a figure on this floor");
        return {ApiCallStatus::Ok, {{"id", id}}, {}};
    }
    // net.*: online play through the co-op relay (moonmod/net_session.hpp). One session at a time, owned by the mod that
    // opened it; the others may read net.status but not use the session.
    if (operation.path.starts_with("net.")) return NetCall(mod_id, operation, args);
    // messages.*: notes players leave for each other through the messages folder (moonmod/messages_moonmod.hpp). The
    // folder is the player's choice, never a mod's: no call takes a path.
    if (operation.path.starts_with("messages.")) {
        if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid("expected one table");
        const Json &request = args[0];
        const auto  text_of = [&request](const char *key) -> std::string {
            return request.contains(key) && request[key].is_string() ? request[key].get<std::string>() : std::string();
        };
        const bool  shared = moonmod::messages::FolderFor(save_root).shared;
        std::string error;
        if (operation.path == "messages.post") {
            std::string id;
            if (!moonmod::messages::ValidPlace(text_of("place")) || !moonmod::messages::ValidText(text_of("text"))) {
                return Invalid("expected {place, text}: place 1-64 of a-z 0-9 : _ . -, text 1-120 printable ASCII");
            }
            if (!moonmod::messages::Post(save_root, mod_id, text_of("place"), text_of("text"), id, error)) return Failed(error);
            return {ApiCallStatus::Ok, {{"id", id}, {"shared", shared}}, {}};
        }
        if (operation.path == "messages.list") {
            std::vector<std::string> places;
            if (request.contains("place")) places.push_back(text_of("place"));
            if (request.contains("places")) {
                if (!request["places"].is_array() || request["places"].size() > 32) return Invalid("places is a list of 1-32 places");
                for (const Json &place : request["places"]) {
                    if (!place.is_string()) return Invalid("places is a list of 1-32 places");
                    places.push_back(place.get<std::string>());
                }
            }
            if (places.empty()) return Invalid("expected {place} or {places = {...}}");
            std::vector<moonmod::messages::Message> found;
            if (!moonmod::messages::List(save_root, mod_id, places, found, error)) return Invalid(error);
            Json list = Json::array();
            for (const moonmod::messages::Message &m : found) {
                list.push_back({{"id", m.id}, {"place", m.place}, {"text", m.text}, {"at", m.at}, {"good", m.good}, {"poor", m.poor},
                                {"mine", m.mine}, {"rated", m.rated}});
            }
            return {ApiCallStatus::Ok, {{"messages", list}, {"shared", shared}}, {}};
        }
        if (operation.path == "messages.rate") {
            if (!request.contains("rating") || !request["rating"].is_number_integer()) return Invalid("expected {id, rating = 1, -1 or 0}");
            if (!moonmod::messages::Rate(save_root, text_of("id"), request["rating"].get<int>(), error)) return Failed(error);
            return {ApiCallStatus::Ok, {{"id", text_of("id")}, {"rating", request["rating"]}}, {}};
        }
        if (operation.path == "messages.remove") {
            if (!moonmod::messages::Remove(save_root, text_of("id"), error)) return Failed(error);
            return {ApiCallStatus::Ok, {{"removed", text_of("id")}}, {}};
        }
    }
    // world.ghost_get: {character, frames = {{x, y, z, heading, motion}, ...}} oldest first, every other frame.
    // georama.town_export / town_import: one town's Georama for co-op (moonmod/coop_save.hpp). A town open in Georama's
    // edit mode isn't imported over; a town the player walks in is walked into again, so it shows what came.
    if (operation.path == "georama.town_export" || operation.path == "georama.town_import") {
        const bool importing = operation.path == "georama.town_import";
        if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(importing ? "expected {town_id = 0-4, data}" : "expected {town_id = 0-4}");
        int town = 0;
        if (!ReadBoundedInt(args[0], "town_id", 0, TOWN_COUNT - 1, town)) return Invalid("town_id must be 0-" + std::to_string(TOWN_COUNT - 1));
        if (SaveData == nullptr) return Failed("the game has no save data yet");
        if (!importing) return {ApiCallStatus::Ok, {{"town_id", town}, {"data", moonmod::coop::ExportTown(*SaveData, town)}}, {}};
        if (!args[0].contains("data") || !args[0]["data"].is_string()) return Invalid("data must be georama.town_export text");
        const bool here = mode == GAME_MODE_EDIT && MapNo == town;
        if (here && GameMode != ED_MODE_WALK) return Failed("busy: the player is editing or in a menu in that town; try again in a moment");
        std::string error;
        if (!moonmod::coop::ImportTown(args[0]["data"].get_ref<const std::string &>(), town, *SaveData, error)) return Invalid(error);
        const bool reload = here && NextMapNo < 0;
        if (reload) MapJump(town, -1);
        return {ApiCallStatus::Ok, {{"town_id", town}, {"imported", true}, {"reloaded", reload}}, {}};
    }
    // world.avatar_set / avatar_clear: co-op's remote players on the dungeon floor (moonmod/coop_avatars.hpp).
    if (operation.path == "world.avatar_set") {
        static constexpr const char *kUsage =
            "expected {slot = 1-3, character = a character id, weapon = item_no or nil, x, y, z, heading, motion, tint = {r, g, b} or nil}";
        if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(kUsage);
        const Json &request = args[0];
        for (auto field = request.begin(); field != request.end(); ++field) {
            static constexpr std::array<std::string_view, 9> kFields = {"slot", "character", "weapon", "x", "y", "z", "heading", "motion", "tint"};
            if (std::ranges::find(kFields, field.key()) == kFields.end()) return Invalid(std::string("unknown field: ") + field.key());
        }
        int slot = 0;
        if (!ReadBoundedInt(request, "slot", 1, kMoonModAvatars, slot)) return Invalid(kUsage);
        MoonModAvatarState state;
        if (!request.contains("character") || !request["character"].is_string()) return Invalid(kUsage);
        const auto found = std::ranges::find(kCharacterIds, request["character"].get<std::string>());
        if (found == kCharacterIds.end() || found - kCharacterIds.begin() >= CharaCount()) return Invalid("character must be a playable character id");
        state.character = static_cast<int>(found - kCharacterIds.begin());
        if (request.contains("weapon") && !request["weapon"].is_null()) {
            if (!ReadBoundedInt(request, "weapon", ITEM_WEAPON_START, 1023, state.weapon)) return Invalid("weapon must be a weapon's item_no");
        }
        const auto number = [&request](const char *key, float &out) {
            if (!request.contains(key)) return true;
            if (!request[key].is_number()) return false;
            out = request[key].get<float>();
            return std::isfinite(out) && std::fabs(out) < 1.0e6f;
        };
        if (!number("x", state.x) || !number("y", state.y) || !number("z", state.z) || !number("heading", state.heading)) return Invalid(kUsage);
        if (request.contains("motion") && !ReadBoundedInt(request, "motion", 0, 255, state.motion)) return Invalid("motion must be 0-255");
        if (request.contains("tint") && !request["tint"].is_null()) {
            const Json &tint = request["tint"];
            if (!tint.is_array() || tint.size() != 3) return Invalid("tint must be {r, g, b}, each 0-128");
            for (int c = 0; c < 3; ++c) {
                if (!tint[c].is_number() || tint[c].get<float>() < 0.0f || tint[c].get<float>() > 128.0f) return Invalid("tint must be {r, g, b}, each 0-128");
                state.tint[c] = tint[c].get<float>();
            }
        }
        if (!MoonModAvatarSet(slot - 1, state)) return Failed("that character can't be shown");
        return {ApiCallStatus::Ok, {{"slot", slot}, {"shown", MoonModAvatarShown(slot - 1)}}, {}};
    }
    if (operation.path == "world.avatar_clear") {
        int slot = 0;
        if (!args.is_array() || args.size() > 1 || (args.size() == 1 && (!args[0].is_object() ||
            (args[0].contains("slot") && !ReadBoundedInt(args[0], "slot", 1, kMoonModAvatars, slot))))) {
            return Invalid("expected {slot = 1-3}, or nothing for every one");
        }
        MoonModAvatarClear(slot - 1);
        return {ApiCallStatus::Ok, {{"cleared", true}}, {}};
    }
    if (operation.path == "world.ghost_get") {
        int  character = -1;
        Json frames = Json::array();
        for (const dc::game::MoonModGhostFrame &f : dc::game::MoonModGhostRecorded(character)) {
            frames.push_back({std::lround(f[0]), std::lround(f[1]), std::lround(f[2]), std::round(f[3] * 1000.0f) / 1000.0f,
                              static_cast<int>(f[4])});
        }
        return {ApiCallStatus::Ok, {{"character", character >= 0 ? Json(kCharacterIds[character]) : Json(nullptr)}, {"frames", frames}}, {}};
    }
    // world.ghost_play: {character, frames} as ghost_get gives them (at most 600), or {} to stop.
    if (operation.path == "world.ghost_play") {
        const char *usage = "expected {character, frames = {{x, y, z, heading, motion}, ...}} (1-600 frames), or {}";
        if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
        const Json &request = args[0];
        if (request.empty()) {
            dc::game::MoonModGhostPlay(-1, {});
            return {ApiCallStatus::Ok, {{"playing", false}}, {}};
        }
        if (!request.contains("character") || !request["character"].is_string() || !request.contains("frames") ||
            !request["frames"].is_array() || request["frames"].empty() || request["frames"].size() > 600) {
            return Invalid(usage);
        }
        const auto found = std::ranges::find(kCharacterIds, request["character"].get<std::string>());
        if (found == kCharacterIds.end()) return Invalid("character must be a playable character id");
        std::vector<dc::game::MoonModGhostFrame> frames;
        for (const Json &f : request["frames"]) {
            if (!f.is_array() || f.size() != 5) return Invalid(usage);
            dc::game::MoonModGhostFrame frame{};
            for (int i = 0; i < 5; ++i) {
                if (!f[i].is_number()) return Invalid(usage);
                frame[i] = f[i].get<float>();
                if (!std::isfinite(frame[i]) || std::fabs(frame[i]) > 100000.0f) return Invalid(usage);
            }
            frames.push_back(frame);
        }
        if (mode != GAME_MODE_DUNGEON) return Failed("not in a dungeon");
        if (!dc::game::MoonModGhostPlay(static_cast<int>(found - kCharacterIds.begin()), frames)) return Failed("could not play the ghost");
        return {ApiCallStatus::Ok, {{"playing", true}, {"frames", static_cast<int>(frames.size())}}, {}};
    }
    if (operation.path == "world.dungeon_npc_clear") {
        dc::game::MoonModNpcClear(std::string(mod_id));
        return {ApiCallStatus::Ok, {{"cleared", true}}, {}};
    }
    if (operation.path == "ui.run_menu_clear") {
        dc::game::MoonModMenuClear(mod_id);
        return {ApiCallStatus::Ok, {{"cleared", true}}, {}};
    }
    if (operation.path == "ui.run_menu_open") {
        std::string tab;
        if (args.is_array() && args.size() == 1 && args[0].is_object() && args[0].contains("tab") && args[0]["tab"].is_string()) {
            tab = args[0]["tab"].get<std::string>();
        }
        if (mode != GAME_MODE_DUNGEON) return Failed("the run menu opens during dungeon play");
        if (!dc::game::MoonModMenuRequestOpen(mod_id, tab)) return Failed("this mod has no run menu set");
        return {ApiCallStatus::Ok, {{"opening", true}}, {}};
    }
    if (operation.path == "ui.run_menu_close") {
        dc::game::MoonModMenuRequestClose(mod_id);
        return {ApiCallStatus::Ok, {{"closing", true}}, {}};
    }
    if (operation.path == "ui.choice_close") {
        dc::game::MoonModChoiceCloseOwner(mod_id);
        return {ApiCallStatus::Ok, {{"closed", true}}, {}};
    }
    if (operation.path == "dungeons.shortcuts_get") {
        Json doors = Json::array();
        for (int d = 0; d < DngShortcutDoorCount; ++d) {
            const DNG_SHORTCUT_DOOR &door = DngShortcutDoorTable[d];
            if (door.key_item == 0) {
                doors.push_back({{"x", door.x}, {"y", door.y}, {"illusory", true}, {"locked", door.locked != 0}});
                continue;
            }
            doors.push_back({{"x", door.x}, {"y", door.y}, {"key", ItemStableId(door.key_item)}, {"key_item", door.key_item},
                             {"locked", door.locked != 0}});
        }
        return {ApiCallStatus::Ok, {{"requested", DngShortcutDoors}, {"doors", std::move(doors)}}, {}};
    }
    if (operation.path == "dungeons.loot_get") return GetDungeonLoot(args);
    if (operation.path == "dungeons.loot_override") return OverrideDungeonLoot(args);
    if (operation.path == "dungeons.list") return ListDungeons(args);
    if (operation.path == "dungeons.get") return GetDungeon(mod_id, args);
    if (operation.path == "dungeons.register" || operation.path == "world.register_dungeon") {
        return RegisterDungeon(mod_id, args);
    }
    if (operation.path == "georama.get") return GetGeorama(args);
    if (operation.path == "georama.register") return RegisterGeoramaPiece(mod_id, save_root, args);
    if (operation.path == "georama.element_set") return SetGeoramaElement(args);
    if (operation.path == "georama.request_set") return SetGeoramaRequest(args);
    if (operation.path == "georama.atla_list") return ListAtla(args);
    if (operation.path == "georama.atla_collect") return CollectAtla(args);
    if (operation.path == "georama.atla_spawn") return SpawnModAtla(mod_id, args);
    if (operation.path == "georama.satisfaction_get") return GetGeoramaSatisfaction(args);
    if (operation.path == "dungeons.back_floor_get") return GetBackFloor(args);
    if (operation.path == "dungeons.back_floor_enter") return EnterBackFloor(args);
    if (operation.path == "georama.place") return PlaceGeorama(args);
    if (operation.path == "georama.remove") return RemoveGeorama(args);
    if (operation.path == "player.get") {
        return GetPlayer(args);
    }
    if (operation.path == "player.heal") {
        return HealPlayer(args);
    }
    if (operation.path == "player.damage") {
        return DamagePlayer(args);
    }
    if (operation.path == "player.set") return SetPlayer(args);
    if (operation.path == "player.thirst_get") return GetThirst(args);
    if (operation.path == "player.thirst_set") return SetThirst(args);
    if (operation.path == "player.tuning_set") return SetPlayerTuning(mod_id, args);
    if (operation.path == "player.tuning_clear") return ClearPlayerTuning(mod_id, args);
    if (operation.path == "player.invincible") return SetPlayerInvincible(args);
    if (operation.path == "player.moves_set") return SetPlayerMoves(mod_id, args);
    if (operation.path == "animations.player_register") return RegisterPlayerMotion(mod_id, args);
    if (operation.path == "animations.player_unregister") return UnregisterPlayerMotion(mod_id, args);
    if (operation.path == "animations.player_list") return ListPlayerMotions(mod_id, args);
    if (operation.path == "animations.monster_register") return RegisterMonsterMotion(mod_id, args);
    if (operation.path == "animations.monster_unregister") return UnregisterMonsterMotion(mod_id, args);
    if (operation.path == "animations.monster_list") return ListMonsterMotions(mod_id, args);
    if (operation.path == "player.moves_clear") return ClearPlayerMoves(mod_id, args);
    if (operation.path == "player.form_set") return SetPlayerForm(mod_id, args);
    if (operation.path == "player.move_play") return PlayPlayerMove(mod_id, args);
    if (operation.path == "player.critical_set") return SetCriticalTarget(mod_id, args);
    if (operation.path == "player.critical_clear") return ClearCriticalTarget(mod_id, args);
    if (operation.path == "player.form_clear") return ClearPlayerForm(mod_id, args);
    if (operation.path == "player.fatal_guard") return SetPlayerFatalGuard(args);
    if (operation.path == "player.guard_rules_set") return SetGuardRules(mod_id, args);
    if (operation.path == "player.guard_rules_clear") return ClearPlayerRules(mod_id, args, true);
    if (operation.path == "player.gauge_rules_set") return SetGaugeRules(mod_id, args);
    if (operation.path == "player.gauge_rules_clear") return ClearPlayerRules(mod_id, args, false);
    if (operation.path == "player.gauge_get") return GetPlayerGauge(args);
    if (operation.path == "player.gauge_set") return SetPlayerGauge(args);
    if (operation.path == "player.state") return GetPlayerState(args);
    if (operation.path == "party.companion_set") return SetCompanion(mod_id, args);
    if (operation.path == "party.companion_get") {
        const int out = dc::game::MoonModCompanionLoaded();
        const dc::game::MoonModCompanionLife life = dc::game::MoonModCompanionLifeGet();
        Json result = {{"character", out >= 0 ? Json(kCharacterIds[out]) : Json(nullptr)}, {"out", out >= 0}, {"hostile", life.hostile}};
        if (life.hostile) {
            result["hp"] = life.hp;
            result["max_hp"] = life.max_hp;
        }
        return {ApiCallStatus::Ok, result, {}};
    }
    if (operation.path == "player.status_add") return AddPlayerStatus(args);
    if (operation.path == "player.status_remove") return RemovePlayerStatus(args);
    if (operation.path == "party.list") {
        return ListParty(args);
    }
    if (operation.path == "party.active") {
        return GetActivePartyMember(args);
    }
    if (operation.path == "party.set_active") return SetActivePartyMember(args);
    if (operation.path == "party.join") return SetPartyJoined(args, true);
    if (operation.path == "party.leave") return SetPartyJoined(args, false);
    if (operation.path == "party.town_active") return GetTownActivePartyMember(args);
    if (operation.path == "party.town_set_active") return SetTownActivePartyMember(args);
    if (operation.path == "locations.list") return ListLocations(args);
    if (operation.path == "locations.get") return GetLocation(mod_id, args);
    if (operation.path == "locations.register") return RegisterLocation(mod_id, args);
    if (operation.path == "characters.list") return ListCharacters(args);
    if (operation.path == "characters.get") return GetCharacter(args);
    if (operation.path == "economy.get") return GetEconomy(args);
    if (operation.path == "economy.add") return AddEconomy(args);
    if (operation.path == "economy.spend") return SpendEconomy(args);
    if (operation.path == "world.get") {
        return GetWorld(args);
    }
    if (operation.path == "debug.inspect") return InspectNativeRuntime(args);
    if (operation.path == "title.mode_register") return RegisterTitleMode(mod_id, args);
    if (operation.path == "title.mode_set") return SetTitleMode(mod_id, args);
    if (operation.path == "title.mode_remove") return RemoveTitleMode(mod_id, args);
    // save.world_export / player_export and their imports: co-op's split of a save (moonmod/coop_save.hpp). Importing
    // is a visit's alone, so no mod can write another game's world or things into a player's own save.
    if (operation.path == "save.world_export" || operation.path == "save.player_export") {
        if (!args.is_array() || !args.empty()) return Invalid(std::string(operation.path) + " takes no arguments");
        if (SaveData == nullptr) return Failed("the game has no save data yet");
        const bool world = operation.path == "save.world_export";
        return {ApiCallStatus::Ok, {{"data", world ? moonmod::coop::ExportWorld(*SaveData) : moonmod::coop::ExportPlayer(*SaveData)}}, {}};
    }
    // save.account_*: the host's guests' things, kept in the host's save (moonmod/coop_save.hpp). Not during a visit: the
    // guests' things belong to the host's game.
    if (operation.path.starts_with("save.account")) {
        if (VisitActive()) return Failed("a visit keeps no guests; the host's game does");
        const Json  empty = Json::object();
        const Json &request = args.is_array() && args.size() == 1 && args[0].is_object() ? args[0] : empty;
        if (!args.is_array() || args.size() > 1 || (args.size() == 1 && !args[0].is_object())) return Invalid("expected one table, or none");
        const std::string id = request.contains("id") && request["id"].is_string() ? request["id"].get<std::string>() : std::string();
        if (operation.path == "save.account_list") {
            Json list = Json::array();
            for (const moonmod::coop::Account *account : moonmod::coop::Accounts()) list.push_back({{"id", account->id}, {"name", account->name}});
            return {ApiCallStatus::Ok, {{"accounts", std::move(list)}}, {}};
        }
        if (!moonmod::coop::ValidAccountId(id)) return Invalid("id must be 8-64 of a-z, 0-9 and -");
        if (operation.path == "save.account_get") {
            const moonmod::coop::Account *account = moonmod::coop::AccountGet(id);
            if (account == nullptr) return {ApiCallStatus::Ok, Json::object(), {}};
            return {ApiCallStatus::Ok, {{"id", account->id}, {"name", account->name}, {"data", account->player}}, {}};
        }
        if (operation.path == "save.account_remove") return {ApiCallStatus::Ok, {{"removed", moonmod::coop::AccountRemove(id)}}, {}};
        if (operation.path == "save.account_set") {
            if (!request.contains("data") || !request["data"].is_string()) return Invalid("data must be save.player_export text");
            std::string error;
            if (!moonmod::coop::AccountSet({id, request.value("name", std::string()), request["data"].get<std::string>()}, error)) return Invalid(error);
            return {ApiCallStatus::Ok, {{"kept", true}}, {}};
        }
        return Invalid("unknown save.account operation");
    }
    if (operation.path == "save.world_import" || operation.path == "save.player_import") {
        if (!args.is_array() || args.size() != 1 || !args[0].is_object() || args[0].size() != 1 || !args[0].contains("data") ||
            !args[0]["data"].is_string()) {
            return Invalid("expected {data = the export's text}");
        }
        const SandboxSession *session = ActiveSandbox();
        if (session == nullptr || !session->visit || session->owner != mod_id) return Failed("only this mod's co-op visit imports");
        if (SaveData == nullptr) return Failed("the game has no save data yet");
        std::string error;
        const std::string &data = args[0]["data"].get_ref<const std::string &>();
        const bool ok = operation.path == "save.world_import" ? moonmod::coop::ImportWorld(data, *SaveData, error)
                                                              : moonmod::coop::ImportPlayer(data, *SaveData, error);
        if (!ok) return Invalid(error);
        return {ApiCallStatus::Ok, {{"imported", true}}, {}};
    }
    if (operation.path == "session.get") {
        if (!args.is_array() || !args.empty()) return Invalid("session.get takes no arguments");
        return {ApiCallStatus::Ok, SessionJson(mode == GAME_MODE_TITLE), {}};
    }
    if (operation.path == "session.start") return StartSandboxSession(mod_id, args);
    if (operation.path == "session.finish") return EndSandboxSessionApi(mod_id, args);
    if (operation.path == "world.transition") return TransitionWorld(mod_id, args);
    if (operation.path == "world.set_time") return SetWorldTime(args);
    if (operation.path == "world.story_flag_get") return GetStoryFlag(args);
    if (operation.path == "world.story_flag_set") return SetStoryFlag(args);
    if (operation.path == "world.story_value_get") return GetStoryValue(args);
    if (operation.path == "world.story_value_set") return SetStoryValue(args);
    if (operation.path == "world.map_flag_get") return GetMapFlag(args);
    if (operation.path == "world.map_flag_set") return SetMapFlag(args);
    if (operation.path == "weapons.list") return ListWeapons(args);
    if (operation.path == "weapons.get") return GetWeapon(args);
    if (operation.path == "weapons.register") return RegisterWeapon(mod_id, save_root, args);
    if (operation.path == "weapons.definition_set") return SetWeaponDefinition(mod_id, args);
    if (operation.path == "weapons.stat_label_set") return SetWeaponStatLabels(args);
    if (operation.path == "weapons.give") return GiveWeapon(args);
    if (operation.path == "weapons.equip") return EquipWeapon(args);
    if (operation.path == "weapons.remove") return RemoveWeapon(args);
    if (operation.path == "weapons.upgrade") return UpgradeWeapon(args);
    if (operation.path == "weapons.stats_set") return SetWeaponStats(args);
    if (operation.path == "weapons.durability_set") return SetWeaponDurability(args);
    if (operation.path == "weapons.abs_get") return GetWeaponAbs(args);
    if (operation.path == "weapons.abs_add") return AddWeaponAbs(args);
    if (operation.path == "weapons.attachments_get") return GetWeaponAttachments(args);
    if (operation.path == "weapons.attachment_insert") return InsertWeaponAttachment(args);
    if (operation.path == "weapons.attachment_remove") return RemoveWeaponAttachment(args);
    if (operation.path == "weapons.build_up_list") return ListWeaponBuildUps(args);
    if (operation.path == "weapons.build_up_register") return RegisterWeaponBuildUp(args);
    if (operation.path == "weapons.synthesize") return SynthesizeWeapon(args);
    if (operation.path == "shops.list") return ListShops(args);
    if (operation.path == "shops.get") return GetShop(args);
    if (operation.path == "shops.register_item") return RegisterShopItem(mod_id, args);
    if (operation.path == "shops.remove_item") return RemoveShopItem(mod_id, args);
    if (operation.path == "shops.set_items") return SetShopItems(mod_id, args);
    if (operation.path == "shops.set_price") return SetShopPrice(mod_id, args);
    if (operation.path == "items.list") return ListItems(args);
    if (operation.path == "items.get") return GetItem(args);
    if (operation.path == "items.register") return RegisterItem(mod_id, save_root, args);
    if (operation.path == "items.give") return GiveItems(args);
    if (operation.path == "items.remove") return RemoveItems(args);
    if (operation.path == "items.icon_set") return SetBuiltinItemIcon(mod_id, args);
    if (operation.path == "items.use") return UseItem(args);
    if (operation.path == "items.quick_slots") return GetQuickItemSlots(args);
    if (operation.path == "items.charm_slots") return GetCharmSlots(args);
    if (operation.path == "items.charm_slots_set") return SetCharmSlots(args);
    if (operation.path == "items.gear") return GetGear(args);
    if (operation.path == "items.gear_give") return GiveGear(args);
    if (operation.path == "items.gear_stock_set") return StockGear(args);
    if (operation.path == "items.gear_page_set") return SetGearPage(args);
    if (operation.path == "items.gear_equip") return EquipGearPiece(args);
    if (operation.path == "items.gear_unequip") return UnequipGearPiece(args);
    if (operation.path == "items.gear_remove") return RemoveGearPiece(args);
    if (operation.path == "items.quick_slot_select") return SelectQuickItemSlot(args);
    if (operation.path == "items.quick_slot_use") return UseQuickItemSlot(args);
    if (operation.path == "render.first_person_toggle") return RequestFirstPersonToggle(args);
    if (operation.path == "save.autosave_set") return SetNativeAutosave(args);
    if (operation.path == "input.bind") return BindInputAction(mod_id, args);
    if (operation.path == "input.wheel_mode_set") return SetWheelMode(mod_id, args);
    if (operation.path == "input.block_native") {
        // {buttons = {"square", ...}, frames = 1..120}: the game does not see those buttons until the
        // frame count runs out; a mod renews it each frame it still wants them (e.g. while L2 is held).
        static const std::pair<const char *, int> names[] = {
            {"l2", 0x0001}, {"r2", 0x0002}, {"l1", 0x0004}, {"r1", 0x0008}, {"triangle", 0x0010}, {"circle", 0x0020},
            {"cross", 0x0040}, {"square", 0x0080}, {"select", 0x0100}, {"l3", 0x0200}, {"r3", 0x0400}, {"start", 0x0800}};
        if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("buttons") || !args[0]["buttons"].is_array()) {
            return Invalid("expected {buttons = {\"square\", ...}, frames = 1..120}");
        }
        int mask = 0;
        for (const Json &name : args[0]["buttons"]) {
            if (!name.is_string()) return Invalid("button names must be strings");
            bool known = false;
            for (const auto &[label, bit] : names) if (name.get<std::string>() == label) { mask |= bit; known = true; }
            if (!known) return Invalid("unknown button " + name.get<std::string>());
        }
        const int frames = args[0].contains("frames") && args[0]["frames"].is_number_integer() ? args[0]["frames"].get<int>() : 2;
        if (frames < 1 || frames > 120) return Invalid("frames must be 1..120");
        g_native_block_mask = mask;
        g_native_block_until = SharedRuntime().Frame() + static_cast<std::uint64_t>(frames);
        return {ApiCallStatus::Ok, {{"mask", mask}, {"frames", frames}}, {}};
    }
    if (operation.path == "input.state") return ReadInputAction(mod_id, args, false);
    if (operation.path == "input.consume") return ReadInputAction(mod_id, args, true);
    if (operation.path == "input.axis") return ReadInputAxis(args);
    if (operation.path == "input.unbind") return UnbindInputAction(mod_id, args);
    if (operation.path == "input.context_push") return PushInputContext(mod_id, args);
    if (operation.path == "input.context_pop") return PopInputContext(mod_id, args);
    if (operation.path == "input.context_get") return GetInputContext(args);
    if (operation.path == "render.camera_get") return ReadDungeonCamera(args);
    if (operation.path == "render.camera_set") return SetDungeonCamera(args);
    if (operation.path == "dungeons.lighting_get") return ReadDungeonLighting(args);
    if (operation.path == "monsters.natives") return ListMonsterNatives(args);
    if (operation.path == "events.natives") return ListEventNatives(args);
    if (operation.path == "text.get") return GetText(args);
    if (operation.path == "text.set") return SetText(mod_id, args, false);
    if (operation.path == "text.clear") return SetText(mod_id, args, true);
    if (operation.path == "events.call") return CallEventNative(args);
    if (operation.path == "monsters.call") return CallMonsterNative(args);
    if (operation.path == "dungeons.lighting_set") return SetDungeonLighting(args);
    if (operation.path == "render.town_camera_set") return SetTownCamera(mod_id, args);
    if (operation.path == "render.layer_register") return RegisterRenderLayer(mod_id, args);
    if (operation.path == "render.layer_remove") return RemoveRenderLayer(mod_id, args);
    if (operation.path == "render.bar_register") return RegisterRenderBar(mod_id, args);
    if (operation.path == "render.bar_remove") return RemoveRenderBar(mod_id, args);
    if (operation.path == "render.target_bar_set") return SetTargetBar(args);
    if (operation.path == "render.target_meter_set") return SetTargetMeter(args);
    if (operation.path == "render.hud_icon_set") return SetHudIcon(mod_id, args);
    if (operation.path == "render.hud_icon_remove") return RemoveHudIcon(mod_id, args);
    if (operation.path == "render.floor_label_set") return SetFloorLabel(mod_id, args);
    if (operation.path == "render.floor_label_clear") return ClearFloorLabel(mod_id, args);
    if (operation.path == "debug.overlay_register") return RegisterDebugOverlay(mod_id, args);
    return {ApiCallStatus::Unsupported, nullptr, "this Chronicle adapter does not implement the operation"};
}

} // namespace

void ApplyChronicleTownCamera() {
    if (g_town_camera_requests.empty() || mode != GAME_MODE_EDIT || GameMode != ED_MODE_WALK ||
        MapNo < 0 || MapNo >= TOWN_COUNT || NowCamera != static_cast<CCamera *>(&MainCamera) ||
        Chara == nullptr || CCamera::StopCamera != 0) return;
    const auto &offset = g_town_camera_requests.back().second;
    float target[4]{};
    Chara->GetPosition(target);
    if (!std::isfinite(target[0]) || !std::isfinite(target[1]) || !std::isfinite(target[2])) return;
    NowCamera->SetPos(nullptr, target[0], target[1] + offset[0], target[2] + offset[1]);
    NowCamera->SetRef(target[0], target[1], target[2]);
}

int RefreshChronicleWheelMode() {
    if (g_wheel_mode_requests.empty()) {
        InputSetWheelMode(2);
        return 2;
    }
    const int requested = g_wheel_mode_requests.back().second;
    const bool live_dungeon_play = mode == GAME_MODE_DUNGEON && BtGameModeFlag == BT_GAME_MODE_PLAY &&
                                   gameTask == GAME_TASK_PLAY;
    const int effective = requested == 0 && live_dungeon_play ? 0 : 1;
    InputSetWheelMode(effective);
    return effective;
}

void LoadChronicleTownCharacter() {
    if (!TownCharacterAvailable(g_town_character)) g_town_character = CHARA_TOAN;
    static const char *const packs[6] = {
        "chara/c01d.chr", "dun/mainchara/c04b.chr", "dun/mainchara/c06b.chr",
        "dun/mainchara/c05a.chr", "dun/mainchara/c10b.chr", "dun/mainchara/c18a.chr"};
    const int selected = g_town_character;
    EdLoadMainChara(const_cast<char *>(packs[selected]),
                    const_cast<char *>(selected == CHARA_TOAN ? "info.cfg" : "base.cfg"), &CharaBuffer);
    g_town_loaded_character = selected;
    g_town_character_reload = false;
}

void ApplyChronicleTownPartySwitch() {
    if (!g_town_character_reload || mode != GAME_MODE_EDIT || GameMode != ED_MODE_WALK ||
        MapNo < 0 || MapNo >= TOWN_COUNT || Chara == nullptr || pEditGround == nullptr) return;
    if (!TownCharacterAvailable(g_town_character)) g_town_character = CHARA_TOAN;
    float position[4] = {}, rotation[4] = {};
    Chara->GetPosition(position);
    Chara->GetRotation(rotation);
    for (int axis = 0; axis < 3; ++axis) {
        if (!std::isfinite(position[axis]) || !std::isfinite(rotation[axis])) return;
    }
    LoadChronicleTownCharacter();
    if (Chara == nullptr) return;
    Chara->SetPosition(position);
    Chara->SetRotation(rotation);
    Chara->motion_no = 0;
}

int ChronicleTownMotionRemap(CCharacter *character, int motion_no) {
    if (mode != GAME_MODE_EDIT || g_town_loaded_character == CHARA_TOAN ||
        character != static_cast<CCharacter *>(&MainChara)) return motion_no;
    static constexpr int motion_map[11] = {0, 1, 2, 0, 0, 34, 35, 31, 32, 0, 0};
    return motion_no >= 0 && motion_no < 11 ? motion_map[motion_no] : motion_no;
}

bool ChronicleDungeonBadgesVisible() { return !g_floor_badges.empty(); }
int ChronicleDungeonTierBadge(int dungeon, int floor) {
    const std::string key=std::to_string(dungeon)+":"+std::to_string(floor);
    int tier=0;
    for (const auto &[owner,badges]:g_floor_badges) tier=std::max(tier,badges.value(key,badges.value("default",0)));
    return tier;
}
bool ChronicleDungeonDrawsModText() { return mode == GAME_MODE_DUNGEON; }
std::vector<std::string> ChronicleOverlayLines() {
    std::vector<std::string> lines;
    for (const auto &[id, overlay] : g_debug_overlays) {
        if (!overlay.render_bar) lines.push_back(overlay.text);
    }
    return lines;
}
int ChronicleBlockedNativeButtons() {
    return SharedRuntime().Frame() <= g_native_block_until ? g_native_block_mask : 0;
}
std::vector<ChronicleHudIcon> ChronicleHudIcons() {
    std::vector<ChronicleHudIcon> icons;
    for (const auto &[id, icon] : g_hud_icons) icons.push_back({icon.native, icon.image, icon.blink});
    return icons;
}
std::string ChronicleFloorLabel() { return g_floor_label; }
int ChronicleFloorNumber() { return g_floor_number; }
std::vector<ChronicleHudBar> ChronicleHudBars() {
    std::vector<ChronicleHudBar> bars;
    for (const auto &[id, overlay] : g_debug_overlays) {
        if (!overlay.render_bar) continue;
        int number = -1;
        std::size_t digits = overlay.label.find_last_not_of("0123456789");
        digits = digits == std::string::npos ? 0 : digits + 1;
        if (digits < overlay.label.size() && overlay.label.size() - digits <= 4) number = std::stoi(overlay.label.substr(digits));
        bars.push_back({overlay.current, overlay.maximum, overlay.boss_bar ? -1 : number, overlay.boss_bar, overlay.label});
    }
    return bars;
}
std::string ChronicleDebugOverlayText() {
    std::string text;
    for (const auto &[id, overlay] : g_debug_overlays) {
        (void)id;
        const std::size_t separator_size = text.empty() ? 0 : 3;
        if (text.size() + separator_size + overlay.text.size() > kMaximumDebugOverlayLine) break;
        if (separator_size != 0) text += " / ";
        text += overlay.text;
    }
    return text;
}

void StepHeldPicture(); // quick passage's held picture, below

void ChronicleTick() {
    StepExitRoom();
    StepHeldPicture();
    {
        // Test hook: DC_REST_TEST=spring|stairs|gate stands the player, once per rest floor (dungeons.set_rest_room), in
        // front of its spring, on its back floor stairs, or north of its exit gate (no monsters live there to block Cross).
        static int rest_seed = -1;
        static int rest_settle = 0;
        const char *rest_test = std::getenv("DC_REST_TEST");
        if (rest_test != nullptr && *rest_test != '\0' && mode == GAME_MODE_DUNGEON && NowDngMap != nullptr && gameTask == GAME_TASK_PLAY &&
            BtUraDongeon == 0 && CharaFrame != nullptr && DngRestRoomSeed == NowDngMap->map_seed && NowDngMap->map_seed != rest_seed &&
            ++rest_settle > 120) {
            rest_settle = 0;
            rest_seed = NowDngMap->map_seed;
            const std::string_view where = rest_test;
            int col = DngRestSpringX, row = DngRestSpringY + 1;
            if (where == "stairs") {
                col = DngRestStairsX;
                row = DngRestStairsY;
            } else if (where == "gate") {
                for (int i = 0; i < DNG_CELLS; i++) {
                    if (NowDngMap->cells[i].parts_no == MAP_PARTS_STAIR_UP) col = i % DNG_GRID, row = i / DNG_GRID - 1;
                }
            }
            float x = 160.0f * static_cast<float>(col), z = 160.0f * static_cast<float>(row);
            // The stairs and the gate answer only on their event's small trigger, a little way in front of the cell.
            for (int i = 0; i < std::ssize(NowEventMan->event) && where != "spring"; ++i) {
                CDungeonEventData &event = NowEventMan->event[i];
                if (event.event == nullptr || event.CheckSwitch() == 0) continue;
                const int part = event.event->parts_id;
                if (where == "stairs" ? part < MAP_PARTS_URA_ENTRANCE_NORTH || part > MAP_PARTS_URA_ENTRANCE_WEST : part != MAP_PARTS_STAIR_UP) continue;
                x = event.pos[0];
                z = event.pos[2];
            }
            TeleportPlayer(x, z);
            std::printf("rest test: player at the %s, cell %d,%d\n", rest_test, col, row);
        }
    }
    {
        // Test hook: DC_EXIT_ROOM_TEST=1 gives the gate key and stands the player
        // on the corridor cell north of the floor's real exit, once per floor.
        // DC_BACKROOM_TEST=1 stands the player on the back floor's entrance instead.
        static int tested_seed = -1;
        static int settle = 0;
        const bool backroom_test = std::getenv("DC_BACKROOM_TEST") != nullptr;
        if ((backroom_test || std::getenv("DC_EXIT_ROOM_TEST") != nullptr) && mode == GAME_MODE_DUNGEON &&
            NowDngMap != nullptr && gameTask == GAME_TASK_PLAY && BtUraDongeon == 0 && NowDngMap->map_seed != tested_seed) {
            if (++settle > 120) {
                settle = 0;
                tested_seed = NowDngMap->map_seed;
                for (int i = 0; i < DNG_CELLS && backroom_test; i++) {
                    const int part = NowDngMap->cells[i].parts_no;
                    if (part < MAP_PARTS_URA_ENTRANCE_NORTH || part > MAP_PARTS_URA_ENTRANCE_WEST || CharaFrame == nullptr) continue;
                    TeleportPlayer(160.0f * (i % DNG_GRID), 160.0f * (i / DNG_GRID));
                    if (NowMonstorUnit != nullptr) {
                        for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) NowMonstorUnit->monster[slot].state = -1;
                    }
                    std::printf("backroom test: player on the back floor entrance %d,%d\n", i % DNG_GRID, i / DNG_GRID);
                    break;
                }
                for (int i = 0; i < DNG_CELLS && !backroom_test; i++) {
                    const int col = i % DNG_GRID, row = i / DNG_GRID;
                    if (NowDngMap->cells[i].parts_no != MAP_PARTS_STAIR_UP || DNG_IN_EXIT_ROOM(col, row)) continue;
                    CDngStatusData *status = CurrentStatus();
                    if (status != nullptr && NowDngMap->gate_key_no > 0) status->GetItem(NowDngMap->gate_key_no, 1);
                    if (CharaFrame == nullptr) break;
                    TeleportPlayer(160.0f * col, 160.0f * (row - 1));
                    if (NowMonstorUnit != nullptr) {
                        // Test only: no monsters, so Cross uses the gate instead of attacking.
                        for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) NowMonstorUnit->monster[slot].state = -1;
                    }
                    std::printf("exit room test: key given, player north of exit %d,%d\n", col, row);
                    break;
                }
            }
        }
    }
    RestoreBackroomKey();
    if (g_backroom_descend && mode == GAME_MODE_DUNGEON && gameTask == GAME_TASK_PLAY) {
        g_backroom_descend = false;
        if (BtUraDongeon == 0) {
            gameTask = GAME_TASK_URA_SWAP;
            g_backroom_place = true;
        }
    } else if (g_backroom_place && BtUraDongeon != 0 && NowDngMap != nullptr && CharaFrame != nullptr) {
        g_backroom_place = false;
        for (int i = 0; i < DNG_CELLS; i++) {
            const int part = NowDngMap->cells[i].parts_no;
            if (part < MAP_PARTS_URA_ENTRANCE_NORTH || part > MAP_PARTS_URA_ENTRANCE_WEST) continue;
            // The entrance stands in a room's wall on the side it names; the party stands one cell into the room.
            constexpr int kInward[4][2] = {{0, 1}, {-1, 0}, {0, -1}, {1, 0}};
            const int *step = kInward[part - MAP_PARTS_URA_ENTRANCE_NORTH];
            TeleportPlayer(160.0f * (i % DNG_GRID + step[0]), 160.0f * (i / DNG_GRID + step[1]));
            break;
        }
    } else if (g_backroom_place && mode != GAME_MODE_DUNGEON) {
        g_backroom_place = false;
    }
    if (g_pending_back_floor_request) {
        if (BtUraDongeon != 0 || selectMapNo != g_pending_back_floor_dungeon) {
            g_pending_back_floor_request = false;
            g_pending_back_floor_dungeon = -1;
        } else if (mode == GAME_MODE_DUNGEON && NowDngMap != nullptr && gameTask == GAME_TASK_SCRIPT_RUN) {
            BtEventInfo.request = BT_REQUEST_URA_DUNGEON;
            g_pending_back_floor_request = false;
            g_pending_back_floor_dungeon = -1;
        }
    }

    CDngStatusData *status = CurrentStatus();
    if (status == nullptr) {
        g_thirst_baseline = false;
        g_thirst_status = nullptr;
        g_thirst_character = -1;
        return;
    }
    const int character_id = status->cur_chara;
    if (character_id < CHARA_TOAN || character_id >= CharaCount()) {
        g_thirst_baseline = false;
        g_thirst_status = status;
        g_thirst_character = -1;
        return;
    }
    if (!g_thirst_baseline || g_thirst_status != status || g_thirst_character != character_id) {
        for (int index = 0; index < CHARA_MAX; ++index) g_thirst_water[index] = status->water_now[index];
        g_thirst_status = status;
        g_thirst_character = character_id;
        g_thirst_baseline = true;
        return;
    }
    for (int index = 0; index < CHARA_MAX; ++index) {
        const float previous = g_thirst_water[index];
        const float current = status->water_now[index];
        if (current == previous) continue;
        g_thirst_water[index] = current;
        if (index != character_id) continue;
        const bool crossed_empty = previous > 0.0f && current <= 0.0f;
        const bool crossed_unit = static_cast<int>(previous) != static_cast<int>(current);
        if (!crossed_empty && !crossed_unit) continue;
        SharedRuntime().QueueEvent("game.player.thirst_changed",
                                   ChronicleThirstPayload(*status, index, previous, current, "native",
                                                          current < previous ? "drain" : "drink"));
    }
}

void ResetChronicleSignatureForTests() {
    g_pending_back_floor_request = false;
    g_pending_back_floor_dungeon = -1;
    g_thirst_status = nullptr;
    g_thirst_character = -1;
    g_thirst_water.fill(0.0f);
    g_thirst_baseline = false;
}

// A co-op visit begins: the new game's save takes the host's world and the guest's own things (a new account keeps the
// new game's), every character in the host's party gets a weapon if it has none, and the game walks into the town.
bool StartChronicleVisit(const SandboxRequest &request) {
    CDngStatusData *status = CurrentStatus();
    std::string     error;
    if (status == nullptr || SaveData == nullptr || request.map < 0 || request.map >= TOWN_COUNT ||
        !moonmod::coop::ImportWorld(request.world, *SaveData, error) ||
        (!request.player.empty() && !moonmod::coop::ImportPlayer(request.player, *SaveData, error))) {
        std::fprintf(stderr, "MoonMod: the visit could not start: %s\n", error.c_str());
        SharedRuntime().PublishToMod(request.owner, "game.session.failed", {{"kind", "visit"}, {"reason", error}});
        CloseTitleModeScreen();
        return false;
    }
    constexpr int kStarterWeapons[6] = {ITEM_WEAPON_DAGGER, ITEM_WEAPON_WOODEN_SLINGSHOT, ITEM_WEAPON_MALLET,
                                        ITEM_WEAPON_GOLD_RING, ITEM_WEAPON_FIGHTING_STICK, ITEM_WEAPON_MACHINE_GUN};
    const int party = std::clamp<int>(status->party_size, 1, CharaCount());
    for (int character = 0; character < party; ++character) {
        if (status->equipped_weapon_slot[character] >= 0 && status->chara_weapons[character][status->equipped_weapon_slot[character]].item_no >= ITEM_WEAPON_START) continue;
        WEAPON_HAVE &owned = status->chara_weapons[character][0];
        if (owned.item_no < ITEM_WEAPON_START) WepDataListToHaveCopy(kStarterWeapons[CombatChara(character)], &owned);
        status->equipped_weapon_slot[character] = 0;
    }
    status->cur_chara = 0;
    status->cur_georama = static_cast<s8>(request.map);
    BeginSandboxSession(request);
    MapJump(request.map, -1);
    return true;
}

bool StartPendingChronicleSandbox() {
    std::optional<SandboxRequest> request = TakeSandboxRequest();
    if (!request) {
        CloseTitleModeScreen();
        return false;
    }
    if (request->visit) return StartChronicleVisit(*request);
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr || request->dungeon_id < 0 || request->dungeon_id >= DUNGEON_COUNT || request->floor < 0 ||
        request->floor >= maxFloorTbl__3[request->dungeon_id]) {
        std::fprintf(stderr, "MoonMod: sandbox session could not start\n");
        CloseTitleModeScreen();
        return false;
    }
    // A new game's save gives every character start HP and water but only Toan a weapon (and party size 1).
    // MoonMod: a registered character starts with his CombatChara's (Opar: Toan's dagger, until his staffs come).
    constexpr int kStarterWeapons[6] = {ITEM_WEAPON_DAGGER, ITEM_WEAPON_WOODEN_SLINGSHOT, ITEM_WEAPON_MALLET,
                                        ITEM_WEAPON_GOLD_RING, ITEM_WEAPON_FIGHTING_STICK, ITEM_WEAPON_MACHINE_GUN};
    status->SetPartySize(static_cast<s8>(request->party_size));
    for (int character = 0; character < request->party_size; ++character) {
        if (status->equipped_weapon_slot[character] >= 0) continue;
        WEAPON_HAVE &owned = status->chara_weapons[character][0];
        if (owned.item_no < ITEM_WEAPON_START) WepDataListToHaveCopy(kStarterWeapons[CombatChara(character)], &owned);
        status->equipped_weapon_slot[character] = 0;
    }
    status->cur_chara = request->character;
    // A new game's save has no area yet (-1); the dungeon code indexes its per-dungeon tables with it.
    status->cur_georama = request->dungeon_id;
    BeginSandboxSession(*request);
    g_pending_dungeon_floor_entry = PendingDungeonFloorEntry{request->dungeon_id, request->floor};
    MapJump(request->dungeon_id + 200, -1);
    return true;
}

bool RedirectChronicleSandboxMapJump() {
    if (!SandboxActive() || NextMapNo < 0) return false;
    // A visit goes wherever the host's world goes; only the way to the title ends it.
    if (VisitActive()) {
        if (NextMapNo == 800) {
            g_pending_dungeon_floor_entry.reset();
            EndSandboxSession("left");
            return true;
        }
        return false;
    }
    if (NextMapNo >= 200 && NextMapNo < 300) return false; // another floor or dungeon: the session goes on
    NextMapNo = 800;
    g_pending_dungeon_floor_entry.reset();
    EndSandboxSession("left_dungeon");
    return true;
}

// Every party defeat in a dungeon, before the fade to town: mods hear game.dungeon.defeated.
void NoteChronicleSandboxDefeat() {
    if (mode == GAME_MODE_DUNGEON && UserStatus != nullptr) {
        auto *dng = static_cast<CDngStatusData *>(UserStatus);
        SharedRuntime().QueueEvent("game.dungeon.defeated", {{"dungeon_id", static_cast<int>(dng->cur_georama)},
                                                             {"floor_id", static_cast<int>(UserStatus->cur_floor)},
                                                             {"gilda", static_cast<int>(dng->money)}, // before the knockout halves it
                                                             {"frame", SharedRuntime().Frame()}});
    }
    NoteSandboxEnding("defeat");
}

void EndChronicleSandboxInPlace(std::string_view reason) {
    if (SandboxActive()) EndSandboxSession(reason, false);
}

bool PreparePendingChronicleDungeonEntry() {
    if (!g_pending_dungeon_floor_entry.has_value()) return false;
    const PendingDungeonFloorEntry target = *g_pending_dungeon_floor_entry;
    g_pending_dungeon_floor_entry.reset();

    CDngStatusData *status = CurrentStatus();
    if (mode != GAME_MODE_DUNGEON || status == nullptr || selectMapNo != target.dungeon_id ||
        target.dungeon_id < 0 || target.dungeon_id >= DUNGEON_COUNT || target.floor_id < 0 ||
        target.floor_id >= maxFloorTbl__3[target.dungeon_id]) {
        std::fprintf(stderr, "MoonMod: registered dungeon-floor transition could not be applied\n");
        return false;
    }
    g_active_dungeon_floor_entry = target;
    // A sandbox session moves between dungeons without a town: keep its area on the dungeon it enters.
    if (SandboxActive()) status->cur_georama = target.dungeon_id;
    return true;
}

int TakePendingChronicleDungeonFloor(int dungeon_id) {
    if (!g_active_dungeon_floor_entry.has_value() ||
        g_active_dungeon_floor_entry->dungeon_id != dungeon_id) {
        return -1;
    }
    const int floor_id = g_active_dungeon_floor_entry->floor_id;
    g_active_dungeon_floor_entry.reset();
    return floor_id;
}

int ChronicleScriptFloorLevel(int dungeon_id, int floor) {
    // Retail's exit gate asks for the next floor with no check: on a mod's last registered floor (which, unlike
    // retail's last, has an exit) that is past the floor tables. Stay on the last floor instead.
    if (dungeon_id < 0 || dungeon_id >= DUNGEON_COUNT) return floor;
    // A mod's redirect (dungeons.floor_redirect) sends the gate somewhere else, such as past a floor a run skips.
    if (const auto redirect = g_floor_redirects.find({dungeon_id, floor}); redirect != g_floor_redirects.end()) {
        floor = redirect->second.second;
    }
    // A floor built from several floors' zones (dungeons.set_zones exit_floor) leads past the last of them. Only from that
    // floor itself: a story floor of the game's own design keeps the zones' tables but leads to its own next floor.
    if (UserStatus != nullptr && floor == UserStatus->cur_floor + 1 && dungeon_id == selectMapNo && DngZoneFloorExit >= 0 &&
        DngZoneFloorCount > 0 && DngZoneFloorSeed == MainDungeonMap.map_seed && NowDngMap == &MainDungeonMap && MainDungeonMap.map_type == 1) {
        floor = DngZoneFloorExit;
    }
    return std::min(floor, maxFloorTbl__3[dungeon_id] - 1);
}

void ChronicleMonsterModelLoaded(dc::game::CMonstorUnit *unit, int slot, int native_model_no) {
    if (unit == nullptr || slot < 0 || slot >= static_cast<int>(g_loaded_monster_model_ids.size())) return;
    PrepareMonsterHandleUnit(unit);
    g_loaded_monster_model_ids[slot] = native_model_no;
    ApplyMonsterMotions(unit, slot, native_model_no);
}

// The override generation each monster's native idle loop started under (0: not in it), for ChronicleMonsterRunAgain.
std::array<std::uint64_t, MONSTOR_SLOT_MAX> g_monster_idle_generation{};

// The monster's script program (1 spawn, 50 revealed, 100 idle loop, 110 hit, 120 killed) through
// the native.monster.run hook. A mod that does not call the original takes the monster over for
// that program; the script then counts as ended so the next frame asks again.
void ChronicleMonsterRun(dc::game::CMonstorUnit *unit, int slot, int program) {
    if (unit == nullptr || slot < 0 || slot >= MONSTOR_SLOT_MAX) return;
    PrepareMonsterHandleUnit(unit);
    const std::uint32_t generation = EnsureMonsterHandleGeneration(unit, slot);
    const Json payload = {{"game", "chronicle"},
                          {"entity", MonsterHandleId(slot, generation)},
                          {"slot", slot},
                          {"program_no", program},
                          {"model", unit->monster[slot].base_model},
                          {"frame", SharedRuntime().Frame()}};
    bool ran = false;
    SharedRuntime().DispatchNativeScriptHook("native.monster.run", payload, [unit, slot, program, &ran]() {
        ran = true;
        unit->interpreter[slot].run(program);
        return ApiCallResult{ApiCallStatus::Ok, {{"ran", true}}, {}};
    });
    if (!ran) unit->interpreter[slot].end = 1;
    if (program == 1) SetMonsterMotionHits(unit, slot);
    g_monster_idle_generation[slot] = program == 100 && ran ? MonsterScriptOverrideGeneration() : 0;
}

bool ChronicleMonsterRunAgain(dc::game::CMonstorUnit *unit, int slot) {
    if (unit == nullptr || slot < 0 || slot >= MONSTOR_SLOT_MAX) return false;
    const std::uint64_t since = g_monster_idle_generation[slot];
    if (since == 0 || since == MonsterScriptOverrideGeneration()) return false;
    g_monster_idle_generation[slot] = 0;
    return true;
}

void ChronicleMonsterActorCreated(dc::game::CMonstorUnit *unit, int slot) {
    if (unit == nullptr || slot < 0 || slot >= static_cast<int>(g_monster_handle_generations.size())) return;
    PrepareMonsterHandleUnit(unit);
    AdvanceMonsterHandleGeneration(slot);
    g_monster_handle_records[slot].generation = g_monster_handle_generations[slot];
    g_arena_slots[slot] = {};
    g_monster_damage_scales[slot] = 1.0f;
    g_monster_hit_reach[slot] = 0.0f;
    g_monster_hit_share[slot] = false;
    g_monster_idle_generation[slot] = 0;
}

float ChronicleMonsterHitReach(dc::game::CMonstorUnit *unit, int slot) {
    if (unit == nullptr || unit != g_monster_handle_unit || slot < 0 || slot >= MONSTOR_SLOT_MAX) return 0.0f;
    return g_monster_hit_reach[slot];
}

bool ChronicleMonsterHitShare(dc::game::CMonstorUnit *unit, int slot) {
    return unit != nullptr && unit == g_monster_handle_unit && slot >= 0 && slot < MONSTOR_SLOT_MAX && g_monster_hit_share[slot];
}

int ChronicleMonsterHitDamage(int damage, int owner, int monster_no) {
    // A monster's own attacks name it as owner 200 + slot * 5; the shots it fires carry its slot as monster_no.
    int slot = owner >= 200 ? (owner - 200) / 5 : monster_no;
    if (damage <= 0 || slot < 0 || slot >= MONSTOR_SLOT_MAX || g_monster_damage_scales[slot] == 1.0f) return damage;
    return std::clamp(static_cast<int>(std::lround(damage * g_monster_damage_scales[slot])), 1, 1000000);
}

float *ChronicleMonsterScriptHeight(dc::game::CMonstorUnit *unit, int slot) {
    if (slot >= 0 && slot == g_pending_arena_origin.slot && unit == NowMonstorUnit) {
        return g_pending_arena_origin.grounded ? &g_pending_arena_origin.script_y : nullptr;
    }
    if (unit == nullptr || unit != g_monster_handle_unit || slot < 0 || slot >= MONSTOR_SLOT_MAX ||
        !g_arena_slots[slot].active || !g_arena_slots[slot].grounded) {
        return nullptr;
    }
    return &g_arena_slots[slot].script_y;
}

bool ChronicleMonsterScriptOrigin(dc::game::CMonstorUnit *unit, int slot, float origin[3], bool *grounded) {
    if (slot >= 0 && slot == g_pending_arena_origin.slot && unit == NowMonstorUnit) {
        origin[0] = g_pending_arena_origin.origin[0];
        origin[1] = g_pending_arena_origin.origin[1];
        origin[2] = g_pending_arena_origin.origin[2];
        if (grounded != nullptr) *grounded = g_pending_arena_origin.grounded;
        return true;
    }
    if (unit == nullptr || unit != g_monster_handle_unit || slot < 0 || slot >= MONSTOR_SLOT_MAX ||
        !g_arena_slots[slot].active) {
        return false;
    }
    origin[0] = g_arena_slots[slot].origin[0];
    origin[1] = g_arena_slots[slot].origin[1];
    origin[2] = g_arena_slots[slot].origin[2];
    if (grounded != nullptr) *grounded = g_arena_slots[slot].grounded;
    return true;
}

namespace {

// A cast is its bank: two casts may share an arena origin (a boss rising where the one before it fell).
bool SameCast(const ArenaSlot &a, const ArenaSlot &b) {
    return a.active && b.active && a.cast_no >= 0 && b.cast_no >= 0 && a.bank >= 0 && a.bank == b.bank;
}

// Fades out the cast of `slot` (with `slot` itself when `self`), as a monster's own death fade does (STATUS_SET_ALPHA),
// over a second and a half. ChronicleMonsterDefeatFading then holds each one's script until it is gone.
void FadeCast(dc::game::CMonstorUnit *unit, int slot, bool self) {
    for (int member = 0; member < MONSTOR_SLOT_MAX; ++member) {
        if (member == slot ? !self : !SameCast(g_arena_slots[slot], g_arena_slots[member])) continue;
        if (unit->monster[member].state != 2 || g_arena_slots[member].fading) continue;
        g_arena_slots[member].fading = true;
        unit->monster[member].palette_alpha_step = 128.0f / 90.0f;
        unit->monster[member].palette_delay = 0;
        unit->monster[member].movement_speed = 0.0f;
    }
}

} // namespace

void ChronicleMonsterDefeatScene(dc::game::CMonstorUnit *unit, int slot) {
    if (unit == nullptr || unit != g_monster_handle_unit || slot < 0 || slot >= MONSTOR_SLOT_MAX ||
        !g_arena_slots[slot].active || unit->monster[slot].hp > 0 || g_arena_slots[slot].fading) {
        return;
    }
    // Its cast goes with it, as the defeat scene ends the whole arena.
    FadeCast(unit, slot, true);
}

int ChronicleMonsterGlobalBank(dc::game::CMonstorUnit *unit, int slot) {
    if (slot >= 0 && slot == g_pending_arena_origin.slot && unit == NowMonstorUnit) return g_pending_arena_origin.bank;
    if (unit == nullptr || unit != g_monster_handle_unit || slot < 0 || slot >= MONSTOR_SLOT_MAX ||
        !g_arena_slots[slot].active) {
        return -1;
    }
    return g_arena_slots[slot].bank;
}

int ChronicleMonsterCastNumber(dc::game::CMonstorUnit *unit, int slot) {
    if (slot >= 0 && slot == g_pending_arena_origin.slot && unit == NowMonstorUnit) {
        return g_pending_arena_origin.cast_no >= 0 ? g_pending_arena_origin.cast_no : slot;
    }
    if (unit == nullptr || unit != g_monster_handle_unit || slot < 0 || slot >= MONSTOR_SLOT_MAX ||
        !g_arena_slots[slot].active || g_arena_slots[slot].cast_no < 0) {
        return slot;
    }
    return g_arena_slots[slot].cast_no;
}

int ChronicleMonsterCastSlot(dc::game::CMonstorUnit *unit, int slot, int native) {
    // The member being set up isn't recorded yet: it names its cast through the pending origin.
    ArenaSlot self{};
    if (slot >= 0 && slot == g_pending_arena_origin.slot && unit == NowMonstorUnit) {
        self = {true, g_pending_arena_origin.origin, g_pending_arena_origin.grounded, 0.0f, false,
                g_pending_arena_origin.cast_no, g_pending_arena_origin.bank};
    } else if (unit != nullptr && unit == g_monster_handle_unit && slot >= 0 && slot < MONSTOR_SLOT_MAX) {
        self = g_arena_slots[slot];
    }
    if (!self.active || self.cast_no < 0) {
        return native >= 0 && native < MONSTOR_SLOT_MAX ? native : -1;
    }
    if (native == self.cast_no) return slot;
    for (int member = 0; member < MONSTOR_SLOT_MAX; ++member) {
        if (g_arena_slots[member].cast_no == native && SameCast(self, g_arena_slots[member]) &&
            unit->monster[member].state != -1) {
            return member;
        }
    }
    return -1;
}

bool ChronicleMonsterDefeatFading(dc::game::CMonstorUnit *unit, int slot) {
    return unit != nullptr && unit == g_monster_handle_unit && slot >= 0 && slot < MONSTOR_SLOT_MAX &&
           g_arena_slots[slot].active && g_arena_slots[slot].fading;
}

bool ChronicleMonsterActorIsModOwned(dc::game::CMonstorUnit *unit, int slot) {
    if (unit == nullptr || slot < 0 || slot >= static_cast<int>(g_monster_handle_records.size())) return false;
    PrepareMonsterHandleUnit(unit);
    const ChronicleMonsterHandleRecord &record = g_monster_handle_records[slot];
    return record.generation == g_monster_handle_generations[slot] && !record.owner.empty() &&
           !record.character_id.empty();
}

void ChronicleMonsterActorOwned(dc::game::CMonstorUnit *unit, int slot, std::string_view owner,
                                std::string_view character_id, int native_model_no) {
    if (unit == nullptr || slot < 0 || slot >= static_cast<int>(g_monster_handle_generations.size()) ||
        owner.empty() || character_id.empty()) {
        return;
    }
    const std::uint32_t generation = EnsureMonsterHandleGeneration(unit, slot);
    ChronicleMonsterHandleRecord &record = g_monster_handle_records[slot];
    record = {};
    record.generation = generation;
    record.owner = std::string(owner);
    record.character_id = std::string(character_id);
    record.native_model_no = native_model_no;
}

void QueueChronicleMonsterDamageResult(dc::game::CMonstorUnit *unit, int slot, int result) {
    if (unit == nullptr || slot < 0 || slot >= static_cast<int>(g_monster_handle_generations.size()) ||
        (result != static_cast<int>(MonsterDamageResult::Hit) && result != static_cast<int>(MonsterDamageResult::Killed))) {
        return;
    }
    const std::uint32_t generation = EnsureMonsterHandleGeneration(unit, slot);
    ChronicleMonsterHandleRecord &record = g_monster_handle_records[slot];
    if (record.generation != generation) record = {};
    record.generation = generation;
    if (record.pending_damage_result == static_cast<int>(MonsterDamageResult::Killed) || result == static_cast<int>(MonsterDamageResult::Killed)) {
        record.pending_damage_result = static_cast<int>(MonsterDamageResult::Killed);
    } else {
        record.pending_damage_result = static_cast<int>(MonsterDamageResult::Hit);
    }
}

int TakeChronicleMonsterDamageResult(dc::game::CMonstorUnit *unit, int slot) {
    if (unit == nullptr || unit != g_monster_handle_unit || slot < 0 ||
        slot >= static_cast<int>(g_monster_handle_generations.size())) {
        return static_cast<int>(MonsterDamageResult::None);
    }
    ChronicleMonsterHandleRecord &record = g_monster_handle_records[slot];
    if (record.generation != g_monster_handle_generations[slot]) return static_cast<int>(MonsterDamageResult::None);
    const int result = record.pending_damage_result;
    record.pending_damage_result = static_cast<int>(MonsterDamageResult::None);
    return result;
}

void ChronicleMonsterDefeated(dc::game::CMonstorUnit *unit, int slot, bool scripted) {
    if (unit == nullptr || slot < 0 || slot >= static_cast<int>(g_monster_handle_generations.size())) return;
    PrepareMonsterHandleUnit(unit);
    const std::uint32_t generation = EnsureMonsterHandleGeneration(unit, slot);
    const MONSTOR &monster = unit->monster[slot];
    Json payload = {{"entity", MonsterHandleId(slot, generation)}, {"slot", slot},
                    {"source", scripted ? "scripted" : "combat"}};
    auto *status = CurrentStatus();
    int attacker = monster.last_attacker;
    // Poison, traps and scripted combat may not preserve a weapon owner on the
    // monster. Attribute those deaths to the active character for progression;
    // the scripted flag below prevents granting weapon mastery credit.
    if ((attacker < 0 || attacker >= CharaCount()) && status != nullptr) attacker = status->cur_chara;
    payload["attacker"] = attacker;
    // Only native player-combat defeats earn weapon mastery. Scripted deaths
    // (poison, traps, API kills) may still use the active character for XP,
    // but must not be credited to whatever weapon happens to be equipped.
    if (!scripted && status != nullptr && attacker >= 0 && attacker < CharaCount()) {
        const int equipped = status->equipped_weapon_slot[attacker];
        if (equipped >= 0 && equipped < 11) {
            const auto &weapon = status->chara_weapons[attacker][equipped];
            if (weapon.item_no >= ITEM_WEAPON_START) payload["weapon_id"] = "chronicle:weapon:" + std::to_string(weapon.item_no);
        }
    }
    if (monster.exp > 0) payload["reward_exp"] = monster.exp;
    if (monster.money > 0) payload["reward_money"] = monster.money;
    if (monster.max_hp > 0) payload["enemy_max_hp"] = monster.max_hp;
    if (monster.last_hit_damage > 0) payload["damage"] = monster.last_hit_damage;
    if (monster.base_model >= 0 && monster.base_model < static_cast<int>(g_loaded_monster_model_ids.size())) {
        const int model_id = g_loaded_monster_model_ids[monster.base_model];
        if (model_id >= 0) payload["native_model_id"] = model_id;
    }
    const ChronicleMonsterHandleRecord &record = g_monster_handle_records[slot];
    if (record.generation == generation && !record.character_id.empty()) {
        payload["character_id"] = record.character_id;
    }
    SharedRuntime().QueueEvent("game.enemy_defeated", payload);
    AdvanceMonsterHandleGeneration(slot);
    // A boss whose own script ends its fight (the Ice Queen dies by STATUS_SET_DEAD) leaves its cast behind: the cast
    // fades out with it.
    if (unit == g_monster_handle_unit && g_arena_slots[slot].active && g_arena_slots[slot].cast_no == 0) {
        FadeCast(unit, slot, false);
    }
}

bool ChronicleTryEnemyVarietySurvival(dc::game::CMonstorUnit *unit, int slot, int &damage) {
    if (DngEnemySurvivalChance <= 0 || unit == nullptr || slot < 0 ||
        slot >= static_cast<int>(g_enemy_survival_attempted_generation.size())) return false;
    PrepareMonsterHandleUnit(unit);
    if (g_enemy_survival_unit != unit) {
        g_enemy_survival_unit = unit;
        g_enemy_survival_attempted_generation.fill(0);
    }
    MONSTOR &monster = unit->monster[slot];
    // Match the legacy mod's eligibility rule: exclude key carriers and
    // enemies at or above its max-HP threshold. Do not add a separate boss
    // or monster-kind exclusion that the legacy callback never applied.
    if (monster.state != 2 || monster.hp <= 0 || monster.max_hp <= 0 || monster.hp > monster.max_hp ||
        monster.drop_item >= 0 || monster.max_hp >= 3000) {
        return false;
    }
    const std::uint32_t generation = EnsureMonsterHandleGeneration(unit, slot);
    if (g_enemy_survival_attempted_generation[slot] == generation) return false;
    g_enemy_survival_attempted_generation[slot] = generation;
    const double roll = 100.0 * FormulaRandom01();
    if (roll >= DngEnemySurvivalChance) return false;

    const int previous_hp = monster.hp;
    const int survive_hp = std::max(1, monster.max_hp * 30 / 100);
    monster.hp = survive_hp;
    damage = std::max(0, previous_hp - survive_hp);
    monster.stop_timer = std::max(monster.stop_timer, 70);
    return true;
}

void QueueChronicleMonsterHit(dc::game::CMonstorUnit *unit, int slot, int attacker, int weapon_id, int damage, int element) {
    if (unit == nullptr || slot < 0 || slot >= static_cast<int>(g_monster_handle_generations.size()) ||
        attacker < 0 || attacker >= CharaCount() || weapon_id < ITEM_WEAPON_START || damage <= 0) return;
    PrepareMonsterHandleUnit(unit);
    const std::uint32_t generation = EnsureMonsterHandleGeneration(unit, slot);
    const MONSTOR &monster = unit->monster[slot];
    std::string weapon_stable_id = "chronicle:weapon:" + std::to_string(weapon_id);
    if (const auto custom = g_mod_weapon_by_native_id.find(weapon_id); custom != g_mod_weapon_by_native_id.end()) {
        weapon_stable_id = custom->second;
    }
    Json payload = {{"entity", MonsterHandleId(slot, generation)}, {"slot", slot}, {"attacker", attacker},
                    {"weapon_id", weapon_stable_id}, {"native_weapon_id", weapon_id},
                    {"damage", damage}, {"element", element}, {"frame", SharedRuntime().Frame()},
                    {"charged", ChronicleHitIsCharged()}};
    if (monster.base_model >= 0 && monster.base_model < static_cast<int>(g_loaded_monster_model_ids.size())) {
        const int model_id = g_loaded_monster_model_ids[monster.base_model];
        if (model_id >= 0) payload["native_model_id"] = model_id;
    }
    SharedRuntime().QueueEvent("game.enemy_hit", payload);
}

void ChronicleNativeItemPickedUp(int item_no, int count, int character, int slot) {
    if (count <= 0 || item_no < ITEM_DUNGEON_START ||
        item_no >= ITEM_WEAPON_START + kChronicleWeaponCount) {
        return;
    }
    const bool is_weapon = item_no >= ITEM_WEAPON_START;
    std::string stable_id;
    if (is_weapon) {
        const auto mod_weapon = g_mod_weapon_by_native_id.find(item_no);
        stable_id = mod_weapon == g_mod_weapon_by_native_id.end()
                        ? "chronicle:weapon:" + std::to_string(item_no)
                        : mod_weapon->second;
    } else {
        stable_id = ItemStableId(item_no);
    }
    Json payload = {{"id", stable_id}, {"kind", is_weapon ? "weapon" : "item"},
                    {"native_item_no", item_no}, {"count", count}, {"source", "native"},
                    {"frame", SharedRuntime().Frame()}};
    if (is_weapon && character >= 0 && character < CharaCount() && slot >= 0 && slot < 11) {
        payload["character"] = character;
        payload["slot"] = slot;
    }
    SharedRuntime().QueueEvent("game.item_picked_up", std::move(payload));
}

void ChronicleMonstersCleared(dc::game::CMonstorUnit *unit) {
    if (unit == nullptr) return;
    PrepareMonsterHandleUnit(unit);
    // CleanViewMonstor is also used to clear live actors while retaining the
    // model table for scripted events. Only a zero or invalid model count means
    // the native model table has actually been discarded.
    if (unit->model_count <= 0 || unit->model_count > static_cast<int>(g_loaded_monster_model_ids.size())) {
        g_loaded_monster_model_ids.fill(-1);
    }
    g_arena_slots.fill({});
    for (int slot = 0; slot < static_cast<int>(g_monster_handle_generations.size()); ++slot) {
        AdvanceMonsterHandleGeneration(slot);
    }
}

void RecordChronicleChestValue(CDungeonMap *map, int slot, int item) {
    if (!map || slot < 0 || slot >= DNG_BOX_MAX) return;
    const int price_slot = item - ITEM_ATTACH_START;
    const int price = price_slot >= 0 && price_slot < static_cast<int>(std::size(PriceList)) ? std::max(0, int(PriceList[price_slot].sell_price)) : 0;
    g_chest_value_rewards[map][slot] = (price * DngChestValuePercent) / 100;
}
void CollectChronicleChestValue(CDungeonMap *map, int slot) {
    if (!map || slot < 0 || slot >= DNG_BOX_MAX || map->boxes[slot].closed == 0) return;
    auto found = g_chest_value_rewards.find(map);
    CDngStatusData *status = CurrentStatus();
    if (found == g_chest_value_rewards.end() || !status) return;
    const int bonus = found->second[slot];
    found->second[slot] = 0;
    status->money = static_cast<u16>(std::min(65535, int(status->money) + bonus));
}
float ApplyChroniclePlayerDamageFormula(float damage, int character_id, int weapon_id) {
    return g_player_damage_formulas.Apply(damage, weapon_id, -1, character_id);
}
void ChroniclePrepareFloorGeneration(bool back_floor) {
    if (!SharedRuntime().Running() || !UserStatus) return;
    SharedRuntime().Publish("game.dungeon.generating", {{"dungeon_id", selectMapNo}, {"floor_id", UserStatus->cur_floor}, {"back_floor", back_floor},
                                                        {"prebuild", ChronicleDungeonPrebuilding()}});
}
// The floor is built (once, after any rebuilds of a floor that couldn't be crossed): the seed it came from, which
// dungeons.set_seed takes to build it again.
void ChronicleFloorEventsBuilt(CDungeonMap *map) {
    if (!map) return;
    const int side = map == &UraDungeonMap ? 1 : 0;
    if (!g_opened_chests_set[side]) return;
    g_opened_chests_set[side] = false;
    for (const int chest : g_opened_chests[side]) {
        if (chest < 0 || chest >= DNG_BOX_MAX || map->boxes[chest].used == 0) continue;
        for (auto &event : map->events) {
            if (event.kind != DNG_EVENT_TREASURE_BOX || event.index != chest) continue;
            event.kind = DNG_EVENT_NONE;
            map->boxes[chest].closed = 0;
            map->boxes[chest].lid_angle = -30.0f;
        }
    }
    g_opened_chests[side].clear();
}
void ChronicleChestOpened(CDungeonMap *map, int slot) {
    if (!map || slot < 0 || slot >= DNG_BOX_MAX || map->boxes[slot].closed == 0) return;
    if (!SharedRuntime().Running() || !UserStatus) return;
    SharedRuntime().Publish("game.chest.opened", {{"dungeon_id", selectMapNo}, {"floor_id", UserStatus->cur_floor},
                                                  {"back_floor", map == &UraDungeonMap}, {"chest", slot},
                                                  {"item_id", map->boxes[slot].item_no},
                                                  {"large", map->boxes[slot].kind == TREASURE_BOX_LARGE}});
}
void ChronicleDungeonWalkInBuilt();
void ChronicleFloorGenerated(bool back_floor, int seed, int rooms) {
    if (!back_floor) ChronicleDungeonWalkInBuilt();
    if (!back_floor && !ChronicleDungeonPrebuilding() && g_arrival.has_value() && UserStatus != nullptr &&
        (g_arrival->dungeon < 0 || (selectMapNo == g_arrival->dungeon && UserStatus->cur_floor == g_arrival->floor))) {
        g_arrival->built = true;
        g_arrival->ticks = 0;
        // a zone's shrine, or failing that the cell inside its gate
        if (g_arrival->zone >= 0) {
            const int z = g_arrival->zone;
            if (DngZoneFloorCount > z && DngZoneShrineX[z] >= 0) {
                g_arrival->x = DngZoneShrineX[z];
                g_arrival->y = DngZoneShrineY[z] + 1;
            } else if (DngZoneFloorCount > z && z > 0 && z - 1 < DngZoneGateCount) {
                g_arrival->x = DngZoneGateTable[z - 1].x;
                g_arrival->y = DngZoneGateTable[z - 1].y;
            } else {
                g_arrival.reset();
            }
        }
    }
    if (!ChronicleDungeonPrebuilding()) ChronicleWalkPlaceReady(); // built unseen, the town is still to come
    if (!SharedRuntime().Running() || !UserStatus) return;
    SharedRuntime().Publish("game.dungeon.generated", {{"dungeon_id", selectMapNo}, {"floor_id", UserStatus->cur_floor},
                                                       {"back_floor", back_floor}, {"floor_seed", seed}, {"room_count", rooms},
                                                       {"prebuild", ChronicleDungeonPrebuilding()}});
}
float ApplyChronicleCombatDamageFormula(float damage, int weapon_id, int native_model_id, int character_id,
                                        int target_hp, int target_max_hp) {
    // a real weapon hit (it has a target) knows whether it was charged; scripted damage does not
    const int charged = target_max_hp > 0 ? (ChronicleHitIsCharged() ? 1 : 0) : -1;
    const float modified = g_combat_damage_formulas.Apply(damage, weapon_id, native_model_id, character_id,
                                                           target_hp, target_max_hp, SharedRuntime().Frame(), charged);
    const float final_damage = modified <= 0.0f ? modified : ApplyCombatDamageBonuses(modified, SharedRuntime().CombatDamageBonuses());
    return final_damage;
}
int ChronicleNativeMonsterModelId(dc::game::CMonstorUnit *unit, int slot) {
    if (unit == nullptr || slot < 0 || slot >= static_cast<int>(g_monster_handle_generations.size())) return -1;
    const int model_index = unit->monster[slot].base_model;
    if (model_index < 0 || model_index >= static_cast<int>(g_loaded_monster_model_ids.size())) return -1;
    return g_loaded_monster_model_ids[model_index];
}

void ResetChronicleCombatFormulasForTests() {
    g_combat_damage_formulas.Clear();
}

const ChroniclePlayerTuning &ChroniclePlayerTuningGet() { return g_player_tuning; }

void ClearChroniclePlayerTuning(std::string_view owner) {
    std::erase_if(g_player_tuning_requests, [owner](const auto &entry) { return entry.first == owner; });
    RefreshPlayerTuning();
}

void ResetChroniclePlayerTuningForTests() {
    g_player_tuning_requests.clear();
    RefreshPlayerTuning();
}

float ChroniclePlayerGaugeRefillScale() { return g_player_tuning.gauge_refill; }
float ChroniclePlayerThirstDrainScale() { return g_player_tuning.thirst_drain; }
float ChroniclePlayerDurabilityWearScale() { return g_player_tuning.durability_wear; }
float ChroniclePlayerAttackAnimSpeed() { return g_player_tuning.attack_anim_speed; }
bool ChroniclePlayerMapReveal() { return g_player_tuning.map_reveal; }
bool ChroniclePlayerMonsterReveal() { return g_player_tuning.monster_reveal; }
bool ChroniclePlayerMapHidden() { return g_player_tuning.map_hidden; }
bool ChroniclePlayerMapWindow() { return g_player_tuning.map_window; }

// A charged move, by character: Toan's held charge, Xiao's full-gauge shot, Goro's swing held into a second spin,
// Ruby's full charge, Osmond's charged shot. Ungaga's attack is a combo with no charge.
int g_charge_clock = 0, g_charged_until = -1, g_goro_last_step = 0;
bool g_goro_held = false;
void PlayerMoveTick();
void ChroniclePlayerModelLoaded(int chara) { ApplyPlayerMotions(chara); }
// The character whose mod motions a chest took off (-1: none). They go back once the chest has given play back.
int g_motions_suspended_chara = -1;
void ChroniclePlayerMotionsSuspend() {
    if (!AppliedMotionsLive() || UserStatus == nullptr) return;
    g_motions_suspended_chara = UserStatus->cur_chara;
    RestoreAppliedMotions();
}
void ChronicleChargedActionTick() {
    ++g_charge_clock;
    if (UserStatus == nullptr) return;
    const BT_ACT_STATUS &act = BtActStatus;
    bool charged = false;
    if (act.action_on) {
        switch (CombatChara(UserStatus->cur_chara)) {
            case CHARA_TOAN: charged = act.charge_level >= 1; break;
            case CHARA_XIAO: charged = act.action_no == 11; break;
            case CHARA_GORO:
                if (act.action_no == 0x24 && act.action_step < g_goro_last_step) g_goro_held = true;
                charged = act.action_no == 0x24 && g_goro_held;
                break;
            case CHARA_RUBY: charged = (act.action_no == 14 || act.action_no == 16) && act.charge_time >= 58.0f; break;
            case CHARA_OSMOND: charged = act.gun_type == 1 && act.action_no == 11; break;
            default: break;
        }
    } else {
        g_goro_held = false;
    }
    g_goro_last_step = act.action_step;
    if (charged) g_charged_until = g_charge_clock + 60;
    PlayerMoveTick();
}
bool ChronicleHitIsCharged() { return g_charge_clock <= g_charged_until; }

// The mod move playing, in the native action's place (action number kModMoveAction).
constexpr int kModMoveAction = 0x6E;
struct MoveRun {
    bool active = false;
    PlayerMove move;
    int frame = 0, frames = 1;
    bool facing_target = false; // a locked-on side roll: he faces the target, the stick carries him
    float start = 0.0f, span = 0.0f, heading = 0.0f;
    std::vector<bool> hit_done, cue_done;
};
MoveRun g_move_run;
// The game's own fade into a new motion, while a move's blend_in stands in for it (-1: not replaced).
float g_saved_blend_step = -1.0f;
float g_saved_camera_speed = -1.0f; // the camera's, while a move's camera stands in for it (-1: not replaced)
void RestoreMoveFade() {
    if (g_saved_blend_step < 0.0f) return;
    CharaMain.motion_type.state.blend_step = g_saved_blend_step;
    g_saved_blend_step = -1.0f;
}
// The fade and the camera's own catch-up speed, when a move ends or is dropped.
void RestoreMoveBlend() {
    RestoreMoveFade();
    if (g_saved_camera_speed > 0.0f) {
        if (NowCamera__3 != nullptr) NowCamera__3->SetSpeed(g_saved_camera_speed);
        g_saved_camera_speed = -1.0f;
    }
}
int g_run_frames = 0, g_move_cooldown = 0, g_dodge_cooldown = 0;
// The last blow the player's attack laid (a native swing's hit or a mod move's): the step it came on and the action
// it came from. An evade can cancel that action's recovery once the blow is past.
int g_move_clock = 0, g_blow_step = -1000, g_blow_action = -1;
// The attack combo so far: how many swings (or shots) in a row laid a blow, each within kChainGap steps of the last,
// and the step of the latest. A combo_finisher spends it. g_evade_end_step: when the last evade move finished.
// The gap is long enough for Goro's slow slam (a full gauge each) to chain.
constexpr int kChainGap = 150, kFinisherWindow = 60, kEvadeFollowWindow = 20, kSwingBlowSteps = 6;
int g_chain = 0, g_chain_step = -1000, g_evade_end_step = -1000;
float g_last_charge_held = 0.0f;

bool IsEvade(const PlayerMove &move) { return move.trigger == 3 || move.trigger == 4; }

// The lock target's heading from the player, if one is locked on.
bool LockTargetHeading(float &heading) {
    if (lockOnTargetFlag == 0 || lockOnTargetNo < 0 || lockOnTargetNo >= MONSTOR_SLOT_MAX || NowMonstorUnit == nullptr) return false;
    float target[4]{};
    NowMonstorUnit->chara[lockOnTargetNo][0].GetPosition(target);
    heading = std::atan2(target[0] - CharaMain.pos[0], target[2] - CharaMain.pos[2]);
    return true;
}

float PlayerHeading() {
    sceVu0FVECTOR rotation;
    CharaMain.frame->GetRotation(rotation);
    return rotation[1];
}

void EndPlayerMove() {
    RestoreMoveBlend();
    if (g_move_run.active && IsEvade(g_move_run.move)) g_evade_end_step = g_move_clock;
    if (BtActStatus.action_no == kModMoveAction) {
        BtActStatus.motion_no = 0;
        BtActStatus.action_on = 0;
        BtActStatus.move_power = 0.0f;
        BtActStatus.move_power_decay = 0.0f;
        if (g_move_run.move.invincible) BtActStatus.invincible_frames = 0;
        BtActStatus.action_busy = 0;
        BtActStatus.can_act = 1;
    }
    g_move_run.active = false;
}

bool SoftLockHeading(float &heading); // soft_lock, below

bool StartPlayerMove(const PlayerMove &source, bool cancelling = false, bool facing_target = false) {
    if (CharaMain.frame == nullptr || CharaMain.motion_type.motion_info == nullptr || NowWeaponHave == nullptr || NowColData == nullptr) return false;
    PlayerMove move = source;
    // A variant made for the motion he is in, at the point he is at in it, plays instead (his push off the lead foot).
    int variant = -1;
    for (std::size_t i = 0; i < move.variants.size(); ++i) {
        const MoveVariant &v = move.variants[i];
        if (CharaMain.motion_no != v.motion) continue;
        const MOTION_INFO &now = CharaMain.motion_type.motion_info[v.motion];
        const float span = static_cast<float>(now.end - now.start);
        if (!(span > 0.0f)) continue;
        const float phase = (CharaMain.motion_type.state.time - static_cast<float>(now.start)) / span;
        if (phase >= v.from && phase < v.to && PlayerMotionNumber(v.animation) >= 0) {
            move.animation = v.animation;
            variant = static_cast<int>(i);
            break;
        }
    }
    if (!move.animation.empty()) {
        move.motion = PlayerMotionNumber(move.animation);
        if (move.motion < 0) return false;
    }
    const MOTION_INFO &info = CharaMain.motion_type.motion_info[move.motion];
    const float begin = static_cast<float>(info.start), end = static_cast<float>(info.end);
    if (!(end > begin)) return false;
    const float gauge_cost = move.gauge * (move.trigger == 3 || move.trigger == 4 ? 1.0f : ChroniclePlayerSwingCostScale());
    if (BtActStatus.action_gauge < gauge_cost) return false;

    MoveRun run;
    run.active = true;
    run.move = move;
    run.start = begin + (end - begin) * move.from;
    run.span = (end - begin) * (move.to - move.from);
    run.frames = std::max(1, static_cast<int>(std::ceil(run.span / move.speed)));
    // The way the stick points, if it's pushed; a counter out of a locked-on guard turns to the target (the game keeps a
    // guarding player facing it, but the stick's last direction can point anywhere); else the way he faces. Every
    // attack but the run attack and the evades faces the lock target, if there is one: a lock_attack's stick picks the
    // move, its lunge_angle carries him away.
    const bool faces_target = move.trigger == 2 || move.trigger >= 5;
    run.heading = BtActStatus.stick_strength >= 0.3f && move.trigger != 4 && !faces_target ? std::atan2(BtActStatus.move_x, BtActStatus.move_z)
                                                                                         : PlayerHeading();
    if (faces_target) {
        float target = 0.0f;
        if (LockTargetHeading(target)) run.heading = target;
        else if (move.trigger != 2 && BtActStatus.stick_strength >= 0.3f) run.heading = std::atan2(BtActStatus.move_x, BtActStatus.move_z);
        if (lockOnTargetFlag == 0) SoftLockHeading(run.heading); // turned to the nearest enemy that way, with soft_lock
    }
    float target_heading = 0.0f;
    if (facing_target && LockTargetHeading(target_heading)) {
        run.facing_target = true;
        run.heading = target_heading;
    }
    run.hit_done.assign(move.hits.size(), false);
    run.cue_done.assign(move.cues.size(), false);
    g_move_run = std::move(run);

    CharaMain.frame->SetRotation(0.0f, g_move_run.heading, 0.0f);
    BtActStatus.action_on = 1;
    BtActStatus.action_no = kModMoveAction;
    BtActStatus.action_step = 0;
    BtActStatus.motion_no = move.motion;
    BtActStatus.charge_level = 0;
    BtActStatus.combo_window = 0;
    BtActStatus.combo_queued = 0;
    BtActStatus.refill_frames = 0;
    BtActStatus.frames_since_attack = 0;
    driveNoInterpolate = 1;
    CharaMain.motion_type.state.time = g_move_run.start;
    // A quick fade, so a move's first frames show at once instead of smeared into the pose he was in.
    RestoreMoveBlend();
    if (move.blend_in > 0) {
        g_saved_blend_step = CharaMain.motion_type.state.blend_step;
        CharaMain.motion_type.state.blend_step = 1.0f / static_cast<float>(move.blend_in);
    }
    BtActStatus.action_gauge = std::max(0.0f, BtActStatus.action_gauge - gauge_cost);
    if (move.lunge > 0.0f) {
        const float push = g_move_run.facing_target ? std::atan2(BtActStatus.move_x, BtActStatus.move_z)
                                                    : g_move_run.heading + move.lunge_angle * 0.017453292f;
        BtActStatus.move_vector[0] = std::sin(push);
        BtActStatus.move_vector[1] = 0.0f;
        BtActStatus.move_vector[2] = std::cos(push);
        BtActStatus.move_vector[3] = 1.0f;
        BtActStatus.move_power = move.lunge;
        BtActStatus.move_power_decay = move.lunge_decay;
    } else {
        BtActStatus.move_power = 0.0f;
        BtActStatus.move_power_decay = 0.0f;
    }
    if (move.invincible) {
        BtActStatus.invincible_frames = std::max(BtActStatus.invincible_frames, g_move_run.frames + 4);
        BtActStatus.action_busy = 1;
        BtActStatus.can_act = 0;
    }
    if (move.wear > 0.0f) SwordDmgCheck1(move.wear, 0);
    // Evades keep their own cooldown, so an attack move doesn't hold back the roll that cancels it (nor the other way).
    (IsEvade(move) ? g_dodge_cooldown : g_move_cooldown) = move.cooldown + g_move_run.frames;
    g_blow_action = -1;
    g_chain = 0; // a move ends the combo; a finisher spends it
    const CharacterIdView &kNames = kCharacterIds;
    SharedRuntime().QueueEvent("game.player.move", {{"id", move.id}, {"owner", move.owner}, {"character", kNames[move.chara]}, {"cancel", cancelling}, {"frames", g_move_run.frames},
                                                     {"variant", variant >= 0 ? Json(variant + 1) : Json(nullptr)}});
    return true;
}

// The shot a move is firing right now (set around MoonModSpawnPlayerShots), and the look each slot of the player's
// shots was given when it was spawned (the native shots get none).
struct PendingShotStyle {
    bool active = false;
    const MoveHit *hit = nullptr;
    float heading = 0.0f;
};
PendingShotStyle g_shot_style;
struct ShotLook {
    int cell = -1;
    float size = 1.0f;
    std::string burst;
    float burst_scale = 1.0f;
};
std::array<ShotLook, 12> g_shot_looks{};

void FirePlayerMoveHit(const MoveHit &hit) {
    const float heading = PlayerHeading();
    if (hit.shot) {
        // Ruby's casts count her ring's magic as her own do (half attack, half magic).
        const float base = UserStatus != nullptr && UserStatus->cur_chara == CHARA_RUBY ? (NowWeaponHave->attack + NowWeaponHave->magic) * 0.5f
                                                                                      : static_cast<float>(NowWeaponHave->attack);
        int damage = static_cast<int>(std::lround(base * hit.damage));
        if (StatusErrCheck(AILMENT_STAMINA) != 0) damage *= 2;
        g_shot_style = {true, &hit, heading};
        const int fired = MoonModSpawnPlayerShots(heading, hit.speed, std::max(1, damage), hit.count, hit.spread * 0.017453292f);
        g_shot_style = {};
        if (fired > 0) {
            g_blow_step = g_move_clock;
            g_blow_action = kModMoveAction;
            if (hit.charged) g_charged_until = g_charge_clock + 30;
        }
        return;
    }
    sceVu0FVECTOR at = {CharaMain.pos[0] + std::sin(heading) * hit.reach, CharaMain.pos[1] + hit.height,
                        CharaMain.pos[2] + std::cos(heading) * hit.reach, 1.0f};
    int damage = static_cast<int>(std::lround(NowWeaponHave->attack * hit.damage));
    if (StatusErrCheck(AILMENT_STAMINA) != 0) damage *= 2;
    // A hit record is spent on the first enemy it strikes, so a wide hit lays one per enemy in its reach. Each lasts
    // three steps, as a native swing's hit window does, so the enemies' own test meets it.
    int copies = 1;
    if (NowMonstorUnit != nullptr) {
        copies = 0;
        for (int m = 0; m < MONSTOR_SLOT_MAX; ++m) {
            if (NowMonstorUnit->monster[m].state != 2 || NowMonstorUnit->monster[m].hp <= 0) continue;
            float where[4]{};
            NowMonstorUnit->chara[m][0].GetPosition(where);
            const float dx = where[0] - at[0], dz = where[2] - at[2];
            if (dx * dx + dz * dz <= (hit.radius + 12.0f) * (hit.radius + 12.0f)) ++copies;
        }
        copies = std::clamp(copies, 1, 6);
    }
    sceVu0FVECTOR from;
    sceVu0CopyVector(from, CharaMain.pos);
    for (int n = 0; n < copies; ++n) {
        if (NowColData->Set(at, damage, 3, hit.radius, 0.0f, 2, hit.kind, 0, 0) < 0) break;
        auto &record = NowColData->hit[NowColData->now_hit];
        record.vs_monster = NowWeaponHave->vs_monster;
        record.weapon_flags = NowWeaponHave->flags;
        record.flags = GetWeaponElementAttr(NowWeaponHave->best_elem);
        record.owner = 0;
        record.attack_no = 0;
        if (hit.knockback > 0.0f) NowColData->SetKickBack(from, hit.knockback, 0.3f, 2);
    }
    if (hit.charged) g_charged_until = g_charge_clock + 30;
    g_blow_step = g_move_clock;
    g_blow_action = kModMoveAction;
    if (!hit.burst.empty()) {
        BurstEffect(Json::array({{{"kind", hit.burst}, {"position", {{"x", at[0]}, {"y", at[1]}, {"z", at[2]}}}, {"scale", hit.burst_scale}}}));
    }
}

void PlayerMoveTick() {
    const BT_ACT_STATUS &act = BtActStatus;
    ++g_move_clock;
    if (g_motions_suspended_chara >= 0 && mode == GAME_MODE_DUNGEON && UserStatus != nullptr && BtActStatus.in_presentation == 0 &&
        UserStatus->step_disable == 0) {
        const int chara = g_motions_suspended_chara;
        g_motions_suspended_chara = -1;
        if (UserStatus->cur_chara == chara) ApplyPlayerMotions(chara);
    }
    if (!act.action_on) g_blow_action = -1; // between attacks: a blow belongs to the swing that laid it, not the next one
    if (g_move_cooldown > 0) --g_move_cooldown;
    if (g_dodge_cooldown > 0) --g_dodge_cooldown;
    // A run: the stick pushed hard, nothing locked on (locking on turns a run into a strafe), for run_frames steps.
    if (!act.action_on) {
        g_run_frames = act.stick_strength >= 0.7f && act.movement_locked == 0 && act.in_water == 0 && lockOnTargetFlag == 0 ? g_run_frames + 1 : 0;
    }
    if (g_move_run.active && act.action_no != kModMoveAction) {
        g_move_run.active = false;
        RestoreMoveBlend();
    }
    // Toan's charge flashes again as it reaches a charge move.
    const float held = UserStatus != nullptr && CombatChara(UserStatus->cur_chara) == CHARA_TOAN && act.action_on && act.action_no == 14 ? act.charge_time - 1.0f : 0.0f;
    for (const PlayerMove &move : g_player_moves) {
        if (CombatChara(move.chara) == CHARA_TOAN && move.trigger == 1 && move.min_charge > 0.0f && g_last_charge_held < move.min_charge && held >= move.min_charge) {
            setUnitAmbientAnime(15.0f, 1.0f, 230.0f, 140.0f, 40.0f);
            SndSePlay(SE_CHARA_ACTION, -1, 0);
        }
    }
    g_last_charge_held = held;
}

// A chest being opened, an item shown, an event: the game has taken the character (and may have swapped its motions,
// as the large chest does), so no move starts.
bool PlayerInPresentation() {
    return UserStatus == nullptr || BtActStatus.in_presentation != 0 || UserStatus->step_disable != 0;
}

// How far through the mod move playing now, 0..1.
float MoveProgress() {
    return std::min(1.0f, static_cast<float>(g_move_run.frame + 1) / static_cast<float>(std::max(1, g_move_run.frames)));
}

// Criticals (player.moves_set trigger critical): the enemy a mod armed for a riposte (player.critical_set), until a
// step of g_move_clock, and how many more steps each monster slot is held in a critical's pair motion, its script
// paused (ChronicleMonsterHeld).
struct CriticalArm {
    std::string owner;
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    int until = 0;
};
CriticalArm g_critical_arm;
std::array<int, MONSTOR_SLOT_MAX> g_critical_hold{};

// The target's motion number for a pair: its model's loaded copy of the monster motion, or -1.
int CriticalPairMotion(const CMonstorUnit *unit, int slot, const PlayerMove::CriticalPair &pair, int &length) {
    const int base = unit->monster[slot].base_model;
    if (base < 0 || base >= static_cast<int>(g_applied_monster_motions.size())) return -1;
    for (const AppliedMonsterMotions::Entry &entry : g_applied_monster_motions[base].entries) {
        if (entry.owner == pair.owner && entry.id == pair.id) {
            length = entry.length;
            return entry.number;
        }
    }
    return -1;
}

bool BeginCritical(const PlayerMove &move, CMonstorUnit *unit, int slot) {
    float target[4]{}, at[4]{};
    unit->chara[slot][0].GetPosition(target);
    CharaMain.GetPosition(at);
    float dx = at[0] - target[0], dz = at[2] - target[2];
    const float distance = std::hypot(dx, dz);
    if (distance > 0.01f) {
        dx /= distance;
        dz /= distance;
    } else {
        dx = 0.0f;
        dz = 1.0f;
    }
    if (!StartPlayerMove(move)) return false;
    // Drawn in to the gap (never pushed out, which could put him in a wall), facing the target.
    if (distance > move.critical_gap) {
        at[0] = target[0] + dx * move.critical_gap;
        at[2] = target[2] + dz * move.critical_gap;
        CharaMain.SetPosition(at);
        CharaMain.frame->SetPosition(at);
    }
    g_move_run.heading = std::atan2(-dx, -dz);
    CharaMain.frame->SetRotation(0.0f, g_move_run.heading, 0.0f);
    BtActStatus.move_power = 0.0f;
    // The target stops where it is, turned to him (a riposte) or away from him (a backstab), and plays its half.
    MONSTOR &monster = unit->monster[slot];
    monster.movement_speed = 0.0f;
    monster.knockback[0] = monster.knockback[1] = 0.0f;
    monster.turn_speed = 0.0f;
    monster.stop_timer = 0; // a parried or staggered enemy is stopped (frozen, grey): its crumple plays instead
    float rotation[4]{};
    unit->chara[slot][0].GetRotation(rotation);
    rotation[1] = move.critical == 1 ? std::atan2(-dx, -dz) : std::atan2(dx, dz);
    unit->chara[slot][0].SetRotation(rotation);
    const int model = ChronicleNativeMonsterModelId(unit, slot);
    int hold = move.critical_hold;
    for (const PlayerMove::CriticalPair &pair : move.pairs) {
        if (pair.model != model) continue;
        int length = 0;
        const int number = CriticalPairMotion(unit, slot, pair, length);
        if (number < 0) break;
        for (int part = 0; part <= monster.attachment_count; ++part) {
            if (part > 0 && unit->chara[slot][part].frame == nullptr) continue;
            unit->chara[slot][part].SetMotion(number, 0);
            unit->chara[slot][part].SetMotionSpeed(pair.speed);
        }
        monster.requested_motion = number;
        monster.requested_motion_flags = 0;
        monster.requested_motion_speed = pair.speed;
        if (hold == 0) hold = static_cast<int>(std::ceil(static_cast<float>(length) / pair.speed));
        break;
    }
    if (hold == 0) hold = g_move_run.frames;
    g_critical_hold[slot] = hold;
    if (g_critical_arm.slot == slot) g_critical_arm = {};
    Json event = {{"id", move.id}, {"owner", move.owner}, {"kind", move.critical == 1 ? "backstab" : "riposte"},
                  {"entity", MonsterHandleId(slot, g_monster_handle_generations[slot])}, {"hold", hold}};
    if (model >= 0) event["native_model_id"] = model;
    SharedRuntime().QueueEvent("game.player.critical", event);
    ChronicleFeelCritical();
    return true;
}

// The attack button at rest: the character's riposte on the armed enemy within its reach, else a backstab on the
// nearest enemy within reach whose back he stands at (the lock target first).
bool TryCritical(int chara) {
    CMonstorUnit *unit = NowMonstorUnit;
    if (unit == nullptr) return false;
    PrepareMonsterHandleUnit(unit);
    float at[4]{};
    CharaMain.GetPosition(at);
    const auto live = [&](int slot) {
        const MONSTOR &m = unit->monster[slot];
        return m.state == 2 && m.hp > 0 && m.revealed != 0 && g_critical_hold[slot] <= 0;
    };
    const auto away = [&](int slot) {
        float there[4]{};
        unit->chara[slot][0].GetPosition(there);
        return std::hypot(at[0] - there[0], at[2] - there[2]);
    };
    for (const PlayerMove &move : g_player_moves) {
        if (move.trigger != 9 || move.chara != chara) continue;
        int target = -1;
        if (move.critical == 2) {
            const CriticalArm &arm = g_critical_arm;
            if (arm.slot < 0 || arm.owner != move.owner || g_move_clock > arm.until || !live(arm.slot) ||
                EnsureMonsterHandleGeneration(unit, arm.slot) != arm.generation || away(arm.slot) > move.critical_reach) {
                continue;
            }
            target = arm.slot;
        } else {
            float best = move.critical_reach;
            for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) {
                if (!live(slot)) continue;
                const float d = away(slot);
                if (d > move.critical_reach) continue;
                float there[4]{}, rotation[4]{};
                unit->chara[slot][0].GetPosition(there);
                unit->chara[slot][0].GetRotation(rotation);
                // 0 when he stands where it faces, 180 at its back.
                if (GuardAngle(rotation[1], there[0], there[2], at[0], at[2]) < 180.0f - move.critical_arc * 0.5f) continue;
                const bool locked = lockOnTargetFlag != 0 && lockOnTargetNo == slot;
                if (locked || d < best) {
                    best = locked ? -1.0f : d;
                    target = slot;
                }
            }
        }
        if (target >= 0 && BeginCritical(move, unit, target)) return true;
    }
    return false;
}

namespace {
// player.critical_set({entity, frames?}): that enemy open to this mod's riposte (a critical move) for frames steps
// (default 120), as after a parry or a stagger. One enemy at a time; a new one replaces it.
ApiCallResult SetCriticalTarget(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("entity")) {
        return Invalid("expected {entity = monster handle, frames = 1..600?}");
    }
    for (auto field = args[0].begin(); field != args[0].end(); ++field) {
        if (field.key() != "entity" && field.key() != "frames") return Invalid("player.critical_set only takes entity and frames");
    }
    std::string handle;
    int slot = -1;
    std::uint32_t generation = 0;
    int frames = 120;
    if (!ReadId(args[0]["entity"], handle) || !ReadMonsterHandle(handle, slot, generation)) {
        return Invalid("entity must be a generation-checked Chronicle monster handle");
    }
    if (args[0].contains("frames") && !ReadBoundedInt(args[0], "frames", 1, 600, frames)) return Invalid("frames is 1..600 steps");
    CMonstorUnit *unit = NowMonstorUnit;
    if (mode != GAME_MODE_DUNGEON || unit == nullptr) return Failed("player.critical_set needs an active dungeon");
    PrepareMonsterHandleUnit(unit);
    if (EnsureMonsterHandleGeneration(unit, slot) != generation) return Failed("the entity handle is stale because its monster slot was reused");
    const MONSTOR &monster = unit->monster[slot];
    if (monster.state != 2 || monster.hp <= 0 || monster.revealed == 0) return Failed("the selected entity is not a live, revealed enemy");
    g_critical_arm = {std::string(owner), handle, slot, generation, g_move_clock + frames};
    return {ApiCallStatus::Ok, {{"entity", handle}, {"frames", frames}}, {}};
}

// player.critical_clear(): this mod's armed enemy, if any, no longer open.
ApiCallResult ClearCriticalTarget(std::string_view owner, const Json &args) {
    if (!args.is_array() || !args.empty()) return Invalid("player.critical_clear takes no arguments");
    const bool cleared = g_critical_arm.slot >= 0 && g_critical_arm.owner == owner;
    if (cleared) g_critical_arm = {};
    return {ApiCallStatus::Ok, {{"cleared", cleared}}, {}};
}

void ReleaseCriticals(std::string_view owner) {
    if (g_critical_arm.owner == owner) g_critical_arm = {};
}
} // namespace

int ChronicleMonsterHeld(CMonstorUnit * /*unit*/, int slot) {
    if (slot >= 0 && slot < MONSTOR_SLOT_MAX && g_monster_haste[slot] > 0) --g_monster_haste[slot];
    if (slot < 0 || slot >= MONSTOR_SLOT_MAX || g_critical_hold[slot] <= 0) return 0;
    return --g_critical_hold[slot] > 0 ? 1 : 2;
}

bool ChronicleMonsterIsHeld(int slot) { return slot >= 0 && slot < MONSTOR_SLOT_MAX && g_critical_hold[slot] > 0; }

void ChronicleMonsterHoldReset(int slot) {
    if (slot < 0) {
        g_critical_hold.fill(0);
        g_monster_haste.fill(0);
    } else if (slot < MONSTOR_SLOT_MAX) {
        g_critical_hold[slot] = 0;
        g_monster_haste[slot] = 0;
    }
}

float ChronicleMonsterSpeedScale(int slot) {
    return slot >= 0 && slot < MONSTOR_SLOT_MAX && g_monster_haste[slot] > 0 ? kHasteScale : 1.0f;
}

bool ChroniclePlayerMoveOnAttack() {
    if (mode != GAME_MODE_DUNGEON || PlayerInPresentation() || BtActStatus.jumping != 0 || BtActStatus.in_water != 0) return false;
    const int chara = UserStatus->cur_chara;
    // An evade's follow-up: attack late in a roll or backstep (it cuts the rest short), or just after one ends.
    const bool late_evade = BtActStatus.action_on == 1 && BtActStatus.action_no == kModMoveAction && g_move_run.active && IsEvade(g_move_run.move) &&
                            MoveProgress() >= 0.6f;
    const bool after_evade = BtActStatus.action_on == 0 && BtActStatus.can_act != 0 && g_move_clock - g_evade_end_step <= kEvadeFollowWindow;
    if (late_evade || after_evade) {
        for (const PlayerMove &move : g_player_moves) {
            if (move.trigger != 8 || move.chara != chara) continue;
            if (late_evade) EndPlayerMove();
            if (StartPlayerMove(move, late_evade)) {
                g_evade_end_step = -1000;
                return true;
            }
        }
    }
    if (BtActStatus.can_act == 0 || g_move_cooldown > 0) return false;
    if (BtActStatus.action_on == 6 && BtActStatus.action_no == 9) {
        for (const PlayerMove &move : g_player_moves) {
            if (move.trigger == 2 && move.chara == chara && StartPlayerMove(move)) {
                BtActStatus.guard_mode = 0;
                return true;
            }
        }
        return false;
    }
    if (BtActStatus.action_on != 0) return false;
    if (TryCritical(chara)) return true;
    // Locked on with the stick pushed toward the target or away from it.
    float target = 0.0f;
    if (BtActStatus.stick_strength >= 0.5f && LockTargetHeading(target)) {
        float off = std::atan2(BtActStatus.move_x, BtActStatus.move_z) - target;
        while (off > 3.14159265f) off -= 6.2831853f;
        while (off < -3.14159265f) off += 6.2831853f;
        const int direction = std::fabs(off) <= 1.0472f ? 0 : std::fabs(off) >= 2.0944f ? 1 : -1;
        for (const PlayerMove &move : g_player_moves) {
            if (move.trigger == 7 && move.chara == chara && move.direction == direction && StartPlayerMove(move)) return true;
        }
    }
    for (const PlayerMove &move : g_player_moves) {
        // Still running as attack is pressed: the stick still pushed hard and nothing locked on.
        const bool running = BtActStatus.stick_strength >= 0.7f && lockOnTargetFlag == 0;
        if (move.trigger == 0 && move.chara == chara && running && g_run_frames >= move.run_frames && StartPlayerMove(move)) {
            g_run_frames = 0;
            return true;
        }
    }
    return false;
}

// max_charge hands a longer charge back to Toan's native wave, so it only counts while he has the wave's skill.
void ChroniclePlayerSwingBlow() {
    // A blow from a new swing (the next stage of the combo, or the next attack soon enough) adds to the chain; more
    // blows of the same swing (laid on the steps right after it) don't. Ungaga's three thrusts share one action, so a
    // pause between blows also starts a new swing.
    if (g_blow_action != BtActStatus.action_no || g_move_clock - g_blow_step > kSwingBlowSteps) {
        g_chain = g_move_clock - g_chain_step <= kChainGap ? g_chain + 1 : 1;
        g_chain_step = g_move_clock;
    }
    g_blow_step = g_move_clock;
    g_blow_action = BtActStatus.action_no;
}

// Whether L2 may cut the attack playing now short: a native swing or a mod attack move whose last blow is at least
// `after` steps past. A special (the jump slash, the wave), a charge, a hit taken or an evade can't be cut.
bool AttackRecoveryCancellable(int after) {
    const BT_ACT_STATUS &act = BtActStatus;
    if (act.action_on != 1 || g_blow_action != act.action_no || g_move_clock - g_blow_step < after) return false;
    if (act.action_no == kModMoveAction) {
        if (!g_move_run.active || IsEvade(g_move_run.move)) return false;
        for (bool done : g_move_run.hit_done) {
            if (!done) return false;
        }
        return true;
    }
    // A combo swing (Toan's, Goro's, Ungaga's: actions 0x24-0x27) may be cut whatever it says of acting; Ungaga's hold
    // the character for their whole length.
    return act.can_act != 0 || (act.action_no >= 0x24 && act.action_no <= 0x27);
}

bool ChroniclePlayerChainToAttack() {
    if (g_player_tuning.chain_cancel <= 0 || BtActStatus.action_no != kModMoveAction || !g_move_run.active || IsEvade(g_move_run.move)) return false;
    if (!AttackRecoveryCancellable(g_player_tuning.chain_cancel)) return false;
    EndPlayerMove();
    return true;
}

// R2: a combo finisher if the combo has laid enough blows (the longest chain it has reached wins), else the heavy
// attack. A finisher cuts the attack playing short; the heavy attack starts only from rest.
namespace {
ChroniclePressBuffer g_r2_press, g_dodge_press;
} // namespace

bool ChroniclePlayerMoveOnR2Buffered(bool pressed);
bool ChroniclePlayerMoveOnR2(bool pressed) {
    // input_buffer: R2 pressed while it can't start (mid-swing before a finisher may, mid-roll) comes out at the
    // first step it can.
    if (!g_r2_press.Wanted(pressed, g_player_tuning.input_buffer)) return false;
    if (!ChroniclePlayerMoveOnR2Buffered(true)) return false;
    g_r2_press.Taken();
    return true;
}
bool ChroniclePlayerMoveOnR2Buffered(bool pressed) {
    if (!pressed || mode != GAME_MODE_DUNGEON || PlayerInPresentation() || BtActStatus.jumping != 0 || BtActStatus.in_water != 0 ||
        BtActStatus.movement_locked != 0 || BtActStatus.recoil_frames != 0) {
        return false;
    }
    const int chara = UserStatus->cur_chara;
    // Guarding: the equipped weapon's art, if it has one at its level.
    if (BtActStatus.action_on == 6 && BtActStatus.action_no == 9 && BtActStatus.can_act != 0 && g_move_cooldown <= 0 && NowWeaponHave != nullptr) {
        for (const PlayerMove &move : g_player_moves) {
            if (move.trigger != 11 || move.chara != chara || NowWeaponHave->level < move.min_level) continue;
            if (!move.weapons.empty() && std::ranges::find(move.weapons, static_cast<int>(NowWeaponHave->item_no)) == move.weapons.end()) continue;
            if (StartPlayerMove(move)) {
                BtActStatus.guard_mode = 0;
                return true;
            }
        }
        return false;
    }
    const bool idle = BtActStatus.action_on == 0 && BtActStatus.can_act != 0;
    // Any native attack playing (the chain's own swing past its blow, or the next swing already started by a
    // mashed button), but not Toan's held charge, which has its own release.
    const bool swinging = BtActStatus.action_on == 1 && BtActStatus.action_no != kModMoveAction && BtActStatus.action_no != 14;
    if ((idle || swinging) && g_chain > 0 && g_move_clock - g_chain_step <= kFinisherWindow) {
        const PlayerMove *best = nullptr;
        for (const PlayerMove &move : g_player_moves) {
            if (move.trigger == 6 && move.chara == chara && move.chain <= g_chain && (best == nullptr || move.chain > best->chain)) best = &move;
        }
        if (best != nullptr && StartPlayerMove(*best, swinging)) return true;
    }
    // chain_cancel: the heavy cuts short the recovery of the attack playing (a native swing, or one of the mod's own
    // attack moves, a heavy among them) once its last blow is that many steps past.
    const bool chaining = !idle && g_player_tuning.chain_cancel > 0 && AttackRecoveryCancellable(g_player_tuning.chain_cancel);
    if ((!idle && !chaining) || g_move_cooldown > 0) return false;
    for (const PlayerMove &move : g_player_moves) {
        if (move.trigger == 5 && move.chara == chara && StartPlayerMove(move, chaining)) return true;
    }
    return false;
}

bool ChroniclePlayerHasDodgeMove() {
    if (UserStatus == nullptr) return false;
    return std::ranges::any_of(g_player_moves, [](const PlayerMove &move) {
        return (move.trigger == 3 || move.trigger == 4) && move.chara == UserStatus->cur_chara;
    });
}

// Whether the character playing now has an R2 move (which takes R2 from the first-person view).
bool PlayerHasR2Moves() {
    if (UserStatus == nullptr) return false;
    for (const PlayerMove &move : g_player_moves) {
        if ((move.trigger == 5 || move.trigger == 6 || move.trigger == 11) && move.chara == UserStatus->cur_chara) return true;
    }
    return false;
}

bool ChroniclePlayerMoveOnDodgeBuffered(bool pressed);
bool ChroniclePlayerMoveOnDodge(bool pressed) {
    // input_buffer: a roll pressed late in a swing's recovery, mid-roll or in a hit-stop comes out at the first step
    // it can (the swing's cancel point, the roll's end) instead of being lost.
    if (!g_dodge_press.Wanted(pressed, g_player_tuning.input_buffer)) return false;
    if (!ChroniclePlayerMoveOnDodgeBuffered(true)) return false;
    g_dodge_press.Taken();
    return true;
}
bool ChroniclePlayerMoveOnDodgeBuffered(bool pressed) {
    if (!pressed || mode != GAME_MODE_DUNGEON || PlayerInPresentation() || BtActStatus.jumping != 0 || BtActStatus.in_water != 0 ||
        BtActStatus.movement_locked != 0 || BtActStatus.recoil_frames != 0 || g_dodge_cooldown > 0) {
        return false;
    }
    const bool idle = BtActStatus.action_on == 0 && BtActStatus.can_act != 0;
    const int wanted = BtActStatus.stick_strength >= 0.3f ? 3 : 4;
    // Locked on, the stick's way from the target picks a dodge made for it (a side roll); the plain dodge otherwise.
    int way = -1;
    float target = 0.0f;
    if (wanted == 3 && LockTargetHeading(target)) {
        float off = std::atan2(BtActStatus.move_x, BtActStatus.move_z) - target;
        while (off > 3.14159265f) off -= 6.2831853f;
        while (off < -3.14159265f) off += 6.2831853f;
        way = std::fabs(off) <= 0.7854f ? 0 : std::fabs(off) >= 2.3562f ? 1 : off > 0.0f ? 2 : 3; // stick headings turn left as they grow
    }
    for (int pass = way >= 0 ? 0 : 1; pass < 2; ++pass) {
        for (const PlayerMove &move : g_player_moves) {
            if (move.trigger != wanted || move.chara != UserStatus->cur_chara) continue;
            if (wanted == 3 && move.direction != (pass == 0 ? way : -1)) continue;
            const bool cancelling = !idle && move.cancel && AttackRecoveryCancellable(move.cancel_after);
            if ((idle || cancelling) && StartPlayerMove(move, cancelling, pass == 0 && way >= 2)) return true;
        }
    }
    return false;
}

bool ChroniclePlayerMoveOnChargeRelease(float held_seconds) {
    if (mode != GAME_MODE_DUNGEON || PlayerInPresentation()) return false;
    const bool wave = UserStatus->cur_chara == CHARA_TOAN && UserStatus->skill_owned[0] != 0;
    for (const PlayerMove &move : g_player_moves) {
        if (move.trigger == 1 && move.chara == UserStatus->cur_chara && held_seconds >= move.min_charge && (!wave || held_seconds < move.max_charge) &&
            StartPlayerMove(move)) {
            return true;
        }
    }
    return false;
}

namespace {
// player.move_play: one of the mod's own moves (player.moves_set) started now, whatever its trigger, for the
// character playing it: a chained move (Opar's big slam once he has turned into the monster). As a trigger would, it
// needs him free to act and the move's gauge; being chained, it doesn't wait on the moves' cooldown.
ApiCallResult PlayPlayerMove(std::string_view owner, const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object() || !args[0].contains("id") || !args[0]["id"].is_string() ||
        args[0].size() != 1) {
        return Invalid("expected {id}");
    }
    const std::string id = args[0]["id"].get<std::string>();
    const PlayerMove *found = nullptr;
    for (const PlayerMove &move : g_player_moves) {
        if (move.owner == owner && move.id == id) found = &move;
    }
    if (found == nullptr) return Invalid("id names none of this mod's moves");
    if (mode != GAME_MODE_DUNGEON || UserStatus == nullptr || PlayerInPresentation()) return Failed("a move needs an active dungeon");
    if (found->chara != UserStatus->cur_chara) return Failed("the move is another character's");
    if (BtActStatus.action_on != 0 || BtActStatus.can_act == 0 || BtActStatus.movement_locked != 0 || BtActStatus.recoil_frames != 0) {
        return Failed("the player is in an action");
    }
    if (!StartPlayerMove(*found)) return Failed("the move could not start (its gauge, or its motion)");
    return {ApiCallStatus::Ok, {{"id", id}}, {}};
}
} // namespace

bool ChroniclePlayerMovePlay() {
    if (!g_move_run.active) return false;
    if (BtActStatus.action_no != kModMoveAction || CharaMain.frame == nullptr || NowColData == nullptr || NowWeaponHave == nullptr) {
        g_move_run.active = false;
        RestoreMoveBlend();
        return false;
    }
    MoveRun &run = g_move_run;
    const float progress = std::min(1.0f, static_cast<float>(run.frame + 1) / static_cast<float>(run.frames));
    BtActStatus.motion_no = run.move.motion;
    CharaMain.motion_type.state.time = run.start + run.span * progress;
    BtActStatus.swinging = 1;
    if (run.move.spin != 0.0f) {
        const float turned = std::clamp((progress - run.move.spin_from) / (run.move.spin_to - run.move.spin_from), 0.0f, 1.0f);
        CharaMain.frame->SetRotation(0.0f, run.heading + 6.2831853f * run.move.spin * turned, 0.0f);
    }
    for (size_t n = 0; n < run.move.cues.size(); ++n) {
        const MoveCue &cue = run.move.cues[n];
        if (run.cue_done[n] || cue.at > progress) continue;
        run.cue_done[n] = true;
        if (cue.sound >= 0) SndSePlay(cue.sound, -1, 0);
        static const float kTrail[5][3] = {{8, 32, 64}, {0xA, 32, 32}, {0xE, 24, 32}, {0x1E, 28, 16}, {0x14, 16, 64}};
        if (cue.trail >= 0) CWeaponFx.Set(static_cast<int>(kTrail[cue.trail][0]), kTrail[cue.trail][1], kTrail[cue.trail][2]);
    }
    for (size_t n = 0; n < run.move.hits.size(); ++n) {
        if (run.hit_done[n] || run.move.hits[n].at > progress) continue;
        run.hit_done[n] = true;
        FirePlayerMoveHit(run.move.hits[n]);
    }
    if (run.move.blend_in > 0 && run.frame > run.move.blend_in + 1) RestoreMoveFade();
    // The camera keeps up with a quick move (an evade), set every step since the dungeon's stick code sets its own.
    if (run.move.camera > 0 && NowCamera__3 != nullptr) {
        if (g_saved_camera_speed <= 0.0f) g_saved_camera_speed = NowCamera__3->speed > 0.0f ? NowCamera__3->speed : 8.0f;
        NowCamera__3->SetSpeed(static_cast<float>(run.move.camera));
    }
    if (++run.frame >= run.frames) EndPlayerMove();
    return true;
}

void ChronicleWallStruck() {
    if (mode != GAME_MODE_DUNGEON || NowDngMap == nullptr || CharaMain.frame == nullptr || DngShortcutDoorCount <= 0 ||
        DngShortcutDoorSeed != NowDngMap->map_seed) {
        return;
    }
    float at[4];
    CharaMain.GetPosition(at);
    const float heading = PlayerHeading();
    int wall_no = 0;
    for (int d = 0; d < DngShortcutDoorCount; ++d) {
        DNG_SHORTCUT_DOOR &door = DngShortcutDoorTable[d];
        if (door.key_item != 0) continue;
        const int number = wall_no++;
        if (!door.locked) continue;
        // The doorway's wall stands on the cell's edge toward its corridor; the blow is struck from inside the cell.
        const float dx = 160.0f * door.x - at[0], dz = 160.0f * door.y - at[2];
        const float dist = std::hypot(dx, dz);
        if (dist > 150.0f) continue;
        const int side = door.open_part - MAP_PARTS_ROOM_DOOR_NORTH; // 0 north (-z), 1 east (+x), 2 south (+z), 3 west (-x)
        const float wx = side == 1 ? 1.0f : (side == 3 ? -1.0f : 0.0f), wz = side == 2 ? 1.0f : (side == 0 ? -1.0f : 0.0f);
        const float facing_x = std::sin(heading), facing_z = std::cos(heading);
        if (facing_x * wx + facing_z * wz < 0.5f) continue; // not facing that wall
        door.locked = 0;
        MAP_CELL &cell = NowDngMap->cells[door.x + door.y * DNG_GRID];
        cell.parts_no = door.open_part;
        cell.direction = door.open_direction;
        SndSePlay(SE_ITEM_GET, -1, 0);
        SharedRuntime().QueueEvent("game.dungeon.wall_opened", {{"wall", number}, {"x", door.x}, {"y", door.y},
                                                                {"frame", SharedRuntime().Frame()}});
        return;
    }
}

// A locked shortcut door opens when the player reaches it holding its key: the key is used up and the cell gets its
// doorway back. Without the key, mods hear game.dungeon.door_locked once while the player stays at it.
void ChronicleShortcutDoorsTick() {
    static int told = -1;
    if (mode != GAME_MODE_DUNGEON || NowDngMap == nullptr || UserStatus == nullptr || CharaMain.frame == nullptr ||
        DngShortcutDoorCount <= 0 || DngShortcutDoorSeed != NowDngMap->map_seed) {
        return;
    }
    // Test hook: DC_SHORTCUT_VISIT puts the player in the corridor outside the first door, ten seconds into the floor.
    static int visit_clock = 0, visit_seed = -1;
    if (const char *visit = std::getenv("DC_SHORTCUT_VISIT"); visit != nullptr && *visit != '\0' && visit_seed != NowDngMap->map_seed &&
                                                               ++visit_clock > 600) {
        visit_seed = NowDngMap->map_seed;
        visit_clock = 0;
        const DNG_SHORTCUT_DOOR &door = DngShortcutDoorTable[0];
        const int dx = door.gate_part == MAP_PARTS_ROOM_GATE_EAST ? 1 : (door.gate_part == MAP_PARTS_ROOM_GATE_WEST ? -1 : 0);
        const int dy = door.gate_part == MAP_PARTS_ROOM_GATE_SOUTH ? 1 : (door.gate_part == MAP_PARTS_ROOM_GATE_NORTH ? -1 : 0);
        float outside[4];
        sceVu0CopyVector(outside, CharaMain.frame->position); // the frame's position is the one on the map grid
        outside[0] = 160.0f * static_cast<float>(door.x - dx);
        outside[2] = 160.0f * static_cast<float>(door.y - dy);
        CharaMain.SetPosition(outside);
        CharaMain.frame->SetPosition(outside);
        // The camera jumps along (the floor collision is taken around it), as a script's warp does.
        if (NowCamera__3 != nullptr) {
            NowCamera__3->SetFollow(outside[0], outside[1] + 13.0f, outside[2]);
            NowCamera__3->Step(-1);
        }
        // Collision only takes the cells near the last drawn view: measure them from the new place at once.
        for (int row = 0; row < DNG_GRID; ++row) {
            for (int col = 0; col < DNG_GRID; ++col) {
                NowDngMap->cells[col + row * DNG_GRID].camera_dist = std::hypot(160.0f * col - outside[0], 160.0f * row - outside[2]);
            }
        }
        // face the door
        const float facing = std::atan2(static_cast<float>(dx), static_cast<float>(dy)); // from the room, toward the door
        CharaMain.SetRotation(0.0f, facing, 0.0f);
        CharaMain.frame->SetRotation(0.0f, facing, 0.0f);
        if (NowCamera__3 != nullptr) { NowCamera__3->SetAngleSoon(facing - PI); NowCamera__3->Step(-1); } // the camera behind her
    }
    float at[4];
    CharaMain.GetPosition(at);
    int near = -1;
    for (int d = 0; d < DngShortcutDoorCount; ++d) {
        const DNG_SHORTCUT_DOOR &door = DngShortcutDoorTable[d];
        if (!door.locked || door.key_item == 0) continue; // an illusory wall opens to a blow (ChronicleWallStruck)
        if (std::hypot(at[0] - 160.0f * door.x, at[2] - 160.0f * door.y) < 130.0f) near = d;
    }
    if (near < 0) {
        told = -1;
        return;
    }
    DNG_SHORTCUT_DOOR &door = DngShortcutDoorTable[near];
    auto *status = static_cast<CDngStatusData *>(UserStatus);
    if (GetNowItemNum(static_cast<short>(door.key_item), &status->item_pack) > 0) {
        DeleteItemAfterUseItem(static_cast<short>(door.key_item), &status->item_pack);
        door.locked = 0;
        MAP_CELL &cell = NowDngMap->cells[door.x + door.y * DNG_GRID];
        cell.parts_no = door.open_part;
        cell.direction = door.open_direction;
        SndSePlay(SE_ITEM_GET, -1, 0);
        SharedRuntime().QueueEvent("game.dungeon.door_opened", {{"key", ItemStableId(door.key_item)}, {"x", door.x}, {"y", door.y},
                                                                {"frame", SharedRuntime().Frame()}});
        told = -1;
    } else if (told != near) {
        told = near;
        SharedRuntime().QueueEvent("game.dungeon.door_locked", {{"key", ItemStableId(door.key_item)}, {"x", door.x}, {"y", door.y},
                                                                {"frame", SharedRuntime().Frame()}});
    }
}

// Zones (dungeons.set_zones): a locked zone gate opens when the player reaches it holding its key, which is used up;
// without it mods hear game.dungeon.zone_gate_locked once while the player stays there. Walking into another zone
// (and arriving on a zoned floor) is game.dungeon.zone_entered.
bool ChronicleTownExitRefused(const char *via) {
    if (g_exits_owner.empty() || mode != GAME_MODE_DUNGEON) return false;
    SharedRuntime().QueueEvent("game.dungeon.town_refused", {{"via", via}, {"frame", SharedRuntime().Frame()}});
    return true;
}

// A teleport arrival: the floor's opening script stops once it has built the floor, the screen stays black while the
// player is put where they arrive (on the first frame of play, after the script's camera is let go), then comes up as the
// escape item's light (dun/effect/escape.chr) plays around them; monsters and the player wait until it has played.
int g_teleport_in = -1; // -1 none, -2 to be placed, 0 loading the light, then the frames it has played
int g_teleport_x = -1, g_teleport_y = -1, g_teleport_wait = 0;
u_char *g_teleport_file = nullptr;

void StartTeleportLight() {
    int size = 0;
    BtCashBuffer.base = reinterpret_cast<u_char *>(read_buffer); // as the escape item lays out its read
    BtCashBuffer.limit = 0x445C0;
    BtCashBuffer.used = 0;
    StartReadBG();
    g_teleport_file = BtCashBuffer.base;
    LoadFileBG(const_cast<char *>("dun/effect/escape.chr"), reinterpret_cast<u_long128 *>(g_teleport_file), &size);
    BtCashBuffer.Alloc((((size >> 6) + 1) << 6) >> 4);
    EdFadeInit();
    EdFadeIn(-1, 0.0f, 0.0f, 0.0f); // black until the light is loaded
    UserStatus->step_disable = 1;
    CMonUnitHold = 1;
    g_teleport_in = 0;
}

bool ChronicleArrivalTeleport() {
    if (!g_arrival.has_value() || !g_arrival->built || !g_arrival->teleport || UserStatus == nullptr || CharaMain.frame == nullptr) return false;
    if (NowDngMap != &MainDungeonMap || !StandableCell(g_arrival->x, g_arrival->y)) return false;
    g_teleport_x = g_arrival->x;
    g_teleport_y = g_arrival->y;
    g_arrival.reset();
    EdFadeInit();
    EdFadeIn(-1, 0.0f, 0.0f, 0.0f);
    g_teleport_in = -2;
    g_teleport_wait = 0;
    return true;
}

void TeleportInTick() {
    if (g_teleport_in == -1) return;
    if (mode != GAME_MODE_DUNGEON || UserStatus == nullptr || CharaMain.frame == nullptr) {
        g_teleport_in = -1;
        EscapeFlag = 0;
        return;
    }
    if (g_teleport_in == -2) {
        // as an arrival without the light: once play has run a few frames, so the camera has settled behind the player
        if (gameTask != GAME_TASK_PLAY || NowDngMap != &MainDungeonMap) return;
        UserStatus->step_disable = 1;
        CMonUnitHold = 1;
        if (++g_teleport_wait < 10) return;
        PlaceOnCell(g_teleport_x, g_teleport_y);
        StartTeleportLight();
        return;
    }
    if (g_teleport_in == 0) {
        if (ReadBGSync() != 0) return;
        sceVu0FVECTOR position;
        sceVu0FVECTOR rotation;
        EscapeEffect.LoadPackData2(reinterpret_cast<u_int *>(g_teleport_file), "info.cfg", &BtCashBuffer, 0x1C, &BtCashBuffer, 0);
        EscapeEffect.SetMotion(0, 6);
        EscapeEffect.motion_type.state.time = 10.0f;
        sceVu0CopyVector(position, CharaMain.pos);
        CharaMain.GetRotation(rotation);
        EscapeEffect.SetPosition(position);
        EscapeEffect.SetRotation(rotation);
        EscapeFlag = 1;
        EdFadeIn(40, 0.0f, 0.0f, 0.0f);
        g_teleport_in = 1;
        return;
    }
    if (++g_teleport_in < 70) return;
    EscapeFlag = 0;
    UserStatus->step_disable = 0;
    CMonUnitHold = 0;
    g_teleport_in = -1;
}

void ChronicleZoneGatesTick() {
    static int told = -1, current = -1, current_seed = -1, at_shrine = -1;
    TeleportInTick();
    // dungeons.enter_floor: once the floor is built and play has begun on it, the player goes to the cell asked for.
    if (g_arrival.has_value()) {
        const bool here = mode == GAME_MODE_DUNGEON && NowDngMap == &MainDungeonMap && UserStatus != nullptr && CharaMain.frame != nullptr &&
                          gameTask == GAME_TASK_PLAY && BtGameModeFlag == BT_GAME_MODE_PLAY;
        if (!g_arrival->built) {
            if (++g_arrival->ticks > 3600) g_arrival.reset(); // the jump never came
        } else if (here && ++g_arrival->ticks >= 10) {
            if (StandableCell(g_arrival->x, g_arrival->y)) PlaceOnCell(g_arrival->x, g_arrival->y);
            g_arrival.reset();
        }
    }
    // Until the player is put where they arrive, they aren't in a zone yet: the zone they land in is their arrival, not a
    // walk from wherever the floor first stood them (or from the zone they were in before the same floor was built again).
    if (!ZonedFloorShown() || UserStatus == nullptr || CharaMain.frame == nullptr || (g_arrival.has_value() && g_arrival->built)) {
        current = -1;
        at_shrine = -1;
        return;
    }
    // Test hook: DC_ZONE_VISIT=frames puts the player in the corridor outside the first gate that many frames into the
    // floor (600 when it names none).
    static int visit_clock = 0, visit_seed = -1;
    if (const char *visit = std::getenv("DC_ZONE_VISIT"); visit != nullptr && *visit != '\0' && DngZoneGateCount > 0 &&
                                                          visit_seed != NowDngMap->map_seed &&
                                                          ++visit_clock > (std::atoi(visit) > 1 ? std::atoi(visit) : 600)) {
        visit_seed = NowDngMap->map_seed;
        visit_clock = 0;
        const DNG_ZONE_GATE &gate = DngZoneGateTable[0];
        const int dx = gate.gate_part == MAP_PARTS_ROOM_GATE_EAST ? 1 : (gate.gate_part == MAP_PARTS_ROOM_GATE_WEST ? -1 : 0);
        const int dy = gate.gate_part == MAP_PARTS_ROOM_GATE_SOUTH ? 1 : (gate.gate_part == MAP_PARTS_ROOM_GATE_NORTH ? -1 : 0);
        float outside[4];
        sceVu0CopyVector(outside, CharaMain.frame->position);
        outside[0] = 160.0f * static_cast<float>(gate.x) + 100.0f * static_cast<float>(dx); // just inside the gate's reach
        outside[2] = 160.0f * static_cast<float>(gate.y) + 100.0f * static_cast<float>(dy);
        CharaMain.SetPosition(outside);
        CharaMain.frame->SetPosition(outside);
        if (NowCamera__3 != nullptr) {
            NowCamera__3->SetFollow(outside[0], outside[1] + 13.0f, outside[2]);
            NowCamera__3->Step(-1);
        }
        for (int row = 0; row < DNG_GRID; ++row) {
            for (int col = 0; col < DNG_GRID; ++col) {
                NowDngMap->cells[col + row * DNG_GRID].camera_dist = std::hypot(160.0f * col - outside[0], 160.0f * row - outside[2]);
            }
        }
        const float facing = std::atan2(static_cast<float>(-dx), static_cast<float>(-dy)); // toward the gate
        CharaMain.SetRotation(0.0f, facing, 0.0f);
        CharaMain.frame->SetRotation(0.0f, facing, 0.0f);
        if (NowCamera__3 != nullptr) { NowCamera__3->SetAngleSoon(facing - PI); NowCamera__3->Step(-1); }
    }
    float at[4];
    CharaMain.GetPosition(at);
    if (current_seed != NowDngMap->map_seed) {
        current_seed = NowDngMap->map_seed;
        current = -1;
    }
    // Shrines: standing in front of a zone's spring is game.dungeon.shrine_reached, once until the player walks away.
    int shrine = -1;
    for (int z = 0; z < DngZoneFloorCount; ++z) {
        if (DngZoneShrineX[z] >= 0 && std::hypot(at[0] - 160.0f * DngZoneShrineX[z], at[2] - 160.0f * (DngZoneShrineY[z] + 1)) < 90.0f) shrine = z;
    }
    if (shrine != at_shrine && !g_shrine_quiet && shrine >= 0) {
        SharedRuntime().QueueEvent("game.dungeon.shrine_reached", {{"zone", shrine}, {"x", DngZoneShrineX[shrine]}, {"y", DngZoneShrineY[shrine]},
                                                                   {"frame", SharedRuntime().Frame()}});
    }
    if (!(g_shrine_quiet && TeleportPinned())) g_shrine_quiet = false;
    at_shrine = shrine;
    const int zone = ZoneAt(at);
    if (zone >= 0 && zone != current) {
        const int zone_floor = DngZoneFloorSource[zone] >= 0 ? DngZoneFloorSource[zone] : UserStatus->cur_floor;
        auto     *dng = static_cast<CDngStatusData *>(UserStatus);
        if (dng->cur_georama >= 0 && zone_floor > dng->floor_reached[dng->cur_georama]) dng->floor_reached[dng->cur_georama] = static_cast<s8>(zone_floor);
        SharedRuntime().QueueEvent("game.dungeon.zone_entered", {{"zone", zone}, {"previous", current}, {"zones", DngZoneFloorCount},
                                                                 {"floor_id", zone_floor}, {"frame", SharedRuntime().Frame()}});
        current = zone;
    }
    int near = -1;
    for (int g = 0; g < DngZoneGateCount; ++g) {
        const DNG_ZONE_GATE &gate = DngZoneGateTable[g];
        // a smoke gate's wall stops the player about 131 units out along some approaches, so it reaches a little further
        const float reach = gate.key_item < 0 ? 150.0f : 130.0f;
        if (gate.locked && std::hypot(at[0] - 160.0f * gate.x, at[2] - 160.0f * gate.y) < reach) near = g;
    }
    if (near < 0) {
        told = -1;
        return;
    }
    DNG_ZONE_GATE &gate = DngZoneGateTable[near];
    auto *status = static_cast<CDngStatusData *>(UserStatus);
    if (gate.key_item < 0) {
        // a smoke gate: the mod is told once until the player walks away
        if (told != near) {
            told = near;
            SharedRuntime().QueueEvent("game.dungeon.smoke_touched", {{"zone", gate.zone}, {"x", gate.x}, {"y", gate.y},
                                                                      {"frame", SharedRuntime().Frame()}});
        }
    } else if (GetNowItemNum(static_cast<short>(gate.key_item), &status->item_pack) > 0) {
        DeleteItemAfterUseItem(static_cast<short>(gate.key_item), &status->item_pack);
        gate.locked = 0;
        MAP_CELL &cell = NowDngMap->cells[gate.x + gate.y * DNG_GRID];
        cell.parts_no = gate.open_part;
        cell.direction = gate.open_direction;
        SndSePlay(SE_ITEM_GET, -1, 0);
        SharedRuntime().QueueEvent("game.dungeon.zone_gate_opened", {{"zone", gate.zone}, {"key", ItemStableId(gate.key_item)}, {"x", gate.x},
                                                                     {"y", gate.y}, {"frame", SharedRuntime().Frame()}});
        told = -1;
    } else if (told != near) {
        told = near;
        SharedRuntime().QueueEvent("game.dungeon.zone_gate_locked", {{"zone", gate.zone}, {"key", ItemStableId(gate.key_item)}, {"x", gate.x},
                                                                     {"y", gate.y}, {"frame", SharedRuntime().Frame()}});
    }
}

// A registered weapon (weapons.register) a mod hasn't styled: its own name and description, not its template's.
const ModWeapon *RegisteredWeapon(int item_no) {
    const auto id = g_mod_weapon_by_native_id.find(item_no);
    if (id == g_mod_weapon_by_native_id.end()) return nullptr;
    const auto found = g_mod_weapons.find(id->second);
    return found != g_mod_weapons.end() ? &found->second : nullptr;
}

bool ChronicleWeaponStyle(int chara, int slot, int item_no, const char **name, unsigned char rgb[3], unsigned char glow[3],
                          bool *has_glow) {
    const auto found = g_weapon_styles.find(chara * 16 + slot);
    if (found == g_weapon_styles.end() || found->second.item_no != item_no) {
        const ModWeapon *weapon = RegisteredWeapon(item_no);
        if (weapon == nullptr) return false;
        if (name != nullptr) *name = weapon->name.c_str();
        if (rgb != nullptr) std::fill(rgb, rgb + 3, static_cast<unsigned char>(235));
        if (has_glow != nullptr) *has_glow = false;
        return true;
    }
    if (name != nullptr) *name = found->second.name.empty() ? nullptr : found->second.name.c_str();
    if (rgb != nullptr) std::copy(found->second.rgb.begin(), found->second.rgb.end(), rgb);
    if (glow != nullptr) std::copy(found->second.glow.begin(), found->second.glow.end(), glow);
    if (has_glow != nullptr) *has_glow = found->second.has_glow;
    return true;
}

bool ChronicleWeaponCardLine(int chara, int slot, int item_no, int part, int n, const char **text, unsigned char rgb[3],
                             float *scale) {
    const auto found = g_weapon_styles.find(chara * 16 + slot);
    if (found == g_weapon_styles.end() || found->second.item_no != item_no) {
        const ModWeapon *weapon = RegisteredWeapon(item_no);
        if (weapon == nullptr || part != 1 || n < 0 || n >= static_cast<int>(weapon->description.size())) return false;
        *text = weapon->description[n].c_str();
        std::fill(rgb, rgb + 3, static_cast<unsigned char>(215));
        *scale = 0.85f;
        return true;
    }
    const auto &lines = part == 0 ? found->second.badge : found->second.details;
    if (n < 0 || n >= static_cast<int>(lines.size())) return false;
    *text = lines[n].text.c_str();
    std::copy(lines[n].rgb.begin(), lines[n].rgb.end(), rgb);
    *scale = lines[n].scale;
    return true;
}

void ChronicleMonsterTintTick() {
    CMonstorUnit *unit = NowMonstorUnit;
    if (mode != GAME_MODE_DUNGEON || unit == nullptr) return;
    for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) {
        MonsterTint &tint = g_monster_tints[slot];
        if (tint.frames <= 0) continue;
        if (tint.generation != g_monster_handle_generations[slot] || unit->monster[slot].state == -1) {
            tint.frames = 0;
            continue;
        }
        MONSTOR &monster = unit->monster[slot];
        // the ambient the monster draws with runs 0..128
        monster.palette_override[0] = tint.rgb[0] / 2.0f;
        monster.palette_override[1] = tint.rgb[1] / 2.0f;
        monster.palette_override[2] = tint.rgb[2] / 2.0f;
        monster.palette_override_pending = 1;
        --tint.frames;
    }
}

// The Warden room: sealed until no monster is left outside it (a hidden mimic doesn't count), then open; with
// lock_in the doors close again behind the player who walks in, until the mod releases them.
void ChronicleWardenRoomTick() {
    if (mode != GAME_MODE_DUNGEON || NowDngMap != &MainDungeonMap || BtUraDongeon != 0 || NowMonstorUnit == nullptr ||
        CharaMain.frame == nullptr || g_warden_room_owner.empty() || DngWardenRoomSeed != MainDungeonMap.map_seed ||
        gameTask != GAME_TASK_PLAY) {
        return;
    }
    // Test hook: DC_WARDEN_CHASE=<range> keeps the player within <range> units of the shut room's boss (arena slot 0),
    // so a scripted fight stays in reach of it.
    if (const char *chase = std::getenv("DC_WARDEN_CHASE"); chase != nullptr && *chase != '\0' && DngWardenRoomState == DNG_WARDEN_SHUT) {
        const float range = std::max(5.0f, static_cast<float>(std::atof(chase)));
        for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) {
            if (!g_arena_slots[slot].active || g_arena_slots[slot].cast_no != 0 || NowMonstorUnit->monster[slot].state == -1) continue;
            float boss[4]{}, at[4];
            NowMonstorUnit->chara[slot][0].GetPosition(boss);
            CharaMain.GetPosition(at);
            const float dx = at[0] - boss[0], dz = at[2] - boss[2], d = std::hypot(dx, dz);
            if (d > range * 1.5f) {
                at[0] = boss[0] + (d > 0.01f ? dx / d : 1.0f) * range;
                at[2] = boss[2] + (d > 0.01f ? dz / d : 0.0f) * range;
                CharaMain.SetPosition(at);
                CharaMain.frame->SetPosition(at);
            }
            break;
        }
    }
    const Json center = {{"x", 160.0f * (DngWardenRoomX + 0.5f * (DngWardenRoomW - 1))}, {"y", 0.0f},
                         {"z", 160.0f * (DngWardenRoomY + 0.5f * (DngWardenRoomH - 1))}};
    if (DngWardenRoomState == DNG_WARDEN_SEALED) {
        int left = 0;
        for (int i = 0; i < MONSTOR_SLOT_MAX; ++i) {
            const MONSTOR &m = NowMonstorUnit->monster[i];
            if (m.state != -1 && m.revealed != 0) ++left;
        }
        static int seen_seed = -1, clear_frames = 0;
        if (seen_seed != MainDungeonMap.map_seed) {
            seen_seed = MainDungeonMap.map_seed;
            clear_frames = 0;
        }
        // The floor's monsters are placed a moment after it loads: wait for a clear count to hold.
        clear_frames = left == 0 ? clear_frames + 1 : 0;
        // Test hook: DC_WARDEN_VISIT opens the room ten seconds into the floor, cleared or not.
        static int visit_clock = 0;
        if (const char *visit = std::getenv("DC_WARDEN_VISIT"); visit != nullptr && visit[0] != 0 && ++visit_clock > 600) {
            clear_frames = 30;
            visit_clock = 0;
        }
        if (clear_frames < 30) return;
        DngWardenRoomState = DNG_WARDEN_OPEN;
        SetWardenDoors(false);
        SndSePlay(SE_ITEM_GET, -1, 0);
        SharedRuntime().PublishToMod(g_warden_room_owner, "game.dungeon.warden_room",
                                     {{"state", "open"}, {"center", center}, {"frame", SharedRuntime().Frame()}});
        return;
    }
    if (DngWardenRoomState != DNG_WARDEN_OPEN) return;
    // Test hook: DC_WARDEN_VISIT puts the player in the middle of the room once it opens.
    if (const char *visit = std::getenv("DC_WARDEN_VISIT"); visit != nullptr && *visit != '\0') {
        float inside[4];
        sceVu0CopyVector(inside, CharaMain.frame->position);
        inside[0] = center["x"].get<float>() - 60.0f;
        inside[2] = center["z"].get<float>() - 60.0f;
        CharaMain.SetPosition(inside);
        CharaMain.frame->SetPosition(inside);
        if (NowCamera__3 != nullptr) {
            NowCamera__3->SetFollow(inside[0], inside[1] + 13.0f, inside[2]);
            NowCamera__3->Step(-1);
        }
        for (int row = 0; row < DNG_GRID; ++row) {
            for (int col = 0; col < DNG_GRID; ++col) {
                MainDungeonMap.cells[col + row * DNG_GRID].camera_dist = std::hypot(160.0f * col - inside[0], 160.0f * row - inside[2]);
            }
        }
    }
    // Inside: past the room's edge cells, so no doorway is under the player when the gates close.
    float at[4];
    CharaMain.GetPosition(at);
    const float col = at[0] / 160.0f, row = at[2] / 160.0f;
    const float margin = (DngWardenRoomW > 2 && DngWardenRoomH > 2) ? 0.6f : 0.2f;
    if (col < DngWardenRoomX - 0.5f + margin || col > DngWardenRoomX + DngWardenRoomW - 0.5f - margin ||
        row < DngWardenRoomY - 0.5f + margin || row > DngWardenRoomY + DngWardenRoomH - 0.5f - margin) {
        return;
    }
    if (DngWardenRoomLockIn) {
        DngWardenRoomState = DNG_WARDEN_SHUT;
        SetWardenDoors(true);
        SndSePlay(SE_ITEM_GET, -1, 0);
    } else {
        DngWardenRoomState = DNG_WARDEN_RELEASED;
    }
    SharedRuntime().PublishToMod(g_warden_room_owner, "game.dungeon.warden_room",
                                 {{"state", "entered"}, {"center", center},
                                  {"size", {{"x", 160.0f * DngWardenRoomW}, {"z", 160.0f * DngWardenRoomH}}},
                                  {"shut", DngWardenRoomLockIn != 0}, {"frame", SharedRuntime().Frame()}});
}

int ChronicleCompanionBaseAttack(int character) {
    if (UserStatus == nullptr || character < 0 || character >= CharaCount()) return 1;
    const int slot = UserStatus->equipped_weapon_slot[character];
    if (slot < 0 || slot >= 11) return 1;
    const WEAPON_DATA *weapon = GetWeaponData(UserStatus->chara_weapons[character][slot].item_no);
    return weapon != nullptr ? std::max(1, static_cast<int>(weapon->attack)) : 1;
}

void ChronicleMapChoice(const std::string &owner, const std::string &id, int x, int y, int mark) {
    SharedRuntime().PublishToMod(owner, "ui.map_choice", {{"id", id}, {"x", x}, {"y", y}, {"mark", mark}, {"frame", SharedRuntime().Frame()}});
}

void ChronicleMapWaypoint(const std::string &owner, const std::string &id, int x, int y, bool set) {
    SharedRuntime().PublishToMod(owner, "ui.map_waypoint", {{"id", id}, {"x", x}, {"y", y}, {"set", set}, {"frame", SharedRuntime().Frame()}});
}

void ChronicleMapPage(const std::string &owner, const std::string &id, int way) {
    SharedRuntime().PublishToMod(owner, "ui.map_page", {{"id", id}, {"way", way}, {"frame", SharedRuntime().Frame()}});
}

void ChronicleMapClosed(const std::string &owner, const std::string &id) {
    SharedRuntime().PublishToMod(owner, "ui.map_closed", {{"id", id}, {"frame", SharedRuntime().Frame()}});
}

void ChronicleChoiceMade(const std::string &owner, const std::string &id, int index) {
    SharedRuntime().PublishToMod(owner, "ui.choice_made",
                                 {{"id", id}, {"index", index >= 0 ? Json(index + 1) : Json(nullptr)}, {"cancelled", index < 0},
                                  {"frame", SharedRuntime().Frame()}});
}

// Whether the player stands within two cells of the floor's back floor entrance.
bool NearBackroomEntrance() {
    if (NowDngMap == nullptr) return false;
    float at[4];
    CharaMain.GetPosition(at);
    for (int row = 0; row < DNG_GRID; ++row) {
        for (int col = 0; col < DNG_GRID; ++col) {
            const int part = NowDngMap->cells[col + row * DNG_GRID].parts_no;
            if (part >= MAP_PARTS_URA_ENTRANCE_NORTH && part <= MAP_PARTS_URA_ENTRANCE_WEST &&
                std::hypot(at[0] - 160.0f * static_cast<float>(col), at[2] - 160.0f * static_cast<float>(row)) < 320.0f) {
                return true;
            }
        }
    }
    return false;
}

bool ChronicleBackroomEntranceAsk() {
    if (g_backroom_owner.empty() || BtUraDongeon != 0 || mode != GAME_MODE_DUNGEON || dc::game::MoonModChoiceUp() ||
        !NearBackroomEntrance()) {
        return false;
    }
    dc::game::MoonModChoice choice;
    choice.owner = g_backroom_owner;
    choice.id = "backroom";
    choice.title = g_backroom_title;
    choice.subtitle = g_backroom_text;
    choice.cancel = true;
    choice.options.push_back({"Proceed", g_backroom_stay ? "Take this way" : "Go down now", BackroomLines()});
    choice.options.push_back({"Not now", "Stay on this floor", {{"The way down stays open.", 3}}});
    const std::string owner = g_backroom_owner;
    choice.done = [owner](int index) {
        g_backroom_descend = index == 0 && !g_backroom_stay;
        SharedRuntime().PublishToMod(owner, "game.dungeon.backroom", {{"entered", index == 0}, {"frame", SharedRuntime().Frame()}});
    };
    return dc::game::MoonModChoiceOpen(std::move(choice));
}

// A fixed floor's gate (its gatekey marker), pressed at: the story-exit owner's question instead of the lock.
bool ChronicleFloorSelectOff() { return !g_floor_select_owner.empty(); }

bool ChronicleQuickPassage() { return !g_quick_passage_owner.empty(); }

namespace {
bool          g_walk_arrival = false;
std::uint64_t g_walk_arrived_at = 0; // the runtime frame of the last walk arrival, or 0
}
void ChronicleMarkWalkArrival(bool walked) { g_walk_arrival = walked; }
bool ChronicleTakeWalkArrival() {
    const bool walked = g_walk_arrival;
    g_walk_arrival = false;
    if (walked) g_walk_arrived_at = SharedRuntime().Frame() + 1;
    return walked;
}

// Walked into a dungeon through its town's door (quick passage): the floor's opening script still builds the floor, but
// its walk down the entrance stairs plays out unseen, so the party stands inside as it came through the door.
namespace {
bool g_dungeon_walk_in = false;
bool g_dungeon_walk_in_built = false;
bool g_dungeon_walk_in_done = false;
bool g_dungeon_prebuilding = false;
}
void ChronicleDungeonPrebuild(bool on) { g_dungeon_prebuilding = on; }
bool ChronicleDungeonPrebuilding() { return g_dungeon_prebuilding; }
bool ChronicleDungeonWalkInDone() { return g_dungeon_walk_in_done; }
void ChronicleDungeonWalkInBegin(bool walked) {
    g_dungeon_walk_in = walked;
    g_dungeon_walk_in_built = false;
    g_dungeon_walk_in_done = false;
}
void ChronicleDungeonWalkInBuilt() {
    if (g_dungeon_walk_in) g_dungeon_walk_in_built = true;
}
void ChronicleDungeonResumed() {
    g_dungeon_walk_in = g_dungeon_walk_in_built = false;
    ChronicleWalkPlaceReady();
    if (!SharedRuntime().Running() || !UserStatus) return;
    SharedRuntime().Publish("game.dungeon.resumed", {{"dungeon_id", selectMapNo}, {"floor_id", UserStatus->cur_floor}});
}
bool ChronicleDungeonWalkInSkip() {
    if (!g_dungeon_walk_in || !g_dungeon_walk_in_built) return false;
    g_dungeon_walk_in = g_dungeon_walk_in_built = false;
    g_dungeon_walk_in_done = true;
    return true;
}
namespace {
int g_hold_frames = -1; // frames the picture has been held, or -1
int g_hold_release = -1; // frames left once the place walked into is ready, or -1
bool g_road_jump = false; // the jump under way is a town's road to another town, or a floor to another of its dungeon
bool g_floor_jump = false; // the latter: let go once the floor is played on (a story floor's fixed map says no more)
}
void ChronicleMapJumped(int map_no) {
    g_road_jump = g_floor_jump = false;
    if (!ChronicleQuickPassage()) return;
    const bool into_dungeon = mode == GAME_MODE_EDIT && map_no >= 200 && map_no < 200 + DUNGEON_COUNT;
    // a town, not a field a story floor's scene goes out to (Sun and Moon Temple's to the desert, s32): nothing there
    // would let the held picture go before its ten seconds were up
    const bool into_town = mode == GAME_MODE_DUNGEON && map_no >= 0 && map_no < TOWN_COUNT;
    const bool by_road = mode == GAME_MODE_EDIT && MapNo >= 0 && MapNo < TOWN_COUNT && map_no >= 0 && map_no < TOWN_COUNT &&
                         map_no != MapNo;
    // a floor of the same dungeon (dungeons.enter_floor: a story floor through its smoke, a shrine's travel)
    const bool between_floors = mode == GAME_MODE_DUNGEON && map_no == MapNo && map_no >= 200 && map_no < 200 + DUNGEON_COUNT;
    if (!into_dungeon && !into_town && !by_road && !between_floors) return;
    g_hold_frames = 0;
    g_hold_release = -1;
    g_road_jump = by_road || between_floors;
    g_floor_jump = between_floors;
    if (!by_road && !between_floors) dc::game::DoorwayJumped(MapNo, map_no);
}
bool ChronicleTakeRoadJump() {
    // read by both the loader's and the dungeon's mode start; the jump's end (StepHeldPicture) clears it
    return g_road_jump;
}
bool ChronicleTownRoads() { return !g_roads_owner.empty(); }
void ChronicleTownRoad(int town_map) {
    if (!SharedRuntime().Running() || town_map < 0 || town_map >= TOWN_COUNT) return;
    std::string location;
    for (int place = 0; place < WORLD_PLACE_COUNT; ++place) {
        if (MenuGrobalMapNoTbl[place] == town_map && std::string_view(kNativeLocations[place].kind) == "settlement") {
            location = kNativeLocations[place].id;
        }
    }
    SharedRuntime().Publish("game.town.road", {{"town_id", town_map}, {"location_id", location}, {"frame", SharedRuntime().Frame()}});
}
void ChronicleWalkPlaceReady() {
    if (g_hold_frames >= 0 && g_hold_release < 0) g_hold_release = 4;
}
bool ChronicleHoldingPicture() { return g_hold_frames >= 0; }
int  ChronicleHeldReleaseIn() { return g_hold_frames >= 0 ? g_hold_release : -1; }
bool ChronicleQuickPassageOn() { return ChronicleQuickPassage(); }
int  ChronicleConnectedWorld(int map_no) {
    if (!ChronicleQuickPassage() || map_no < 0) return 0;
    return dc::game::IsDungeonMap(map_no) ? map_no : 100 + map_no;
}
void StepHeldPicture() {
    if (g_hold_frames < 0) return;
    // a floor of the same dungeon is ready once the party plays on it (a fixed story map sends no generated)
    if (g_floor_jump && g_hold_release < 0 && mode == GAME_MODE_DUNGEON && gameTask == GAME_TASK_PLAY && NowDngMap != nullptr) {
        g_hold_release = 4;
    }
    if (++g_hold_frames > 600 || (g_hold_release >= 0 && --g_hold_release <= 0)) {
        g_hold_frames = -1;
        g_hold_release = -1;
        g_road_jump = g_floor_jump = false;
    }
}

bool ChronicleWalkArrivalFade() {
    if (g_walk_arrived_at == 0) return false;
    const bool soon = SharedRuntime().Frame() + 1 - g_walk_arrived_at < 600;
    g_walk_arrived_at = 0;
    return soon;
}

void ChronicleEventEnded(int program) {
    if (g_play_event_program == -1 || program != g_play_event_program) return;
    const std::string owner = g_play_event_owner;
    g_play_event_owner.clear();
    g_play_event_program = -1;
    if (owner.empty()) return;
    SharedRuntime().PublishToMod(owner, "game.dungeon.event_ended",
                                 {{"program", program}, {"dungeon_id", selectMapNo},
                                  {"floor_id", UserStatus != nullptr ? UserStatus->cur_floor : -1}, {"frame", SharedRuntime().Frame()}});
}

bool ChronicleStoryExitAsk(const char *marker) {
    if (g_story_exit_owner.empty() || marker == nullptr || std::strncmp(marker, "gatekey", 7) != 0 || BtUraDongeon != 0 ||
        mode != GAME_MODE_DUNGEON || NowDngMap != &MainDungeonMap || NowDngMap->map_type == 1 || UserStatus == nullptr ||
        dc::game::MoonModChoiceUp()) {
        return false;
    }
    dc::game::MoonModChoice choice;
    choice.owner = g_story_exit_owner;
    choice.id = "story_exit";
    choice.title = g_story_exit_title;
    choice.subtitle = g_story_exit_text;
    choice.cancel = true;
    choice.options.push_back({"Leave", "Go on", {}});
    choice.options.push_back({"Stay", "Stay on this floor", {}});
    choice.cursor = 1;
    const std::string owner = g_story_exit_owner;
    const int dungeon = selectMapNo, floor = UserStatus->cur_floor;
    // The gate stays shut either way: leaving is the owner's to carry out (dungeons.enter_floor on the event).
    choice.done = [owner, dungeon, floor](int index) {
        SharedRuntime().PublishToMod(owner, "game.dungeon.story_exit",
                                     {{"left", index == 0}, {"dungeon_id", dungeon}, {"floor_id", floor}, {"frame", SharedRuntime().Frame()}});
    };
    return dc::game::MoonModChoiceOpen(std::move(choice));
}

bool ChronicleBackroomAsk() {
    if (g_backroom_owner.empty() || BtUraDongeon != 0 || NowDngMap == nullptr || BtEventInfo.item_select_result == 0 ||
        PortItemSelectResult == nullptr) {
        return false;
    }
    const int key = NowDngMap->gate_key_no;
    bool      listed = false;
    for (int i = 0; i < 16 && BtEventInfo.item_select_list[i] != -1; ++i) listed = listed || BtEventInfo.item_select_list[i] == key;
    if (key <= 0 || !listed) return false;
    // Only the back floor's stairs: the player stands within two cells of them.
    float at[4];
    CharaMain.GetPosition(at);
    bool near = false;
    for (int row = 0; row < DNG_GRID && !near; ++row) {
        for (int col = 0; col < DNG_GRID && !near; ++col) {
            const int part = NowDngMap->cells[col + row * DNG_GRID].parts_no;
            if (part >= MAP_PARTS_URA_ENTRANCE_NORTH && part <= MAP_PARTS_URA_ENTRANCE_WEST) {
                near = std::hypot(at[0] - 160.0f * static_cast<float>(col), at[2] - 160.0f * static_cast<float>(row)) < 320.0f;
            }
        }
    }
    if (!near) return false;
    dc::game::MoonModChoice choice;
    choice.owner = g_backroom_owner;
    choice.id = "backroom";
    choice.title = g_backroom_title;
    choice.subtitle = g_backroom_text;
    choice.cancel = true;
    choice.options.push_back({"Enter", g_backroom_stay ? "Take this way" : "Go down", BackroomLines()});
    choice.options.push_back({"Not now", "Stay on this floor", {{"The stairs stay open.", 3}}});
    choice.resume_task = GAME_TASK_SCRIPT_RUN;
    const std::string owner = g_backroom_owner;
    const bool stay = g_backroom_stay;
    choice.done = [key, owner, stay](int index) {
        CDngStatusData *status = CurrentStatus();
        if (index == 0 && status != nullptr && !stay) {
            // The stairs take the floor's gate key; it comes back once the party is below.
            g_backroom_key = key;
            g_backroom_key_count = status->CheckItemGet(key);
            g_backroom_key_frames = 1800;
            if (g_backroom_key_count <= 0) status->GetItem(key, 0);
        }
        PortItemSelectResult->i = index == 0 && !stay ? key : -1;
        BtEventInfo.item_select_result = 0;
        SharedRuntime().PublishToMod(owner, "game.dungeon.backroom", {{"entered", index == 0}, {"frame", SharedRuntime().Frame()}});
    };
    return dc::game::MoonModChoiceOpen(std::move(choice));
}

// The gate key a free back floor's stairs took, given back once the party is down there.
void RestoreBackroomKey() {
    if (g_backroom_key <= 0) return;
    CDngStatusData *status = CurrentStatus();
    if (status == nullptr || mode != GAME_MODE_DUNGEON || --g_backroom_key_frames <= 0) {
        g_backroom_key = 0;
        return;
    }
    if (BtUraDongeon == 0) return;
    while (status->CheckItemGet(g_backroom_key) < std::max(1, g_backroom_key_count)) {
        if (status->GetItem(g_backroom_key, 0) < 0) break;
    }
    g_backroom_key = 0;
}

void ChronicleNpcNear(const std::string &owner, const std::string &id) {
    SharedRuntime().PublishToMod(owner, "game.dungeon_npc.near", {{"id", id}, {"frame", SharedRuntime().Frame()}});
}

void ChronicleMenuAction(const std::string &owner, const std::string &tab, const std::string &row) {
    SharedRuntime().PublishToMod(owner, "ui.run_menu_action", {{"tab", tab}, {"row", row}, {"frame", SharedRuntime().Frame()}});
}

void ChronicleMenuShown(const std::string &owner, bool open, const std::string &tab) {
    SharedRuntime().PublishToMod(owner, "ui.run_menu_shown", {{"open", open}, {"tab", tab}, {"frame", SharedRuntime().Frame()}});
}

void ChronicleCompanionStrike(int slot, int amount, int character) {
    CMonstorUnit *unit = NowMonstorUnit;
    if (unit == nullptr || slot < 0 || slot >= MONSTOR_SLOT_MAX || amount <= 0) return;
    MONSTOR &monster = unit->monster[slot];
    if (monster.state != 2 || monster.hp <= 0 || monster.revealed == 0 || monster.invincible_timer > 0) return;
    PrepareMonsterHandleUnit(unit);
    const std::uint32_t generation = EnsureMonsterHandleGeneration(unit, slot);
    // A helper's blow, not a second hero's: 60% of its weapon's attack, less the monster's defence as a hit takes it.
    const int blow = std::max(1, static_cast<int>(std::lround(amount * 0.6)) - std::max<int>(0, monster.defense));
    const int dealt = std::min(monster.hp, blow);
    monster.hp -= dealt;
    monster.last_attacker = -1;
    monster.last_hit_damage = dealt;
    monster.last_hit_id = -1;
    monster.hit_element = WEAPON_ELEMENT_NONE;
    const bool killed = monster.hp <= 0;
    QueueChronicleMonsterDamageResult(unit, slot, static_cast<int>(killed ? MonsterDamageResult::Killed : MonsterDamageResult::Hit));
    SharedRuntime().QueueEvent("game.companion.hit", {{"entity", MonsterHandleId(slot, generation)}, {"character", kCharacterIds[character]},
                                                      {"damage", dealt}, {"killed", killed}, {"reward_exp", monster.exp},
                                                      {"frame", SharedRuntime().Frame()}});
}
void ChronicleCompanionStruck(int character, int damage, int hp, int max_hp, bool killed) {
    if (character < 0 || character >= static_cast<int>(kCharacterIds.size())) return;
    SharedRuntime().QueueEvent("game.companion.struck", {{"character", kCharacterIds[character]}, {"damage", damage}, {"hp", hp},
                                                         {"max_hp", max_hp}, {"killed", killed}, {"frame", SharedRuntime().Frame()}});
}
void ResetChronicleChargedForTests() { g_charge_clock = 0; g_charged_until = -1; g_goro_held = false; g_goro_last_step = 0; }
float ChroniclePlayerStatusDuration() { return g_player_tuning.status_duration; }
int ChroniclePlayerStatusImmune() { return g_player_tuning.status_immune; }
float ChroniclePlayerSwapDelay() { return g_player_tuning.swap_delay; }
float ChroniclePlayerDodgePower() { return g_player_tuning.dodge_power; }
int ChroniclePlayerDodgeInvincibleFrames() { return g_player_tuning.dodge_invincible_frames; }
int ChroniclePlayerDodgeGauge() { return g_player_tuning.dodge_gauge; }
bool ChroniclePlayerDodgeHop() { return g_player_tuning.dodge_hop; }
float ChroniclePlayerDodgeRun() { return g_player_tuning.dodge_run; }
bool ChroniclePlayerLightness() { return g_player_tuning.lightness; }
float ChroniclePlayerStamina(int chara) {
    return chara >= 0 && chara < static_cast<int>(g_player_tuning.stamina.size()) ? g_player_tuning.stamina[chara] : 0.0f;
}
float ChroniclePlayerSwingCostScale() {
    const float gear = UserStatus != nullptr ? GearSwingScale(UserStatus->cur_chara) : 1.0f;
    const float rules = g_gauge_rules.on ? g_gauge_rules.swing_cost_scale : 1.0f; // gauge_rules_set
    return rules * gear * (g_player_tuning.lightness && NowWeaponHave != nullptr ? LightnessSwingScale(NowWeaponHave->speed) : 1.0f);
}

ChroniclePlayerHitResult ChroniclePlayerHitResolve(const ChroniclePlayerHit &hit) {
    const GuardOutcome guard = ResolveGuard(g_guard_rules, {.guarding = hit.guarding,
                                                            .held = hit.held,
                                                            .heavy = hit.kind == 3,
                                                            .damage = hit.damage,
                                                            .angle = hit.angle,
                                                            .gauge = hit.gauge,
                                                            .guard_frames = g_guard_frames});
    ChroniclePlayerHitResult result{.blocked = guard.blocked,
                                    .parried = guard.parried,
                                    .broke = guard.broke,
                                    .damage = guard.damage,
                                    .gauge = GaugeAfterHit(g_gauge_rules, hit.gauge, guard, g_guard_rules.on)};
    // A light or heavy blow that lands (not a trap, not a broken guard, not the last of the player's life) is the
    // worn gear's poise's to take.
    if (!result.blocked && !result.broke && (hit.kind == 2 || hit.kind == 3) && UserStatus != nullptr &&
        UserStatus->cur_chara >= 0 && UserStatus->cur_chara < CharaCount() &&
        result.damage < UserStatus->hp[static_cast<int>(UserStatus->cur_chara)]) {
        result.poised = PoiseTakes(g_poise, GearWornEffects(UserStatus->cur_chara).poise, hit.kind == 3, g_poise_clock);
    }
    static constexpr std::array<const char *, 5> kKinds{"other", "other", "light", "heavy", "trap"};
    Json event = {{"kind", hit.kind >= 0 && hit.kind < 5 ? kKinds[hit.kind] : "other"},
                  {"damage", result.damage},
                  {"incoming", hit.damage},
                  {"blocked", result.blocked},
                  {"parried", result.parried},
                  {"broke", result.broke},
                  {"poised", result.poised},
                  {"angle", std::lround(hit.angle)},
                  {"gauge", result.gauge}};
    if (UserStatus != nullptr && UserStatus->cur_chara >= 0 && UserStatus->cur_chara < CharaCount()) {
        event["character"] = CharaIdName(UserStatus->cur_chara);
    }
    if (hit.monster >= 0 && hit.monster < static_cast<int>(g_monster_handle_generations.size())) {
        event["entity"] = MonsterHandleId(hit.monster, g_monster_handle_generations[hit.monster]);
    }
    SharedRuntime().QueueEvent("game.player.hit", event);
    return result;
}

// combat.feel_set: hit-stop (frames the floor holds still), camera shake (its strength) and rumble when blows land.
// One mod's feel at a time; feel_clear or unloading the mod takes it off.
struct HitFeel {
    int  stop_light = 0, stop_heavy = 0, stop_critical = 0, stop_hurt = 0;
    int  shake_light = 0, shake_heavy = 0, shake_hurt = 0;
    bool rumble = false;
};
HitFeel     g_feel;
std::string g_feel_owner;
int         g_hit_stop = 0, g_feel_frame = -1;

void Shake(int strength, int frames) {
    if (strength <= 0) return;
    const float s = static_cast<float>(strength) * 0.6f;
    if (BtActStatus.camera_shake_frames > 0 && BtActStatus.camera_shake >= s) return;
    BtActStatus.camera_shake = s;
    BtActStatus.camera_shake_frames = frames;
    BtActStatus.camera_shake_decay = s / static_cast<float>(frames);
}

void Feel(int stop, int shake, int shake_frames, int rumble, int rumble_frames) {
    g_hit_stop = std::max(g_hit_stop, stop);
    Shake(shake, shake_frames);
    if (g_feel.rumble && rumble > 0) GamePad.SetVibration(1, rumble, rumble_frames);
}

void ChronicleFeelPlayerHit(bool heavy) {
    if (g_feel_owner.empty()) return;
    // A swing striking several monsters in one frame freezes once.
    const int frame = static_cast<int>(SharedRuntime().Frame());
    if (frame == g_feel_frame) return;
    g_feel_frame = frame;
    if (heavy || ChronicleHitIsCharged()) Feel(g_feel.stop_heavy, g_feel.shake_heavy, 14, 0xC8, 10);
    else Feel(g_feel.stop_light, g_feel.shake_light, 8, 0x8C, 5);
}
void ChronicleFeelPlayerHurt(bool heavy) {
    if (g_feel_owner.empty()) return;
    Feel(heavy ? g_feel.stop_hurt : g_feel.stop_hurt / 2, g_feel.shake_hurt, 16, 0, 0);
}
void ChronicleFeelCritical() {
    if (g_feel_owner.empty()) return;
    Feel(g_feel.stop_critical, g_feel.shake_heavy, 18, 0xFF, 14);
}
bool ChronicleHitStopHold() {
    if (g_hit_stop <= 0) return false;
    --g_hit_stop;
    // The pad is read but the floor isn't stepped: keep an R2 pressed now for after the freeze. (A roll needs L2's
    // tap to end, which the step after the freeze sees.)
    if (GamePad.Down(PAD_R2) != 0) g_r2_press.left = std::max(g_r2_press.left, g_player_tuning.input_buffer + 1);
    return true;
}

ApiCallResult SetHitFeel(std::string_view owner, const Json &args) {
    const char *usage = "expected {hit_stop = optional {light, heavy, critical, hurt} 0..12, shake = optional {light, heavy, hurt} 0..10, "
                        "rumble = optional boolean}";
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return Invalid(usage);
    const Json &request = args[0];
    HitFeel feel;
    for (auto field = request.begin(); field != request.end(); ++field) {
        if (field.key() != "hit_stop" && field.key() != "shake" && field.key() != "rumble") {
            return Invalid("combat.feel_set received an unsupported field: " + field.key());
        }
    }
    if (request.contains("hit_stop")) {
        const Json &stop = request["hit_stop"];
        if (!stop.is_object()) return Invalid(usage);
        for (auto field = stop.begin(); field != stop.end(); ++field) {
            int *to = field.key() == "light" ? &feel.stop_light : field.key() == "heavy" ? &feel.stop_heavy
                    : field.key() == "critical" ? &feel.stop_critical : field.key() == "hurt" ? &feel.stop_hurt : nullptr;
            if (to == nullptr || !ReadBoundedInt(stop, field.key().c_str(), 0, 12, *to)) return Invalid(usage);
        }
    }
    if (request.contains("shake")) {
        const Json &shake = request["shake"];
        if (!shake.is_object()) return Invalid(usage);
        for (auto field = shake.begin(); field != shake.end(); ++field) {
            int *to = field.key() == "light" ? &feel.shake_light : field.key() == "heavy" ? &feel.shake_heavy
                    : field.key() == "hurt" ? &feel.shake_hurt : nullptr;
            if (to == nullptr || !ReadBoundedInt(shake, field.key().c_str(), 0, 10, *to)) return Invalid(usage);
        }
    }
    if (request.contains("rumble")) {
        if (!request["rumble"].is_boolean()) return Invalid(usage);
        feel.rumble = request["rumble"].get<bool>();
    }
    if (!g_feel_owner.empty() && g_feel_owner != owner) return Failed("another mod's hit feel is set");
    g_feel = feel;
    g_feel_owner = std::string(owner);
    return {ApiCallStatus::Ok,
            {{"hit_stop", {{"light", feel.stop_light}, {"heavy", feel.stop_heavy}, {"critical", feel.stop_critical}, {"hurt", feel.stop_hurt}}},
             {"shake", {{"light", feel.shake_light}, {"heavy", feel.shake_heavy}, {"hurt", feel.shake_hurt}}},
             {"rumble", feel.rumble}},
            {}};
}
ApiCallResult ClearHitFeel(std::string_view owner) {
    if (g_feel_owner == owner) {
        g_feel_owner.clear();
        g_feel = {};
        g_hit_stop = 0;
    }
    return {ApiCallStatus::Ok, {{"cleared", true}}, {}};
}

void ChroniclePlayerHitRecoil(bool broke) {
    if (broke && g_guard_rules.on && g_guard_rules.break_recoil > 0) {
        BtActStatus.recoil_frames = g_guard_rules.break_recoil;
    } else if (g_player_tuning.recoil_scale != 1.0f && BtActStatus.recoil_frames > 0) {
        const int scaled = static_cast<int>(std::lround(static_cast<float>(BtActStatus.recoil_frames) * g_player_tuning.recoil_scale));
        BtActStatus.recoil_frames = std::max(scaled, 1);
    }
    if (g_player_tuning.invincible_bonus_frames > 0) {
        BtActStatus.invincible_frames = std::max(BtActStatus.invincible_frames,
                                                 BtActStatus.recoil_frames + g_player_tuning.invincible_bonus_frames);
    }
}

bool ChroniclePlayerGuardWithoutLock() { return g_guard_rules.on && g_guard_rules.without_lock; }

bool ChroniclePlayerGaugeTick() {
    g_guard_frames = BtActStatus.action_on == 6 ? g_guard_frames + 1 : 0;
    ++g_poise_clock;
    return GaugeDelayStep(g_gauge_delay, BtActStatus.action_gauge, g_gauge_rules.on ? g_gauge_rules.refill_delay : 0);
}

void ChroniclePlayerGaugeSeen() { GaugeDelaySeen(g_gauge_delay, BtActStatus.action_gauge); }

bool ChroniclePlayerSwingAllowed(bool idle) {
    return !g_gauge_rules.on || !idle || BtActStatus.action_gauge >= g_gauge_rules.min_to_swing;
}

void ChroniclePlayerSwingStarted(bool idle) {
    if (!g_gauge_rules.on || !idle || BtActStatus.action_on == 0 || g_gauge_rules.first_swing_cost <= 0.0f) return;
    BtActStatus.action_gauge = std::max(BtActStatus.action_gauge - g_gauge_rules.first_swing_cost, 0.0f);
}

void ResetChroniclePlayerRulesForTests() {
    g_guard_rules_owner.clear();
    g_guard_rules = {};
    g_gauge_rules_owner.clear();
    g_gauge_rules = {};
    g_gauge_delay = {};
    g_guard_frames = 0;
    g_poise = {};
    g_poise_clock = 0;
}
int ChroniclePlayerExtraShots() { return g_player_tuning.extra_shots; }
bool ChroniclePlayerShotsPierce() { return g_player_tuning.shots_pierce; }

bool ChronicleLockFlick() { return g_player_tuning.lock_flick; }
bool ChronicleLockSight() { return g_player_tuning.lock_sight; }
bool ChronicleLockCamera() { return g_player_tuning.lock_camera; }

// soft_lock: the nearest live enemy within 64 of the player and soft_lock degrees of `heading`, if any.
bool SoftLockHeading(float &heading) {
    CMonstorUnit *unit = NowMonstorUnit;
    if (g_player_tuning.soft_lock <= 0 || lockOnTargetFlag != 0 || mode != GAME_MODE_DUNGEON || unit == nullptr) return false;
    float at[4]{};
    CharaMain.GetPosition(at);
    const float arc = static_cast<float>(g_player_tuning.soft_lock) * 3.14159265f / 180.0f;
    float best = 64.0f;
    bool found = false;
    float turned = heading;
    for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) {
        const MONSTOR &m = unit->monster[slot];
        if (m.state != 2 || m.hp <= 0 || m.revealed == 0 || m.lockon_enabled == 0) continue;
        float there[4]{};
        unit->chara[slot][0].GetPosition(there);
        const float dx = there[0] - at[0], dz = there[2] - at[2];
        const float d = std::hypot(dx, dz);
        if (d >= best) continue;
        float off = std::atan2(dx, dz) - heading;
        while (off > 3.14159265f) off -= 6.2831853f;
        while (off < -3.14159265f) off += 6.2831853f;
        if (std::fabs(off) > arc) continue;
        best = d;
        found = true;
        turned = std::atan2(dx, dz);
    }
    heading = turned;
    return found;
}

void ChronicleSoftLockTurn() {
    if (CharaMain.frame == nullptr) return;
    float heading = BtActStatus.stick_strength >= 0.3f ? std::atan2(BtActStatus.move_x, BtActStatus.move_z) : PlayerHeading();
    if (SoftLockHeading(heading)) CharaMain.frame->SetRotation(0.0f, heading, 0.0f);
}

bool ChronicleHealItemLocked() {
    const int lock = g_player_tuning.item_heal_lock;
    CMonstorUnit *unit = NowMonstorUnit;
    if (lock <= 0 || g_mod_item_use || mode != GAME_MODE_DUNGEON || unit == nullptr) return false;
    float at[4]{};
    CharaMain.GetPosition(at);
    for (int slot = 0; slot < MONSTOR_SLOT_MAX; ++slot) {
        const MONSTOR &m = unit->monster[slot];
        if (m.state != 2 || m.hp <= 0 || m.revealed == 0) continue;
        float there[4]{};
        unit->chara[slot][0].GetPosition(there);
        if (std::hypot(at[0] - there[0], at[2] - there[2]) <= static_cast<float>(lock)) return true;
    }
    return false;
}

bool ChronicleMoveShotOrigin(float *world) {
    if (!g_shot_style.active || g_shot_style.hit == nullptr || g_shot_style.hit->from.empty() || CharaMain.frame == nullptr) return false;
    CFrame *frame = CharaMain.frame->SearchFrame(const_cast<char *>(g_shot_style.hit->from.c_str()));
    if (frame == nullptr) return false;
    float offset[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    frame->GetWorldPosition(world, offset);
    world[0] += std::sin(g_shot_style.heading) * g_shot_style.hit->from_ahead;
    world[1] += g_shot_style.hit->from_rise;
    world[2] += std::cos(g_shot_style.heading) * g_shot_style.hit->from_ahead;
    world[3] = 1.0f;
    return true;
}

void ChroniclePlayerShotSpawned(int slot) {
    if (slot < 0 || slot >= static_cast<int>(g_shot_looks.size())) return;
    ShotLook &look = g_shot_looks[static_cast<size_t>(slot)];
    look = {};
    if (!g_shot_style.active || g_shot_style.hit == nullptr) return;
    look.cell = g_shot_style.hit->cell;
    look.size = g_shot_style.hit->size;
    look.burst = g_shot_style.hit->burst;
    look.burst_scale = g_shot_style.hit->burst_scale;
}

bool ChroniclePlayerShotLook(int slot, int &cell, float &size) {
    if (slot < 0 || slot >= static_cast<int>(g_shot_looks.size())) return false;
    const ShotLook &look = g_shot_looks[static_cast<size_t>(slot)];
    if (look.cell < 0) return false;
    cell = look.cell;
    size = look.size;
    return true;
}

void ChroniclePlayerShotLanded(int slot, const float *where) {
    if (slot < 0 || slot >= static_cast<int>(g_shot_looks.size())) return;
    ShotLook &look = g_shot_looks[static_cast<size_t>(slot)];
    if (!look.burst.empty() && mode == GAME_MODE_DUNGEON) {
        BurstEffect(Json::array({{{"kind", look.burst}, {"position", {{"x", where[0]}, {"y", where[1]}, {"z", where[2]}}}, {"scale", look.burst_scale}}}));
    }
    look = {};
}
bool ChroniclePlayerFirstPersonAllowed() { return !g_player_tuning.no_first_person && !PlayerHasR2Moves(); }
float ChroniclePlayerReachScale() { return g_player_tuning.reach_scale; }

bool ConsumeChronicleFatalGuard(int character) {
    if (character < 0 || character >= CharaCount() || g_fatal_guard[character] <= 0) return false;
    --g_fatal_guard[character];
    if (BtActStatus.invincible_frames < 90) BtActStatus.invincible_frames = 90;
    SharedRuntime().QueueEvent("game.player.guarded", {{"character", kCharacterIds[character]}, {"charges", g_fatal_guard[character]}});
    return true;
}
void ResetChronicleFatalGuardForTests() { g_fatal_guard.fill(0); }
void ResetChronicleFloorRedirectsForTests() { g_floor_redirects.clear(); }


void ConfigureChronicleApi(RuntimeConfig &config) {
    config.current_language = CurrentLanguageTag;
    SetModAtlaCollectedHandler([](const ModAtla &atla) {
        SharedRuntime().QueueEvent("game.georama.atla_collected", {{"id", atla.id}, {"owner", atla.owner}, {"name", atla.name}});
    });
    config.world_location_reader = ReadWorldLocationForEvents;
    for (const char *path : {"player.get", "player.set", "player.heal", "player.damage", "party.list", "party.active", "party.set_active", "party.join", "party.leave", "party.town_active", "party.town_set_active",
                             "save.slot_id",
                             "player.status_add", "player.status_remove", "player.thirst_get", "player.thirst_set",
                             "player.tuning_set", "player.tuning_clear", "player.invincible", "player.moves_set", "player.moves_clear", "player.form_set", "player.form_clear", "player.move_play", "player.critical_set", "player.critical_clear", "animations.player_register", "animations.player_unregister", "animations.player_list", "animations.monster_register", "animations.monster_unregister", "animations.monster_list", "player.fatal_guard", "player.guard_rules_set", "player.guard_rules_clear", "player.gauge_rules_set", "player.gauge_rules_clear", "player.gauge_get", "player.gauge_set", "player.state", "party.companion_set", "party.companion_get",
                             "characters.list", "characters.get", "world.get", "world.transition", "world.set_time",
                             "world.story_flag_get", "world.story_flag_set", "world.map_flag_get", "world.map_flag_set",
                             "world.story_value_get", "world.story_value_set",
                             "economy.get", "economy.add", "economy.spend", "georama.get", "georama.element_set",
                             "georama.register", "georama.place", "georama.remove",
                             "georama.atla_list", "georama.atla_collect", "georama.atla_spawn", "georama.satisfaction_get",
                             "locations.list", "locations.get", "locations.register",
                             "georama.request_set", "dungeons.list", "dungeons.get", "dungeons.register", "dungeons.set_layout", "dungeons.set_seed", "dungeons.set_opened_chests", "dungeons.chests_get", "dungeons.set_zones", "dungeons.zones_get", "dungeons.set_exits", "dungeons.warp", "dungeons.drop_gilda", "dungeons.enter_floor", "dungeons.set_arrival", "dungeons.set_exit_room", "dungeons.set_population", "dungeons.set_enemy_survival",
                             "dungeons.floor_rules_get", "dungeons.floor_rules_set", "dungeons.floor_redirect", "dungeons.set_shortcuts", "dungeons.shortcuts_get", "dungeons.set_weapon_drops", "dungeons.chest_weapons_add", "dungeons.chest_weapons_clear", "dungeons.set_backrooms", "dungeons.set_story_exits", "dungeons.set_floor_select", "dungeons.set_quick_passage", "world.set_roads", "dungeons.play_event", "dungeons.set_npc_marks", "dungeons.set_npc_models", "dungeons.set_warden_room", "dungeons.set_character_blocks", "weapons.style_set", "weapons.style_clear", "effects.burst", "entities.tint", "entities.knockback", "dungeons.warden_room_get", "dungeons.warden_room_release", "dungeons.room_seal", "dungeons.room_release", "dungeons.zone_gate_open", "dungeons.set_rest_room", "dungeons.rest_room_get", "ui.choice_open", "ui.choice_close", "ui.map_open", "ui.map_close", "ui.sky_open", "ui.sky_close", "dungeons.map_snapshot", "dungeons.set_seen", "dungeons.set_waypoint", "ui.boss_card", "world.dungeon_npc_set", "world.dungeon_npc_clear", "world.ghost_get", "world.ghost_play", "ui.run_menu_set", "ui.run_menu_clear", "ui.run_menu_open", "ui.run_menu_close", "dungeons.loot_get", "dungeons.loot_override",
                             "dungeons.back_floor_get", "dungeons.back_floor_enter",
                             "world.register_dungeon",
                             "weapons.list", "weapons.get", "weapons.give", "weapons.register", "weapons.equip", "weapons.remove", "weapons.upgrade",
                             "weapons.definition_set", "weapons.stat_label_set", "weapons.stats_set", "weapons.durability_set", "weapons.abs_get", "weapons.abs_add", "weapons.attachments_get", "weapons.attachment_insert",
                             "weapons.attachment_remove", "weapons.build_up_list", "weapons.build_up_register", "weapons.synthesize",
                             "shops.list", "shops.get", "shops.register_item", "shops.remove_item", "shops.set_items", "shops.set_price",
                             "combat.get", "combat.damage", "combat.status_add", "combat.strike_player", "combat.feel_set", "combat.feel_clear", "combat.register_formula",
                             "combat.heal", "entities.query", "models.resolve", "models.register", "models.unregister", "models.replace",
                             "models.remove", "animations.register", "animations.play", "animations.stop", "debug.inspect", "debug.overlay_register",
                             "entities.get", "entities.set_stats", "entities.scale_set", "entities.script_int_set", "entities.spawn", "entities.despawn", "entities.set_active_limit", "characters.spawn",
                             "physics.query", "physics.velocity_get", "physics.velocity_set",
                             "items.list", "items.get", "items.register", "items.give", "items.remove", "items.icon_set", "items.use", "items.quick_slots", "items.quick_slot_select", "items.quick_slot_use", "items.charm_slots", "items.charm_slots_set", "items.gear", "items.gear_give", "items.gear_stock_set", "items.gear_page_set", "items.gear_equip", "items.gear_unequip", "items.gear_remove",
                             "content.list", "content.register", "content.unregister"}) {
        config.api_capabilities.push_back({path, true, {}});
    }
    config.api_capabilities.push_back({"characters.register", true, {}});
    for (const char *path : {"messages.post", "messages.list", "messages.rate", "messages.remove"}) {
        config.api_capabilities.push_back({path, true, {}});
    }
    {
        const bool online = moonmod::net::Shared().available();
        for (const char *path : {"net.host", "net.join", "net.leave", "net.status", "net.send", "net.receive"}) {
            config.api_capabilities.push_back({path, online, online ? std::string() : "this build has no network socket (no libcurl was found)"});
        }
    }
    config.api_capabilities.push_back({"characters.register_playable", true, {}});
    config.api_capabilities.push_back({"render.camera_get", true, {}});
    config.api_capabilities.push_back({"render.camera_set", true, {}});
    config.api_capabilities.push_back({"dungeons.lighting_get", true, {}});
    config.api_capabilities.push_back({"monsters.natives", true, {}});
    config.api_capabilities.push_back({"events.natives", true, {}});
    config.api_capabilities.push_back({"text.get", true, {}});
    config.api_capabilities.push_back({"text.set", true, {}});
    config.api_capabilities.push_back({"text.clear", true, {}});
    config.api_capabilities.push_back({"events.call", true, {}});
    config.api_capabilities.push_back({"monsters.call", true, {}});
    config.api_capabilities.push_back({"dungeons.lighting_set", true, {}});
    config.api_capabilities.push_back({"render.town_camera_set", true, {}});
    config.api_capabilities.push_back({"render.first_person_toggle", true, {}});
    config.api_capabilities.push_back({"save.autosave_set", true, {}});
    config.api_capabilities.push_back({"render.layer_register", true, {}});
    config.api_capabilities.push_back({"render.layer_remove", true, {}});
    config.api_capabilities.push_back({"render.bar_register", true, {}});
    config.api_capabilities.push_back({"render.bar_remove", true, {}});
    config.api_capabilities.push_back({"render.target_bar_set", true, {}});
    config.api_capabilities.push_back({"render.target_meter_set", true, {}});
    config.api_capabilities.push_back({"render.hud_icon_set", true, {}});
    config.api_capabilities.push_back({"render.hud_icon_remove", true, {}});
    config.api_capabilities.push_back({"render.floor_label_set", true, {}});
    config.api_capabilities.push_back({"render.floor_label_clear", true, {}});
    for (const char *path : {"input.bind", "input.unbind", "input.state", "input.consume", "input.axis", "input.wheel_mode_set", "input.block_native",
                             "input.context_push", "input.context_pop", "input.context_get"}) {
        config.api_capabilities.push_back({path, true, {}});
    }
    config.api_capabilities.push_back({"georama.request_known_set", false,
                                       "Chronicle does not store Georama request knowledge separately from native plot progress."});
    const std::filesystem::path save_root = config.save_root;
    config.api_handler = [save_root](std::string_view mod_id, const ApiOperationSpec &operation, const Json &args) {
        return Call(mod_id, save_root, operation, args);
    };
    config.api_capabilities.push_back({"dungeons.selection", true, {}});
    config.api_capabilities.push_back({"dungeons.set_badges", true, {}});
    config.api_capabilities.push_back({"dungeons.set_modifiers", true, {}});
    config.api_capabilities.push_back({"combat.register_player_formula", true, {}});
    for (const char *path : {"title.mode_register", "title.mode_set", "title.mode_remove",
                             "session.get", "session.start", "session.finish", "save.world_export", "save.world_import",
                             "save.player_export", "save.player_import", "save.account_get", "save.account_set",
                             "save.account_list", "save.account_remove", "world.avatar_set", "world.avatar_clear",
                             "georama.town_export", "georama.town_import"}) {
        config.api_capabilities.push_back({path, true, {}});
    }
    config.api_resource_override_removal_guard = ChronicleModelOverrideRemovalGuard;
    SetRetailAudio({[](int se_no, int volume) { SndSePlay(se_no, volume, 0); },
                    [](int set_no, int track_no) {
                        SndBgmStop();
                        SndBgmLoad(set_no); // 0 also when the set is already loaded
                        SndBgmPlay(track_no);
                        return true;
                    },
                    [] { SndBgmStop(); }});
    for (const char *path : {"audio.se_play", "audio.bgm_play", "audio.bgm_stop"}) {
        config.api_capabilities.push_back({path, true, {}});
    }
    config.api_owner_unloaded = [](std::string_view mod_id) {
        if (moonmod::net::Shared().owner() == mod_id) moonmod::net::Shared().Leave("the mod left");
        const std::string prefix = std::string(mod_id) + ":";
        InputUnbindModActionsByPrefix(prefix);
        g_input_contexts.UnloadOwner(mod_id);
        std::erase_if(g_wheel_mode_requests, [mod_id](const auto &entry) { return entry.first == mod_id; });
        RefreshChronicleWheelMode();
        UnloadChronicleAnimations(mod_id);
        UnloadModText(mod_id);
        UnloadModNpcs(mod_id);
        g_combat_damage_formulas.RemoveOwner(mod_id);
        g_player_damage_formulas.RemoveOwner(mod_id);
        RestoreWeaponDefinitionsByOwner(mod_id);
        RestoreShopOverridesByOwner(mod_id);
        g_floor_badges.erase(std::string(mod_id));
        std::erase_if(g_layout_requests, [mod_id](const auto &entry) { return entry.first == mod_id; });
        RestoreLayoutRequest();
        g_floor_modifiers.erase(std::string(mod_id));
        RefreshFloorModifiers();
        std::erase_if(g_population_requests, [mod_id](const auto &entry) { return entry.first == mod_id; });
        RestorePopulationRequest();
        std::erase_if(g_enemy_survival_requests, [mod_id](const auto &entry) { return entry.first == mod_id; });
        RestoreEnemySurvivalRequest();
        std::erase_if(g_town_camera_requests, [mod_id](const auto &entry) { return entry.first == mod_id; });
        ClearChroniclePlayerTuning(mod_id);
        if (g_guard_rules_owner == mod_id) { g_guard_rules_owner.clear(); g_guard_rules = {}; }
        if (g_gauge_rules_owner == mod_id) { g_gauge_rules_owner.clear(); g_gauge_rules = {}; }
        ReleaseCriticals(mod_id);
        std::erase_if(g_floor_redirects, [mod_id](const auto &entry) { return entry.second.first == mod_id; });
        g_shortcut_open.erase(std::string(mod_id));
        const bool had_walls = g_illusory_requests.erase(std::string(mod_id)) != 0;
        if (g_shortcut_requests.erase(std::string(mod_id)) != 0 || had_walls) RefreshShortcutRequests();
        dc::game::MoonModChoiceCloseOwner(mod_id);
        dc::game::MoonModMapCloseOwner(mod_id);
        dc::game::MoonModSkyCloseOwner(mod_id);
        dc::game::MoonModMenuClear(mod_id);
        dc::game::MoonModNpcClear(std::string(mod_id));
        ClearMoonModChestWeapons(mod_id);
        if (g_weapon_drops_owner == mod_id) {
            g_weapon_drops_owner.clear();
            DngWeaponUnique = DngWeaponRarePercent = 0;
            DngLargeChestPercent = -1;
        }
        if (g_backroom_owner == mod_id) g_backroom_owner.clear();
        if (g_story_exit_owner == mod_id) g_story_exit_owner.clear();
        if (g_floor_select_owner == mod_id) g_floor_select_owner.clear();
        if (g_quick_passage_owner == mod_id) {
            g_quick_passage_owner.clear();
            dc::game::DoorwayReset();
        }
        if (g_roads_owner == mod_id) g_roads_owner.clear();
        if (g_npc_models_owner == mod_id) {
            g_npc_models_owner.clear();
            DngNpcModelCount = 0;
        }
        if (g_npc_marks_owner == mod_id) {
            g_npc_marks_owner.clear();
            DngNpcMarkCount = 0;
        }
        if (g_play_event_owner == mod_id) {
            g_play_event_owner.clear();
            g_play_event_program = -1;
        }
        if (std::erase_if(g_weapon_styles, [mod_id](const auto &entry) { return entry.second.owner == mod_id; }) != 0) RefreshWeaponGlow();
        std::erase_if(g_player_moves, [mod_id](const PlayerMove &m) { return m.owner == mod_id; });
        // a form the mod left on ends: his own model comes back now if the game allows it, else at its next load
        {
            const dc::game::CharaForm *held = UserStatus != nullptr ? dc::game::ActiveCharaForm(UserStatus->cur_chara) : nullptr;
            const bool active = held != nullptr && held->owner == mod_id;
            dc::game::ClearCharaForm(-1, mod_id);
            if (active && FormReloadBlocked().empty()) g_character_switch_routine(UserStatus->cur_chara, 1);
        }
        std::erase_if(g_monster_motions, [mod_id](const MonsterMotion &m) { return m.motion.owner == mod_id; }); // loaded models keep theirs until they load again
        if (std::erase_if(g_player_motions, [mod_id](const PlayerMotion &m) { return m.owner == mod_id; }) != 0 && UserStatus != nullptr) {
            if (BtActStatus.action_no == kModMoveAction) EndPlayerMove();
            ApplyPlayerMotions(UserStatus->cur_chara);
        }
        if (g_chara_block_owner == mod_id) {
            g_chara_block_owner.clear();
            DngCharaBlockMask = -1;
        }
        if (g_rest_room_owner == mod_id) {
            g_rest_room_owner.clear();
            DngRestRoomEnabled = 0;
        }
        if (g_exits_owner == mod_id) g_exits_owner.clear();
        if (g_zones_owner == mod_id) {
            g_zones_owner.clear();
            DngZoneCount = 0;
            DngZoneOpenMask = 0;
            DngZoneShrines = 0;
            DngZoneExitFloor = -1;
            std::fill(std::begin(DngZoneSourceFloor), std::end(DngZoneSourceFloor), -1);
        }
        if (g_warden_room_owner == mod_id) {
            g_warden_room_owner.clear();
            DngWardenRoomEnabled = DngWardenRoomLockIn = 0;
            if (DngWardenRoomState == DNG_WARDEN_SEALED || DngWardenRoomState == DNG_WARDEN_SHUT) {
                DngWardenRoomState = DNG_WARDEN_RELEASED;
                SetWardenDoors(false);
            }
        }
        std::erase_if(g_builtin_icon_owners, [mod_id](const auto &entry) {
            if (entry.second != mod_id) return false;
            SetItemIconOverride(entry.first, "");
            return true;
        });
        if (g_companion_owner == mod_id) {
            dc::game::MoonModCompanionRequest(-1, -1, 1, 1.0f);
            g_companion_owner.clear();
        }
        ClearHitFeel(mod_id);
        RestoreChronicleModelReplacements(mod_id);
        RestoreMonsterScalesByOwner(mod_id);
        DespawnOwnedChronicleActors(mod_id);
        std::erase_if(g_debug_overlays, [mod_id](const auto &entry) { return entry.second.owner == mod_id; });
        std::erase_if(g_hud_icons, [mod_id](const auto &entry) { return entry.second.owner == mod_id; });
        if (g_floor_label_owner == mod_id) {
            g_floor_label_owner.clear();
            g_floor_label.clear();
            g_floor_number = 0;
        }
        RemoveTitleModesByOwner(mod_id);
    };
    ConfigureAudioApi(config);
    CompleteApiCapabilityLedger(config.game_id, config.api_capabilities);
    config.require_complete_api_capabilities = true;
}

void ResetChronicleItemsForTests() {
    for (const auto &[item_no, original] : g_original_mod_item_slots) {
        if (COM_ITEM_INFO *common = GetCommonItemInfo(item_no); common != nullptr) *common = original.first;
        if (ITEM_DATA *item = GetItemData(item_no); item != nullptr) *item = original.second;
    }
    g_original_mod_item_slots.clear();
    g_mod_items.clear();
    g_mod_item_by_native_id.clear();
    for (const auto &[shop_no, backup] : g_original_shop_lists) {
        if (s16 *list = GetItemShopList(shop_no); list != nullptr) {
            for (int index = 0; index < 20; ++index) list[index] = backup.goods[index];
        }
    }
    g_original_shop_lists.clear();
    g_shop_list_owners.clear();
    g_shop_additions.clear();
    for (const auto &[price_idx, entry] : g_original_shop_prices) {
        if (price_idx >= 0 && price_idx < static_cast<int>(std::size(PriceList))) PriceList[price_idx] = entry;
    }
    g_original_shop_prices.clear();
    g_shop_price_owners.clear();
}

void ResetChronicleWeaponsForTests() {
    RestoreWeaponDefinitionsByOwner();
    for (const auto &[item_no, masks] : g_original_weapon_buildup_masks) {
        if (WEAPON_DATA *weapon = GetWeaponData(item_no); weapon != nullptr) {
            weapon->buildup_mask0 = masks.first;
            weapon->buildup_mask1 = masks.second;
        }
    }
    g_original_weapon_buildup_masks.clear();
    g_mod_weapon_buildups.clear();
    ClearMoonModBuildUps();
    ClearMoonModChestWeapons("");
    g_mod_weapons.clear();
    g_mod_weapon_by_native_id.clear();
    ResetMoonModWeaponSlotsForTests();
}

void ChronicleModVillagerTalked(int index) {
    const auto npc = std::ranges::find_if(g_mod_npcs, [index](const auto &entry) { return entry.second.index == index; });
    if (npc == g_mod_npcs.end()) return;
    SharedRuntime().QueueEvent("game.npc.talk", {{"id", npc->second.id}, {"name", npc->second.name},
                                                 {"town_id", npc->second.town_id}, {"source_mod", npc->second.owner},
                                                 {"frame", SharedRuntime().Frame()}});
}

void ResetChronicleCharactersForTests() {
    UnloadModNpcs({});
    ResetChronicleModVillagersForTests();
    RestoreChronicleModelReplacements();
    g_mod_animations.clear();
    g_mod_animation_plays.clear();
    g_mod_characters.clear();
    g_mod_models.clear();
    ClearPlayableCharas();
    g_playable_owners.clear();
    ResetMoonModMonstorVariants();
    g_monster_handle_unit = nullptr;
    g_loaded_monster_model_ids.fill(-1);
    for (int slot = 0; slot < static_cast<int>(g_monster_handle_generations.size()); ++slot) {
        AdvanceMonsterHandleGeneration(slot);
    }
}

void SetChronicleCharacterSwitchRoutineForTests(void (*routine)(int, int)) {
    g_character_switch_routine = routine == nullptr ? ::selectChrUnit : routine;
}

void SetChronicleGeoramaRoutinesForTests(
    int (*place)(dc::game::CEditGround *, int, float, float, float, int),
    int (*remove)(dc::game::CEditGround *, int *, int *, float, float, float)) {
    g_georama_place_routine = place == nullptr ? PlaceChronicleGeoramaPart : place;
    g_georama_remove_routine = remove == nullptr ? RemoveChronicleGeoramaPart : remove;
}

void ResetChronicleGeoramaForTests() {
    g_mod_georama_pieces.clear();
    g_mod_georama_piece_by_native_id.clear();
    ResetChronicleModGeoramaPiecesForTests();
    g_georama_place_routine = PlaceChronicleGeoramaPart;
    g_georama_remove_routine = RemoveChronicleGeoramaPart;
}

void ResetChronicleDungeonsForTests() {
    for (const auto &[monster_id, backup] : g_original_monster_drops) {
        if (MONSTOR_MODEL *model = GetMonstorTable(monster_id); model != nullptr) {
            model->rare_item = backup.rare_item;
            model->steal_item = backup.steal_item;
            model->attachment_kind = backup.attachment_kind;
        }
    }
    g_original_monster_drops.clear();
    g_pending_dungeon_floor_entry.reset();
    g_active_dungeon_floor_entry.reset();
    g_mod_dungeons.clear();
    for (int dungeon = 0; dungeon < DUNGEON_COUNT; ++dungeon) {
        if (g_native_enemy_layouts[dungeon] != nullptr) {
            BtEnemyLayoutList[dungeon] = g_native_enemy_layouts[dungeon];
            BtUraEnemyLayoutList[dungeon] = g_native_ura_enemy_layouts[dungeon];
        }
        g_modded_enemy_layouts[dungeon].clear();
        g_modded_ura_enemy_layouts[dungeon].clear();
        maxFloorTbl__3[dungeon] = kNativeDungeonFloorCounts[dungeon];
    }
}

void ResetChronicleLocationsForTests() {
    g_mod_locations.clear();
}

} // namespace moonmod

// Floor pack BtMapJumpLoad should read for a mod-registered floor, or nullptr for the native one.
const char *dc::game::ChronicleModFloorMap(int dungeon, int floor) {
    for (const auto &[id, mod_dungeon] : moonmod::g_mod_dungeons) {
        (void)id;
        if (mod_dungeon.native_dungeon_id != dungeon) continue;
        for (const auto &mod_floor : mod_dungeon.floors) {
            if (mod_floor.native_floor_id == floor && !mod_floor.map.empty()) return mod_floor.map.c_str();
        }
    }
    return nullptr;
}

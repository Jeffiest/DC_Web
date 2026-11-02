#pragma once

#include <string>
#include <vector>
#include <string_view>

#include "moonmod/runtime.hpp"

namespace dc::game {
class CMonstorUnit;
class CEditGround;
class CCharacter;
struct RS_STACKDATA;
} // namespace dc::game

namespace moonmod {

using dc::game::CCharacter;
using dc::game::CEditGround;
using dc::game::CMonstorUnit;
using dc::game::RS_STACKDATA;

void ConfigureChronicleApi(RuntimeConfig &config);
std::string ChronicleDebugOverlayText();
std::vector<std::string> ChronicleOverlayLines();

/**
 * Whether a dungeon is drawing the mod HUD lines and notices in its own HUD pass (with the game's lettering), so the
 * overlay presenter leaves them out.
 */
bool ChronicleDungeonDrawsModText();
// Mod HUD bars (render.bar_register) in id order, drawn under the water gauge.
struct ChronicleHudBar {
    double current = 0.0;
    double maximum = 1.0;
    int    number = -1; // trailing number of the label ("LV5" -> 5), drawn left of the bar
};
std::vector<ChronicleHudBar> ChronicleHudBars();

/**
 * A mod's HUD icon (render.hud_icon_set): a native status badge (0 freeze, 1 stamina, 2 poison, 3 curse, 4 goo) or a
 * png behind a native resource override, drawn beside the key and floor plate; blinking ones pulse.
 */
struct ChronicleHudIcon {
    int         native = -1;
    std::string image;
    bool        blink = false;
};
std::vector<ChronicleHudIcon> ChronicleHudIcons();

/**
 * The text a mod put on the floor plate in place of its number (render.floor_label_set), or empty.
 */
std::string ChronicleFloorLabel();

/**
 * The number a mod gave the floor card ("B3") with its label, or 0 for the native one.
 */
int ChronicleFloorNumber();
// Pad buttons a mod has hidden from the game (input.block_native), as PAD_* bits.
int ChronicleBlockedNativeButtons();
bool ChronicleLoadMonsterModelForMod(dc::game::CMonstorUnit *unit, int native_model_no, int *out_slot);
void ChronicleMonsterModelLoaded(dc::game::CMonstorUnit *unit, int slot, int native_model_no);
void ChronicleMonsterActorCreated(dc::game::CMonstorUnit *unit, int slot);
void ChronicleMonsterRun(dc::game::CMonstorUnit *unit, int slot, int program);
bool ChronicleMonsterRunAgain(dc::game::CMonstorUnit *unit, int slot);
void ChronicleMonsterActorOwned(dc::game::CMonstorUnit *unit, int slot, std::string_view owner,
                                std::string_view character_id, int native_model_no);
bool ChronicleMonsterActorIsModOwned(dc::game::CMonstorUnit *unit, int slot);
void ChronicleMonstersCleared(dc::game::CMonstorUnit *unit);
void QueueChronicleMonsterDamageResult(dc::game::CMonstorUnit *unit, int slot, int result);
int TakeChronicleMonsterDamageResult(dc::game::CMonstorUnit *unit, int slot);
float ApplyChronicleCombatDamageFormula(float damage, int weapon_id = -1, int native_model_id = -1,
                                        int character_id = -1, int target_hp = -1, int target_max_hp = -1);
void ResetChronicleCombatFormulasForTests();
// Player tuning (player.tuning_set): every loaded owner's request combined, read by the dungeon loop.
// Multipliers multiply across owners, invincible frames add (cap 120), the reveal flags OR.
struct ChroniclePlayerTuning {
    float run_speed = 1.0f;             // multiplies the dungeon walk and dash speed
    float attack_anim_speed = 1.0f;     // multiplies the recovery tail of Toan's sword swings
    int   invincible_bonus_frames = 0;  // invincibility kept after a hit, past the recoil frames
    float recoil_scale = 1.0f;          // scales the recoil (stun) frames a hit sets
    float gauge_refill = 1.0f;          // scales the action gauge refill per frame
    float thirst_drain = 1.0f;          // scales the water drain per frame
    float durability_wear = 1.0f;       // scales weapon wear per hit
    bool  map_reveal = false;           // the floor map acts as if the Map item were held
    bool  monster_reveal = false;       // monsters show on the map as if the Magical Crystal were held
    bool  map_hidden = false;           // no floor map at all (wins over map_reveal)
    float status_duration = 1.0f;       // scales the frames Freeze and Stamina last
    int   status_immune = 0;            // AILMENT_* bits the active player never takes
    float swap_delay = 1.0f;            // scales the battle menu's character-change waits (0 = none)
    float dodge_power = 0.0f;           // L2 dodges in the facing direction this hard (0 = no dodge)
    int   dodge_invincible_frames = 0;  // invincibility a dodge gives
    bool  dodge_hop = false;            // the dodge leaves the ground (Xiao's tumble)
    int   extra_shots = 0;              // Xiao's shots and Ruby's orbs fire this many more, fanned out
    bool  shots_pierce = false;         // Ruby's orbs fly on through monsters
    float reach_scale = 1.0f;           // Ungaga's spear hits reach this much further
    bool  no_first_person = false;      // R2 (and a mod's toggle) no longer switch to the first-person view in dungeons
};
const ChroniclePlayerTuning &ChroniclePlayerTuningGet();
void ClearChroniclePlayerTuning(std::string_view owner);
void ResetChroniclePlayerTuningForTests();
// Plain getters for the PS2 sources, which forward-declare them under PORT.
float ChroniclePlayerGaugeRefillScale();
float ChroniclePlayerThirstDrainScale();
float ChroniclePlayerDurabilityWearScale();
float ChroniclePlayerAttackAnimSpeed();
bool ChroniclePlayerMapReveal();
bool ChroniclePlayerMonsterReveal();
bool ChroniclePlayerMapHidden();
// Charged hits: the dungeon loop reports each frame; a hit within a second of a charged move counts.
void ChronicleChargedActionTick();
// Shortcut doors: each frame, a locked door the player stands at opens with its key (dungeons.set_shortcuts).
void ChronicleShortcutDoorsTick();
void ChronicleZoneGatesTick();
void ChronicleWardenRoomTick();
bool ChronicleHitIsCharged();
void ResetChronicleChargedForTests();
float ChroniclePlayerStatusDuration();
int ChroniclePlayerStatusImmune();
float ChroniclePlayerSwapDelay();
float ChroniclePlayerDodgePower();
int ChroniclePlayerDodgeInvincibleFrames();
bool ChroniclePlayerDodgeHop();
int ChroniclePlayerExtraShots();
bool ChroniclePlayerShotsPierce();
bool ChroniclePlayerFirstPersonAllowed();
float ChroniclePlayerReachScale();
// player.fatal_guard: charges that turn a fatal hit on a character into 1 HP.
bool ConsumeChronicleFatalGuard(int character);
void ResetChronicleFatalGuardForTests();
void ResetChronicleFloorRedirectsForTests();
void ChronicleMonsterDefeated(dc::game::CMonstorUnit *unit, int slot, bool scripted);
void ChronicleNativeItemPickedUp(int item_no, int count, int character = -1, int slot = -1);
void ResetChronicleItemsForTests();
void ResetChronicleWeaponsForTests();
void ResetChronicleCharactersForTests();
void ResetChronicleDungeonsForTests();
void ResetChronicleLocationsForTests();
void ResetChronicleSignatureForTests();
void ChronicleTick();
void ApplyChronicleTownCamera();
void LoadChronicleTownCharacter();
void ApplyChronicleTownPartySwitch();
int ChronicleTownMotionRemap(CCharacter *character, int motion_no);
int RefreshChronicleWheelMode();
void SetChronicleCharacterSwitchRoutineForTests(void (*routine)(int, int));
void SetChronicleGeoramaRoutinesForTests(
    int (*place)(dc::game::CEditGround *, int, float, float, float, int),
    int (*remove)(dc::game::CEditGround *, int *, int *, float, float, float));
void ResetChronicleGeoramaForTests();
// Sandbox sessions (title modes): the game loop starts a queued session from the title, redirects its
// exits back to the title (true when it ended the session, so the caller resets the save), notes a game
// over, and ends it in place when something else takes over the game (a save loaded, a developer jump).
bool StartPendingChronicleSandbox();
bool RedirectChronicleSandboxMapJump();
void NoteChronicleSandboxDefeat();
void EndChronicleSandboxInPlace(std::string_view reason);
bool PreparePendingChronicleDungeonEntry();
int TakePendingChronicleDungeonFloor(int dungeon_id);
bool IsSeamlessDungeonEnabled();
void SetSeamlessDungeonEnabled(bool enabled);
void TriggerExitDoorPhysicalOpen();
bool IsExitDoorPhysicallyOpened();
void ResetSeamlessDungeonForTests();

struct SeamlessAntechamberInfo {
    bool active = false;
    int exit_cell_x = -1;
    int exit_cell_z = -1;
    float next_floor_portal_min[3] = {};
    float next_floor_portal_max[3] = {};
    float village_portal_min[3] = {};
    float village_portal_max[3] = {};
};

const SeamlessAntechamberInfo &GetSeamlessAntechamberInfo();
void SetupSeamlessAntechamber(int exit_x, int exit_z);
int CheckSeamlessPortalCrossing(const float *pos);

// Walk-through exit room. BeginExitRoom takes over a pending continue/leave
// answer (returns false when the floor has no exit room); StepExitRoom answers it
// when the player walks out through one of the room's doors.
constexpr int kExitRoomAnswerNextFloor = 1;
constexpr int kExitRoomAnswerTown = 2;
bool BeginExitRoom(RS_STACKDATA *escape_result);
void StepExitRoom();
bool ExitRoomPending();
void TeleportPlayer(float x, float z);
bool TeleportPinned();
void ResetExitRoomForTests();

} // namespace moonmod



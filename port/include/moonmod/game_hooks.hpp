#pragma once

/**
 * @file
 * The MoonFactory hooks the game's units call (port/src/game), defined by the adapter and its helpers in
 * port/src/moonmod. They live in the global moonmod namespace with the rest of MoonFactory.
 */

namespace dc::game {
class CCharacter;
class CDungeonMap;
class CMonstorUnit;
} // namespace dc::game

namespace moonmod {

using dc::game::CCharacter;
using dc::game::CDungeonMap;
using dc::game::CMonstorUnit;

// The player (player.tuning_set).
float ChroniclePlayerThirstDrainScale();
float ChroniclePlayerGaugeRefillScale();
float ChroniclePlayerDurabilityWearScale();
bool  ChroniclePlayerMonsterReveal();
bool  ChroniclePlayerMapReveal();
bool  ChroniclePlayerMapHidden();
bool  ChroniclePlayerMapWindow();
void  ChronicleChargedActionTick();
void  ChronicleShortcutDoorsTick();
void  ChronicleZoneGatesTick();
// While a floor's opening script runs: true once it has built the floor and a mod's arrival (dungeons.enter_floor /
// set_arrival with teleport) is to skip the rest of it (the stairs walk-in); the player is already where they arrive.
bool  ChronicleArrivalTeleport();
void  ChronicleWardenRoomTick();
// A weapon a mod styled (weapons.style_set): its name and colour for the menus, and its swing trail's glow.
// False when the weapon in that slot has no style (or a different weapon took the slot).
bool  ChronicleWeaponStyle(int chara, int slot, int item_no, const char **name, unsigned char rgb[3], unsigned char glow[3],
                           bool *has_glow);
// The card a mod gave a styled weapon (weapons.style_set badge/details): line n of the badge (part 0) or of the
// details (part 1), with its colour and size. False past the last line, or when the weapon has no card.
bool  ChronicleWeaponCardLine(int chara, int slot, int item_no, int part, int n, const char **text, unsigned char rgb[3],
                              float *scale);
// Mod moves (player.moves_set): the attack button starting one from a run, Toan's charge let go into one, and the
// move playing in the native action's place. Each returns true when a mod move took over.
// The active character's model was (re)loaded in a dungeon (LoadChara2): mod motions are appended to it.
void  ChroniclePlayerModelLoaded(int chara);
// A chest is about to load the character's chest-opening motions as a second set (numbered right after the native
// ones, where the mod motions sit): the mod motions come off until the chest is done.
void  ChroniclePlayerMotionsSuspend();
bool  ChroniclePlayerMoveOnAttack();
// L2 pressed this frame: a mod dodge move (trigger dodge or dodge_back) starts in place of the tuning dodge.
bool  ChroniclePlayerMoveOnDodge(bool pressed);
// R2 pressed this frame: a combo_finisher (after enough combo blows) or the heavy attack, if a mod set one.
bool  ChroniclePlayerMoveOnR2(bool pressed);
void  ChroniclePlayerSwingBlow(); // a native swing laid its hit (evades can cancel the recovery after it)
bool  ChroniclePlayerMoveOnChargeRelease(float held_seconds);
bool  ChroniclePlayerMovePlay();
// Enemy tints (entities.tint) held on for their frames; called once a dungeon frame.
void  ChronicleMonsterTintTick();
float ChroniclePlayerStatusDuration();
int   ChroniclePlayerStatusImmune();
float ChroniclePlayerSwapDelay();
float ChroniclePlayerDodgePower();
int   ChroniclePlayerDodgeInvincibleFrames();
bool  ChroniclePlayerDodgeHop();
int   ChroniclePlayerExtraShots();
bool  ChroniclePlayerShotsPierce();
// A mod move's shots (player.moves_set, a hit with shot = true): where they leave from (false: the weapon's tip, as
// before), the look each slot of the player's shots takes as it is spawned, and a burst where one lands.
bool  ChronicleMoveShotOrigin(float *world);
void  ChroniclePlayerShotSpawned(int slot);
bool  ChroniclePlayerShotLook(int slot, int &cell, float &size);
void  ChroniclePlayerShotLanded(int slot, const float *where);
bool  ChroniclePlayerFirstPersonAllowed();
float ChroniclePlayerReachScale();
bool  ConsumeChronicleFatalGuard(int character);
void  ChronicleNativeItemPickedUp(int item_no, int count, int character, int slot);
// The companion: its weapon's base attack, and a blow on a monster slot (no formulas; reported to mods).
int   ChronicleCompanionBaseAttack(int character);
void  ChronicleCompanionStrike(int slot, int amount, int character);
// Free back floors (dungeons.set_backrooms): the stairs' key window becomes a panel; true when it took over.
bool  ChronicleBackroomAsk();
// A fixed floor's gate pressed at (its marker's name): true when a mod's story-exit panel opened in place of the lock.
bool  ChronicleStoryExitAsk(const char *marker);
// A mod turned the dungeon entrance's floor menu off (dungeons.set_floor_select): the entrance goes straight in.
bool  ChronicleFloorSelectOff();
// A mod asked for town and dungeon to be gone between with no loading card (dungeons.set_quick_passage).
bool  ChronicleQuickPassage();
// Quick passage's cut (O2 C0): the mode switch between a town and a dungeon marks the arrival, and the arriving
// mode's init takes the mark once and starts with the picture clear instead of fading in from black.
void  ChronicleMarkWalkArrival(bool walked);
bool  ChronicleTakeWalkArrival();
// A dungeon entered by walking through its town's door: the opening script's stairs walk-in is skipped once the floor is
// built (Begin at the dungeon's init; Skip is true once, in the opening script, after the floor is built).
void  ChronicleDungeonWalkInBegin(bool walked);
bool  ChronicleDungeonWalkInSkip();
// L3: the dungeon was gone back to as it was left, not built (no generating/generated): game.dungeon.resumed.
void  ChronicleDungeonResumed();
// L4: a town's dungeon built unseen as the town loads (dun DunPrebuild): no save, no held picture let go, no Vigil
// arrival taken; generating/generated carry prebuild = true. WalkInDone: the walk-in was skipped (the floor stands).
void  ChronicleDungeonPrebuild(bool on);
bool  ChronicleDungeonPrebuilding();
bool  ChronicleDungeonWalkInDone();
// The arriving place fades itself in more than once (its init, its start script, an arrival event): every Ed fade in
// while the picture is held, and the first one within ten seconds of a walk arrival, take one frame. True once.
bool  ChronicleWalkArrivalFade();
// The held picture (O2 C0): with quick passage on, a jump between a town and a dungeon keeps the last picture on screen
// (presentation drops the frames drawn meanwhile) until the place walked into is ready: a town once its init is done,
// a dungeon once its floor is built, then a few frames more. At most ten seconds.
void  ChronicleMapJumped(int map_no);
// A jump from one town to another (the open world's road), or from a dungeon floor to another of the same dungeon (a
// story floor through its smoke), holds the picture as well (with quick passage on); the arriving mode's start reads
// the mark (true) and starts with the picture clear. The floor's hold lets go once the party plays on the new floor.
bool  ChronicleTakeRoadJump();
// A mod's roads (world.set_roads): a town's gate asks the mod (game.town.road, ChronicleTownRoad) instead of opening
// the world map.
bool  ChronicleTownRoads();
void  ChronicleTownRoad(int town_map);
void  ChronicleWalkPlaceReady();
bool  ChronicleHoldingPicture();
// Frames until the held picture goes (1: this is its last frame), or -1 while it isn't going yet.
int   ChronicleHeldReleaseIn();
// Connected worlds (O2 C1): the arena world a town's or dungeon's init carves into, so each place loaded stays in
// memory beside the others; 0 (the shared world) unless quick passage is on. A town's is 100 + its map, a
// dungeon's its map (200 + n).
int   ChronicleConnectedWorld(int map_no);
bool  ChronicleQuickPassageOn();
// A dungeon event program has ended: a mod that started it (dungeons.play_event) hears game.dungeon.event_ended.
void  ChronicleEventEnded(int program);
// The player acts at a free back floor's entrance: a panel asks whether to go down (no item needed). True when it opened.
bool  ChronicleBackroomEntranceAsk();
void  RestoreBackroomKey();
float ApplyChroniclePlayerDamageFormula(float damage, int character, int weapon_id);

// The menus and the town.
bool TakeManagerMenuOpenRequest();
void ChronicleModVillagerTalked(int index);
int  ChronicleTownMotionRemap(CCharacter *character, int motion_no);
void DrawChronicleDungeonBadges();

// The floor builder.
void RecordChronicleChestValue(CDungeonMap *map, int slot, int item);
void CollectChronicleChestValue(CDungeonMap *map, int slot);
void ChroniclePrepareFloorGeneration(bool);
void ChronicleFloorGenerated(bool back_floor, int seed, int rooms);
void ChronicleFloorEventsBuilt(CDungeonMap *map);
void ChronicleChestOpened(CDungeonMap *map, int slot);
// dungeons.set_exits: true (and game.dungeon.town_refused for mods) when a mod keeps the party from leaving for town
// this way ("menu": the Escape menu, "gate": the exit gate's continue/leave window, which then answers continue).
bool ChronicleTownExitRefused(const char *via);
int  ChronicleScriptFloorLevel(int dungeon_id, int floor);


// The monsters.
void  ChronicleMonsterModelLoaded(CMonstorUnit *unit, int slot, int native_model_no);
void  ChronicleMonsterActorCreated(CMonstorUnit *unit, int slot);
void  ChronicleMonsterRun(CMonstorUnit *unit, int slot, int program);
// True once for a monster in its native idle loop when a script override was installed since the loop started: the
// game starts its idle program again, so the override takes it over now rather than after its next hit.
bool  ChronicleMonsterRunAgain(CMonstorUnit *unit, int slot);
bool  ChronicleMonsterActorIsModOwned(CMonstorUnit *unit, int slot);
// A boss a mod placed with an arena origin (characters.spawn arena_origin): its script works in its arena's
// coordinates, centred on the world origin, and this is where that origin lies on the floor.
bool  ChronicleMonsterScriptOrigin(CMonstorUnit *unit, int slot, float origin[3], bool *grounded = nullptr);
// A grounded boss's script still flies: the height it last asked for, which it reads back as its own.
float *ChronicleMonsterScriptHeight(CMonstorUnit *unit, int slot);
// Such a boss asks for its defeat scene once its life is gone, and that scene can't run away from its arena: it
// fades out instead, and ChronicleMonsterDefeatFading holds its script while it does.
void  ChronicleMonsterDefeatScene(CMonstorUnit *unit, int slot);
bool  ChronicleMonsterDefeatFading(CMonstorUnit *unit, int slot);
// A boss's cast (characters.spawn arena_slot) names its members by the slots they hold in their own arena: the slot
// here of the member of `slot`'s cast that holds `native` there (`native` itself outside a cast), or -1; and the
// arena slot `slot` holds (its own slot outside a cast).
int   ChronicleMonsterCastSlot(CMonstorUnit *unit, int slot, int native);
int   ChronicleMonsterCastNumber(CMonstorUnit *unit, int slot);
// The bank of shared script integers (SET/GET_GLOBAL_INT) a cast's member uses, or -1 for the floor's own.
int   ChronicleMonsterGlobalBank(CMonstorUnit *unit, int slot);
// A monster's hit on the player (its attack's owner, or the slot a shot it fired carries) times its
// entities.set_stats damage_scale.
int   ChronicleMonsterHitDamage(int damage, int owner, int monster_no);
// entities.set_stats hit_reach: game units added to each body sphere of the monster in `slot` when a blow is checked.
float ChronicleMonsterHitReach(CMonstorUnit *unit, int slot);
// entities.set_stats hit_share: each body sphere of the monster in `slot` takes its best character's share of a blow.
bool  ChronicleMonsterHitShare(CMonstorUnit *unit, int slot);
void  ChronicleMonstersCleared(CMonstorUnit *unit);
void  QueueChronicleMonsterDamageResult(CMonstorUnit *unit, int slot, int result);
int   TakeChronicleMonsterDamageResult(CMonstorUnit *unit, int slot);
float ApplyChronicleCombatDamageFormula(float damage, int weapon_id, int native_model_id, int character_id,
                                        int target_hp, int target_max_hp);
int   ChronicleNativeMonsterModelId(CMonstorUnit *unit, int slot);
void  QueueChronicleMonsterHit(CMonstorUnit *unit, int slot, int attacker, int weapon_id, int damage, int element);
void  ChronicleMonsterDefeated(CMonstorUnit *unit, int slot, bool scripted);
bool  ChronicleTryEnemyVarietySurvival(CMonstorUnit *unit, int slot, int &damage);

} // namespace moonmod

namespace dc::game {

/**
 * buildEventData's chest count with a mod's bonus applied (dungeonmap_moonmod.cpp).
 */
int DngChestTarget(CDungeonMap *map, int retail_target);

/**
 * A floor added by game.dungeons.register with its own map pack, or nullptr.
 */
const char *ChronicleModFloorMap(int dungeon, int floor);

/**
 * The walk's swing motion with the player's tuning applied (gameloop_moonmod.cpp).
 */
void MoonModTuneSwingMotion();

/**
 * A mod move's shot (player.moves_set, a hit with shot = true): `count` shots fanned `spread` radians apart, from the
 * slingshot's tip (Xiao) or the weapon hand, toward the locked-on target or along `heading`. Returns the shots fired.
 */
int MoonModSpawnPlayerShots(float heading, float speed, int damage, int count, float spread);

} // namespace dc::game

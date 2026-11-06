#pragma once

// Co-op's monsters (monsters.mirror_get / puppet_set / mirror_hit), from src/moonmod/coop_monsters_moonmod.cpp. The
// host's game runs the floor's monsters: they see every player on it (the avatars, coop_avatars.hpp, count as players:
// a monster wakes for the nearest and chases it), and a monster's blow that lands on an avatar is queued for the mod to
// send to that player's game. A guest's game mirrors them: each of its monsters is a puppet of the host's in the same
// slot, with no thoughts or blows of its own, moved and animated as the host's are, its life the host's; it dies when
// the host's does, its own death and drops playing out.

#include <vector>

namespace dc::game {

class CMonstorUnit;

struct MoonModPuppet {
    int   kind = -1;      // the host's monster's kind: a slot holding another kind isn't puppeted (the floors differ)
    int   state = -1;     // the host's: 1 asleep, 2 up; -1 gone (dead or empty)
    int   hp = 0, max_hp = 0;
    float x = 0.0f, y = 0.0f, z = 0.0f, heading = 0.0f;
    int   motion = 0;
};

// Guest: the host's monsters, by slot. A slot left out is gone on the host. False for a slot that can't follow (empty
// here, or another kind), which the result reports.
bool MoonModCoopPuppetSet(int slot, const MoonModPuppet &puppet);
void MoonModCoopPuppetsClear();
bool MoonModCoopIsPuppet(int slot);
void MoonModCoopPuppetsStep(); // after the monsters' own step
// A blow this player landed on a copy (any: a weapon's, a bomb's, a thrown item's), for the host's monster.
void MoonModCoopPuppetHit(int slot, int damage);
struct MoonModPuppetHit {
    int slot = 0, damage = 0;
};
std::vector<MoonModPuppetHit> MoonModCoopTakePuppetHits();

// Host: the remote players as the monsters' targets, around the monsters' step.
float MoonModCoopNearestDistance(const float *player, const float *monster);
void  MoonModCoopMonstersStepBegin();
void  MoonModCoopMonsterTarget(int slot);
void  MoonModCoopMonstersStepEnd();

// Host: what would strike the player that lands on a hero instead, from the player's own damage check each frame.
void MoonModCoopAvatarHitsScan();

struct MoonModAvatarHit {
    int avatar = 0;  // 1..3, as world.avatar_set numbers them
    int monster = 0; // its slot
    int damage = 0, kind = 0, flags = 0; // kind: 2 a small hit, 3 a blow, 4 a heavy blow (COLLISION_HIT)
};
std::vector<MoonModAvatarHit> MoonModCoopTakeAvatarHits();

} // namespace dc::game

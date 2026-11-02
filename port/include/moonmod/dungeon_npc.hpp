#pragma once

#include <string>

// A mod's figure on a dungeon floor (world.dungeon_npc_set), drawn and driven by src/moonmod/dungeon_npc_moonmod.cpp.
namespace dc::game {
// The model (a game path to a .chr) and where it stands: 0 just inside the exit gate, 1 beside the player. False when
// another mod has the figure. A new floor sends it away.
bool MoonModNpcSet(const std::string &owner, const std::string &id, const std::string &model, int place);
void MoonModNpcClear(const std::string &owner);
void MoonModNpcStep();
void MoonModNpcDraw(bool shadow);
} // namespace dc::game

namespace moonmod {
// The player walked up to it: game.dungeon_npc.near to its owner.
void ChronicleNpcNear(const std::string &owner, const std::string &id);
} // namespace moonmod

#pragma once

/**
 * @file
 * MoonMod: Atla spheres a mod puts on a dungeon floor (georama.atla_spawn). One looks and is picked up like the
 * game's own (Toan opens it, its name is shown), but what is inside is the mod's: picking it up grants no Georama
 * part or chip and touches no save record; the mod hears of it (game.georama.atla_collected) and keeps it itself.
 * A floor's ball holds kModAtlaBase + the sphere's index in place of an Atla registry index.
 */

#include <functional>
#include <string>
#include <string_view>

#include "common.h"

namespace dc::game {

/** A ball's atra_no at or past this is a mod's sphere (its index added); the game's are registry indexes, under 100. */
constexpr int kModAtlaBase = 1000;
/** How many mod spheres there can be. */
constexpr int kModAtlaMax = 16;
/** The system message line naming mod sphere i (for the pickup's "... acquired"): this plus i. */
constexpr int kModAtlaNameLine = 31600;

/** A mod's sphere: its owner, its id and the name its pickup shows. */
struct ModAtla {
    std::string owner;
    std::string id;
    std::string name;
};

/** Registers (or renames) a mod's sphere; its index, or -1 when kModAtlaMax are taken. */
int RegisterModAtla(std::string_view owner, std::string_view id, std::string_view name);

/** The sphere a ball's atra_no stands for, or null for the game's own. */
const ModAtla *ModAtlaOf(int atra_no);

/** Whether a ball's atra_no is a mod's sphere. */
inline bool IsModAtla(int atra_no) {
    return atra_no >= kModAtlaBase && atra_no < kModAtlaBase + kModAtlaMax;
}

/** The name line of mod sphere atra_no (its pickup message), or null where line is not one. */
const s16 *ModAtlaNameLine(int line);

/** Called when Toan picks up a mod's sphere (the adapter turns it into game.georama.atla_collected). */
void SetModAtlaCollectedHandler(std::function<void(const ModAtla &)> handler);
void ModAtlaCollected(int atra_no);

/** Forgets every mod sphere (tests). */
void ClearModAtlas();

} // namespace dc::game

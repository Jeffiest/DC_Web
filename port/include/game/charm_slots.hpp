#pragma once

/**
 * @file
 * MoonMod: charm slots, each character's own slots on the item page beside their weapon and shield (an empty one
 * shows its charm blacked out) that hold only charms: amulet slots for the four amulets, and after them one slot of
 * its own for Dran's Feather, which lightens the run on the dodge button (dodge_run.hpp), spending a use a tick of
 * it. When a hit rolls an ailment, an amulet in the
 * active character's slots guards against it as one in a quick-use slot would, and spends a use, so carrying one no
 * longer costs a quick slot. A charm in a slot is out of the bag and takes none of its room. A mod opens the slots
 * (items.charm_slots_set, up to CHARM_SLOT_MAX amulet slots each, and the Feather's); with none open the page is the
 * game's own. A slot that holds a
 * charm is always shown, so a charm is never out of reach when its mod is off. The save never holds them: they are
 * kept beside it as charms.json, by character id, as the registered characters keep theirs (chara_extras_save.hpp),
 * written and read with it.
 */

#include <filesystem>
#include <string>

#include "types.h"

namespace dc::game {

/** Most amulet slots a mod can open for each character. */
constexpr int CHARM_SLOT_MAX = 4;

/** The index of each character's Feather slot, after their amulet slots. */
constexpr int CHARM_FEATHER_SLOT = CHARM_SLOT_MAX;

/** Slots each character has: the amulet slots and the Feather's. */
constexpr int CHARM_SLOT_COUNT = CHARM_SLOT_MAX + 1;

/**
 * Where the dungeon HUD's sheet ("itempack", 256x192) holds a copy of each of the active character's charm icons: the
 * middle CHARM_HUD_ICON_SIZE square of each, two by two from here, in a part of the sheet nothing draws from. They are
 * copied in with the quick slots' icons (DngActiveItemTextureCopy).
 */
constexpr int CHARM_HUD_ICON_SIZE = 24;

/** Where in the HUD sheet slot index's copy sits (a CHARM_HUD_ICON_SIZE square). */
void CharmHudCell(int index, int &u, int &v);

/** One charm slot: its charm's item number (or -1 when empty) and the uses left of it. */
struct CharmSlot {
    s16 item_no = -1;
    s32 uses = 0;
};

/** Whether item_no can go in a charm slot (one of them): an amulet or Dran's Feather. */
bool IsCharmItem(int item_no);

/** Whether slot index takes item_no: an amulet slot an amulet, the Feather slot the Feather. */
bool CharmSlotAccepts(int index, int item_no);

/** The count a charm with uses left shows in its corner: its uses, or for Dran's Feather (a use a tick) its seconds. */
int CharmUsesShown(int item_no, int uses);

/** Amulet slots a mod has opened for each character (0 to CHARM_SLOT_MAX). */
int CharmSlotsOpen();

/** Opens count amulet slots each (clamped to 0..CHARM_SLOT_MAX); charms in slots past it stay, shown until taken out. */
void SetCharmSlotsOpen(int count);

/** Whether a mod has opened the Feather slot. */
bool CharmFeatherOpen();

/** Opens or closes every character's Feather slot; a Feather in it stays, shown until taken out. */
void SetCharmFeatherOpen(bool open);

/**
 * Slots the item page shows for chara, side by side: the amulet slots open and any further one holding a charm, then
 * the Feather slot if it is open or holds the Feather.
 */
int CharmSlotsShown(int chara);

/** The slot index shown at position (0 to CharmSlotsShown - 1) for chara. */
int CharmSlotIndex(int chara, int position);

/** chara's slot index, 0 to CHARM_SLOT_COUNT - 1; neither is range-checked. */
CharmSlot &CharmSlotAt(int chara, int index);

/** chara's slot holding item_no, or -1. */
int FindCharmSlot(int chara, int item_no);

/** What SpendCharm did. */
enum class CharmGuard {
    None,    /**< None of the character's slots holds the charm. */
    Guarded, /**< A slot's charm guarded and spent a use. */
    Broke,   /**< ...and that was its last: the slot is empty now. */
};

/** A hit on chara rolled the ailment item_no guards against: chara's slot holding it, if any, guards and spends a use. */
CharmGuard SpendCharm(int chara, int item_no);

/**
 * What a charm with uses left of full_uses sells for, from its full price: the price shrinks with each use spent, and
 * one worn below half its uses is worthless (0). Anything with no full uses sells for its price.
 */
int WornCharmPrice(int price, int uses, int full_uses);

/** Empties every character's slots (a new game, or a save with no charms). */
void ClearCharmSlots();

/** Writes the slots to save_directory/charms.json (removes it when all are empty). Returns false when it cannot. */
bool CharmSlotsSave(const std::filesystem::path &save_directory);

/** Reads save_directory/charms.json into the slots; with no file, or a bad one, they are empty. */
void CharmSlotsLoad(const std::filesystem::path &save_directory);

void CharmSlotsDelete(const std::filesystem::path &save_directory);

/** The file's text for the slots (tests). */
std::string CharmSlotsJson();

/** Reads the file's text into the slots (tests); false, with every slot empty, if it is not one. */
bool CharmSlotsApply(const std::string &text);

} // namespace dc::game

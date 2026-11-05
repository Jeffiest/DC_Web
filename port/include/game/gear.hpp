#pragma once

/**
 * @file
 * MoonMod: gear, pieces a character wears in slots of their own (hat, tunic, gloves, boots and a pouch) beside their
 * weapon.
 * A mod makes each piece (items.gear_give) with a name, a rarity and up to a few effects, and the engine reads the worn
 * pieces' effects where the game decides each thing: a chance to shrug off an ailment a hit rolled (before an amulet is
 * spent), defence against a hit, gauge refilled a second, a cut in what a swing costs, a cut in what the run drains
 * and poise, hits taken without a stagger (guard_rules.hpp PoiseTakes). A pouch grows the item bag while it is worn
 * (GearSyncBag), past what the game's Pockets give it.
 * Pieces are not the game's items: they never take room in the bag of items, and they are kept beside the save as
 * gear.json, written and read with it, as the charm slots are (charm_slots.hpp). A piece worn by a character no mod
 * registers now stays worn by them in the file.
 */

#include <array>
#include <filesystem>
#include <map>
#include <string>
#include <utility>
#include <vector>

#include "types.h"

namespace dc::game {

class CFrame;
struct ITEM_PACK;

/**
 * The gear slots, one for each kind of piece. The pouch's slot shows on the hero page only when the mod names its icon
 * (items.gear_page_set), so a mod that knows the first four keeps a page of four.
 */
enum GearKind : int { GEAR_HAT, GEAR_TUNIC, GEAR_GLOVES, GEAR_BOOTS, GEAR_POUCH, GEAR_KIND_COUNT };

/** The ailments a piece can guard against, in this order: freeze, poison, curse, goo. */
constexpr int GEAR_GUARD_COUNT = 4;

/** Pieces the gear bag holds besides those worn. */
constexpr int GEAR_BAG_MAX = 99;

/** Rarities, the Gauntlet's five: common, uncommon, rare, epic, legendary. (Before gear.json version 2 there were
 * four, the last "unique"; GearApply reads an old file's unique as legendary.) */
constexpr int GEAR_RARITY_COUNT = 5;

/** Longest name a piece has, in bytes. */
constexpr int GEAR_NAME_MAX = 32;

/** What a piece does, or what all of a character's worn pieces do together (GearWornEffects caps that). */
struct GearEffects {
    std::array<int, GEAR_GUARD_COUNT> guard{}; /**< Percent chance to shrug off each ailment a hit rolls. */
    int stamina = 0;                           /**< Gauge refilled a second, on top of the character's own. */
    int lightness = 0;                         /**< Percent off what a swing costs. */
    int defence = 0;                           /**< Taken off each hit, as the character's defence is. */
    int run_cost = 0;                          /**< Percent off what the run drains (dodge_run.hpp). */
    int poise = 0;                             /**< Points of hits taken without a stagger: light 1, heavy 2. */
    int bag = 0;                               /**< Slots the item bag gains while it is worn (GearBagSlots). */

    bool operator==(const GearEffects &) const = default;
};

/** The most the worn pieces add up to: the guards, stamina, lightness, defence, run cost, poise and bag. */
constexpr int GEAR_GUARD_CAP = 90;
constexpr int GEAR_STAMINA_CAP = 120;
constexpr int GEAR_LIGHTNESS_CAP = 50;
constexpr int GEAR_DEFENCE_CAP = 99;
constexpr int GEAR_RUN_COST_CAP = 75;
constexpr int GEAR_POISE_CAP = 8;
constexpr int GEAR_BAG_SLOTS_CAP = 50;

struct GearPiece {
    u32         uid = 0;      /**< The piece's own number, never reused in a save. */
    int         kind = GEAR_HAT;
    int         rarity = 0;
    std::string name;
    int         value = 0;    /**< Gilda it sells for. */
    GearEffects effects;
    std::string worn_by;      /**< The character id wearing it (CharaIdName), or empty in the bag. */
    std::string icon;         /**< Its icon's game path ("icons/<stem>.png", a mod's), or empty for the kind's. */
    u32         color = 0xFFFFFF; /**< 0xRRGGBB the icon is tinted with; white leaves it as drawn. */
};

/** The id a kind goes by ("hat", "tunic", "gloves", "boots"), and the kind an id names (-1 for none). */
const char *GearKindName(int kind);
int GearKindFromName(const std::string &name);

/** The id an ailment guard goes by ("freeze", "poison", "curse", "goo"), and back (-1 for none). */
const char *GearGuardName(int guard);
int GearGuardFromName(const std::string &name);

/** Every piece, worn or in the bag, in the order they came. */
const std::vector<GearPiece> &GearPieces();

/** The piece numbered uid, or null. */
const GearPiece *FindGear(u32 uid);

/** Pieces in the bag (not worn). */
int GearBagCount();

/** Adds piece to the bag with a new uid, which it returns; 0 when the bag is full. Its uid and worn_by are ignored. */
u32 AddGear(GearPiece piece);

/** Takes the piece away, worn or not. False when there is none. */
bool RemoveGear(u32 uid);

/** The piece chara wears in slot kind, or null. */
const GearPiece *WornGear(int chara, int kind);

/**
 * Puts the piece on chara, taking it off whoever wore it; what chara wore in that slot goes to the bag. False when
 * there is no such piece.
 */
bool EquipGear(int chara, u32 uid);

/** Takes chara's piece in slot kind off into the bag. False when the slot is empty or the bag is full. */
bool UnequipGear(int chara, int kind);

/** Pieces in the bag, in order: the index-th (0 on), or null past the last. The held piece (HeldGear) is skipped. */
const GearPiece *GearBagPiece(int index);

/**
 * The piece the item page's cursor holds, picked up from the bag or a slot (0 for none). It stays in the bag while it
 * is held, so closing the menu loses nothing; the bag's cells skip it.
 */
u32 HeldGear();
void SetHeldGear(u32 uid);

/**
 * Takes chara's piece in slot kind off into the bag whatever the bag holds (the item page picks it up into the hand;
 * the bag may hold one past GEAR_BAG_MAX until it is put down). Returns its uid, or 0 when the slot is empty.
 */
u32 TakeOffGear(int chara, int kind);

/**
 * Whether a mod has opened the gear page: the item board's Equipment tab and the gear slots on the character page
 * (items.gear_page_set). Closed, the item page is the game's own.
 */
bool GearPageOpen();
void SetGearPageOpen(bool open);

/** The icon a kind's empty slot shows blacked out, and a piece with no icon of its own shows (a game path, or ""). */
const std::string &GearKindIcon(int kind);
void SetGearKindIcon(int kind, std::string game_path);

/**
 * The frames of a character's menu model that show a kind of gear (items.gear_page_set parts): `draw` the frames drawn
 * in a slot, `fit` those the slot is fitted to (all of `draw` when empty), `turn` how far round he is seen and `tilt`
 * how far he leans back, so a boot is seen from the side rather than from above. A piece with no icon of its own (or its
 * kind's) shows them in place of the icon, so a hat slot holds the hero's own hat. `bare` names the frames his models
 * leave out while nothing is worn in the slot (his hat, with no hat on), in the menu and the world. `below` cuts the
 * slot's drawing off above the lowest edge of those frames, so a part of a bigger mesh shows alone (his shoes, below his
 * ankle wraps, from his body). Empty where no mod named any.
 */
struct GearKindParts {
    std::vector<std::string> draw;
    std::vector<std::string> fit;
    std::vector<std::string> bare;
    std::vector<std::string> below;
    float                    turn = 0.0f; /**< Radians the model is turned about its height to show them, left positive. */
    float                    tilt = 0.0f; /**< Radians the model leans back to show them from lower down; forwards when negative. */
};

using GearCharaParts = std::array<GearKindParts, GEAR_KIND_COUNT>;

const GearKindParts &GearPartsFor(int chara, int kind);
/** Every character's parts, by character id (CharaIdName); replaces those set before. */
void SetGearParts(std::map<std::string, GearCharaParts> parts);

/**
 * While it lives, a character model's `bare` frames (GearKindParts) for each empty slot are hidden: CCharacter::Draw
 * holds one round its drawing, and leaves out the cloths that hang from them (Hides; Toan's vest is two). The model is
 * a hero's when its root frame is named for their pack (c01d for Toan's c01d.chr, any c01 model). Nothing is hidden
 * while the gear page is closed.
 */
class GearBareScope {
public:
    explicit GearBareScope(CFrame *root);
    ~GearBareScope();
    GearBareScope(const GearBareScope &) = delete;
    GearBareScope &operator=(const GearBareScope &) = delete;

    /** Whether frame is one this scope leaves out. */
    bool Hides(const CFrame *frame) const;

private:
    std::vector<std::pair<CFrame *, short>> hidden_;
    std::vector<const CFrame *>             bare_;
};

/** What chara's worn pieces do together, each capped (GEAR_*_CAP). */
GearEffects GearWornEffects(int chara);

/** Slots the worn pouches add to the item bag: the largest any character wears (one bag for the party). */
int GearBagSlots();

/**
 * Brings the item bag's size in line with the worn pouches (GearBagSlots), within the game's most of 100: grows it, or
 * shrinks it as far as its items let, moving items from the slots that go into empty ones that stay (while it is too
 * full it stays larger, and shrinks as it empties). The slots it added are kept with the gear (gear.json), so the size
 * the save gave it (its Pockets) is left alone. The item page calls it each frame, as do the gear calls; returns the
 * slots added.
 */
int GearSyncBag(ITEM_PACK &pack);

/** The slots GearSyncBag has added to the bag now. */
int GearBagAdded();

/**
 * A gear piece a shop sells (items.gear_stock_set): the piece bought (a copy goes to the bag) and its price in gilda.
 * Sold, it stays in the list but is gone from the shop's board until the mod stocks the shop again.
 */
struct GearGood {
    GearPiece piece;
    int       price = 0;
    bool      sold = false;
};
constexpr int GEAR_STOCK_MAX = 10;     /**< Goods a shop can hold, beside its own (its board has 30 cells). */
constexpr int GEAR_PRICE_MAX = 0xFFFF; /**< The most a wallet holds. */

/** Sets shop_no's gear goods (at most GEAR_STOCK_MAX); an empty list takes them away. */
void SetGearStock(int shop_no, std::vector<GearGood> goods);

/** shop_no's gear goods, sold ones included (null when it has none). */
std::vector<GearGood> *GearStock(int shop_no);

/** Takes every shop's gear goods away. */
void ClearGearStock();

/** Whether chara's gear shrugs off ailment guard (0..3) for a roll of 0..99: the roll falls under the worn chance. */
bool GearGuards(int chara, int guard, int roll);

/** What a swing costs chara, as a scale of its cost: 1 less their gear's lightness. */
float GearSwingScale(int chara);

/** What the run drains for chara, as a scale: 1 less their gear's run cost. */
float GearRunScale(int chara);

/** Takes every piece away (a new game, or a save with no gear). */
void ClearGear();

/** Writes the gear to save_directory/gear.json (removes it when there is none). Returns false when it cannot. */
bool GearSave(const std::filesystem::path &save_directory);

/** Reads save_directory/gear.json; with no file, or a bad one, there is no gear. */
void GearLoad(const std::filesystem::path &save_directory);

void GearDelete(const std::filesystem::path &save_directory);

/** The file's text for the gear (tests). */
std::string GearJson();

/** Reads the file's text into the gear (tests); false, with no gear, if it is not one. */
bool GearApply(const std::string &text);

} // namespace dc::game

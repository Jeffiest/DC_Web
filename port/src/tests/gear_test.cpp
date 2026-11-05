#include <gtest/gtest.h>

#include <filesystem>
#include <string>

#include "gauge_rules.hpp"
#include "gear.hpp"
#include "itemdata.hpp"
#include "userstatus.hpp"

namespace {

using namespace dc::game;

class GearTest : public ::testing::Test {
protected:
    void SetUp() override { ClearGear(); }
    void TearDown() override {
        ClearGear();
        SetGearPageOpen(false);
    }

    static u32 Give(int kind, GearEffects effects = {}, int value = 0) {
        return AddGear({.kind = kind, .name = "Test piece", .value = value, .effects = effects});
    }
};

TEST_F(GearTest, KindsAndGuardsGoByTheirNames) {
    for (int kind = 0; kind < GEAR_KIND_COUNT; kind++) EXPECT_EQ(GearKindFromName(GearKindName(kind)), kind);
    for (int guard = 0; guard < GEAR_GUARD_COUNT; guard++) EXPECT_EQ(GearGuardFromName(GearGuardName(guard)), guard);
    EXPECT_EQ(GearKindFromName("cape"), -1);
    EXPECT_EQ(GearGuardFromName("stamina"), -1);
}

TEST_F(GearTest, APieceGoesOnAndWhatWasThereGoesToTheBag) {
    const u32 cap = Give(GEAR_HAT);
    const u32 hood = Give(GEAR_HAT);
    EXPECT_EQ(GearBagCount(), 2);

    ASSERT_TRUE(EquipGear(CHARA_TOAN, cap));
    EXPECT_EQ(WornGear(CHARA_TOAN, GEAR_HAT)->uid, cap);
    EXPECT_EQ(GearBagCount(), 1);

    ASSERT_TRUE(EquipGear(CHARA_TOAN, hood));
    EXPECT_EQ(WornGear(CHARA_TOAN, GEAR_HAT)->uid, hood);
    EXPECT_TRUE(FindGear(cap)->worn_by.empty());

    // Put on someone else, it comes off Toan.
    ASSERT_TRUE(EquipGear(CHARA_XIAO, hood));
    EXPECT_EQ(WornGear(CHARA_TOAN, GEAR_HAT), nullptr);
    EXPECT_EQ(WornGear(CHARA_XIAO, GEAR_HAT)->uid, hood);

    ASSERT_TRUE(UnequipGear(CHARA_XIAO, GEAR_HAT));
    EXPECT_EQ(GearBagCount(), 2);
    EXPECT_FALSE(UnequipGear(CHARA_XIAO, GEAR_HAT));
    EXPECT_FALSE(EquipGear(CHARA_TOAN, 999));
}

TEST_F(GearTest, TheBagHoldsSoManyBesidesWhatIsWorn) {
    for (int i = 0; i < GEAR_BAG_MAX; i++) ASSERT_NE(Give(GEAR_BOOTS), 0u);
    EXPECT_EQ(Give(GEAR_BOOTS), 0u);
    // Wearing one makes room, and taking it off again needs room.
    ASSERT_TRUE(EquipGear(CHARA_GORO, GearPieces().front().uid));
    const u32 more = Give(GEAR_BOOTS);
    ASSERT_NE(more, 0u);
    EXPECT_FALSE(UnequipGear(CHARA_GORO, GEAR_BOOTS));
    ASSERT_TRUE(RemoveGear(more));
    EXPECT_TRUE(UnequipGear(CHARA_GORO, GEAR_BOOTS));
}

TEST_F(GearTest, WornEffectsAddUpToTheirCaps) {
    GearEffects hat;
    hat.guard[1] = 60;
    hat.stamina = 100;
    GearEffects tunic;
    tunic.guard[1] = 60;
    tunic.defence = 4;
    tunic.stamina = 30;
    tunic.poise = 5;
    GearEffects gloves;
    gloves.lightness = 40;
    GearEffects boots;
    boots.lightness = 20;
    boots.run_cost = 40;
    boots.poise = 6;
    EquipGear(CHARA_RUBY, Give(GEAR_HAT, hat));
    EquipGear(CHARA_RUBY, Give(GEAR_TUNIC, tunic));
    EquipGear(CHARA_RUBY, Give(GEAR_GLOVES, gloves));
    EquipGear(CHARA_RUBY, Give(GEAR_BOOTS, boots));
    // A piece in the bag counts for nothing.
    GearEffects spare;
    spare.defence = 50;
    Give(GEAR_TUNIC, spare);

    const GearEffects total = GearWornEffects(CHARA_RUBY);
    EXPECT_EQ(total.guard[1], GEAR_GUARD_CAP);
    EXPECT_EQ(total.guard[0], 0);
    EXPECT_EQ(total.stamina, GEAR_STAMINA_CAP);
    EXPECT_EQ(total.defence, 4);
    EXPECT_EQ(total.lightness, GEAR_LIGHTNESS_CAP);
    EXPECT_EQ(total.run_cost, 40);
    EXPECT_EQ(total.poise, GEAR_POISE_CAP);
    EXPECT_FLOAT_EQ(GearSwingScale(CHARA_RUBY), 0.5f);
    EXPECT_FLOAT_EQ(GearRunScale(CHARA_RUBY), 0.6f);
    EXPECT_EQ(GearWornEffects(CHARA_TOAN), GearEffects{});
    EXPECT_FLOAT_EQ(GearSwingScale(CHARA_TOAN), 1.0f);

    // A 90% guard shrugs off rolls 0 to 89.
    EXPECT_TRUE(GearGuards(CHARA_RUBY, 1, 0));
    EXPECT_TRUE(GearGuards(CHARA_RUBY, 1, 89));
    EXPECT_FALSE(GearGuards(CHARA_RUBY, 1, 90));
    EXPECT_FALSE(GearGuards(CHARA_RUBY, 0, 0));
    EXPECT_FALSE(GearGuards(CHARA_RUBY, -1, 0));
}

TEST_F(GearTest, StaminaComesOnTopUnlessTheRefillIsStopped) {
    // Goro's own 60 a second is a point a tick; 30 more is half a point.
    EXPECT_FLOAT_EQ(GaugeRefillGain({.chara = CHARA_GORO, .stamina = 60.0f, .gear_stamina = 30.0f}), 1.5f);
    // Toan mid-swing refills nothing, gear or not.
    EXPECT_FLOAT_EQ(GaugeRefillGain({.chara = CHARA_TOAN, .stamina = 35.0f, .swinging = true, .gear_stamina = 30.0f}), 0.0f);
    // Goo halves it too.
    EXPECT_FLOAT_EQ(GaugeRefillGain({.chara = CHARA_GORO, .stamina = 60.0f, .goo = true, .gear_stamina = 30.0f}), 0.75f);
}

TEST_F(GearTest, KeptBesideTheSaveWithWhoWearsWhat) {
    const auto directory = std::filesystem::temp_directory_path() / "dc_gear_test";
    std::filesystem::create_directories(directory);

    GearEffects effects;
    effects.guard[3] = 25;
    effects.run_cost = 16;
    effects.poise = 2;
    const u32 boots = AddGear({.kind = GEAR_BOOTS, .rarity = 2, .name = "Swift Boots", .value = 400, .effects = effects});
    const u32 cap = Give(GEAR_HAT);
    EquipGear(CHARA_UNGAGA, boots);
    RemoveGear(cap);
    ASSERT_TRUE(GearSave(directory));

    ClearGear();
    GearLoad(directory);
    ASSERT_EQ(GearPieces().size(), 1u);
    const GearPiece *piece = WornGear(CHARA_UNGAGA, GEAR_BOOTS);
    ASSERT_NE(piece, nullptr);
    EXPECT_EQ(piece->uid, boots);
    EXPECT_EQ(piece->name, "Swift Boots");
    EXPECT_EQ(piece->rarity, 2);
    EXPECT_EQ(piece->value, 400);
    EXPECT_EQ(piece->effects, effects);
    // A uid is never given twice, even one whose piece is gone.
    EXPECT_GT(Give(GEAR_HAT), cap);

    // No gear writes no file, and loading that leaves none.
    ClearGear();
    ASSERT_TRUE(GearSave(directory));
    EXPECT_FALSE(std::filesystem::exists(directory / "gear.json"));
    Give(GEAR_HAT);
    GearLoad(directory);
    EXPECT_TRUE(GearPieces().empty());

    std::filesystem::remove_all(directory);
}

TEST_F(GearTest, AnOldFilesUniqueIsLegendary) {
    // Version 1 had four rarities, the last "unique"; version 2 has the Gauntlet's five.
    ASSERT_TRUE(GearApply(R"({"version": 1, "next_uid": 5, "pieces": [
        {"uid": 1, "kind": "hat", "name": "Plain", "rarity": 0}, {"uid": 2, "kind": "hat", "name": "Fine", "rarity": 1},
        {"uid": 3, "kind": "hat", "name": "Rare", "rarity": 2}, {"uid": 4, "kind": "hat", "name": "Unique", "rarity": 3}]})"));
    EXPECT_EQ(FindGear(1)->rarity, 0);
    EXPECT_EQ(FindGear(2)->rarity, 1);
    EXPECT_EQ(FindGear(3)->rarity, 2);
    EXPECT_EQ(FindGear(4)->rarity, 4);
    // Written back as version 2, an epic stays an epic.
    const u32 epic = AddGear({.kind = GEAR_TUNIC, .rarity = 3, .name = "Epic Vest"});
    ASSERT_TRUE(GearApply(GearJson()));
    EXPECT_EQ(FindGear(epic)->rarity, 3);
    EXPECT_EQ(FindGear(4)->rarity, 4);
}

TEST_F(GearTest, AWearerNoModRegistersKeepsTheirPiece) {
    ASSERT_TRUE(GearApply(R"({"version": 1, "next_uid": 4, "pieces": [
        {"uid": 2, "kind": "gloves", "name": "Old Gloves", "worn_by": "someone"},
        {"uid": 3, "kind": "hat", "name": "Cap", "worn_by": "toan"},
        {"uid": 1, "kind": "hat", "name": "Hood", "worn_by": "toan"}]})"));
    EXPECT_EQ(FindGear(2)->worn_by, "someone");
    EXPECT_EQ(GearBagCount(), 1); // the second hat on Toan went to the bag
    EXPECT_EQ(WornGear(CHARA_TOAN, GEAR_HAT)->uid, 3u);
    EXPECT_NE(GearJson().find("someone"), std::string::npos);
}

TEST_F(GearTest, APieceKeepsItsIconAndColour) {
    const u32 hood = AddGear({.kind = GEAR_HAT, .name = "Viper's Hood", .icon = "icons/gear_hat.png", .color = 0x6EC45A});
    const u32 cap = Give(GEAR_HAT);
    ASSERT_TRUE(GearApply(GearJson()));
    EXPECT_EQ(FindGear(hood)->icon, "icons/gear_hat.png");
    EXPECT_EQ(FindGear(hood)->color, 0x6EC45Au);
    // A piece with neither is its kind's icon, untinted.
    EXPECT_TRUE(FindGear(cap)->icon.empty());
    EXPECT_EQ(FindGear(cap)->color, 0xFFFFFFu);
}

TEST_F(GearTest, APieceInHandLeavesTheBagCells) {
    const u32 cap = Give(GEAR_HAT);
    const u32 vest = Give(GEAR_TUNIC);
    SetHeldGear(cap);
    EXPECT_EQ(HeldGear(), cap);
    EXPECT_EQ(GearBagPiece(0)->uid, vest); // the held cap is skipped
    EXPECT_EQ(GearBagPiece(1), nullptr);
    EXPECT_EQ(GearBagCount(), 2);           // but it is still in the bag

    // A piece that is gone is not in hand.
    RemoveGear(cap);
    EXPECT_EQ(HeldGear(), 0u);

    // Closing the page, or clearing the gear, empties the hand.
    SetGearPageOpen(true);
    SetHeldGear(vest);
    SetGearPageOpen(false);
    EXPECT_EQ(HeldGear(), 0u);
    SetHeldGear(vest);
    ClearGear();
    EXPECT_EQ(HeldGear(), 0u);
}

TEST_F(GearTest, TakingOffIntoTheHandPassesAFullBag) {
    const u32 worn = Give(GEAR_BOOTS);
    ASSERT_TRUE(EquipGear(CHARA_TOAN, worn));
    while (GearBagCount() < GEAR_BAG_MAX) Give(GEAR_HAT);

    EXPECT_FALSE(UnequipGear(CHARA_TOAN, GEAR_BOOTS)); // Square has no room
    EXPECT_EQ(TakeOffGear(CHARA_TOAN, GEAR_BOOTS), worn);
    EXPECT_EQ(WornGear(CHARA_TOAN, GEAR_BOOTS), nullptr);
    EXPECT_EQ(GearBagCount(), GEAR_BAG_MAX + 1);
    EXPECT_EQ(TakeOffGear(CHARA_TOAN, GEAR_BOOTS), 0u); // nothing left to take
}

TEST_F(GearTest, EachKindHasTheIconAModGaveIt) {
    SetGearKindIcon(GEAR_GLOVES, "icons/gear_gloves.png");
    EXPECT_EQ(GearKindIcon(GEAR_GLOVES), "icons/gear_gloves.png");
    SetGearKindIcon(GEAR_GLOVES, "");
    EXPECT_TRUE(GearKindIcon(GEAR_GLOVES).empty());
}

TEST_F(GearTest, APouchGrowsTheBagAndItShrinksAsFarAsItsItemsLet) {
    ITEM_PACK pack{};
    pack.num = 50;
    for (int slot = 0; slot < 103; slot++) pack.item[slot] = -1;
    pack.item[0] = ITEM_DUNGEON_START;
    pack.item[60] = 5; // not an item, left past the bag's end: emptied when it opens (a real one there stays, as overflow)

    // The largest pouch in the party counts, not their sum.
    const u32 small = Give(GEAR_POUCH, {.bag = 10});
    const u32 large = Give(GEAR_POUCH, {.bag = 20});
    EXPECT_EQ(GearSyncBag(pack), 0);
    ASSERT_TRUE(EquipGear(CHARA_TOAN, small));
    ASSERT_TRUE(EquipGear(CHARA_XIAO, large));
    EXPECT_EQ(GearBagSlots(), 20);
    EXPECT_EQ(GearSyncBag(pack), 20);
    EXPECT_EQ(pack.num, 70);
    EXPECT_EQ(pack.item[60], -1);

    // Items in the slots that go move into the bag's empty ones.
    pack.item[65] = static_cast<s16>(ITEM_DUNGEON_START + 2);
    ASSERT_TRUE(UnequipGear(CHARA_XIAO, GEAR_POUCH));
    EXPECT_EQ(GearSyncBag(pack), 10);
    EXPECT_EQ(pack.num, 60);
    EXPECT_EQ(pack.item[1], ITEM_DUNGEON_START + 2);
    EXPECT_EQ(pack.item[65], -1);

    // Too full to shrink: it stays as large as its last item needs, and shrinks as it empties.
    for (int slot = 0; slot < 60; slot++) pack.item[slot] = ITEM_DUNGEON_START;
    ASSERT_TRUE(UnequipGear(CHARA_TOAN, GEAR_POUCH));
    EXPECT_EQ(GearSyncBag(pack), 10);
    pack.item[59] = -1;
    pack.item[58] = -1;
    EXPECT_EQ(GearSyncBag(pack), 8);
    EXPECT_EQ(pack.num, 58);

    // What it added goes with the gear, and the game's most of 100 holds.
    EXPECT_NE(GearJson().find("\"bag_added\": 8"), std::string::npos);
    pack.num = 98 + GearBagAdded();
    ASSERT_TRUE(EquipGear(CHARA_TOAN, large));
    EXPECT_EQ(GearSyncBag(pack), 2);
    EXPECT_EQ(pack.num, 100);
}

TEST_F(GearTest, AFileThatIsNotGearLeavesNone) {
    Give(GEAR_HAT);
    EXPECT_FALSE(GearApply("not json"));
    EXPECT_TRUE(GearPieces().empty());
    EXPECT_FALSE(GearApply(R"({"version": 1, "pieces": [{"uid": 1, "kind": "cape"}]})"));
    EXPECT_FALSE(GearApply(R"({"version": 1, "pieces": [{"uid": 1, "kind": "hat"}, {"uid": 1, "kind": "hat"}]})"));
    EXPECT_TRUE(GearPieces().empty());
}

} // namespace

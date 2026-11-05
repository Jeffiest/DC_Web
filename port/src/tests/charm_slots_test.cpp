#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <string>

#include "charm_slots.hpp"
#include "itemdata.hpp"

namespace {

using namespace dc::game;

class CharmSlotsTest : public ::testing::Test {
protected:
    void SetUp() override {
        ClearCharmSlots();
        SetCharmSlotsOpen(0);
        SetCharmFeatherOpen(false);
    }

    void TearDown() override {
        ClearCharmSlots();
        SetCharmSlotsOpen(0);
        SetCharmFeatherOpen(false);
    }
};

TEST_F(CharmSlotsTest, TheFourAmuletsAndTheFeatherAreCharms) {
    EXPECT_TRUE(IsCharmItem(ITEM_ANTI_FREEZE_AMULET));
    EXPECT_TRUE(IsCharmItem(ITEM_ANTICURSEAMULET));
    EXPECT_TRUE(IsCharmItem(ITEM_ANTIGOO_AMULET));
    EXPECT_TRUE(IsCharmItem(ITEM_ANTIDOTE_AMULET));
    EXPECT_TRUE(IsCharmItem(ITEM_DRAN_S_FEATHER));
    EXPECT_FALSE(IsCharmItem(ITEM_DUNGEON_START - 1));
    EXPECT_FALSE(IsCharmItem(-1));
}

TEST_F(CharmSlotsTest, AmuletSlotsTakeAmuletsAndTheFeatherSlotTheFeather) {
    EXPECT_TRUE(CharmSlotAccepts(0, ITEM_ANTIDOTE_AMULET));
    EXPECT_TRUE(CharmSlotAccepts(CHARM_SLOT_MAX - 1, ITEM_ANTIGOO_AMULET));
    EXPECT_FALSE(CharmSlotAccepts(0, ITEM_DRAN_S_FEATHER));
    EXPECT_TRUE(CharmSlotAccepts(CHARM_FEATHER_SLOT, ITEM_DRAN_S_FEATHER));
    EXPECT_FALSE(CharmSlotAccepts(CHARM_FEATHER_SLOT, ITEM_ANTIDOTE_AMULET));
}

TEST_F(CharmSlotsTest, TheFeatherSlotComesAfterTheAmuletSlots) {
    SetCharmSlotsOpen(1);
    SetCharmFeatherOpen(true);
    EXPECT_EQ(CharmSlotsShown(CHARA_TOAN), 2);
    EXPECT_EQ(CharmSlotIndex(CHARA_TOAN, 0), 0);
    EXPECT_EQ(CharmSlotIndex(CHARA_TOAN, 1), CHARM_FEATHER_SLOT);

    // Closed, the Feather slot stays shown only while it holds the Feather.
    SetCharmFeatherOpen(false);
    EXPECT_EQ(CharmSlotsShown(CHARA_TOAN), 1);
    CharmSlotAt(CHARA_TOAN, CHARM_FEATHER_SLOT) = {ITEM_DRAN_S_FEATHER, 600};
    EXPECT_EQ(CharmSlotsShown(CHARA_TOAN), 2);
    EXPECT_EQ(FindCharmSlot(CHARA_TOAN, ITEM_DRAN_S_FEATHER), CHARM_FEATHER_SLOT);
    EXPECT_EQ(SpendCharm(CHARA_TOAN, ITEM_DRAN_S_FEATHER), CharmGuard::Guarded);
    EXPECT_EQ(CharmSlotAt(CHARA_TOAN, CHARM_FEATHER_SLOT).uses, 599);
}

TEST_F(CharmSlotsTest, AFeatherKeptInAnAmuletSlotMovesToItsOwn) {
    ASSERT_TRUE(CharmSlotsApply(
        R"({"version": 1, "characters": {"toan": [{"item_no": 235, "uses": 900}, {"item_no": 135, "uses": 3}]}})"));
    EXPECT_EQ(CharmSlotAt(CHARA_TOAN, 0).item_no, -1);
    EXPECT_EQ(CharmSlotAt(CHARA_TOAN, 1).item_no, ITEM_ANTIDOTE_AMULET);
    EXPECT_EQ(CharmSlotAt(CHARA_TOAN, CHARM_FEATHER_SLOT).item_no, ITEM_DRAN_S_FEATHER);
    EXPECT_EQ(CharmSlotAt(CHARA_TOAN, CHARM_FEATHER_SLOT).uses, 900);

    // The file keeps it in its own slot.
    const std::string text = CharmSlotsJson();
    ClearCharmSlots();
    ASSERT_TRUE(CharmSlotsApply(text));
    EXPECT_EQ(CharmSlotAt(CHARA_TOAN, CHARM_FEATHER_SLOT).item_no, ITEM_DRAN_S_FEATHER);
}

TEST_F(CharmSlotsTest, TheFeatherCountsItsSecondsAndAnAmuletItsUses) {
    EXPECT_EQ(CharmUsesShown(ITEM_ANTIDOTE_AMULET, 4), 4);
    EXPECT_EQ(CharmUsesShown(ITEM_DRAN_S_FEATHER, 14400), 240);
    EXPECT_EQ(CharmUsesShown(ITEM_DRAN_S_FEATHER, 1), 1);
    EXPECT_EQ(CharmUsesShown(ITEM_ANTIDOTE_AMULET, -2), 0);
}

TEST_F(CharmSlotsTest, OpenIsClampedAndAFilledSlotStaysShown) {
    SetCharmSlotsOpen(9);
    EXPECT_EQ(CharmSlotsOpen(), CHARM_SLOT_MAX);
    SetCharmSlotsOpen(1);
    EXPECT_EQ(CharmSlotsShown(CHARA_TOAN), 1);

    // A charm left in Xiao's third slot keeps it on her page after the mod closes the slots; Toan's has none.
    CharmSlotAt(CHARA_XIAO, 2) = {ITEM_ANTIDOTE_AMULET, 3};
    SetCharmSlotsOpen(0);
    EXPECT_EQ(CharmSlotsShown(CHARA_XIAO), 3);
    EXPECT_EQ(CharmSlotsShown(CHARA_TOAN), 0);
    EXPECT_EQ(FindCharmSlot(CHARA_XIAO, ITEM_ANTIDOTE_AMULET), 2);
    EXPECT_EQ(FindCharmSlot(CHARA_TOAN, ITEM_ANTIDOTE_AMULET), -1);
}

TEST_F(CharmSlotsTest, OnlyTheCharacterHitIsGuardedByTheirOwnCharm) {
    CharmSlotAt(CHARA_XIAO, 0) = {ITEM_ANTIDOTE_AMULET, 2};
    EXPECT_EQ(SpendCharm(CHARA_TOAN, ITEM_ANTIDOTE_AMULET), CharmGuard::None);

    EXPECT_EQ(SpendCharm(CHARA_XIAO, ITEM_ANTIDOTE_AMULET), CharmGuard::Guarded);
    EXPECT_EQ(CharmSlotAt(CHARA_XIAO, 0).uses, 1);
    EXPECT_EQ(SpendCharm(CHARA_XIAO, ITEM_ANTIDOTE_AMULET), CharmGuard::Broke);
    EXPECT_EQ(CharmSlotAt(CHARA_XIAO, 0).item_no, -1);
    EXPECT_EQ(SpendCharm(CHARA_XIAO, ITEM_ANTIDOTE_AMULET), CharmGuard::None);
}

TEST_F(CharmSlotsTest, AWornCharmSellsForLessAndNothingBelowHalf) {
    EXPECT_EQ(WornCharmPrice(200, 4, 4), 200);
    EXPECT_EQ(WornCharmPrice(200, 3, 4), 150);
    EXPECT_EQ(WornCharmPrice(200, 2, 4), 100); // half its uses: half its price
    EXPECT_EQ(WornCharmPrice(200, 1, 4), 0);   // below half: worthless
    EXPECT_EQ(WornCharmPrice(200, 2, 5), 0);
    EXPECT_EQ(WornCharmPrice(200, 3, 5), 120);
    EXPECT_EQ(WornCharmPrice(200, 7, 0), 200); // no full uses known: its price
}

TEST_F(CharmSlotsTest, TheFileKeepsEachCharactersSlots) {
    CharmSlotAt(CHARA_TOAN, 0) = {ITEM_ANTICURSEAMULET, 5};
    CharmSlotAt(CHARA_OSMOND, 3) = {ITEM_ANTI_FREEZE_AMULET, 1};
    const std::string text = CharmSlotsJson();
    ClearCharmSlots();

    ASSERT_TRUE(CharmSlotsApply(text));
    EXPECT_EQ(CharmSlotAt(CHARA_TOAN, 0).item_no, ITEM_ANTICURSEAMULET);
    EXPECT_EQ(CharmSlotAt(CHARA_TOAN, 0).uses, 5);
    EXPECT_EQ(CharmSlotAt(CHARA_TOAN, 1).item_no, -1);
    EXPECT_EQ(CharmSlotAt(CHARA_XIAO, 0).item_no, -1);
    EXPECT_EQ(CharmSlotAt(CHARA_OSMOND, 3).item_no, ITEM_ANTI_FREEZE_AMULET);
    EXPECT_EQ(CharmSlotAt(CHARA_OSMOND, 3).uses, 1);
}

TEST_F(CharmSlotsTest, ACharacterNoModRegistersKeepsTheirCharms) {
    ASSERT_TRUE(CharmSlotsApply(R"({"version": 1, "characters": {"someone": [{"item_no": 133, "uses": 2}]}})"));
    CharmSlotAt(CHARA_TOAN, 0) = {ITEM_ANTIGOO_AMULET, 1};
    const std::string text = CharmSlotsJson();
    EXPECT_NE(text.find("someone"), std::string::npos);
    EXPECT_NE(text.find("toan"), std::string::npos);
}

TEST_F(CharmSlotsTest, ABadFileLeavesEverySlotEmpty) {
    CharmSlotAt(CHARA_TOAN, 0) = {ITEM_ANTICURSEAMULET, 5};
    EXPECT_FALSE(CharmSlotsApply("not json"));
    EXPECT_EQ(CharmSlotAt(CHARA_TOAN, 0).item_no, -1);

    // Something that is not a charm is refused, not put in a slot.
    EXPECT_FALSE(CharmSlotsApply(R"({"version": 1, "characters": {"toan": [{"item_no": 81, "uses": 9}]}})"));
    EXPECT_EQ(CharmSlotAt(CHARA_TOAN, 0).item_no, -1);
}

TEST_F(CharmSlotsTest, SavedBesideTheSaveAndGoneWhenEmpty) {
    const auto directory = std::filesystem::temp_directory_path() / "dc_charm_slots_test";
    std::filesystem::create_directories(directory);

    CharmSlotAt(CHARA_GORO, 1) = {ITEM_ANTIGOO_AMULET, 2};
    ASSERT_TRUE(CharmSlotsSave(directory));
    ClearCharmSlots();
    CharmSlotsLoad(directory);
    EXPECT_EQ(CharmSlotAt(CHARA_GORO, 1).item_no, ITEM_ANTIGOO_AMULET);
    EXPECT_EQ(CharmSlotAt(CHARA_GORO, 1).uses, 2);

    // A save with no charm left writes no file, and loading it empties the slots.
    ClearCharmSlots();
    ASSERT_TRUE(CharmSlotsSave(directory));
    EXPECT_FALSE(std::filesystem::exists(directory / "charms.json"));
    CharmSlotAt(CHARA_TOAN, 0) = {ITEM_ANTIDOTE_AMULET, 1};
    CharmSlotsLoad(directory);
    EXPECT_EQ(CharmSlotAt(CHARA_TOAN, 0).item_no, -1);

    std::filesystem::remove_all(directory);
}

} // namespace

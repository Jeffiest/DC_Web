// Co-op's split of a save (moonmod/coop_save.hpp): the world and a player's own things go across whole, each without
// the other, and a damaged or foreign one changes nothing.
#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <random>
#include <string>

#include "game/savedata.hpp"
#include "moonmod/coop_save.hpp"

namespace {

using dc::game::CSaveData;

// A save as raw bytes: the split reads and writes fields only, so no constructor is needed.
struct RawSave {
    alignas(CSaveData) unsigned char bytes[sizeof(CSaveData)];
    explicit RawSave(unsigned char fill) { std::memset(bytes, fill, sizeof(bytes)); }
    CSaveData       &save() { return *reinterpret_cast<CSaveData *>(bytes); }
    dc::game::CDngStatusData &dng() { return *save().GetDngStatus(); }
};

TEST(MoonModCoopSave, TheWorldCrossesWithoutThePlayersThings) {
    auto host = std::make_unique<RawSave>(0x11);
    auto guest = std::make_unique<RawSave>(0x22);
    host->dng().floor_reached[0] = 9;
    host->dng().party_size = 4;
    host->dng().money = 1234;
    guest->dng().money = 77;
    guest->dng().party_size = 1;
    const std::string world = moonmod::coop::ExportWorld(host->save());
    ASSERT_TRUE(world.starts_with("DCW1"));
    std::string error;
    ASSERT_TRUE(moonmod::coop::ImportWorld(world, guest->save(), error)) << error;
    EXPECT_EQ(guest->dng().floor_reached[0], 9);
    EXPECT_EQ(guest->dng().party_size, 4); // the host's party: the characters the story has brought
    EXPECT_EQ(guest->dng().money, 77);     // the guest keeps their own Gilda
    EXPECT_EQ(moonmod::coop::ExportWorld(guest->save()), world);
}

TEST(MoonModCoopSave, APlayersThingsCrossWithoutTheWorld) {
    auto first = std::make_unique<RawSave>(0x33);
    auto second = std::make_unique<RawSave>(0x44);
    first->dng().money = 500;
    first->dng().max_hp[2] = 180;
    first->dng().equipped_weapon_slot[1] = 3;
    second->dng().floor_reached[3] = 5;
    const std::string player = moonmod::coop::ExportPlayer(first->save());
    ASSERT_TRUE(player.starts_with("DCP1"));
    std::string error;
    ASSERT_TRUE(moonmod::coop::ImportPlayer(player, second->save(), error)) << error;
    EXPECT_EQ(second->dng().money, 500);
    EXPECT_EQ(second->dng().max_hp[2], 180);
    EXPECT_EQ(second->dng().equipped_weapon_slot[1], 3);
    EXPECT_EQ(second->dng().floor_reached[3], 5); // the world stays the receiver's
    EXPECT_EQ(moonmod::coop::ExportPlayer(second->save()), player);
}

TEST(MoonModCoopSave, DamagedOrMistakenDataChangesNothing) {
    auto host = std::make_unique<RawSave>(0x55);
    auto guest = std::make_unique<RawSave>(0x66);
    const std::string world = moonmod::coop::ExportWorld(host->save());
    const std::string player = moonmod::coop::ExportPlayer(host->save());
    const std::string before = moonmod::coop::ExportWorld(guest->save()) + moonmod::coop::ExportPlayer(guest->save());
    std::string error;
    EXPECT_FALSE(moonmod::coop::ImportWorld(player, guest->save(), error)); // a player is not a world
    EXPECT_FALSE(moonmod::coop::ImportPlayer(world, guest->save(), error));
    EXPECT_FALSE(moonmod::coop::ImportWorld(world.substr(0, world.size() / 2), guest->save(), error)); // cut short
    EXPECT_FALSE(moonmod::coop::ImportWorld("DCW1***", guest->save(), error));
    // A field another build sized differently: the first field's size, one byte more.
    std::string other = world;
    other[4 + 3] = other[4 + 3] == 'A' ? 'B' : 'A';
    EXPECT_FALSE(moonmod::coop::ImportWorld(other, guest->save(), error));
    EXPECT_EQ(moonmod::coop::ExportWorld(guest->save()) + moonmod::coop::ExportPlayer(guest->save()), before);
}

struct TempDir {
    std::filesystem::path path = std::filesystem::temp_directory_path() /
                                 ("moonmod-coop-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
                                  std::to_string(std::random_device{}()));
    TempDir() { std::filesystem::create_directories(path); }
    ~TempDir() {
        std::error_code ignored;
        std::filesystem::remove_all(path, ignored);
    }
};

TEST(MoonModCoopSave, TheHostsSaveKeepsItsGuestsThings) {
    using namespace moonmod::coop;
    auto save = std::make_unique<RawSave>(0x10);
    save->dng().money = 900;
    const std::string things = ExportPlayer(save->save());
    AccountsClear();
    std::string error;
    ASSERT_TRUE(AccountSet({"guest-0001", "Xiao", things}, error)) << error;
    ASSERT_TRUE(AccountSet({"guest-0002", "Goro but a much longer name than fits", things}, error)) << error;
    EXPECT_FALSE(AccountSet({"Bad Id", "x", things}, error));
    EXPECT_FALSE(AccountSet({"guest-0003", "x", "not a player"}, error));
    ASSERT_EQ(Accounts().size(), 2u);
    EXPECT_EQ(AccountGet("guest-0002")->name.size(), 24u);

    TempDir slot;
    ASSERT_TRUE(AccountsSave(slot.path));
    AccountsClear();
    EXPECT_EQ(AccountGet("guest-0001"), nullptr);
    AccountsLoad(slot.path);
    ASSERT_NE(AccountGet("guest-0001"), nullptr);
    EXPECT_EQ(AccountGet("guest-0001")->name, "Xiao");
    EXPECT_EQ(AccountGet("guest-0001")->player, things);

    EXPECT_TRUE(AccountRemove("guest-0001"));
    EXPECT_TRUE(AccountRemove("guest-0002"));
    ASSERT_TRUE(AccountsSave(slot.path)); // none left: the file goes
    EXPECT_FALSE(std::filesystem::exists(slot.path / "coop_accounts.json"));
    AccountsLoad(slot.path);
    EXPECT_TRUE(Accounts().empty());

    std::ofstream(slot.path / "coop_accounts.json") << "{not json";
    AccountsLoad(slot.path); // a damaged file keeps no guests, and says so
    EXPECT_TRUE(Accounts().empty());
}

TEST(MoonModCoopSave, ASaveKeepsSixtyFourGuestsAtMost) {
    using namespace moonmod::coop;
    auto save = std::make_unique<RawSave>(0x10);
    const std::string things = ExportPlayer(save->save());
    AccountsClear();
    std::string error;
    for (std::size_t i = 0; i < kAccountsMax; ++i) ASSERT_TRUE(AccountSet({"guest-" + std::to_string(1000 + i), "", things}, error));
    EXPECT_FALSE(AccountSet({"guest-newcomer", "", things}, error));
    EXPECT_TRUE(AccountSet({"guest-1000", "again", things}, error)); // an old guest is updated
    AccountsClear();
}

} // namespace

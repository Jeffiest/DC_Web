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
    for (auto &slot : first->dng().equipped_weapon_slot.data) slot = 0; // a slot a character has, as a real save's is
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

TEST(MoonModCoopSave, OneTownsGeoramaCrossesAlone) {
    auto editor = std::make_unique<RawSave>(0x70);
    auto other = std::make_unique<RawSave>(0x08);
    const std::string before_world = moonmod::coop::ExportWorld(other->save());
    const std::string town = moonmod::coop::ExportTown(editor->save(), 2);
    ASSERT_TRUE(town.starts_with("DCT12:"));
    std::string error;
    EXPECT_FALSE(moonmod::coop::ImportTown(town, 3, other->save(), error)); // another town's
    EXPECT_FALSE(moonmod::coop::ImportTown(town, 9, other->save(), error));
    EXPECT_EQ(moonmod::coop::ExportWorld(other->save()), before_world);
    ASSERT_TRUE(moonmod::coop::ImportTown(town, 2, other->save(), error)) << error;
    EXPECT_EQ(moonmod::coop::ExportTown(other->save(), 2), town);
    EXPECT_NE(moonmod::coop::ExportTown(other->save(), 1), moonmod::coop::ExportTown(editor->save(), 1)); // only town 2 came
    EXPECT_TRUE(moonmod::coop::ExportTown(other->save(), 7).empty());
}

// A town as georama.town_export writes it, made and read here: field 1 the record, field 2 the chips.
struct Town {
    dc::game::SV_GEORAMA_DATA geo{};
    std::int16_t              chips[128]{};
    Town() {
        for (auto &part : geo.placed_parts) part.part_id = -1;
    }
    void Place(std::int16_t id, float x) {
        for (auto &part : geo.placed_parts) {
            if (part.part_id == -1) {
                part = {id, 0, x, 0.0f, 0.0f};
                return;
            }
        }
    }
    std::vector<std::int16_t> Placed() const {
        std::vector<std::int16_t> out;
        for (const auto &part : geo.placed_parts) {
            if (part.part_id == -1) break;
            out.push_back(part.part_id);
        }
        return out;
    }
    std::string Text(int town) const {
        std::string bytes;
        const auto field = [&bytes](std::uint16_t id, const void *data, std::uint32_t size) {
            bytes.push_back(static_cast<char>(id & 0xFF));
            bytes.push_back(static_cast<char>(id >> 8));
            for (int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>((size >> shift) & 0xFF));
            bytes.append(static_cast<const char *>(data), size);
        };
        field(1, &geo, sizeof(geo));
        field(2, chips, sizeof(chips));
        static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
        std::string out = "DCT1" + std::to_string(town) + ":";
        for (std::size_t i = 0; i < bytes.size(); i += 3) {
            const auto     at = [&bytes](std::size_t k) { return static_cast<unsigned>(static_cast<unsigned char>(bytes[k])); };
            const unsigned v = (at(i) << 16) | ((i + 1 < bytes.size() ? at(i + 1) : 0) << 8) | (i + 2 < bytes.size() ? at(i + 2) : 0);
            out.push_back(kTable[(v >> 18) & 63]);
            out.push_back(kTable[(v >> 12) & 63]);
            out.push_back(i + 1 < bytes.size() ? kTable[(v >> 6) & 63] : '=');
            out.push_back(i + 2 < bytes.size() ? kTable[v & 63] : '=');
        }
        return out;
    }
    static Town Read(const std::string &text) {
        std::string bytes;
        unsigned    value = 0;
        int         bits = 0;
        for (char c : text.substr(text.find(':') + 1)) {
            int digit = c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '+' ? 62 : c == '/' ? 63 : -1;
            if (digit < 0) break;
            value = (value << 6) | static_cast<unsigned>(digit);
            bits += 6;
            if (bits >= 8) {
                bits -= 8;
                bytes.push_back(static_cast<char>((value >> bits) & 0xFF));
            }
        }
        Town town;
        std::memcpy(&town.geo, bytes.data() + 6, sizeof(town.geo));
        std::memcpy(town.chips, bytes.data() + 6 + sizeof(town.geo) + 6, sizeof(town.chips));
        return town;
    }
};

TEST(MoonModCoopSave, TwoBuildingInOneTownBothKeepTheirWork) {
    auto host = std::make_unique<RawSave>(0x00);
    std::string error;
    Town start;
    start.Place(10, 1.0f); // a house
    start.Place(11, 2.0f); // a mill
    ASSERT_TRUE(moonmod::coop::ImportTown(start.Text(2), 2, host->save(), error)) << error;
    const std::string base = moonmod::coop::ExportTown(host->save(), 2); // the guest opens the editor here

    // Meanwhile the host puts up a shop and paints chip 6.
    Town hosts = Town::Read(base);
    hosts.Place(12, 3.0f);
    hosts.chips[6] = 77;
    ASSERT_TRUE(moonmod::coop::ImportTown(hosts.Text(2), 2, host->save(), error)) << error;

    // The guest took the house down, put up a well, painted chip 5 and finished plot 4.
    Town guests = Town::Read(base);
    guests.geo.placed_parts[0] = guests.geo.placed_parts[1];
    guests.geo.placed_parts[1].part_id = -1;
    guests.Place(13, 4.0f);
    guests.chips[5] = 55;
    guests.geo.parts_info[4].placed = 1;

    ASSERT_TRUE(moonmod::coop::MergeTown(base, guests.Text(2), 2, host->save(), error)) << error;
    const Town merged = Town::Read(moonmod::coop::ExportTown(host->save(), 2));
    EXPECT_EQ(merged.Placed(), (std::vector<std::int16_t>{11, 12, 13})); // the house gone, the shop and the well both up
    EXPECT_EQ(merged.chips[5], 55);
    EXPECT_EQ(merged.chips[6], 77);
    EXPECT_EQ(merged.geo.parts_info[4].placed, 1);

    EXPECT_FALSE(moonmod::coop::MergeTown(base, guests.Text(3), 2, host->save(), error)); // another town's
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

TEST(MoonModCoopSave, ANameThatIsNotTextNeverBreaksTheHostsSave) {
    using namespace moonmod::coop;
    auto save = std::make_unique<RawSave>(0x10);
    const std::string things = ExportPlayer(save->save());
    AccountsClear();
    std::string error;
    ASSERT_TRUE(AccountSet({"guest-0001", "\xFF\xFE\x80 broken", things}, error)) << error;
    ASSERT_TRUE(AccountSet({"guest-0002", std::string(23, 'a') + "\xC3\xA9", things}, error)) << error; // cut mid-character
    EXPECT_FALSE(AccountSet({"guest-0003", "x", things + "\xFF"}, error));
    TempDir slot;
    EXPECT_NO_THROW(EXPECT_TRUE(AccountsSave(slot.path)));
    AccountsClear();
    AccountsLoad(slot.path);
    ASSERT_NE(AccountGet("guest-0001"), nullptr);
    ASSERT_NE(AccountGet("guest-0002"), nullptr);
    EXPECT_EQ(AccountGet("guest-0002")->name, std::string(23, 'a'));
    EXPECT_EQ(AccountGet("guest-0001")->player, things);
    AccountsClear();
}

TEST(MoonModCoopSave, AGuestsFileThatCannotBeReadIsNeverThrownAway) {
    using namespace moonmod::coop;
    auto save = std::make_unique<RawSave>(0x10);
    const std::string things = ExportPlayer(save->save());
    TempDir slot;
    std::ofstream(slot.path / "coop_accounts.json") << "{not json";
    AccountsClear();
    AccountsLoad(slot.path);
    EXPECT_TRUE(Accounts().empty());
    ASSERT_TRUE(AccountsSave(slot.path)); // no guests known: the file that might hold some stays
    EXPECT_TRUE(std::filesystem::exists(slot.path / "coop_accounts.json"));
    std::string error;
    ASSERT_TRUE(AccountSet({"guest-0001", "Xiao", things}, error)) << error;
    ASSERT_TRUE(AccountsSave(slot.path)); // new guests: the old file is put aside, not written over
    EXPECT_TRUE(std::filesystem::exists(slot.path / "coop_accounts.json.bad"));
    AccountsClear();
    AccountsLoad(slot.path);
    EXPECT_NE(AccountGet("guest-0001"), nullptr);

    // Right shape, wrong kinds: nothing is taken, nothing throws.
    std::ofstream(slot.path / "coop_accounts.json") << R"({"format":1,"accounts":{"guest-0009":5,"guest-0008":{"name":[],"player":{}},"guest-0007":{"name":"x"}}})";
    AccountsClear();
    EXPECT_NO_THROW(AccountsLoad(slot.path));
    EXPECT_TRUE(Accounts().empty());
    AccountsClear();
}

TEST(MoonModCoopSave, AnotherGamesThingsCannotPointOutsideTheirOwn) {
    auto host = std::make_unique<RawSave>(0x00);
    auto guest = std::make_unique<RawSave>(0x00);
    host->dng().equipped_weapon_slot[0] = 100;
    host->dng().equipped_weapon_slot[1] = -77;
    host->dng().equipped_weapon_slot[2] = 5;
    host->dng().equipped_weapon_slot[3] = -1;
    std::string error;
    ASSERT_TRUE(moonmod::coop::ImportPlayer(moonmod::coop::ExportPlayer(host->save()), guest->save(), error)) << error;
    EXPECT_EQ(guest->dng().equipped_weapon_slot[0], 0);
    EXPECT_EQ(guest->dng().equipped_weapon_slot[1], 0);
    EXPECT_EQ(guest->dng().equipped_weapon_slot[2], 5);
    EXPECT_EQ(guest->dng().equipped_weapon_slot[3], -1);
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

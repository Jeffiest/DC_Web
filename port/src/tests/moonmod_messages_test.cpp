#include <gtest/gtest.h>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include "moonmod/messages_moonmod.hpp"

namespace {
namespace fs = std::filesystem;
namespace messages = moonmod::messages;

// Two players' saves pointing at one shared folder (each save's messages-folder.txt), as a sync service would join them.
struct SharedFolder {
    fs::path root, alice, bob, shared;
    SharedFolder() {
        root = fs::temp_directory_path() /
               ("moonmod-messages-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
        alice = root / "alice";
        bob = root / "bob";
        shared = root / "shared";
        for (const fs::path &save : {alice, bob}) {
            fs::create_directories(save / "moonmod");
            std::ofstream(save / "moonmod" / "messages-folder.txt") << shared.string() << "\n";
        }
    }
    ~SharedFolder() {
        std::error_code ec;
        fs::remove_all(root, ec);
    }
};

TEST(MoonModMessages, TwoPlayersShareAFolderAndRateEachOther) {
    SharedFolder f;
    EXPECT_TRUE(messages::FolderFor(f.alice).shared);
    std::string id, error;
    ASSERT_TRUE(messages::Post(f.alice, "moonmod.vigil", "vigil:d0:f3:shrine", "Beware the ambush ahead", id, error)) << error;
    std::vector<messages::Message> seen;
    ASSERT_TRUE(messages::List(f.bob, "moonmod.vigil", {"vigil:d0:f3:shrine"}, seen, error)) << error;
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].text, "Beware the ambush ahead");
    EXPECT_FALSE(seen[0].mine);
    ASSERT_TRUE(messages::Rate(f.bob, seen[0].id, 1, error)) << error;
    EXPECT_FALSE(messages::Rate(f.alice, id, 1, error)); // not one's own
    ASSERT_TRUE(messages::List(f.alice, "moonmod.vigil", {"vigil:d0:f3:shrine"}, seen, error));
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_TRUE(seen[0].mine);
    EXPECT_EQ(seen[0].good, 1);
    // another mod's players, and another place, see none of it
    ASSERT_TRUE(messages::List(f.bob, "other.mod", {"vigil:d0:f3:shrine"}, seen, error));
    EXPECT_TRUE(seen.empty());
    ASSERT_TRUE(messages::List(f.bob, "moonmod.vigil", {"vigil:d0:f4:shrine"}, seen, error));
    EXPECT_TRUE(seen.empty());
    EXPECT_FALSE(messages::Remove(f.bob, id, error)); // only one's own
    EXPECT_TRUE(messages::Remove(f.alice, id, error)) << error;
    ASSERT_TRUE(messages::List(f.bob, "moonmod.vigil", {"vigil:d0:f3:shrine"}, seen, error));
    EXPECT_TRUE(seen.empty());
}

TEST(MoonModMessages, AnotherPlayersFilesAreReadAsUntrustedData) {
    SharedFolder f;
    fs::create_directories(f.shared);
    const std::string place = "vigil:d0:f1:shrine";
    // malformed, oversized, a forged id and text that isn't plain text: all left out
    std::ofstream(f.shared / "0123456789ab-00000001.json") << "{not json";
    std::ofstream(f.shared / "0123456789ab-00000002.json")
        << R"({"v":1,"id":"0123456789ab-00000002","mod":"moonmod.vigil","place":")" << place << R"(","text":")" << std::string(3000, 'a') << R"(","at":1})";
    std::ofstream(f.shared / "0123456789ab-00000003.json")
        << R"({"v":1,"id":"ffffffffffff-00000003","mod":"moonmod.vigil","place":")" << place << R"(","text":"forged","at":1})";
    std::ofstream(f.shared / "0123456789ab-00000004.json")
        << R"({"v":1,"id":"0123456789ab-00000004","mod":"moonmod.vigil","place":")" << place << R"(","text":"bell\u0007","at":1})";
    std::ofstream(f.shared / "0123456789ab-00000005.json")
        << R"({"v":1,"id":"0123456789ab-00000005","mod":"moonmod.vigil","place":")" << place << R"(","text":"Try rolling","at":1})";
    std::ofstream(f.shared / "0123456789ab.ratings.json") << R"({"v":1,"ratings":{"0123456789ab-00000005":1}})"; // their own: not counted
    std::ofstream(f.shared / "../escape.json") << "{}";
    std::vector<messages::Message> seen;
    std::string                    error;
    ASSERT_TRUE(messages::List(f.bob, "moonmod.vigil", {place}, seen, error)) << error;
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].text, "Try rolling");
    EXPECT_EQ(seen[0].good, 0);
}

TEST(MoonModMessages, TextAndPlacesAreChecked) {
    SharedFolder f;
    std::string id, error;
    EXPECT_FALSE(messages::Post(f.alice, "m", "Bad Place", "hi", id, error));
    EXPECT_FALSE(messages::Post(f.alice, "m", "ok", std::string(121, 'a'), id, error));
    EXPECT_FALSE(messages::Post(f.alice, "m", "ok", "   ", id, error));
    EXPECT_FALSE(messages::Post(f.alice, "m", "ok", "line\nbreak", id, error));
    EXPECT_FALSE(messages::Post(f.alice, "m", "../x", "hi", id, error));
    EXPECT_TRUE(messages::Post(f.alice, "m", "ok", "hi", id, error)) << error;
    EXPECT_FALSE(messages::FolderFor(f.root / "nobody").shared);
}

} // namespace

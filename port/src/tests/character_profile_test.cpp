#include <gtest/gtest.h>
#include <nlohmann/json.hpp>

#include <string>
#include <vector>

#include "../../../tools/dcdata/character_profile.hpp"

namespace dc::test {

namespace {

using nlohmann::json;

json Read(std::string_view alias, std::string_view text) {
    return json::parse(dcdata::profile::Read(alias, text));
}

std::vector<json> Commands(const json &document, std::string_view name) {
    std::vector<json> found;
    for (const json &command : document["commands"]) {
        if (command["command"] == name) {
            found.push_back(command);
        }
    }
    return found;
}

// 立ち, 走り and バトル （元気） in Shift-JIS, as the disc's comments are written.
constexpr std::string_view kStand = "\x97\xa7\x82\xbf";
constexpr std::string_view kRun = "\x91\x96\x82\xe8";
constexpr std::string_view kBattle = "\x83\x6f\x83\x67\x83\x8b\x20\x81\x69\x8c\xb3\x8b\x43\x81\x6a";

std::string Hex(std::string_view bytes) {
    std::string out;
    for (unsigned char c : bytes) {
        out += std::format("{:02x}", c);
    }
    return out;
}

TEST(CharacterProfile, KeysTakeTheirNamesFromTheLineCommentAndKeepTheirFrames) {
    const std::string       cfg = std::string("MODEL \"c01d.mds\"\r\n") +
                                  "MOTION 0, \"c01dbas.mot\", \"c01dbas.bbp\", \"c01dbas.wgt\"\r\n" +
                                  "KEY_START 0\r\n" +
                                  "//KEY start key, end key, step\r\n" +
                                  "KEY\t10,\t20,\t0.05\t//" + std::string(kStand) + "\t\t0\r\n" +
                                  "KEY\t70,\t90,\t0.6,\t//" + std::string(kRun) + "\t1\r\n" +
                                  "KEY\t229,\t250\t0.3\t//" + std::string(kBattle) + "   2\r\n" +
                                  "KEY\t300,\t310,\t0.5\t//\t14\r\n" +
                                  "KEY\t320,\t330,\t0.5\r\n" +
                                  "MOTION_END\r\n";
    const json              document = Read("chara/c01d.chr/info.cfg", cfg);
    const std::vector<json> keys = Commands(document, "KEY");
    ASSERT_EQ(keys.size(), 5u);
    EXPECT_EQ(keys[0]["args"][0]["bytes"], Hex(kStand));
    EXPECT_EQ(keys[0]["args"][1]["value"], 10);
    EXPECT_EQ(keys[0]["args"][2]["value"], 20);
    EXPECT_FLOAT_EQ(keys[0]["args"][3]["value"].get<float>(), 0.05f);
    EXPECT_EQ(keys[1]["args"][0]["bytes"], Hex(kRun));
    // A missing comma still separates, as the game's reader splits words on blanks too; a name
    // keeps the blank inside it.
    EXPECT_EQ(keys[2]["args"][0]["bytes"], Hex(kBattle));
    EXPECT_EQ(keys[2]["args"][2]["value"], 250);
    // A comment that is only the index, or none, leaves the key unnamed.
    EXPECT_EQ(keys[3]["args"][0]["type"], "none");
    EXPECT_EQ(keys[4]["args"][0]["type"], "none");
    const json motion = Commands(document, "MOTION").at(0);
    ASSERT_EQ(motion["args"].size(), 4u);
    EXPECT_EQ(motion["args"][3]["bytes"], Hex("c01dbas.wgt"));
}

TEST(CharacterProfile, TextureAnimationsKeepTheirPlaceAmongTheCommands) {
    const std::string        cfg = "IMG 0,\"c01d01.img\"\nIMG_END\nTEX_ANIME 1,1\n"
                                   "TEX_ANIME_DATA \"c01d11\",0,0,128,48,\"c01d01\",0,27,100,0\t//eyes\n"
                                   "TEX_ANIME_END\nMATERIAL_ANIME 0\nVERTEX_ANIME 1\nMODEL \"c01d.mds\"\n";
    const json               document = Read("info.cfg", cfg);
    std::vector<std::string> order;
    for (const json &command : document["commands"]) {
        order.push_back(command["command"]);
    }
    EXPECT_EQ(order, (std::vector<std::string>{"IMG", "IMG_END", "TEX_ANIME", "TEX_ANIME_DATA", "TEX_ANIME_END",
                                               "VERTEX_ANIME", "MODEL"}));
    const json data = Commands(document, "TEX_ANIME_DATA").at(0);
    ASSERT_EQ(data["args"].size(), 10u);
    EXPECT_EQ(data["args"][5]["bytes"], Hex("c01d01"));
    EXPECT_EQ(data["args"][8]["value"], 100);
}

TEST(CharacterProfile, OtherConfigurationsAreNotProfiles) {
    EXPECT_EQ(dcdata::profile::Read("dun/d05main_a.cfg", "PT_BASE \"d05g01_0.mds\",0\nPT_COLS \"d05g01_a.mds\",0\n"),
              "null");
    EXPECT_EQ(dcdata::profile::Read("chara/c01d.chr/c01d.mds", "MODEL \"x.mds\"\n"), "null");
}

TEST(CharacterProfile, ClothReadsItsArgumentsAcrossLines) {
    const std::string clo = "SIZE\t4,\t5\nFRAME\t\"cloth1\"\nK\t0.08,\t0.08,\t0.08\nBOUND\t\"jnt12_1\"\n"
                            "\t0.0,\t1.0,\t0.0\n\t2.15,\t-0.45,\t1.0\n\t2.15,\t-0.45,\t-1.0\n"
                            "\t3.66,\t3.31,\t2.73\n\t0.7\n";
    const json        document = Read("chara/c01d.chr/turban.clo", clo);
    const json        bound = Commands(document, "BOUND").at(0);
    ASSERT_EQ(bound["args"].size(), 14u);
    EXPECT_FLOAT_EQ(bound["args"][5]["value"].get<float>(), -0.45f);
    EXPECT_FLOAT_EQ(bound["args"][13]["value"].get<float>(), 0.7f);
    EXPECT_EQ(Commands(document, "SIZE").at(0)["args"][1]["value"], 5);
}

TEST(CharacterProfile, ArgumentsFollowTheGamesChecks) {
    // ReadInfo takes no sign in an integer, and runs nothing for a command cut off by the end of
    // the file.
    const json document = Read("info.cfg", "MODEL \"a.mds\"\nKEY_START -1\nFOOT 211,-1\nBODY_SIZE 1,2");
    EXPECT_EQ(Commands(document, "KEY_START").at(0)["args"][0]["type"], "invalid");
    EXPECT_FLOAT_EQ(Commands(document, "FOOT").at(0)["args"][1]["value"].get<float>(), -1.0f);
    EXPECT_TRUE(Commands(document, "BODY_SIZE").empty());
}

} // namespace

} // namespace dc::test

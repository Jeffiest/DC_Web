#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#include "shop.hpp"
#include "battlemenu.hpp"
#include "chara_extras_save.hpp"
#include "mod_atla.hpp"
#include "menu_inventory.hpp"
#include "weapon_buildup.hpp"
#include "btactstatus.hpp"
#include "dun/gameloop.hpp"
#include "itemdata.hpp"
#include "main.hpp"
#include "memorycardaccess.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "moonmod/runtime.hpp"
#include "playable_chara.hpp"
#include "savedata.hpp"
#include "userstatus.hpp"

// MoonMod's playable characters past the game's six (playable_chara.hpp): a mod registers them, each takes the next
// index, their status lives outside the save, the party and the mod API take them, and no save ever holds them.
// Two are registered here: Opar (fights as Toan, the Gauntlet's) and Kit (a test character who fights as Ruby).

using namespace dc::game;

namespace {

constexpr int kOpar = 6;
constexpr int kKit = 7;

PlayableChara Entry(const char *id, const char *name, const char *pack, int fights_as, int hp, int defense) {
    PlayableChara entry;
    entry.id = id;
    entry.name = name;
    entry.pack = pack;
    entry.fights_as = fights_as;
    entry.hp = hp;
    entry.defense = defense;
    return entry;
}

class PlayableRoster : public ::testing::Test {
protected:
    void SetUp() override {
        ClearPlayableCharas();
        std::string error;
        ASSERT_EQ(RegisterPlayableChara(Entry("opar", "Opar", "dun/mainchara/c07d.chr", CHARA_TOAN, 90, 21), error), kOpar) << error;
        ASSERT_EQ(RegisterPlayableChara(Entry("kit", "Kit", "dun/mainchara/c08z.chr", CHARA_RUBY, 60, 9), error), kKit) << error;
    }
    void TearDown() override {
        ClearCharaJoined();
        moonmod::ResetChronicleCharactersForTests();
    }
};

struct SaveScope {
    CSaveData                 *previous = SaveData;
    std::unique_ptr<CSaveData> save = std::make_unique<CSaveData>();
    SaveScope() {
        save->Initialize();
        SaveData = save.get();
    }
    ~SaveScope() { SaveData = previous; }
    CDngStatusData *Status() { return save->GetDngStatus(); }
};

std::vector<unsigned char> Bytes(const CSaveData &save) {
    const auto *at = reinterpret_cast<const unsigned char *>(&save);
    return {at, at + sizeof(CSaveData)};
}

int g_switch_target = -1;

void FakeSwitch(int character_id, int) {
    g_switch_target = character_id;
    SaveData->GetDngStatus()->cur_chara = static_cast<s8>(character_id);
}

} // namespace

TEST_F(PlayableRoster, TheSaveKeepsItsSixAndItsLayout) {
    static_assert(sizeof(CUserStatus) == 0x8B20);
    static_assert(sizeof(CDngStatusData) == 0x8C9C);
    static_assert(sizeof(CUserStatus::hp) == 6 * sizeof(s16));
    static_assert(sizeof(CUserStatus::chara_weapons) == 6 * 11 * sizeof(WEAPON_HAVE));
    static_assert(SAVE_CHARA_COUNT == 6 && CHARA_MAX == 16);
    EXPECT_EQ(offsetof(CUserStatus, max_hp), 6u);
    EXPECT_EQ(offsetof(CUserStatus, hp), 0x12u);
}

TEST_F(PlayableRoster, EachTakesTheNextIndexAndKeepsIt) {
    EXPECT_EQ(CharaCount(), 8);
    EXPECT_EQ(FindPlayableChara("opar"), kOpar);
    EXPECT_EQ(FindPlayableChara("kit"), kKit);
    EXPECT_EQ(FindPlayableChara("ruby"), CHARA_RUBY);
    EXPECT_EQ(FindPlayableChara("nobody"), -1);
    EXPECT_STREQ(CharaIdName(kKit), "kit");
    EXPECT_EQ(CharaIdName(8), nullptr);
    EXPECT_EQ(ExtraChara(CHARA_OSMOND), nullptr);
    EXPECT_STREQ(CharaPackPath(kOpar), "dun/mainchara/c07d.chr");
    EXPECT_STREQ(CharaPackPath(CHARA_XIAO), "dun/mainchara/c04b.chr");
    EXPECT_TRUE(IsPlayableCharaPack("dun/mainchara/c08z.chr"));
    EXPECT_FALSE(IsPlayableCharaPack("dun/mainchara/c01d.chr"));

    // Registering the same id again updates him in place.
    std::string error;
    EXPECT_EQ(RegisterPlayableChara(Entry("opar", "Opar", "dun/mainchara/c07d.chr", CHARA_RUBY, 95, 20), error), kOpar);
    EXPECT_EQ(CombatChara(kOpar), CHARA_RUBY);
    EXPECT_EQ(CharaCount(), 8);

    // Bad entries are refused and change nothing.
    EXPECT_EQ(RegisterPlayableChara(Entry("Bad Id", "Bad", "dun/mainchara/c09a.chr", CHARA_TOAN, 70, 3), error), -1);
    EXPECT_EQ(RegisterPlayableChara(Entry("ruby", "Ruby", "dun/mainchara/c09a.chr", CHARA_TOAN, 70, 3), error), -1);
    EXPECT_EQ(RegisterPlayableChara(Entry("nine", "N1ne", "dun/mainchara/c09a.chr", CHARA_TOAN, 70, 3), error), -1);
    EXPECT_EQ(RegisterPlayableChara(Entry("nine", "Nine", "dun/c09a.chr", CHARA_TOAN, 70, 3), error), -1);
    EXPECT_EQ(RegisterPlayableChara(Entry("nine", "Nine", "dun/mainchara/c08z.chr", CHARA_TOAN, 70, 3), error), -1);
    EXPECT_EQ(RegisterPlayableChara(Entry("nine", "Nine", "dun/mainchara/c09a.chr", kOpar, 70, 3), error), -1);
    EXPECT_EQ(RegisterPlayableChara(Entry("nine", "Nine", "dun/mainchara/c09a.chr", CHARA_TOAN, 0, 3), error), -1);
    EXPECT_EQ(CharaCount(), 8);

    // Room for CHARA_MAX in all; the next is refused.
    for (int chara = 8; chara < CHARA_MAX; ++chara) {
        const std::string id = "extra" + std::to_string(chara);
        const std::string pack = "dun/mainchara/x" + std::to_string(chara) + ".chr";
        EXPECT_EQ(RegisterPlayableChara(Entry(id.c_str(), "Extra", pack.c_str(), CHARA_GORO, 70, 3), error), chara) << error;
    }
    EXPECT_EQ(CharaCount(), CHARA_MAX);
    EXPECT_EQ(RegisterPlayableChara(Entry("onemore", "More", "dun/mainchara/x99.chr", CHARA_GORO, 70, 3), error), -1);
}

TEST_F(PlayableRoster, TheirStatusNeverTouchesTheSave) {
    SaveScope scope;
    CDngStatusData *status = scope.Status();
    const std::vector<unsigned char> before = Bytes(*scope.save);

    for (int chara : {kOpar, kKit}) {
        status->max_hp[chara] = 90;
        status->hp[chara] = 41;
        status->water_now[chara] = 12.0f;
        status->ailments[chara] = AILMENT_KNOCKED_OUT;
        status->equipped_weapon_slot[chara] = 0;
        status->chara_weapons[chara][0].item_no = chara == kOpar ? ITEM_WEAPON_DAGGER : ITEM_WEAPON_GOLD_RING;
        status->AddNowLife(chara, chara == kOpar ? 9 : 4, 0.0f);
    }

    EXPECT_EQ(Bytes(*scope.save), before);
    EXPECT_EQ(status->hp[kOpar], 50);
    EXPECT_EQ(status->hp[kKit], 45);
    EXPECT_EQ(status->chara_weapons[kOpar][0].item_no, ITEM_WEAPON_DAGGER);
    EXPECT_EQ(status->chara_weapons[kKit][0].item_no, ITEM_WEAPON_GOLD_RING);
    // Each has his own entries, apart from the six's and from each other's.
    EXPECT_NE(&status->hp[kOpar], &status->hp[kKit]);
    EXPECT_NE(&status->hp[kOpar], &status->hp[CHARA_OSMOND]);
    EXPECT_EQ(status->hp[CHARA_TOAN], 70);
    for (int chara : {kOpar, kKit}) {
        status->ailments[chara] = 0;
        status->chara_weapons[chara][0] = {};
        status->equipped_weapon_slot[chara] = -1;
    }
}

TEST_F(PlayableRoster, ANewGameStartsThemWithTheirOwnHealthAndDefence) {
    SaveScope scope;
    CDngStatusData *status = scope.Status();
    EXPECT_EQ(status->max_hp[kOpar], 90);
    EXPECT_EQ(status->defense[kOpar], 21);
    EXPECT_EQ(status->max_hp[kKit], 60);
    EXPECT_EQ(status->defense[kKit], 9);
    EXPECT_EQ(status->max_hp[CHARA_UNGAGA], 110);
}

TEST_F(PlayableRoster, TheirLifeAndWaterGaugesStep) {
    SaveScope scope;
    CDngStatusData *status = scope.Status();
    status->step_disable = 0;
    for (int chara : {kOpar, kKit}) {
        status->max_hp[chara] = 90;
        status->hp[chara] = 90;
        status->water_max[chara] = 30.0f;
        status->water_now[chara] = 30.0f;
        status->AddNowLife(chara, -30, 10.0f);
        status->AddDrink(chara, chara == kOpar ? -10 : -5, 10.0f);
    }
    for (int frame = 0; frame < 200; ++frame) status->Step(1);
    EXPECT_EQ(status->hp[kOpar], 60);
    EXPECT_EQ(status->hp[kKit], 60);
    EXPECT_FLOAT_EQ(status->water_now[kOpar], 20.0f);
    EXPECT_FLOAT_EQ(status->water_now[kKit], 25.0f);
}

TEST_F(PlayableRoster, AStoryPartyUsesSixSlotsAndALargerPartyItsOwn) {
    EXPECT_EQ(CharaSlotsInPlay(1), 6);
    EXPECT_EQ(CharaSlotsInPlay(6), 6);
    EXPECT_EQ(CharaSlotsInPlay(7), 7);
    EXPECT_EQ(CharaSlotsInPlay(8), 8);
    EXPECT_EQ(CharaSlotsInPlay(9), 8); // no more slots than characters
    // Each fights as the character he names; the six fight as themselves.
    EXPECT_EQ(CombatChara(kOpar), CHARA_TOAN);
    EXPECT_EQ(CombatChara(kKit), CHARA_RUBY);
    for (int chara = CHARA_TOAN; chara <= CHARA_OSMOND; ++chara) EXPECT_EQ(CombatChara(chara), chara);
    EXPECT_EQ(GetDefaultWeaponNo(kOpar), GetDefaultWeaponNo(CHARA_TOAN));
    EXPECT_EQ(GetDefaultWeaponNo(kKit), GetDefaultWeaponNo(CHARA_RUBY));
}

TEST_F(PlayableRoster, TheirNamesComeFromTheRegistryAndNoSave) {
    SaveScope scope;
    const s16 *opar = scope.save->GetCharaName(kOpar);
    const s16 *kit = scope.save->GetCharaName(kKit);
    ASSERT_NE(opar, nullptr);
    ASSERT_NE(kit, nullptr);
    // The name entry's letters: capitals from 162 (A), small from 188 (a), as the six's defaults.
    EXPECT_EQ(opar[0], 162 + ('O' - 'A'));
    EXPECT_EQ(opar[1], 188 + ('p' - 'a'));
    EXPECT_EQ(opar[2], 188 + ('a' - 'a'));
    EXPECT_EQ(opar[3], 188 + ('r' - 'a'));
    EXPECT_EQ(opar[4], 0);
    EXPECT_EQ(kit[0], 162 + ('K' - 'A'));
    EXPECT_EQ(kit[2], 188 + ('t' - 'a'));
    EXPECT_EQ(kit[3], 0);
    EXPECT_EQ(scope.save->GetCharaName(8), nullptr);
    // Their text codes and system name lines sit apart from the six's and from each other.
    EXPECT_EQ(CharaNameCode(CHARA_TOAN), -0x506);
    EXPECT_EQ(CharaNameCode(CHARA_OSMOND), -0x501);
    EXPECT_EQ(CharaNameCode(kOpar), -0x507);
    EXPECT_EQ(CharaNameCode(kKit), -0x508);
    EXPECT_EQ(CharaNameLine(CHARA_OSMOND), 55);
    EXPECT_EQ(CharaNameLine(kKit), EXTRA_CHARA_NAME_LINE + kKit);
}

TEST_F(PlayableRoster, ASaveImageNeverCountsThem) {
    SaveScope scope;
    CDngStatusData *status = scope.Status();
    status->party_size = 8;
    status->cur_chara = kKit;
    status->hp[kKit] = 33;

    auto              buffer = std::make_unique<std::array<char, 0x30000>>();
    CMemoryCardAccess card;
    card.Initialize();
    card.SetBuff(buffer->data());
    const auto *image = reinterpret_cast<const CSaveData *>(
        reinterpret_cast<const char *>(card.save_buffer));
    CDngStatusData *written = const_cast<CSaveData *>(image)->GetDngStatus();
    EXPECT_EQ(written->party_size, 6);
    EXPECT_EQ(written->cur_chara, CHARA_TOAN);
    // The game's own state is as it was: only the written image leaves them out.
    EXPECT_EQ(status->party_size, 8);
    EXPECT_EQ(status->cur_chara, kKit);
}

TEST_F(PlayableRoster, TheModApiRegistersListsAndSwitchesToThem) {
    const s32           previous_mode = mode;
    const s32           previous_map = selectMapNo;
    const BT_ACT_STATUS previous_action = BtActStatus;
    SaveScope           scope;
    CDngStatusData     *status = scope.Status();
    status->cur_georama = 0;
    status->cur_floor = 1;
    status->cur_chara = CHARA_TOAN;
    status->res_limit_zone_current = -1;
    mode = GAME_MODE_DUNGEON;
    selectMapNo = 0;
    BtActStatus = {};
    g_switch_target = -1;
    moonmod::SetChronicleCharacterSwitchRoutineForTests(FakeSwitch);
    struct Restore {
        s32           mode, map;
        BT_ACT_STATUS action;
        ~Restore() {
            moonmod::SetChronicleCharacterSwitchRoutineForTests(nullptr);
            ::mode = mode;
            selectMapNo = map;
            BtActStatus = action;
        }
    } restore{previous_mode, previous_map, previous_action};

    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *register_playable = moonmod::FindApiOperation("characters.register_playable");
    const auto *party_list = moonmod::FindApiOperation("party.list");
    const auto *set_active = moonmod::FindApiOperation("party.set_active");
    const auto *player_get = moonmod::FindApiOperation("player.get");
    const auto *characters_get = moonmod::FindApiOperation("characters.get");
    const auto *give = moonmod::FindApiOperation("weapons.give");
    ASSERT_NE(register_playable, nullptr);
    ASSERT_NE(party_list, nullptr);
    ASSERT_NE(set_active, nullptr);
    ASSERT_NE(player_get, nullptr);
    ASSERT_NE(characters_get, nullptr);
    ASSERT_NE(give, nullptr);
    const char *mod = "test.moonmod.roster";

    // A third, through the API: he fights as Goro and starts with Goro's health unless told.
    const moonmod::ApiCallResult added = config.api_handler(
        mod, *register_playable,
        moonmod::Json::array({{{"id", "moss"}, {"name", "Moss"}, {"pack", "dun/mainchara/c09m.chr"}, {"fights_as", "goro"}}}));
    ASSERT_EQ(added.status, moonmod::ApiCallStatus::Ok) << added.message;
    EXPECT_EQ(added.value.value("character_id", -1), 8);
    EXPECT_EQ(added.value.value("count", -1), 9);
    EXPECT_EQ(ExtraChara(8)->hp, 100);
    const moonmod::ApiCallResult refused = config.api_handler(
        mod, *register_playable, moonmod::Json::array({{{"id", "moss"}, {"name", "Moss"}, {"pack", "dun/mainchara/c09m.chr"}, {"fights_as", "opar"}}}));
    EXPECT_EQ(refused.status, moonmod::ApiCallStatus::InvalidArgument);
    const moonmod::ApiCallResult unknown = config.api_handler(
        mod, *register_playable, moonmod::Json::array({{{"id", "moss2"}, {"name", "Moss"}, {"pack", "dun/mainchara/c09n.chr"}, {"colour", 3}}}));
    EXPECT_EQ(unknown.status, moonmod::ApiCallStatus::InvalidArgument);

    status->party_size = 9;
    status->max_hp[kKit] = 60;
    status->hp[kKit] = 60;
    status->ailments[kKit] = 0;

    const moonmod::ApiCallResult roster = config.api_handler(mod, *party_list, moonmod::Json::array());
    ASSERT_EQ(roster.status, moonmod::ApiCallStatus::Ok) << roster.message;
    ASSERT_EQ(roster.value.size(), 9u);
    EXPECT_EQ(roster.value[6].value("id", ""), "opar");
    EXPECT_EQ(roster.value[7].value("id", ""), "kit");
    EXPECT_EQ(roster.value[8].value("id", ""), "moss");

    const moonmod::ApiCallResult switched = config.api_handler(mod, *set_active, moonmod::Json::array({{{"id", "kit"}}}));
    ASSERT_EQ(switched.status, moonmod::ApiCallStatus::Ok) << switched.message;
    EXPECT_EQ(switched.value.value("active", ""), "kit");
    EXPECT_EQ(g_switch_target, kKit);

    const moonmod::ApiCallResult player = config.api_handler(mod, *player_get, moonmod::Json::array());
    ASSERT_EQ(player.status, moonmod::ApiCallStatus::Ok) << player.message;
    EXPECT_EQ(player.value.value("character", ""), "kit");
    EXPECT_EQ(player.value.value("hp", -1), 60);

    // Ruby's weapons are Kit's (he fights as Ruby); Toan's are not.
    const std::string ring = "chronicle:weapon:" + std::to_string(ITEM_WEAPON_GOLD_RING);
    const std::string dagger = "chronicle:weapon:" + std::to_string(ITEM_WEAPON_DAGGER);
    const moonmod::ApiCallResult given = config.api_handler(mod, *give, moonmod::Json::array({{{"id", ring}, {"character", "kit"}}}));
    ASSERT_EQ(given.status, moonmod::ApiCallStatus::Ok) << given.message;
    const int slot = given.value.value("slot", -1);
    ASSERT_GE(slot, 0);
    EXPECT_EQ(status->chara_weapons[kKit][slot].item_no, ITEM_WEAPON_GOLD_RING);
    const moonmod::ApiCallResult wrong = config.api_handler(mod, *give, moonmod::Json::array({{{"id", dagger}, {"character", "kit"}}}));
    EXPECT_EQ(wrong.status, moonmod::ApiCallStatus::Failed);
    status->chara_weapons[kKit][slot] = {};

    const moonmod::ApiCallResult back = config.api_handler(mod, *set_active, moonmod::Json::array({{{"id", "toan"}}}));
    ASSERT_EQ(back.status, moonmod::ApiCallStatus::Ok) << back.message;
    EXPECT_EQ(back.value.value("previous", ""), "kit");
    EXPECT_EQ(status->cur_chara, CHARA_TOAN);

    // A party of seven reaches Opar and not Kit.
    status->party_size = 7;
    const moonmod::ApiCallResult unjoined = config.api_handler(mod, *set_active, moonmod::Json::array({{{"id", "kit"}}}));
    EXPECT_EQ(unjoined.status, moonmod::ApiCallStatus::Failed);
    const moonmod::ApiCallResult definition = config.api_handler(mod, *characters_get, moonmod::Json::array({"kit"}));
    ASSERT_EQ(definition.status, moonmod::ApiCallStatus::Ok) << definition.message;
    EXPECT_FALSE(definition.value.value("in_party", true));
    EXPECT_EQ(definition.value.value("name", ""), "Kit");
    EXPECT_EQ(definition.value.value("fights_as", ""), "ruby");
}

// Story mode: a registered character joins a story party apart from its size (Opar with Xiao, a party of two), and
// the menus, the loops over the party and the API take him.
TEST_F(PlayableRoster, ARegisteredCharacterJoinsAStoryPartyApartFromItsSize) {
    SaveScope scope;
    CDngStatusData *status = scope.Status();
    status->party_size = 2;
    EXPECT_FALSE(InParty(*status, kOpar));
    EXPECT_EQ(PartyCount(*status), 2);
    EXPECT_EQ(CharaSlotsInPlay(*status), 6);

    SetCharaJoined(kOpar, true);
    EXPECT_TRUE(InParty(*status, CHARA_TOAN));
    EXPECT_TRUE(InParty(*status, CHARA_XIAO));
    EXPECT_FALSE(InParty(*status, CHARA_GORO));
    EXPECT_TRUE(InParty(*status, kOpar));
    EXPECT_FALSE(InParty(*status, kKit));
    EXPECT_EQ(PartyCount(*status), 3);
    EXPECT_EQ(CharaSlotsInPlay(*status), 7);
    // Through the party in index order, round and back: Toan, Xiao, Opar.
    EXPECT_EQ(NextPartyMember(*status, CHARA_XIAO, 1), kOpar);
    EXPECT_EQ(NextPartyMember(*status, kOpar, 1), CHARA_TOAN);
    EXPECT_EQ(NextPartyMember(*status, CHARA_TOAN, -1), kOpar);
    EXPECT_EQ(NextPartyMember(*status, kOpar, -2), CHARA_TOAN);
    // He counts among the living.
    status->hp[CHARA_TOAN] = 0;
    status->hp[CHARA_XIAO] = 0;
    EXPECT_EQ(status->GetLiveUnit(), 1);

    // A party of seven (the Gauntlet's) holds him by its size, joined or not.
    SetCharaJoined(kOpar, false);
    status->party_size = 7;
    EXPECT_TRUE(InParty(*status, kOpar));
    EXPECT_EQ(PartyCount(*status), 7);

    // A new game has nobody joined.
    SetCharaJoined(kOpar, true);
    status->Initialize();
    EXPECT_FALSE(CharaJoined(kOpar));
}

TEST_F(PlayableRoster, HisPartOfASaveGoesBesideItAndComesBack) {
    SaveScope scope;
    CDngStatusData *status = scope.Status();
    status->party_size = 2;
    SetCharaJoined(kOpar, true);
    status->max_hp[kOpar] = 120;
    status->hp[kOpar] = 77;
    status->defense[kOpar] = 30;
    status->water_now[kOpar] = 12.5f;
    status->equipped_weapon_slot[kOpar] = 1;
    status->chara_weapons[kOpar][1].item_no = ITEM_WEAPON_DAGGER;
    status->chara_weapons[kOpar][1].level = 4;
    status->chara_weapons[kOpar][1].elem[2] = 9;
    status->cur_chara = static_cast<s8>(kOpar);

    // A file from before, naming one no mod registers now: kept.
    const std::string text = CharaExtrasJson(*status, R"({"version":1,"characters":{"gone":{"joined":true}}})");

    status->Initialize();
    status->party_size = 2;
    status->cur_chara = CHARA_TOAN;  // as the save writes him (BuildImage)
    EXPECT_FALSE(CharaJoined(kOpar));
    ASSERT_TRUE(CharaExtrasApply(text, *status));
    EXPECT_TRUE(CharaJoined(kOpar));
    EXPECT_FALSE(CharaJoined(kKit));
    EXPECT_EQ(status->max_hp[kOpar], 120);
    EXPECT_EQ(status->hp[kOpar], 77);
    EXPECT_EQ(status->defense[kOpar], 30);
    EXPECT_FLOAT_EQ(status->water_now[kOpar], 12.5f);
    EXPECT_EQ(status->equipped_weapon_slot[kOpar], 1);
    EXPECT_EQ(status->chara_weapons[kOpar][1].item_no, ITEM_WEAPON_DAGGER);
    EXPECT_EQ(status->chara_weapons[kOpar][1].level, 4);
    EXPECT_EQ(status->chara_weapons[kOpar][1].elem[2], 9);
    EXPECT_EQ(status->cur_chara, kOpar);  // he led, so he leads again
    EXPECT_NE(CharaExtrasJson(*status, text).find("\"gone\""), std::string::npos);

    // No file, or a broken one: he starts afresh, out of the party.
    status->hp[kOpar] = 5;
    EXPECT_FALSE(CharaExtrasApply("", *status));
    EXPECT_FALSE(CharaJoined(kOpar));
    EXPECT_EQ(status->hp[kOpar], 90);
    EXPECT_EQ(status->chara_weapons[kOpar][1].item_no, -1);
}

TEST_F(PlayableRoster, TheModApiJoinsAndLeavesHim) {
    SaveScope scope;
    CDngStatusData *status = scope.Status();
    status->party_size = 2;
    moonmod::RuntimeConfig config;
    moonmod::ConfigureChronicleApi(config);
    const auto *join = moonmod::FindApiOperation("party.join");
    const auto *leave = moonmod::FindApiOperation("party.leave");
    const auto *party_list = moonmod::FindApiOperation("party.list");
    ASSERT_NE(join, nullptr);
    ASSERT_NE(leave, nullptr);
    const char *mod = "test.moonmod.roster";

    const moonmod::ApiCallResult joined = config.api_handler(mod, *join, moonmod::Json::array({{{"character", "opar"}}}));
    ASSERT_EQ(joined.status, moonmod::ApiCallStatus::Ok) << joined.message;
    EXPECT_TRUE(joined.value.value("changed", false));
    EXPECT_EQ(joined.value.value("party_count", 0), 3);
    const moonmod::ApiCallResult roster = config.api_handler(mod, *party_list, moonmod::Json::array());
    ASSERT_EQ(roster.status, moonmod::ApiCallStatus::Ok) << roster.message;
    ASSERT_EQ(roster.value.size(), 3u);
    EXPECT_EQ(roster.value[2].value("id", ""), "opar");

    EXPECT_EQ(config.api_handler(mod, *join, moonmod::Json::array({{{"character", "xiao"}}})).status,
              moonmod::ApiCallStatus::InvalidArgument);
    status->cur_chara = static_cast<s8>(kOpar);
    EXPECT_EQ(config.api_handler(mod, *leave, moonmod::Json::array({{{"character", "opar"}}})).status,
              moonmod::ApiCallStatus::Failed);  // he leads
    status->cur_chara = CHARA_TOAN;
    const moonmod::ApiCallResult left = config.api_handler(mod, *leave, moonmod::Json::array({{{"character", "opar"}}}));
    ASSERT_EQ(left.status, moonmod::ApiCallStatus::Ok) << left.message;
    EXPECT_FALSE(CharaJoined(kOpar));
    status->party_size = 7;
    EXPECT_EQ(config.api_handler(mod, *leave, moonmod::Json::array({{{"character", "opar"}}})).status,
              moonmod::ApiCallStatus::Failed);  // in by the party's size

    // Joined with nothing in hand, the first weapon he is given goes in his hand (a switch to him would not load).
    status->party_size = 2;
    ASSERT_EQ(config.api_handler(mod, *join, moonmod::Json::array({{{"character", "opar"}}})).status, moonmod::ApiCallStatus::Ok);
    ASSERT_EQ(status->equipped_weapon_slot[kOpar], -1);
    const auto *give = moonmod::FindApiOperation("weapons.give");
    ASSERT_NE(give, nullptr);
    const std::string dagger = "chronicle:weapon:" + std::to_string(ITEM_WEAPON_DAGGER);
    const moonmod::ApiCallResult given = config.api_handler(mod, *give, moonmod::Json::array({{{"id", dagger}, {"character", "opar"}}}));
    ASSERT_EQ(given.status, moonmod::ApiCallStatus::Ok) << given.message;
    EXPECT_TRUE(given.value.value("equipped", false));
    EXPECT_EQ(status->equipped_weapon_slot[kOpar], given.value.value("slot", -2));
}

// A mod's Atla sphere (mod_atla.hpp, georama.atla_spawn): its own name line for the pickup, and its mod told.
TEST(ModAtla, ASphereIsNamedAndItsModHearsOfIt) {
    ClearModAtlas();
    const int index = RegisterModAtla("moonmod.opar", "babyopar", "Baby Opar");
    ASSERT_EQ(index, 0);
    EXPECT_EQ(RegisterModAtla("moonmod.opar", "babyopar", "Baby Opar!"), 0);  // the same sphere, renamed
    EXPECT_TRUE(IsModAtla(kModAtlaBase + index));
    EXPECT_FALSE(IsModAtla(94));  // the game's own are registry indexes
    ASSERT_NE(ModAtlaOf(kModAtlaBase + index), nullptr);
    EXPECT_EQ(ModAtlaOf(kModAtlaBase + index)->name, "Baby Opar!");
    EXPECT_NE(ModAtlaNameLine(kModAtlaNameLine + index), nullptr);
    EXPECT_EQ(ModAtlaNameLine(kModAtlaNameLine + 5), nullptr);
    std::string heard;
    SetModAtlaCollectedHandler([&heard](const ModAtla &atla) { heard = atla.owner + ":" + atla.id; });
    ModAtlaCollected(kModAtlaBase + index);
    EXPECT_EQ(heard, "moonmod.opar:babyopar");
    for (int i = 1; i < kModAtlaMax; ++i) RegisterModAtla("m", std::to_string(i), "x");
    EXPECT_EQ(RegisterModAtla("m", "one too many", "x"), -1);
    SetModAtlaCollectedHandler(nullptr);
    ClearModAtlas();
}

// A registered weapon builds up into another registered one (weapons.build_up_register; Opar's staffs), which the
// native chains, numbered from the owner's first weapon, cannot reach.
TEST(ModBuildUp, ARegisteredWeaponBuildsUpIntoAnother) {
    ResetMoonModWeaponSlotsForTests();
    ClearMoonModBuildUps();
    char error[192] = {};
    ASSERT_TRUE(RegisterMoonModWeapon(377, 262, 8, 40, -1, -1, CHARA_TOAN, error, sizeof(error))) << error;
    ASSERT_TRUE(RegisterMoonModWeapon(378, 262, 18, 45, -1, -1, CHARA_TOAN, error, sizeof(error))) << error;
    EXPECT_EQ(IsNotBuildUpWeapon(377), 1);
    AddMoonModBuildUp(377, 378);
    EXPECT_EQ(IsNotBuildUpWeapon(377), 0);
    WEAPON_HAVE held{};
    WepDataListToHaveCopy(377, &held);
    WEP_BUILDUP_INFO info[4];
    EnableBuildUpModel(info, &held);
    int found = -1;
    for (int i = 0; i < 4 && info[i].weapon_no != -1; ++i) {
        if (info[i].weapon_no == 378) found = i;
    }
    ASSERT_GE(found, 0);
    EXPECT_EQ(info[found].enabled, 0);  // 8 attack is short of its 18
    held.attack = 18;
    EnableBuildUpModel(info, &held);
    EXPECT_EQ(info[found].enabled, 1);
    ClearMoonModBuildUps();
    ResetMoonModWeaponSlotsForTests();
}

// A registered weapon a mod lets chests hold (dungeons.chest_weapons_add) and what its shop price starts at.
// A registered character's weapon (Opar's staffs) is Toan's to the combat code but sits in its holder's row.
TEST(ModWeaponHolder, AHeldWeaponSitsInItsHoldersRow) {
    ResetMoonModWeaponSlotsForTests();
    char error[192] = {};
    ASSERT_TRUE(RegisterMoonModWeapon(377, 262, 8, 40, -1, -1, CHARA_TOAN, error, sizeof(error))) << error;
    EXPECT_EQ(WhoIsWeaponEquip(377), CHARA_TOAN);
    SetMoonModWeaponHolder(377, 6);
    EXPECT_EQ(WhoIsWeaponEquip(377), 6);
    EXPECT_EQ(GetWeaponData(377)->owner, CHARA_TOAN);
    ResetMoonModWeaponSlotsForTests();
    EXPECT_EQ(MoonModWeaponHolder(377), -1);
}

TEST(ModChestWeapon, AChestRollingAWeaponHoldsTheModsFromItsFloorOn) {
    ResetMoonModWeaponSlotsForTests();
    ClearMoonModChestWeapons("");
    char error[192] = {};
    ASSERT_TRUE(RegisterMoonModWeapon(378, 262, 18, 45, -1, -1, CHARA_TOAN, error, sizeof(error))) << error;
    EXPECT_EQ(PriceList[378 - ITEM_ATTACH_START].buy_price, PriceList[262 - ITEM_ATTACH_START].buy_price);
    AddMoonModChestWeapon("moonmod.opar", 378, 0, 4, 100);
    EXPECT_EQ(MoonModChestWeapon(262, 0, 3, nullptr, 0), 262);  // before its floor
    EXPECT_EQ(MoonModChestWeapon(262, 1, 6, nullptr, 0), 262);  // another dungeon
    EXPECT_EQ(MoonModChestWeapon(262, 0, 4, nullptr, 0), 378);
    const int placed[] = {378};
    EXPECT_EQ(MoonModChestWeapon(262, 0, 6, placed, 1), 262);  // already in a chest on the floor
    ClearMoonModChestWeapons("moonmod.opar");
    EXPECT_EQ(MoonModChestWeapon(262, 0, 6, nullptr, 0), 262);
    ResetMoonModWeaponSlotsForTests();
}

#include "chara_extras_save.hpp"

#include <cstring>
#include <fstream>
#include <iterator>
#include <print>
#include <span>
#include <system_error>

#include <nlohmann/json.hpp>

#include "platform/files.hpp"
#include "playable_chara.hpp"
#include "userstatus.hpp"

namespace dc::game {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr const char *kFileName = "characters.json";
constexpr int kVersion = 1;

std::string Hex(std::span<const unsigned char> bytes) {
    static constexpr char digits[] = "0123456789abcdef";
    std::string text;
    text.reserve(bytes.size() * 2);
    for (unsigned char byte : bytes) {
        text += digits[byte >> 4];
        text += digits[byte & 15];
    }
    return text;
}

bool Unhex(const std::string &text, std::span<unsigned char> bytes) {
    if (text.size() != bytes.size() * 2) {
        return false;
    }
    auto nibble = [](char c) {
        return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : -1;
    };
    for (std::size_t i = 0; i < bytes.size(); i++) {
        const int high = nibble(text[2 * i]);
        const int low = nibble(text[2 * i + 1]);
        if (high < 0 || low < 0) {
            return false;
        }
        bytes[i] = static_cast<unsigned char>(high << 4 | low);
    }
    return true;
}

// A registered character as a new game has him (CDngStatusData::Initialize), out of the party.
void Reset(CUserStatus &status, int chara) {
    const PlayableChara *extra = ExtraChara(chara);
    const int hp = extra != nullptr ? extra->hp : 70;
    status.max_hp[chara] = static_cast<s16>(hp);
    status.hp[chara] = static_cast<s16>(hp);
    status.defense[chara] = extra != nullptr ? extra->defense : 3;
    status.ailments[chara] = 0;
    status.ailment_frames[chara] = 0;
    status.equipped_weapon_slot[chara] = -1;
    for (WEAPON_HAVE &weapon : status.chara_weapons[chara]) {
        std::memset(static_cast<void *>(&weapon), 0, sizeof(weapon));
        weapon.item_no = -1;
    }
    status.water_max[chara] = 30.0f;
    status.water_now[chara] = 30.0f;
    status.skill_owned[chara] = 0;
    SetCharaJoined(chara, false);
}

Json Entry(const CUserStatus &status, int chara) {
    const auto &weapons = status.chara_weapons[chara];
    return {
        {"joined", CharaJoined(chara)},
        {"leader", status.cur_chara == chara},
        {"max_hp", status.max_hp[chara]},
        {"hp", status.hp[chara]},
        {"defense", status.defense[chara]},
        {"ailments", status.ailments[chara]},
        {"ailment_frames", status.ailment_frames[chara]},
        {"water_max", status.water_max[chara]},
        {"water_now", status.water_now[chara]},
        {"skill_owned", status.skill_owned[chara]},
        {"equipped_weapon_slot", status.equipped_weapon_slot[chara]},
        {"weapons", Hex({reinterpret_cast<const unsigned char *>(&weapons), sizeof(weapons)})},
    };
}

bool Apply(const Json &entry, CUserStatus &status, int chara) {
    if (!entry.is_object()) {
        return false;
    }
    auto &weapons = status.chara_weapons[chara];
    std::remove_reference_t<decltype(weapons)> read{};
    if (!Unhex(entry.value("weapons", std::string{}), {reinterpret_cast<unsigned char *>(&read), sizeof(read)})) {
        return false;
    }
    std::memcpy(static_cast<void *>(&weapons), &read, sizeof(read));
    status.max_hp[chara] = entry.value("max_hp", status.max_hp[chara]);
    status.hp[chara] = entry.value("hp", status.hp[chara]);
    status.defense[chara] = entry.value("defense", status.defense[chara]);
    status.ailments[chara] = entry.value("ailments", 0);
    status.ailment_frames[chara] = entry.value("ailment_frames", static_cast<s16>(0));
    status.water_max[chara] = entry.value("water_max", 30.0f);
    status.water_now[chara] = entry.value("water_now", 30.0f);
    status.skill_owned[chara] = entry.value("skill_owned", 0);
    status.equipped_weapon_slot[chara] = entry.value("equipped_weapon_slot", static_cast<s8>(-1));
    const bool joined = entry.value("joined", false);
    SetCharaJoined(chara, joined);
    // The save writes him leading as Toan leading (memorycardaccess.cpp BuildImage); he leads again if he did.
    if (joined && entry.value("leader", false) && status.cur_chara == CHARA_TOAN) {
        status.cur_chara = static_cast<s8>(chara);
    }
    return true;
}

} // namespace

std::string CharaExtrasJson(const CUserStatus &status, const std::string &previous) {
    Json characters = Json::object();
    if (const Json old = Json::parse(previous, nullptr, false); old.is_object() && old.contains("characters") &&
                                                                 old["characters"].is_object()) {
        characters = old["characters"];  // those no mod registers now stay as they were
    }
    for (int chara = SAVE_CHARA_COUNT; chara < CharaCount(); chara++) {
        characters[CharaIdName(chara)] = Entry(status, chara);
    }
    return Json{{"version", kVersion}, {"characters", std::move(characters)}}.dump(2);
}

bool CharaExtrasApply(const std::string &text, CUserStatus &status) {
    for (int chara = SAVE_CHARA_COUNT; chara < CharaCount(); chara++) {
        Reset(status, chara);
    }
    const Json file = Json::parse(text, nullptr, false);
    if (!file.is_object() || !file.contains("characters") || !file["characters"].is_object()) {
        return false;
    }
    const Json &characters = file["characters"];
    bool ok = true;
    for (int chara = SAVE_CHARA_COUNT; chara < CharaCount(); chara++) {
        const auto found = characters.find(CharaIdName(chara));
        if (found != characters.end() && !Apply(*found, status, chara)) {
            Reset(status, chara);
            ok = false;
        }
    }
    return ok;
}

bool CharaExtrasSave(const fs::path &save_directory, const CUserStatus &status) {
    std::string previous;
    if (std::ifstream stream(save_directory / kFileName, std::ios::binary); stream) {
        previous.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    if (CharaCount() == SAVE_CHARA_COUNT && previous.empty()) {
        return true;  // no registered character, and none kept: no file
    }
    const std::string text = CharaExtrasJson(status, previous);
    if (FilesWrite(save_directory / kFileName, text.data(), text.size(), true) != FilesResult::kWritten) {
        std::println(stderr, "characters: could not write {}", (save_directory / kFileName).string());
        return false;
    }
    return true;
}

void CharaExtrasLoad(const fs::path &save_directory, CUserStatus &status) {
    std::string text;
    if (std::ifstream stream(save_directory / kFileName, std::ios::binary); stream) {
        text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    if (!CharaExtrasApply(text, status) && !text.empty()) {
        std::println(stderr, "characters: {} could not be read; the registered characters start afresh",
                     (save_directory / kFileName).string());
    }
}

void CharaExtrasDelete(const fs::path &save_directory) {
    std::error_code error;
    fs::remove(save_directory / kFileName, error);
}

} // namespace dc::game

// Co-op's split of a save into the host's world and a player's own things (moonmod/coop_save.hpp).

#include "moonmod/coop_save.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <cstdio>
#include <vector>

#include <nlohmann/json.hpp>

#include "game/savedata.hpp"
#include "platform/files.hpp"

namespace moonmod::coop {

using dc::game::CDngStatusData;
using dc::game::CSaveData;

// One field of the save: its number in the text, and where it is.
struct Field {
    std::uint16_t id;
    void         *data;
    std::size_t   size;
};

struct SaveAccess {
    // Numbers are for good: a new field takes a new number, and a field that goes keeps its number unused.
    static std::vector<Field> World(CSaveData &save) {
        CDngStatusData &dng = save.dng_status;
        return {
            {1, &save.georama, sizeof(save.georama)},
            {2, &save.elem_data, sizeof(save.elem_data)},
            {3, &save.special_npc, sizeof(save.special_npc)},
            {4, &save.game_flags, sizeof(save.game_flags)},
            {5, &save.game_int_flag, sizeof(save.game_int_flag)},
            {6, &save.map_flags, sizeof(save.map_flags)},
            {7, &save.map_init_flags, sizeof(save.map_init_flags)},
            {8, &save.visit_map, sizeof(save.visit_map)},
            {9, &save.quest_dungeon, sizeof(save.quest_dungeon)},
            {10, &save.quest_dungeon_total, sizeof(save.quest_dungeon_total)},
            {11, &save.mardan_garayan_caught, sizeof(save.mardan_garayan_caught)},
            {12, &save.now_time, sizeof(save.now_time)},
            {13, &save.day, sizeof(save.day)},
            {20, &dng.atra_grid, sizeof(dng.atra_grid)},
            {21, &dng.atra_registry, sizeof(dng.atra_registry)},
            {22, &dng.kills, sizeof(dng.kills)},
            {23, &dng.floor_reached, sizeof(dng.floor_reached)},
            {24, &dng.res_limit_zone_id, sizeof(dng.res_limit_zone_id)},
            {25, &dng.party_size, sizeof(dng.party_size)},
            {26, &dng.skill_owned.data, sizeof(dng.skill_owned.data)}, // the story gives these
        };
    }

    static std::vector<Field> Player(CSaveData &save) {
        CDngStatusData &dng = save.dng_status;
        return {
            {1, &dng.max_hp.data, sizeof(dng.max_hp.data)},
            {2, &dng.hp.data, sizeof(dng.hp.data)},
            {3, &dng.defense.data, sizeof(dng.defense.data)},
            {4, &dng.water_max.data, sizeof(dng.water_max.data)},
            {5, &dng.water_now.data, sizeof(dng.water_now.data)},
            {6, &dng.chara_weapons.data, sizeof(dng.chara_weapons.data)},
            {7, &dng.equipped_weapon_slot.data, sizeof(dng.equipped_weapon_slot.data)},
            {8, &dng.money, sizeof(dng.money)},
            {9, &dng.item_pack, sizeof(dng.item_pack)},
            {10, &dng.active_item, sizeof(dng.active_item)},
            {11, &dng.active_item_vol, sizeof(dng.active_item_vol)},
            {12, &dng.consumable_items, sizeof(dng.consumable_items)},
            {13, &save.stock_item, sizeof(save.stock_item)},
            {14, &save.fishing_point, sizeof(save.fishing_point)},
            {15, &save.fish_data, sizeof(save.fish_data)},
        };
    }
};

namespace {

constexpr char kWorldTag[] = "DCW1";
constexpr char kPlayerTag[] = "DCP1";

std::string Base64(std::string_view bytes) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve((bytes.size() + 2) / 3 * 4);
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

bool Unbase64(std::string_view text, std::string &out) {
    out.clear();
    unsigned value = 0;
    int      bits = 0;
    for (const char c : text) {
        int digit;
        if (c >= 'A' && c <= 'Z') digit = c - 'A';
        else if (c >= 'a' && c <= 'z') digit = c - 'a' + 26;
        else if (c >= '0' && c <= '9') digit = c - '0' + 52;
        else if (c == '+') digit = 62;
        else if (c == '/') digit = 63;
        else if (c == '=') break;
        else return false;
        value = (value << 6) | static_cast<unsigned>(digit);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<char>((value >> bits) & 0xFF));
        }
    }
    return true;
}

std::string Export(const char *tag, const std::vector<Field> &fields) {
    std::string bytes;
    for (const Field &field : fields) {
        const std::uint32_t size = static_cast<std::uint32_t>(field.size);
        bytes.push_back(static_cast<char>(field.id & 0xFF));
        bytes.push_back(static_cast<char>(field.id >> 8));
        for (int shift = 0; shift < 32; shift += 8) bytes.push_back(static_cast<char>((size >> shift) & 0xFF));
        bytes.append(static_cast<const char *>(field.data), field.size);
    }
    return std::string(tag) + Base64(bytes);
}

// Reads every field first and writes none unless all of them fit: a refused import changes nothing.
bool Import(const char *tag, std::string_view text, const std::vector<Field> &fields, std::string &error) {
    if (!text.starts_with(tag)) {
        error = std::string("not ") + (tag == kWorldTag ? "a world" : "a player") + " from save." +
                (tag == kWorldTag ? "world_export" : "player_export");
        return false;
    }
    std::string bytes;
    if (!Unbase64(text.substr(4), bytes)) {
        error = "the data is damaged (not base64)";
        return false;
    }
    std::vector<std::pair<const Field *, std::size_t>> found; // field, offset of its bytes
    std::size_t                                      at = 0;
    while (at < bytes.size()) {
        if (bytes.size() - at < 6) {
            error = "the data is damaged (cut short)";
            return false;
        }
        const auto     byte = [&bytes](std::size_t k) { return static_cast<std::uint32_t>(static_cast<unsigned char>(bytes[k])); };
        const auto     id = static_cast<std::uint16_t>(byte(at) | (byte(at + 1) << 8));
        std::uint32_t  size = 0;
        for (int k = 0; k < 4; ++k) size |= byte(at + 2 + k) << (8 * k);
        at += 6;
        if (bytes.size() - at < size) {
            error = "the data is damaged (cut short)";
            return false;
        }
        const auto field = std::ranges::find(fields, id, &Field::id);
        if (field == fields.end() || field->size != size) {
            error = "the data comes from another build of the game; play on the same build";
            return false;
        }
        found.emplace_back(&*field, at);
        at += size;
    }
    if (found.size() != fields.size()) {
        error = "the data comes from another build of the game; play on the same build";
        return false;
    }
    for (const auto &[field, offset] : found) std::memcpy(field->data, bytes.data() + offset, field->size);
    return true;
}

} // namespace

std::string ExportWorld(const CSaveData &save) { return Export(kWorldTag, SaveAccess::World(const_cast<CSaveData &>(save))); }

bool ImportWorld(std::string_view text, CSaveData &save, std::string &error) {
    return Import(kWorldTag, text, SaveAccess::World(save), error);
}

std::string ExportPlayer(const CSaveData &save) { return Export(kPlayerTag, SaveAccess::Player(const_cast<CSaveData &>(save))); }

bool ImportPlayer(std::string_view text, CSaveData &save, std::string &error) {
    return Import(kPlayerTag, text, SaveAccess::Player(save), error);
}

namespace {

constexpr char kAccountsFile[] = "coop_accounts.json";
std::map<std::string, Account, std::less<>> g_accounts;

} // namespace

bool ValidAccountId(std::string_view id) {
    return id.size() >= 8 && id.size() <= 64 &&
           std::ranges::all_of(id, [](char c) { return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-'; });
}

const Account *AccountGet(std::string_view id) {
    const auto found = g_accounts.find(id);
    return found == g_accounts.end() ? nullptr : &found->second;
}

bool AccountSet(Account account, std::string &error) {
    if (!ValidAccountId(account.id)) {
        error = "an account id is 8-64 of a-z, 0-9 and -";
        return false;
    }
    if (!account.player.starts_with("DCP1") || account.player.size() > kAccountMax) {
        error = "an account holds save.player_export text";
        return false;
    }
    if (account.name.size() > 24) account.name.resize(24);
    if (!g_accounts.contains(account.id) && g_accounts.size() >= kAccountsMax) {
        error = "this save keeps " + std::to_string(kAccountsMax) + " guests at most; remove one first";
        return false;
    }
    const std::string id = account.id;
    g_accounts.insert_or_assign(id, std::move(account));
    return true;
}

bool AccountRemove(std::string_view id) {
    const auto found = g_accounts.find(id);
    if (found == g_accounts.end()) return false;
    g_accounts.erase(found);
    return true;
}

std::vector<const Account *> Accounts() {
    std::vector<const Account *> out;
    for (const auto &[id, account] : g_accounts) out.push_back(&account);
    return out;
}

void AccountsClear() { g_accounts.clear(); }

void AccountsDelete(const std::filesystem::path &save_directory) {
    std::error_code error;
    std::filesystem::remove(save_directory / kAccountsFile, error);
}

bool AccountsSave(const std::filesystem::path &save_directory) {
    if (g_accounts.empty()) {
        AccountsDelete(save_directory);
        return true;
    }
    nlohmann::json accounts = nlohmann::json::object();
    for (const auto &[id, account] : g_accounts) accounts[id] = {{"name", account.name}, {"player", account.player}};
    const std::string text = nlohmann::json{{"format", 1}, {"accounts", std::move(accounts)}}.dump();
    if (::FilesWrite(save_directory / kAccountsFile, text.data(), text.size(), true) != ::FilesResult::kWritten) {
        std::fprintf(stderr, "co-op: could not write %s\n", (save_directory / kAccountsFile).string().c_str());
        return false;
    }
    return true;
}

void AccountsLoad(const std::filesystem::path &save_directory) {
    g_accounts.clear();
    std::string text;
    if (std::ifstream stream(save_directory / kAccountsFile, std::ios::binary); stream) {
        text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    if (text.empty()) return;
    const nlohmann::json file = nlohmann::json::parse(text, nullptr, false);
    if (!file.is_object() || file.value("format", 0) != 1 || !file.contains("accounts") || !file["accounts"].is_object()) {
        std::fprintf(stderr, "co-op: %s could not be read; this save keeps no guests\n", (save_directory / kAccountsFile).string().c_str());
        return;
    }
    for (const auto &[id, entry] : file["accounts"].items()) {
        if (!entry.is_object()) continue;
        std::string error;
        AccountSet({id, entry.value("name", std::string()), entry.value("player", std::string())}, error);
    }
}

} // namespace moonmod::coop

// Co-op's split of a save into the host's world and a player's own things (moonmod/coop_save.hpp).

#include "moonmod/coop_save.hpp"

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <type_traits>
#include <cstdio>
#include <vector>

#include <nlohmann/json.hpp>

#include "game/savedata.hpp"
#include "moonmod/net_session.hpp"
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

    static constexpr int kTowns = static_cast<int>(sizeof(CSaveData::georama) / sizeof(CSaveData::georama[0]));
    static constexpr std::size_t kChips = sizeof(CSaveData::elem_data[0]) / sizeof(CSaveData::elem_data[0][0]);
    static std::vector<Field> Town(CSaveData &save, int town) {
        return {
            {1, &save.georama[town], sizeof(save.georama[town])},
            {2, &save.elem_data[town], sizeof(save.elem_data[town])},
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
        error = std::string("not ") + (tag == kWorldTag ? "a world from save.world_export" : tag == kPlayerTag ? "a player from save.player_export" : "that town's georama.town_export");
        return false;
    }
    std::string bytes;
    if (!Unbase64(text.substr(std::strlen(tag)), bytes)) {
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
        if (std::ranges::any_of(found, [&field](const auto &entry) { return entry.first == &*field; })) {
            error = "the data is damaged (a part comes twice)";
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

std::string ExportTown(const CSaveData &save, int town) {
    constexpr int kTowns = SaveAccess::kTowns;
    if (town < 0 || town >= kTowns) return {};
    return Export(("DCT1" + std::to_string(town) + ":").c_str(), SaveAccess::Town(const_cast<CSaveData &>(save), town));
}

bool ImportTown(std::string_view text, int town, CSaveData &save, std::string &error) {
    constexpr int kTowns = SaveAccess::kTowns;
    if (town < 0 || town >= kTowns) {
        error = "no such town";
        return false;
    }
    const std::string tag = "DCT1" + std::to_string(town) + ":";
    if (!text.starts_with(tag)) {
        error = "not this town's georama.town_export";
        return false;
    }
    return Import(tag.c_str(), text, SaveAccess::Town(save, town), error);
}

namespace {

// A town's two fields, decoded apart from any save.
struct TownCopy {
    dc::game::SV_GEORAMA_DATA geo{};
    std::int16_t              chips[SaveAccess::kChips]{};
    std::vector<Field>        Fields() { return {{1, &geo, sizeof(geo)}, {2, chips, sizeof(chips)}}; }
};

template <typename T> bool Same(const T &a, const T &b) { return std::memcmp(&a, &b, sizeof(T)) == 0; }

// Each record they changed takes their value.
template <typename T, std::size_t N> void TakeChanged(T (&into)[N], const T (&base)[N], const T (&edited)[N]) {
    for (std::size_t i = 0; i < N; ++i) {
        if (!Same(base[i], edited[i])) into[i] = edited[i];
    }
}

// The placed buildings, a list ended by part_id -1: theirs taken away, theirs added.
std::vector<dc::game::SV_GRD_PART> Placed(const dc::game::SV_GEORAMA_DATA &geo) {
    std::vector<dc::game::SV_GRD_PART> out;
    for (const auto &part : geo.placed_parts) {
        if (part.part_id == -1) break;
        out.push_back(part);
    }
    return out;
}

} // namespace

bool MergeTown(std::string_view base, std::string_view edited, int town, CSaveData &save, std::string &error) {
    constexpr int kTowns = SaveAccess::kTowns;
    if (town < 0 || town >= kTowns) {
        error = "no such town";
        return false;
    }
    const std::string tag = "DCT1" + std::to_string(town) + ":";
    if (!base.starts_with(tag) || !edited.starts_with(tag)) {
        error = "not this town's georama.town_export";
        return false;
    }
    auto before = std::make_unique<TownCopy>(), after = std::make_unique<TownCopy>(), now = std::make_unique<TownCopy>();
    if (!Import(tag.c_str(), base, before->Fields(), error) || !Import(tag.c_str(), edited, after->Fields(), error)) return false;
    const std::vector<Field> live = SaveAccess::Town(save, town);
    std::memcpy(&now->geo, live[0].data, sizeof(now->geo));
    std::memcpy(now->chips, live[1].data, sizeof(now->chips));

    auto &geo = now->geo;
    const auto &was = before->geo, &is = after->geo;
    if (was.request_count != is.request_count) geo.request_count = is.request_count;
    if (was.request_event_flag != is.request_event_flag) geo.request_event_flag = is.request_event_flag;
    TakeChanged(geo.request_complete, was.request_complete, is.request_complete);
    TakeChanged(geo.parts_info, was.parts_info, is.parts_info);
    TakeChanged(geo.npc, was.npc, is.npc);
    TakeChanged(now->chips, before->chips, after->chips);

    std::vector<dc::game::SV_GRD_PART> placed = Placed(geo), removed = Placed(was), added = Placed(is);
    // what both lists hold is neither taken away nor added (one for one, as a building may stand twice)
    for (auto it = removed.begin(); it != removed.end();) {
        const auto kept = std::ranges::find_if(added, [&](const auto &part) { return Same(part, *it); });
        if (kept != added.end()) {
            added.erase(kept);
            it = removed.erase(it);
        } else {
            ++it;
        }
    }
    for (const auto &gone : removed) {
        const auto found = std::ranges::find_if(placed, [&](const auto &part) { return Same(part, gone); });
        if (found != placed.end()) placed.erase(found);
    }
    for (const auto &part : added) placed.push_back(part);
    constexpr std::size_t kSlots = std::extent_v<decltype(dc::game::SV_GEORAMA_DATA::placed_parts)>;
    if (placed.size() >= kSlots) placed.resize(kSlots - 1); // the list keeps its end
    for (std::size_t i = 0; i < kSlots; ++i) {
        if (i < placed.size()) {
            geo.placed_parts[i] = placed[i];
        } else {
            geo.placed_parts[i] = {};
            geo.placed_parts[i].part_id = -1;
        }
    }
    std::memcpy(live[0].data, &now->geo, sizeof(now->geo));
    std::memcpy(live[1].data, now->chips, sizeof(now->chips));
    return true;
}

std::string ExportPlayer(const CSaveData &save) { return Export(kPlayerTag, SaveAccess::Player(const_cast<CSaveData &>(save))); }

bool ImportPlayer(std::string_view text, CSaveData &save, std::string &error) {
    if (!Import(kPlayerTag, text, SaveAccess::Player(save), error)) return false;
    // The text is another game's: a weapon slot outside a character's weapons would be read as one.
    CDngStatusData &dng = *save.GetDngStatus();
    constexpr int   kWeapons = static_cast<int>(std::extent_v<std::remove_reference_t<decltype(dng.chara_weapons.data[0])>>);
    for (s8 &slot : dng.equipped_weapon_slot.data) {
        if (slot < -1 || slot >= kWeapons) slot = 0;
    }
    return true;
}

namespace {

constexpr char kAccountsFile[] = "coop_accounts.json";
constexpr char kAccountsBadFile[] = "coop_accounts.json.bad";
std::map<std::string, Account, std::less<>> g_accounts;
// The slot whose guests file was there but couldn't be read: its guests aren't known, so "none" mustn't delete it.
std::filesystem::path g_accounts_unread;

bool PlainText(std::string_view text) {
    return std::ranges::all_of(text, [](char c) { return c >= 0x20 && c < 0x7F; });
}

// Puts a guests file that can't be read aside, so the next save neither deletes it nor writes over it.
void KeepBadAccounts(const std::filesystem::path &save_directory) {
    std::error_code error;
    if (!std::filesystem::exists(save_directory / kAccountsFile, error)) return;
    std::filesystem::remove(save_directory / kAccountsBadFile, error);
    std::filesystem::rename(save_directory / kAccountsFile, save_directory / kAccountsBadFile, error);
}

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
    if (!account.player.starts_with("DCP1") || account.player.size() > kAccountMax || !PlainText(account.player)) {
        error = "an account holds save.player_export text";
        return false;
    }
    // A name comes from another player: cut between characters, and kept only when it is text the save can hold.
    account.name = moonmod::net::CutUtf8(account.name, 24);
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

void AccountsClear() {
    g_accounts.clear();
    g_accounts_unread.clear();
}

void AccountsDelete(const std::filesystem::path &save_directory) {
    std::error_code error;
    std::filesystem::remove(save_directory / kAccountsFile, error);
}

bool AccountsSave(const std::filesystem::path &save_directory) {
    const bool unread = !g_accounts_unread.empty() && g_accounts_unread == save_directory;
    if (g_accounts.empty()) {
        if (!unread) AccountsDelete(save_directory); // a file that couldn't be read stays: its guests aren't ours to drop
        return true;
    }
    if (unread) {
        KeepBadAccounts(save_directory);
        g_accounts_unread.clear();
    }
    std::string text;
    try {
        nlohmann::json accounts = nlohmann::json::object();
        for (const auto &[id, account] : g_accounts) accounts[id] = {{"name", account.name}, {"player", account.player}};
        // Saving the game must never fail on a guest's name: bytes that aren't text are replaced, not thrown on.
        text = nlohmann::json{{"format", 1}, {"accounts", std::move(accounts)}}.dump(-1, ' ', false, nlohmann::json::error_handler_t::replace);
    } catch (const std::exception &failure) {
        std::fprintf(stderr, "co-op: could not write %s (%s)\n", (save_directory / kAccountsFile).string().c_str(), failure.what());
        return false;
    }
    if (::FilesWrite(save_directory / kAccountsFile, text.data(), text.size(), true) != ::FilesResult::kWritten) {
        std::fprintf(stderr, "co-op: could not write %s\n", (save_directory / kAccountsFile).string().c_str());
        return false;
    }
    return true;
}

void AccountsLoad(const std::filesystem::path &save_directory) {
    g_accounts.clear();
    g_accounts_unread.clear();
    std::error_code exists_error;
    if (!std::filesystem::exists(save_directory / kAccountsFile, exists_error)) return;
    std::string text;
    bool        opened = false;
    if (std::ifstream stream(save_directory / kAccountsFile, std::ios::binary); stream) {
        opened = true;
        text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    const nlohmann::json file = text.empty() ? nlohmann::json() : nlohmann::json::parse(text, nullptr, false);
    const bool           readable = opened && file.is_object() && file.contains("format") && file["format"].is_number_integer() &&
                                    file["format"].get<int>() == 1 && file.contains("accounts") && file["accounts"].is_object();
    if (!readable) {
        // Locked, cut short or damaged. The file stays where it is until this slot is saved with guests again (then it
        // is kept as coop_accounts.json.bad), so a bad read never ends in the guests' things being deleted.
        std::fprintf(stderr, "co-op: %s could not be read; this save's guests are not loaded, and the file is kept\n",
                     (save_directory / kAccountsFile).string().c_str());
        g_accounts_unread = save_directory;
        return;
    }
    for (const auto &[id, entry] : file["accounts"].items()) {
        if (!entry.is_object()) continue;
        const auto text_of = [&entry](const char *key) {
            return entry.contains(key) && entry[key].is_string() ? entry[key].get<std::string>() : std::string();
        };
        std::string error;
        AccountSet({id, text_of("name"), text_of("player")}, error);
    }
}

} // namespace moonmod::coop

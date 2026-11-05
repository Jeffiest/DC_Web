#include "gear.hpp"

#include <algorithm>
#include <fstream>
#include <iterator>
#include <print>
#include <system_error>

#include <nlohmann/json.hpp>

#include "itemdata.hpp"
#include "platform/files.hpp"
#include "userstatus.hpp"

namespace dc::game {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr const char *kFileName = "gear.json";
constexpr int kVersion = 2; // 2: five rarities (a version 1 file's 3, unique, is 4, legendary)

constexpr std::array<const char *, GEAR_KIND_COUNT> kKindNames = {"hat", "tunic", "gloves", "boots", "pouch"};
// The game's own most for the item bag (menu_misc.cpp's Pocket): its slots run to 103, three past it for overflow.
constexpr int kBagMost = 100;
constexpr std::array<const char *, GEAR_GUARD_COUNT> kGuardNames = {"freeze", "poison", "curse", "goo"};

std::vector<GearPiece> g_pieces;
u32 g_next_uid = 1;
u32 g_held = 0;
bool g_page_open = false;
std::array<std::string, GEAR_KIND_COUNT> g_kind_icons;
std::map<std::string, GearCharaParts>     g_parts;
int g_bag_added = 0;
std::map<int, std::vector<GearGood>> g_stock;

bool InBag(const GearPiece &piece) {
    return piece.worn_by.empty();
}

Json EffectsJson(const GearEffects &effects) {
    Json out = Json::object();
    Json guard = Json::object();
    for (int g = 0; g < GEAR_GUARD_COUNT; g++) {
        if (effects.guard[g] > 0) guard[kGuardNames[g]] = effects.guard[g];
    }
    if (!guard.empty()) out["guard"] = std::move(guard);
    if (effects.stamina > 0) out["stamina"] = effects.stamina;
    if (effects.lightness > 0) out["lightness"] = effects.lightness;
    if (effects.defence > 0) out["defence"] = effects.defence;
    if (effects.run_cost > 0) out["run_cost"] = effects.run_cost;
    if (effects.poise > 0) out["poise"] = effects.poise;
    if (effects.bag > 0) out["bag"] = effects.bag;
    return out;
}

int ReadCapped(const Json &object, const char *key, int cap) {
    const auto found = object.find(key);
    return found != object.end() && found->is_number_integer() ? std::clamp(found->get<int>(), 0, cap) : 0;
}

GearEffects ReadEffects(const Json &json) {
    GearEffects effects;
    if (!json.is_object()) return effects;
    if (const auto guard = json.find("guard"); guard != json.end() && guard->is_object()) {
        for (int g = 0; g < GEAR_GUARD_COUNT; g++) effects.guard[g] = ReadCapped(*guard, kGuardNames[g], GEAR_GUARD_CAP);
    }
    effects.stamina = ReadCapped(json, "stamina", GEAR_STAMINA_CAP);
    effects.lightness = ReadCapped(json, "lightness", GEAR_LIGHTNESS_CAP);
    effects.defence = ReadCapped(json, "defence", GEAR_DEFENCE_CAP);
    effects.run_cost = ReadCapped(json, "run_cost", GEAR_RUN_COST_CAP);
    effects.poise = ReadCapped(json, "poise", GEAR_POISE_CAP);
    effects.bag = ReadCapped(json, "bag", GEAR_BAG_SLOTS_CAP);
    return effects;
}

GearPiece *FindMutable(u32 uid) {
    const auto found = std::ranges::find(g_pieces, uid, &GearPiece::uid);
    return found == g_pieces.end() ? nullptr : &*found;
}

GearPiece *WornMutable(int chara, int kind) {
    if (chara < 0 || chara >= CharaCount()) return nullptr;
    const std::string id = CharaIdName(chara);
    for (GearPiece &piece : g_pieces) {
        if (piece.kind == kind && piece.worn_by == id) return &piece;
    }
    return nullptr;
}

} // namespace

const char *GearKindName(int kind) {
    return kind >= 0 && kind < GEAR_KIND_COUNT ? kKindNames[kind] : "";
}

int GearKindFromName(const std::string &name) {
    for (int k = 0; k < GEAR_KIND_COUNT; k++) {
        if (name == kKindNames[k]) return k;
    }
    return -1;
}

const char *GearGuardName(int guard) {
    return guard >= 0 && guard < GEAR_GUARD_COUNT ? kGuardNames[guard] : "";
}

int GearGuardFromName(const std::string &name) {
    for (int g = 0; g < GEAR_GUARD_COUNT; g++) {
        if (name == kGuardNames[g]) return g;
    }
    return -1;
}

const std::vector<GearPiece> &GearPieces() {
    return g_pieces;
}

const GearPiece *FindGear(u32 uid) {
    return FindMutable(uid);
}

int GearBagCount() {
    return static_cast<int>(std::ranges::count_if(g_pieces, InBag));
}

u32 AddGear(GearPiece piece) {
    if (GearBagCount() >= GEAR_BAG_MAX) return 0;
    piece.uid = g_next_uid++;
    piece.worn_by.clear();
    piece.kind = std::clamp(piece.kind, 0, GEAR_KIND_COUNT - 1);
    piece.rarity = std::clamp(piece.rarity, 0, GEAR_RARITY_COUNT - 1);
    g_pieces.push_back(std::move(piece));
    return g_pieces.back().uid;
}

bool RemoveGear(u32 uid) {
    return std::erase_if(g_pieces, [uid](const GearPiece &piece) { return piece.uid == uid; }) > 0;
}

const GearPiece *WornGear(int chara, int kind) {
    return WornMutable(chara, kind);
}

bool EquipGear(int chara, u32 uid) {
    GearPiece *piece = FindMutable(uid);
    if (piece == nullptr || chara < 0 || chara >= CharaCount()) return false;
    if (GearPiece *worn = WornMutable(chara, piece->kind); worn != nullptr && worn != piece) worn->worn_by.clear();
    piece->worn_by = CharaIdName(chara);
    return true;
}

bool UnequipGear(int chara, int kind) {
    GearPiece *worn = WornMutable(chara, kind);
    if (worn == nullptr || GearBagCount() >= GEAR_BAG_MAX) return false;
    worn->worn_by.clear();
    return true;
}

const GearPiece *GearBagPiece(int index) {
    for (const GearPiece &piece : g_pieces) {
        if (!InBag(piece) || piece.uid == g_held) continue;
        if (index-- == 0) return &piece;
    }
    return nullptr;
}

u32 HeldGear() {
    return FindGear(g_held) != nullptr ? g_held : 0;
}

void SetHeldGear(u32 uid) {
    g_held = uid;
}

u32 TakeOffGear(int chara, int kind) {
    GearPiece *worn = WornMutable(chara, kind);
    if (worn == nullptr) return 0;
    worn->worn_by.clear();
    return worn->uid;
}

bool GearPageOpen() {
    return g_page_open;
}

void SetGearStock(int shop_no, std::vector<GearGood> goods) {
    if (goods.empty()) {
        g_stock.erase(shop_no);
        return;
    }

    if (goods.size() > GEAR_STOCK_MAX) {
        goods.resize(GEAR_STOCK_MAX);
    }

    g_stock[shop_no] = std::move(goods);
}

std::vector<GearGood> *GearStock(int shop_no) {
    const auto found = g_stock.find(shop_no);
    return found != g_stock.end() ? &found->second : nullptr;
}

void ClearGearStock() {
    g_stock.clear();
}

void SetGearPageOpen(bool open) {
    g_page_open = open;
    if (!open) g_held = 0;
}

const std::string &GearKindIcon(int kind) {
    static const std::string none;
    return kind >= 0 && kind < GEAR_KIND_COUNT ? g_kind_icons[kind] : none;
}

void SetGearKindIcon(int kind, std::string game_path) {
    if (kind >= 0 && kind < GEAR_KIND_COUNT) g_kind_icons[kind] = std::move(game_path);
}

const GearKindParts &GearPartsFor(int chara, int kind) {
    static const GearKindParts none;
    if (kind < 0 || kind >= GEAR_KIND_COUNT || chara < 0 || chara >= CharaCount()) return none;
    const auto found = g_parts.find(CharaIdName(chara));
    return found != g_parts.end() ? found->second[kind] : none;
}

void SetGearParts(std::map<std::string, GearCharaParts> parts) {
    g_parts = std::move(parts);
}

GearEffects GearWornEffects(int chara) {
    GearEffects total;
    if (chara < 0 || chara >= CharaCount() || g_pieces.empty()) return total;
    const std::string id = CharaIdName(chara);
    for (const GearPiece &piece : g_pieces) {
        if (piece.worn_by != id) continue;
        for (int g = 0; g < GEAR_GUARD_COUNT; g++) total.guard[g] += piece.effects.guard[g];
        total.stamina += piece.effects.stamina;
        total.lightness += piece.effects.lightness;
        total.defence += piece.effects.defence;
        total.run_cost += piece.effects.run_cost;
        total.poise += piece.effects.poise;
        total.bag += piece.effects.bag;
    }
    for (int &guard : total.guard) guard = std::min(guard, GEAR_GUARD_CAP);
    total.stamina = std::min(total.stamina, GEAR_STAMINA_CAP);
    total.lightness = std::min(total.lightness, GEAR_LIGHTNESS_CAP);
    total.defence = std::min(total.defence, GEAR_DEFENCE_CAP);
    total.run_cost = std::min(total.run_cost, GEAR_RUN_COST_CAP);
    total.poise = std::min(total.poise, GEAR_POISE_CAP);
    total.bag = std::min(total.bag, GEAR_BAG_SLOTS_CAP);
    return total;
}

int GearBagSlots() {
    int most = 0;
    for (int chara = 0; chara < CharaCount(); chara++) most = std::max(most, GearWornEffects(chara).bag);
    return most;
}

int GearSyncBag(ITEM_PACK &pack) {
    const int base = std::clamp(pack.num - g_bag_added, 0, kBagMost);
    const int target = base + std::clamp(GearBagSlots(), 0, kBagMost - base);
    int       size = std::clamp<int>(pack.num, 0, kBagMost);
    const auto empty = [&](int slot) { return pack.item[slot] < ITEM_DUNGEON_START; };

    if (target > size) {
        // Slots opened are emptied of anything left in them, as a Pocket's are; an overflow item stays, now in the bag.
        for (int slot = size; slot < target; slot++) {
            if (empty(slot)) {
                pack.item[slot] = -1;
                pack.item_vol[slot] = 0;
            }
        }
        size = target;
    } else if (target < size) {
        for (int slot = size - 1; slot >= target; slot--) {
            if (empty(slot)) continue;
            int free = 0;
            while (free < target && !empty(free)) free++;
            if (free == target) break;
            pack.item[free] = pack.item[slot];
            pack.item_vol[free] = pack.item_vol[slot];
            pack.item[slot] = -1;
            pack.item_vol[slot] = 0;
        }
        while (size > target && empty(size - 1)) size--;
    }

    pack.num = static_cast<s8>(size);
    g_bag_added = size - base;
    return g_bag_added;
}

int GearBagAdded() {
    return g_bag_added;
}

bool GearGuards(int chara, int guard, int roll) {
    return guard >= 0 && guard < GEAR_GUARD_COUNT && roll < GearWornEffects(chara).guard[guard];
}

float GearSwingScale(int chara) {
    return 1.0f - static_cast<float>(GearWornEffects(chara).lightness) / 100.0f;
}

float GearRunScale(int chara) {
    return 1.0f - static_cast<float>(GearWornEffects(chara).run_cost) / 100.0f;
}

void ClearGear() {
    g_pieces.clear();
    g_next_uid = 1;
    g_held = 0;
    g_bag_added = 0;
}

std::string GearJson() {
    Json pieces = Json::array();
    for (const GearPiece &piece : g_pieces) {
        Json entry = {{"uid", piece.uid},         {"kind", kKindNames[piece.kind]}, {"rarity", piece.rarity},
                      {"name", piece.name},       {"value", piece.value},           {"effects", EffectsJson(piece.effects)}};
        if (!piece.worn_by.empty()) entry["worn_by"] = piece.worn_by;
        if (!piece.icon.empty()) entry["icon"] = piece.icon;
        if (piece.color != 0xFFFFFF) entry["color"] = piece.color;
        pieces.push_back(std::move(entry));
    }
    Json file = {{"version", kVersion}, {"next_uid", g_next_uid}, {"pieces", std::move(pieces)}};
    if (g_bag_added > 0) file["bag_added"] = g_bag_added;
    return file.dump(2);
}

bool GearApply(const std::string &text) {
    ClearGear();
    const Json file = Json::parse(text, nullptr, false);
    if (!file.is_object() || !file.contains("pieces") || !file["pieces"].is_array()) return false;
    std::vector<GearPiece> pieces;
    u32 next_uid = file.value("next_uid", 1u);
    const bool four_rarities = file.value("version", 1) < 2;
    for (const Json &entry : file["pieces"]) {
        if (!entry.is_object() || !entry.contains("uid") || !entry["uid"].is_number_integer() ||
            entry["uid"].get<std::int64_t>() < 1 || entry["uid"].get<std::int64_t>() > 0xFFFFFFFF || !entry.contains("kind") ||
            !entry["kind"].is_string()) {
            return false;
        }
        GearPiece piece;
        piece.uid = entry["uid"].get<u32>();
        piece.kind = GearKindFromName(entry["kind"].get<std::string>());
        if (piece.uid == 0 || piece.kind < 0 || std::ranges::contains(pieces, piece.uid, &GearPiece::uid)) return false;
        piece.rarity = std::clamp(entry.value("rarity", 0), 0, GEAR_RARITY_COUNT - 1);
        if (four_rarities && piece.rarity == 3) piece.rarity = 4; // unique was the top; legendary is now
        piece.name = entry.value("name", std::string{}).substr(0, GEAR_NAME_MAX);
        piece.value = std::max(entry.value("value", 0), 0);
        piece.effects = ReadEffects(entry.value("effects", Json::object()));
        piece.worn_by = entry.value("worn_by", std::string{});
        piece.icon = entry.value("icon", std::string{});
        piece.color = entry.value("color", 0xFFFFFFu) & 0xFFFFFFu;
        // Two pieces of a kind on one character (a file put together by hand): the later goes to the bag.
        if (!piece.worn_by.empty() && std::ranges::any_of(pieces, [&](const GearPiece &other) {
                return other.kind == piece.kind && other.worn_by == piece.worn_by;
            })) {
            piece.worn_by.clear();
        }
        next_uid = std::max(next_uid, piece.uid + 1);
        pieces.push_back(std::move(piece));
    }
    g_pieces = std::move(pieces);
    g_next_uid = next_uid;
    g_bag_added = std::clamp(file.value("bag_added", 0), 0, GEAR_BAG_SLOTS_CAP);
    return true;
}

bool GearSave(const fs::path &save_directory) {
    if (g_pieces.empty() && g_bag_added == 0) {
        GearDelete(save_directory);
        return true;
    }
    const std::string text = GearJson();
    if (FilesWrite(save_directory / kFileName, text.data(), text.size(), true) != FilesResult::kWritten) {
        std::println(stderr, "gear: could not write {}", (save_directory / kFileName).string());
        return false;
    }
    return true;
}

void GearLoad(const fs::path &save_directory) {
    std::string text;
    if (std::ifstream stream(save_directory / kFileName, std::ios::binary); stream) {
        text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    if (text.empty()) {
        ClearGear();
    } else if (!GearApply(text)) {
        std::println(stderr, "gear: {} could not be read; the party starts with no gear", (save_directory / kFileName).string());
    }
}

void GearDelete(const fs::path &save_directory) {
    std::error_code error;
    fs::remove(save_directory / kFileName, error);
}

} // namespace dc::game

#include "charm_slots.hpp"

#include <algorithm>
#include <array>
#include <fstream>
#include <iterator>
#include <print>
#include <system_error>

#include <nlohmann/json.hpp>

#include "itemdata.hpp"
#include "platform/files.hpp"

namespace dc::game {

namespace {

namespace fs = std::filesystem;
using Json = nlohmann::json;

constexpr const char *kFileName = "charms.json";
constexpr int kVersion = 1;

std::array<std::array<CharmSlot, CHARM_SLOT_COUNT>, CHARA_MAX> g_slots;
int g_open = 0;
bool g_feather_open = false;

int AmuletSlotsShown(int chara) {
    for (int i = CHARM_SLOT_MAX - 1; i >= g_open; i--) {
        if (g_slots[chara][i].item_no >= 0) {
            return i + 1;
        }
    }
    return g_open;
}

bool FeatherSlotShown(int chara) {
    return g_feather_open || g_slots[chara][CHARM_FEATHER_SLOT].item_no >= 0;
}
// What the file held for characters no mod registers now, written back as it was.
Json g_unregistered = Json::object();

bool AnyCharm() {
    return std::ranges::any_of(g_slots, [](const auto &slots) {
        return std::ranges::any_of(slots, [](const CharmSlot &slot) { return slot.item_no >= 0; });
    });
}

} // namespace

bool IsCharmItem(int item_no) {
    return CharmSlotAccepts(0, item_no) || CharmSlotAccepts(CHARM_FEATHER_SLOT, item_no);
}

bool CharmSlotAccepts(int index, int item_no) {
    if (index == CHARM_FEATHER_SLOT) {
        return item_no == ITEM_DRAN_S_FEATHER;
    }
    switch (item_no) {
        case ITEM_ANTI_FREEZE_AMULET:
        case ITEM_ANTICURSEAMULET:
        case ITEM_ANTIGOO_AMULET:
        case ITEM_ANTIDOTE_AMULET:
            return index >= 0 && index < CHARM_SLOT_MAX;
        default:
            return false;
    }
}

void CharmHudCell(int index, int &u, int &v) {
    // The free parts of the 256x192 sheet: a 48x48 block beside the weapon icons, and a square below the "speed" plate.
    constexpr std::array<std::array<int, 2>, CHARM_SLOT_COUNT> kCells = {
        {{0x40, 0x20}, {0x58, 0x20}, {0x40, 0x38}, {0x58, 0x38}, {0x68, 0x54}}};
    u = kCells[index][0];
    v = kCells[index][1];
}

int CharmUsesShown(int item_no, int uses) {
    uses = std::max(uses, 0);
    return item_no == ITEM_DRAN_S_FEATHER ? (uses + 59) / 60 : uses;
}

int CharmSlotsOpen() {
    return g_open;
}

void SetCharmSlotsOpen(int count) {
    g_open = std::clamp(count, 0, CHARM_SLOT_MAX);
}

bool CharmFeatherOpen() {
    return g_feather_open;
}

void SetCharmFeatherOpen(bool open) {
    g_feather_open = open;
}

int CharmSlotsShown(int chara) {
    if (chara < 0 || chara >= CHARA_MAX) {
        return 0;
    }
    return AmuletSlotsShown(chara) + (FeatherSlotShown(chara) ? 1 : 0);
}

int CharmSlotIndex(int chara, int position) {
    const int amulets = chara >= 0 && chara < CHARA_MAX ? AmuletSlotsShown(chara) : 0;
    return position < amulets ? position : CHARM_FEATHER_SLOT;
}

CharmSlot &CharmSlotAt(int chara, int index) {
    return g_slots[chara][index];
}

int FindCharmSlot(int chara, int item_no) {
    if (chara < 0 || chara >= CHARA_MAX) {
        return -1;
    }
    for (int i = 0; i < CHARM_SLOT_COUNT; i++) {
        if (g_slots[chara][i].item_no == item_no) {
            return i;
        }
    }
    return -1;
}

CharmGuard SpendCharm(int chara, int item_no) {
    const int index = FindCharmSlot(chara, item_no);
    if (index < 0) {
        return CharmGuard::None;
    }
    CharmSlot &slot = g_slots[chara][index];
    if (--slot.uses <= 0) {
        slot = CharmSlot{};
        return CharmGuard::Broke;
    }
    return CharmGuard::Guarded;
}

int WornCharmPrice(int price, int uses, int full_uses) {
    if (full_uses <= 0 || uses >= full_uses) {
        return price;
    }
    if (uses * 2 < full_uses) {
        return 0;
    }
    return price * uses / full_uses;
}

void ClearCharmSlots() {
    for (auto &slots : g_slots) {
        slots.fill(CharmSlot{});
    }
    g_unregistered = Json::object();
}

std::string CharmSlotsJson() {
    Json characters = g_unregistered;
    for (int chara = 0; chara < CharaCount(); chara++) {
        Json slots = Json::array();
        bool any = false;
        for (const CharmSlot &slot : g_slots[chara]) {
            any = any || slot.item_no >= 0;
            slots.push_back(slot.item_no >= 0 ? Json{{"item_no", slot.item_no}, {"uses", slot.uses}} : Json(nullptr));
        }
        if (any) {
            characters[CharaIdName(chara)] = std::move(slots);
        } else {
            characters.erase(CharaIdName(chara));
        }
    }
    return Json{{"version", kVersion}, {"characters", std::move(characters)}}.dump(2);
}

bool CharmSlotsApply(const std::string &text) {
    ClearCharmSlots();
    const Json file = Json::parse(text, nullptr, false);
    if (!file.is_object() || !file.contains("characters") || !file["characters"].is_object()) {
        return false;
    }
    for (const auto &[id, slots] : file["characters"].items()) {
        int chara = -1;
        for (int c = 0; c < CharaCount(); c++) {
            if (id == CharaIdName(c)) {
                chara = c;
            }
        }
        if (chara < 0) {
            g_unregistered[id] = slots;
            continue;
        }
        if (!slots.is_array()) {
            ClearCharmSlots();
            return false;
        }
        for (int i = 0; i < CHARM_SLOT_COUNT && i < static_cast<int>(slots.size()); i++) {
            const Json &slot = slots[i];
            if (slot.is_null()) {
                continue;
            }
            if (!slot.is_object() || !slot.contains("item_no") || !slot["item_no"].is_number_integer() ||
                !IsCharmItem(slot["item_no"].get<int>())) {
                ClearCharmSlots();
                return false;
            }
            const int item_no = slot["item_no"].get<int>();
            // A charm kept in a slot not of its kind (a Feather from before it had a slot of its own) goes to a free
            // one that is.
            int index = i;
            for (int other = 0; !CharmSlotAccepts(index, item_no) && other < CHARM_SLOT_COUNT; other++) {
                if (g_slots[chara][other].item_no < 0 && CharmSlotAccepts(other, item_no)) {
                    index = other;
                }
            }
            if (!CharmSlotAccepts(index, item_no) || g_slots[chara][index].item_no >= 0) {
                ClearCharmSlots();
                return false;
            }
            g_slots[chara][index].item_no = static_cast<s16>(item_no);
            g_slots[chara][index].uses = slot.value("uses", 0);
        }
    }
    return true;
}

bool CharmSlotsSave(const fs::path &save_directory) {
    if (!AnyCharm() && g_unregistered.empty()) {
        CharmSlotsDelete(save_directory);
        return true;
    }
    const std::string text = CharmSlotsJson();
    if (FilesWrite(save_directory / kFileName, text.data(), text.size(), true) != FilesResult::kWritten) {
        std::println(stderr, "charms: could not write {}", (save_directory / kFileName).string());
        return false;
    }
    return true;
}

void CharmSlotsLoad(const fs::path &save_directory) {
    std::string text;
    if (std::ifstream stream(save_directory / kFileName, std::ios::binary); stream) {
        text.assign(std::istreambuf_iterator<char>(stream), std::istreambuf_iterator<char>());
    }
    if (text.empty()) {
        ClearCharmSlots();
    } else if (!CharmSlotsApply(text)) {
        std::println(stderr, "charms: {} could not be read; the charm slots start empty",
                     (save_directory / kFileName).string());
    }
}

void CharmSlotsDelete(const fs::path &save_directory) {
    std::error_code error;
    fs::remove(save_directory / kFileName, error);
}

} // namespace dc::game

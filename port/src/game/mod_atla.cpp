#include "mod_atla.hpp"

#include <array>
#include <vector>

#include "text/gametext.hpp"

namespace dc::game {

namespace {

struct Entry {
    ModAtla          atla;
    std::vector<s16> name_codes;
};

std::vector<Entry> &Entries() {
    static std::vector<Entry> entries;
    return entries;
}

std::function<void(const ModAtla &)> &Handler() {
    static std::function<void(const ModAtla &)> handler;
    return handler;
}

} // namespace

int RegisterModAtla(std::string_view owner, std::string_view id, std::string_view name) {
    auto &entries = Entries();
    int   index = -1;
    for (int i = 0; i < static_cast<int>(entries.size()); i++) {
        if (entries[i].atla.owner == owner && entries[i].atla.id == id) {
            index = i;
        }
    }
    if (index < 0) {
        if (static_cast<int>(entries.size()) >= kModAtlaMax) {
            return -1;
        }
        entries.push_back({{std::string(owner), std::string(id), {}}, {}});
        index = static_cast<int>(entries.size()) - 1;
    }
    Entry &entry = entries[index];
    entry.atla.name = name;
    entry.name_codes.clear();
    dc::text::GameTextEncode(entry.atla.name, entry.name_codes);
    return index;
}

const ModAtla *ModAtlaOf(int atra_no) {
    const int index = atra_no - kModAtlaBase;
    return IsModAtla(atra_no) && index < static_cast<int>(Entries().size()) ? &Entries()[index].atla : nullptr;
}

const s16 *ModAtlaNameLine(int line) {
    const int index = line - kModAtlaNameLine;
    return index >= 0 && index < static_cast<int>(Entries().size()) ? Entries()[index].name_codes.data() : nullptr;
}

void SetModAtlaCollectedHandler(std::function<void(const ModAtla &)> handler) {
    Handler() = std::move(handler);
}

void ModAtlaCollected(int atra_no) {
    if (const ModAtla *atla = ModAtlaOf(atra_no); atla != nullptr && Handler()) {
        Handler()(*atla);
    }
}

void ClearModAtlas() {
    Entries().clear();
}

} // namespace dc::game

#include "playable_chara.hpp"

#include "moonmod/native_resource_overrides.hpp"
#include "userstatus.hpp"

#include <algorithm>
#include <format>
#include <vector>

namespace dc::game {

namespace {

constexpr std::array<const char *, SAVE_CHARA_COUNT> kNativeIds = {"toan", "xiao", "goro", "ruby", "ungaga", "osmond"};

constexpr std::array<const char *, SAVE_CHARA_COUNT> kNativePacks = {
    "dun/mainchara/c01d.chr", "dun/mainchara/c04b.chr", "dun/mainchara/c06b.chr",
    "dun/mainchara/c05a.chr", "dun/mainchara/c10b.chr", "dun/mainchara/c18a.chr",
};

std::vector<PlayableChara> &Extras() {
    static std::vector<PlayableChara> extras;
    return extras;
}

bool ValidId(std::string_view id) {
    return !id.empty() && id.size() <= 15 && std::ranges::all_of(id, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

// The name entry's letter codes run A-Z from 162 and a-z from 188 (gameutil.cpp's NameRegistCodeJtoE).
bool EncodeName(std::string_view name, std::array<s16, 32> &codes) {
    if (name.empty() || name.size() > 10) {
        return false;
    }
    codes.fill(0);
    for (std::size_t i = 0; i < name.size(); i++) {
        const char c = name[i];
        if (c >= 'A' && c <= 'Z') {
            codes[i] = static_cast<s16>(162 + (c - 'A'));
        } else if (c >= 'a' && c <= 'z') {
            codes[i] = static_cast<s16>(188 + (c - 'a'));
        } else {
            return false;
        }
    }
    return true;
}

// dun/mainchara/<stem>.chr, the stem a short file name.
bool ValidPack(std::string_view path) {
    constexpr std::string_view prefix = "dun/mainchara/";
    if (!path.starts_with(prefix) || !path.ends_with(".chr")) {
        return false;
    }
    const std::string_view stem = path.substr(prefix.size(), path.size() - prefix.size() - 4);
    return !stem.empty() && stem.size() <= 15 && std::ranges::all_of(stem, [](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_';
    });
}

} // namespace

int CharaCount() {
    return SAVE_CHARA_COUNT + static_cast<int>(Extras().size());
}

int CombatChara(int chara) {
    const PlayableChara *extra = ExtraChara(chara);
    return extra != nullptr ? extra->fights_as : chara;
}

int CharaSlotsInPlay(int party_size) {
    return std::clamp(party_size, SAVE_CHARA_COUNT, CharaCount());
}

namespace {
std::array<bool, CHARA_MAX> &Joined() {
    static std::array<bool, CHARA_MAX> joined{};
    return joined;
}
} // namespace

bool CharaJoined(int chara) {
    return chara >= SAVE_CHARA_COUNT && chara < CharaCount() && Joined()[chara];
}

void SetCharaJoined(int chara, bool joined) {
    if (chara >= SAVE_CHARA_COUNT && chara < CHARA_MAX) {
        Joined()[chara] = joined;
    }
}

void ClearCharaJoined() {
    Joined().fill(false);
}

bool InParty(const CUserStatus &status, int chara) {
    return chara >= 0 && chara < CharaCount() && (chara < status.party_size || CharaJoined(chara));
}

int PartyCount(const CUserStatus &status) {
    int count = 0;
    for (int chara = 0; chara < CharaCount(); chara++) {
        count += InParty(status, chara) ? 1 : 0;
    }
    return count;
}

int NextPartyMember(const CUserStatus &status, int chara, int steps) {
    const int count = CharaCount();
    const int step = steps < 0 ? -1 : 1;
    for (int left = steps < 0 ? -steps : steps; left > 0; left--) {
        for (int tries = 0; tries < count; tries++) {
            chara = (chara + step + count) % count;
            if (InParty(status, chara)) {
                break;
            }
        }
    }
    return chara;
}

int CharaSlotsInPlay(const CUserStatus &status) {
    int slots = CharaSlotsInPlay(status.party_size);
    for (int chara = slots; chara < CharaCount(); chara++) {
        if (CharaJoined(chara)) {
            slots = chara + 1;
        }
    }
    return slots;
}

const PlayableChara *ExtraChara(int chara) {
    const int index = chara - SAVE_CHARA_COUNT;
    return index >= 0 && index < static_cast<int>(Extras().size()) ? &Extras()[index] : nullptr;
}

const char *CharaIdName(int chara) {
    if (chara >= 0 && chara < SAVE_CHARA_COUNT) {
        return kNativeIds[chara];
    }
    const PlayableChara *extra = ExtraChara(chara);
    return extra != nullptr ? extra->id.c_str() : nullptr;
}

int FindPlayableChara(std::string_view id) {
    for (int chara = 0; chara < CharaCount(); chara++) {
        if (id == CharaIdName(chara)) {
            return chara;
        }
    }
    return -1;
}

int RegisterPlayableChara(PlayableChara entry, std::string &error) {
    if (!ValidId(entry.id)) {
        error = "id must be 1-15 lowercase letters, digits or '_'";
        return -1;
    }
    const int existing = FindPlayableChara(entry.id);
    if (existing >= 0 && existing < SAVE_CHARA_COUNT) {
        error = "id names one of the game's own characters";
        return -1;
    }
    if (!EncodeName(entry.name, entry.name_codes)) {
        error = "name must be 1-10 letters";
        return -1;
    }
    if (!ValidPack(entry.pack)) {
        error = "pack must be dun/mainchara/<stem>.chr";
        return -1;
    }
    for (int chara = SAVE_CHARA_COUNT; chara < CharaCount(); chara++) {
        if (chara != existing && ExtraChara(chara)->pack == entry.pack) {
            error = "pack is already another character's";
            return -1;
        }
    }
    if (entry.fights_as < CHARA_TOAN || entry.fights_as >= SAVE_CHARA_COUNT) {
        error = "fights_as must be one of the game's own characters";
        return -1;
    }
    if (entry.hp < 1 || entry.hp > 999 || entry.defense < 0 || entry.defense > 999) {
        error = "hp must be 1-999 and defense 0-999";
        return -1;
    }
    if (existing >= 0) {
        Extras()[existing - SAVE_CHARA_COUNT] = std::move(entry);
        return existing;
    }
    if (CharaCount() >= CHARA_MAX) {
        error = "every playable character index is taken";
        return -1;
    }
    Extras().push_back(std::move(entry));
    return CharaCount() - 1;
}

void ClearPlayableCharas() {
    Extras().clear();
}

namespace {

std::array<CharaForm, CHARA_MAX> &Forms() {
    static std::array<CharaForm, CHARA_MAX> forms;
    return forms;
}

} // namespace

const CharaForm *ActiveCharaForm(int chara) {
    return chara >= 0 && chara < CHARA_MAX && !Forms()[chara].pack.empty() ? &Forms()[chara] : nullptr;
}

void SetCharaForm(int chara, CharaForm form) {
    if (chara >= 0 && chara < CHARA_MAX) Forms()[chara] = std::move(form);
}

bool ClearCharaForm(int chara, std::string_view owner) {
    bool ended = false;
    for (int c = 0; c < CHARA_MAX; c++) {
        CharaForm &form = Forms()[c];
        if ((chara < 0 || c == chara) && !form.pack.empty() && (owner.empty() || form.owner == owner)) {
            form = {};
            ended = true;
        }
    }
    return ended;
}

bool IsPlayableCharaPack(std::string_view path) {
    for (int chara = SAVE_CHARA_COUNT; chara < CharaCount(); chara++) {
        if (path == std::format("commenu/dungeon/c0{}model.pak", chara + 1)) {
            return true; // his item-menu model (StartLoadCharaMDS), like the six's c0Nmodel.pak
        }
        if (path == std::format("sound/voice/voice{}.snd", 100 + chara)) {
            return true; // his own voice set (CharaVoiceSet)
        }
    }
    return std::ranges::any_of(Extras(), [path](const PlayableChara &extra) { return extra.pack == path; }) ||
           std::ranges::any_of(Forms(), [path](const CharaForm &form) { return !form.pack.empty() && form.pack == path; });
}

int CharaVoiceSet(int chara) {
    if (chara >= SAVE_CHARA_COUNT && chara < CharaCount() &&
        moonmod::FindNativeResourceOverride(std::format("sound/voice/voice{}.snd", 100 + chara))) {
        return 100 + chara;
    }
    return CombatChara(chara);
}

const char *CharaPackPath(int chara) {
    if (const CharaForm *form = ActiveCharaForm(chara)) {
        return form->pack.c_str();
    }
    if (chara >= 0 && chara < SAVE_CHARA_COUNT) {
        return kNativePacks[chara];
    }
    const PlayableChara *extra = ExtraChara(chara);
    return extra != nullptr ? extra->pack.c_str() : kNativePacks[CHARA_TOAN];
}

} // namespace dc::game

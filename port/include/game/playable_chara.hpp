#pragma once

/**
 * @file
 * MoonMod: the playable characters past the game's six. A mod registers each one (characters.register_playable)
 * before the session that plays it starts; it takes the next index from SAVE_CHARA_COUNT up to CHARA_MAX - 1 and
 * keeps it for as long as the game runs. None of them is ever written to a save (userstatus.hpp's CharaSlots).
 */

#include <array>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "itemdata.hpp"

namespace dc::game {

/** One registered character past the game's six. */
struct PlayableChara {
    std::string id;       /**< API id, e.g. "opar": lowercase letters, digits and '_'. */
    std::string name;     /**< Display name, letters only, up to 10, e.g. "Opar". */
    std::string pack;     /**< Character pack, e.g. "dun/mainchara/c07d.chr", supplied by a mod's resource override. */
    std::string portrait; /**< 128x128 PNG drawn where the menus show faces, or "" for the empty face. */
    std::string title;    /**< What the item menu says of him under his picture ("Boy from Nolun" for Toan), or "" for his name. */
    int fights_as = CHARA_TOAN; /**< The game's character whose action, motions, weapons and voice he borrows. */
    int hp = 70;                /**< Starting and maximum health. */
    int defense = 3;            /**< Starting defence. */
    std::array<s16, 32> name_codes{}; /**< name in the name entry's letter codes, as CSaveData::GetCharaName holds them. */
};

/** The registered character at index chara, or null for the game's six and for an unused index. */
const PlayableChara *ExtraChara(int chara);

/** The index of the playable character whose API id is id, the game's six included, or -1. */
int FindPlayableChara(std::string_view id);

/**
 * Registers a character, or updates the one registered under the same id (keeping its index). Returns its index,
 * or -1 with error set when an entry is invalid or every index up to CHARA_MAX is taken.
 */
int RegisterPlayableChara(PlayableChara entry, std::string &error);

/** Forgets every registered character (tests). */
void ClearPlayableCharas();

/** Whether path is a registered character's pack (a mod file, which no retail data has). */
/**
 * Whether a path is a registered character's own file: his pack, a form's pack, or his item-menu model pack
 * (commenu/dungeon/c0<index + 1>model.pak).
 */
bool IsPlayableCharaPack(std::string_view path);

/**
 * The voice set a character's voice cues play from (SndVoiceLoad): a registered character's own,
 * sound/voice/voice<100 + index>.snd, when his mod supplies it, else the voice of the character he fights as.
 */
int CharaVoiceSet(int chara);

/** The character pack chara loads in the dungeon and the menus' character change. */
const char *CharaPackPath(int chara);

/**
 * A character's form (player.form_set): while it lasts his model loads from `pack` in place of his own, the frames
 * the player code looks up by name are found under their stand-ins (`frames`: {looked up, stand-in}), and his weapon
 * is hidden if `hide_weapon`, and the model is drawn `scale` times its size. Any character can take one, the six included.
 */
struct CharaForm {
    std::string                                      owner; /**< The mod that set it. */
    std::string                                      pack;  /**< e.g. "dun/mainchara/c07f.chr", a mod's resource override. */
    std::vector<std::pair<std::string, std::string>> frames;
    bool                                             hide_weapon = true;
    float                                            scale = 1.0f; /**< The model drawn this much its size. */
};

const CharaForm *ActiveCharaForm(int chara);

void SetCharaForm(int chara, CharaForm form);

/**
 * Ends `chara`'s form (-1: every character's); with `owner`, only forms that mod set. True if one ended.
 */
bool ClearCharaForm(int chara, std::string_view owner = {});

/**
 * The text code that prints a character's name: -0x506..-0x501 the game's six (the message files' own), then
 * -0x507 down for the registered ones, a stretch no other text code uses.
 */
constexpr int CharaNameCode(int chara) {
    return chara < SAVE_CHARA_COUNT ? chara - 0x506 : -0x501 - chara;
}

/**
 * The system message line that holds just a character's name: 50 + index for the game's six (the message file's),
 * EXTRA_CHARA_NAME_LINE + index for the registered ones, which ClsMes::GetTextLineDataTop_system makes up.
 */
constexpr int EXTRA_CHARA_NAME_LINE = 31000; // past every message file's lines (2000 on is the system file's item text)

constexpr int CharaNameLine(int chara) {
    return chara < SAVE_CHARA_COUNT ? 50 + chara : EXTRA_CHARA_NAME_LINE + chara;
}

/**
 * The menu message line the item menu quotes under a character's picture: 0x32 + index for the game's six (their
 * titles, "Boy from Nolun"), EXTRA_CHARA_TITLE_LINE + index for the registered ones (their PlayableChara title, or
 * name), which ClsMes::GetTextLineDataTop makes up. Out of the way of every message file's lines.
 */
constexpr int EXTRA_CHARA_TITLE_LINE = 32000;

constexpr int CharaTitleLine(int chara) {
    return chara < SAVE_CHARA_COUNT ? 0x32 + chara : EXTRA_CHARA_TITLE_LINE + chara;
}

} // namespace dc::game

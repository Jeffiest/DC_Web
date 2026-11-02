#pragma once

/**
 * @file
 * MoonMod: the registered characters' part of a save (playable_chara.hpp), kept beside it as characters.json, as the
 * quests keep theirs. A save holds the game's six only, so it still loads without the mods; this file holds, for each
 * registered character by id, whether he has joined the party, his life, water, defence, ailments, skill, weapons and
 * equipped slot, and whether he led the party. A character the file names but no mod registers now is kept as it was.
 */

#include <filesystem>
#include <string>

namespace dc::game {

class CUserStatus;

/** Writes status's registered characters to save_directory/characters.json. Returns false when it cannot. */
bool CharaExtrasSave(const std::filesystem::path &save_directory, const CUserStatus &status);

/**
 * Reads save_directory/characters.json into status: each registered character it names gets his slots back and joins
 * if he had; every other registered character starts afresh and out of the party.
 */
void CharaExtrasLoad(const std::filesystem::path &save_directory, CUserStatus &status);

void CharaExtrasDelete(const std::filesystem::path &save_directory);

/** The file's text for status (tests), merged over `previous` (a file's text, kept for characters not registered). */
std::string CharaExtrasJson(const CUserStatus &status, const std::string &previous);

/** Reads the file's text into status (tests); false if it is not one. */
bool CharaExtrasApply(const std::string &text, CUserStatus &status);

} // namespace dc::game

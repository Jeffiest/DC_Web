#pragma once

#include <filesystem>
#include <string>

// What the title needs to know before a save is loaded, kept as JSON beside the saves: the card's
// configuration file held it on the PS2.
struct SaveState {
    int  last_save = 0;      // The folder of the last save loaded or written; 0 for none.
    bool game_clear = false; // The game clear flag of the last save written.
};

// False when there is no such file or it is not JSON; a key missing or of another type keeps
// state's value.
bool SaveStateRead(const std::filesystem::path &path, SaveState &state);

// Written durably (files.hpp), its directory created first if need be.
bool SaveStateWrite(const std::filesystem::path &path, const SaveState &state);

/**
 * Copies a save-root fixture into a new or empty destination before startup reads it.
 * Rejects overlapping roots and source links; failures may leave a partial destination.
 */
bool SaveStateSeed(const std::filesystem::path &source, const std::filesystem::path &destination, std::string &error);

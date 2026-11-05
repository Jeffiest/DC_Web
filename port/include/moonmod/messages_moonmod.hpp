#pragma once

// Messages players leave for each other (messages.*): short notes kept at a place a mod names, rated by the players who
// read them. There is no server: each message is a small file in a messages folder, and pointing that folder at one a
// sync service shares (OneDrive, Dropbox, a LAN share) carries them between players. Each player writes only their
// own files, so two players never write the same one; files from others are read as untrusted data.
//
// The folder is DC_MESSAGES_DIR when set, else the path on the first line of <save>/moonmod/messages-folder.txt, else
// <save>/moonmod/messages (this player's own notes only).

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace moonmod::messages {

inline constexpr std::size_t kTextMax = 120;   // bytes of printable ASCII
inline constexpr std::size_t kPlaceMax = 64;   // a-z 0-9 and : _ . -
inline constexpr int         kOwnMax = 300;    // messages one player keeps at once
inline constexpr int         kListMax = 50;    // messages one list returns
inline constexpr int         kScanMax = 4000;  // files read from the folder in one list

struct Message {
    std::string  id;
    std::string  place;
    std::string  text;
    std::int64_t at = 0; // seconds since 1970
    int          good = 0;
    int          poor = 0;
    bool         mine = false;
    int          rated = 0; // this player's rating: 1, -1 or 0
};

struct Folder {
    std::filesystem::path path;
    bool                  shared = false; // a folder the player chose, not the save's own
};

Folder FolderFor(const std::filesystem::path &save_root);
// This player's id (12 hex digits), made once and kept in the save.
std::string Author(const std::filesystem::path &save_root, std::string &error);

bool ValidText(std::string_view text);
bool ValidPlace(std::string_view place);

// Leaves a message at place for mod_id's players; id is set to its id.
bool Post(const std::filesystem::path &save_root, std::string_view mod_id, std::string_view place, std::string_view text,
          std::string &id, std::string &error);
// mod_id's messages at any of places, best rated first, then newest (at most kListMax).
bool List(const std::filesystem::path &save_root, std::string_view mod_id, const std::vector<std::string> &places,
          std::vector<Message> &out, std::string &error);
// Rates another player's message: 1 good, -1 poor, 0 takes the rating back.
bool Rate(const std::filesystem::path &save_root, std::string_view id, int rating, std::string &error);
// Takes back one of this player's own messages.
bool Remove(const std::filesystem::path &save_root, std::string_view id, std::string &error);

} // namespace moonmod::messages

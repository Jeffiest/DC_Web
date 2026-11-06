#pragma once

// Co-op's split of a save (save.world_export/import, save.player_export/import): a visit plays the host's world with
// the guest's own things. The world is what the host's save says of the story and the places: Georama in every town,
// the story and map flags, the day and time, dungeon progress (the deepest floors, the Atla still to find, the kills)
// and who has joined the party. A player's own things are what each character has and carries: life, defense and
// water, weapons and the one equipped, the item pack and quick slots, the stored items and the stock, Gilda and the
// fishing points. The host's save keeps each guest's things; the guest's own save is never written while visiting.
//
// Both travel as text: a tag ("DCW1" a world, "DCP1" a player), then each field's number, size and bytes, base64. A
// field whose size differs from this build's (another build's layout) refuses the whole import.

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace dc::game {
class CSaveData;
}

namespace moonmod::coop {

std::string ExportWorld(const dc::game::CSaveData &save);
bool        ImportWorld(std::string_view text, dc::game::CSaveData &save, std::string &error);
std::string ExportPlayer(const dc::game::CSaveData &save);
bool        ImportPlayer(std::string_view text, dc::game::CSaveData &save, std::string &error);

// One town's Georama (georama.town_export/import): its record and its chips, for co-op to carry a town edited in one
// game to the others. "DCT1", the town's number, then the bytes, base64.
std::string ExportTown(const dc::game::CSaveData &save, int town);
bool        ImportTown(std::string_view text, int town, dc::game::CSaveData &save, std::string &error);

// The host's guests (save.account_*): each guest's own things, kept in the host's save, as coop_accounts.json in its
// slot folder beside the save, written with every save of the slot and read with its load. A new game has none.
struct Account {
    std::string id;     // the guest's game's own id for its player
    std::string name;
    std::string player; // save.player_export text
};
inline constexpr std::size_t kAccountsMax = 64;
inline constexpr std::size_t kAccountMax = 256 * 1024; // bytes of one account's text

const Account             *AccountGet(std::string_view id);
bool                       AccountSet(Account account, std::string &error);
bool                       AccountRemove(std::string_view id);
std::vector<const Account *> Accounts();
void                       AccountsClear();
bool                       AccountsSave(const std::filesystem::path &save_directory);
void                       AccountsLoad(const std::filesystem::path &save_directory);
void                       AccountsDelete(const std::filesystem::path &save_directory);
bool                       ValidAccountId(std::string_view id);

} // namespace moonmod::coop

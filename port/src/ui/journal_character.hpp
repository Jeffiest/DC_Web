#pragma once

#include <string>
#include <utility>
#include <vector>

// The Journal's Character page: a mod's character sheet (moonmod::CharacterSheet) beside its skill
// tree, drawn with the UI framework like the Chest Tracker. Cross asks the mod to train the selected
// node; the mod answers with its own notices.
namespace dc::ui::journal_character {

/// Whether a loaded mod has set a character sheet, so the Journal lists the page.
bool Available();

/// Resets the selection and snaps the cursor when the page opens from the Journal menu.
void Open();

/// Asks the Journal to open straight on this page once its menu has loaded (a mod's hotkey).
void RequestPendingOpen();

/// True once after RequestPendingOpen; the Journal menu consumes it when it reaches its category list.
bool TakePendingOpen();

/// Handles the pad while the page is active. Returns true when the player went back to the Journal menu.
bool HandleKey();

/// Draws the page.
void Draw();

/// Appends the page's localization keys and default English strings to `out`.
void AppendStrings(std::vector<std::pair<std::string, std::string>> &out);

} // namespace dc::ui::journal_character

#pragma once

// Mod title modes and sandbox sessions (Chronicle).
//
// A mod registers a title mode (game.title.mode_register): one more row on the title menu under NEW GAME,
// LOAD and OPTION. Choosing the row swaps the title screen for the mode's own screen, a page the mod
// drives: a heading, up to six info lines and up to seven menu entries. Confirming an entry, or Circle,
// sends game.title.selected to the owning mod, which can change the page (game.title.mode_set) or start
// a sandbox session (game.session.start).
//
// A sandbox session is a fresh in-memory game that belongs to no save slot: the save data is reset as for
// a new game, native saving never happens (the session never reaches a town or the save screen), and
// leaving the dungeon, a game over or game.session.finish brings the player back to the mode's screen.

#include <cstddef>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "moonmod/api.hpp"

namespace moonmod {

inline constexpr std::size_t kTitleModeLimit = 2;
inline constexpr std::size_t kTitleModeLineLimit = 6;
inline constexpr std::size_t kTitleModeEntryLimit = 7;
inline constexpr std::size_t kTitleModeLabelBytes = 32;
inline constexpr std::size_t kTitleModeTitleBytes = 40;
inline constexpr std::size_t kTitleModeTextBytes = 64;
// The title loop's result for "start the requested sandbox session" (TitleLoop returns select + 1).
inline constexpr int kTitleMenuSandbox = 9;
inline constexpr int kTitleResultSandbox = kTitleMenuSandbox + 1;

struct TitleModeEntry {
    std::string id;
    std::string label;
    bool enabled = true;
};

// A sky page (title.mode_set with sky = {...}): constellations of nodes on a night sky that pans to the cursor's
// constellation. The engine draws it and moves the cursor; Cross, Square, L1, R1 and Circle reach the mod as
// game.title.selected with the node id and an action. Coordinates are in screen units (the 640-wide screen).
inline constexpr std::size_t kTitleSkyNodeLimit = 200;
inline constexpr std::size_t kTitleSkyGroupLimit = 16;
inline constexpr std::size_t kTitleSkyTextLimit = 24;
inline constexpr std::size_t kTitleSkyLinkLimit = 4;

struct TitleSkyGroup {
    std::string   id;
    std::string   name;
    unsigned char r = 255, g = 255, b = 255;
    float         x = 0.0f, y = 0.0f; // where the view centres for this constellation
};

struct TitleSkyNode {
    std::string      id;
    std::string      label;     // under the node
    std::string      rank;      // after the label, e.g. "2/3"
    std::string      icon;      // a native icon name or a png game path a mod registered
    std::string      title;     // the detail plate's heading
    std::string      tag;       // beside the heading (the path's name)
    std::string      text;      // what it does
    std::string      needs;     // what it needs and opens
    std::string      cost;      // the next rank's price
    std::size_t      group = 0;
    float            x = 0.0f, y = 0.0f;
    int              state = 0; // 0 locked, 1 open, 2 owned
    bool             on = true; // owned and switched on
    bool             keystone = false;
    std::vector<std::size_t> links; // the nodes it hangs from, drawn as dotted lines
};

struct TitleSkyText { // a decorative caption on the sky (a path's name)
    std::string   text;
    float         x = 0.0f, y = 0.0f;
    unsigned char r = 255, g = 255, b = 255;
};

struct TitleSky {
    std::vector<TitleSkyGroup> groups;
    std::vector<TitleSkyNode>  nodes;
    std::vector<TitleSkyText>  texts;
    std::vector<std::string>   top; // up to three lines at the top right
    std::size_t                cursor = 0;
    bool                       map = false; // the whole sky at once (R2)
};

struct TitleMode {
    std::string owner;
    std::string id;       // qualified: owner:local
    std::string local_id;
    std::string label;    // the title menu row
    std::string title;    // the mode screen's heading
    std::string subtitle;
    std::string hint;
    std::vector<std::string> lines;
    std::vector<TitleModeEntry> entries;
    std::size_t cursor = 0;
    bool root = true;     // Circle on a root page goes back to the title menu
    std::optional<TitleSky> sky; // a sky page in place of the entries
};

// Registry: the API operations.
ApiCallResult RegisterTitleMode(std::string_view owner, const Json &args);
ApiCallResult SetTitleMode(std::string_view owner, const Json &args);
ApiCallResult RemoveTitleMode(std::string_view owner, const Json &args);
void RemoveTitleModesByOwner(std::string_view owner);
const std::vector<TitleMode> &TitleModes();
const TitleMode *FindTitleMode(std::string_view qualified_or_local, std::string_view owner = {});
Json TitleModeJson(const TitleMode &mode);
// A sky as title.mode_set's `sky` takes it, checked whole (ui.sky_open reads its page the same way). `previous` keeps
// the cursor on the node it was on.
ApiCallResult ReadTitleSky(const Json &request, const std::optional<TitleSky> &previous, std::optional<TitleSky> &out);

// The mode screen, driven by the title loop.
enum class TitleModeInput { None, Up, Down, Confirm, Back, Left, Right, Square, L1, R1, Map, L2, R2 };
enum class TitleModeOutcome { Stay, ReturnToTitle, StartSession };
bool OpenTitleModeScreen(std::size_t index);
const TitleMode *OpenTitleModeScreenMode();
void CloseTitleModeScreen();
TitleModeOutcome HandleTitleModeInput(TitleModeInput input);
// The node a sky's cursor moves to from `from` in direction (dx, dy), or `from` when there is none that way.
std::size_t TitleSkyStep(const TitleSky &sky, std::size_t from, int dx, int dy);
// Where the cursor lands in a constellation it jumps to (L2/R2, or a click on its name): the first node open to
// buy, else its first; nodes.size() when it has none.
std::size_t TitleSkyGroupEntry(const TitleSky &sky, std::size_t group);
// Puts the open sky page's cursor on a node (the mouse); false when it was there already or there is no sky.
bool PointTitleSky(std::size_t node);

// Sandbox sessions.
struct SandboxRequest {
    std::string owner;
    std::string mode_id;
    std::string floor_id;   // stable registered floor id
    int dungeon_id = -1;
    int floor = -1;         // native floor number
    int party_size = 1;
    int character = 0;      // the active character at the start, 0 (Toan) to 5
    // A co-op visit (session.start {visit = ...}): the host's world and the guest's own things in a fresh game, which
    // starts in a town and may go anywhere; nothing of it is written to the guest's saves.
    bool visit = false;
    int map = -1;           // the town map a visit starts in
    std::string world;      // save.world_export text
    std::string player;     // save.player_export text, or empty for a new account
};
struct SandboxSession {
    std::string owner;
    std::string mode_id;
    int started_frame = 0;
    bool visit = false;
};

// session.start validates the caller, then queues the request; the title loop fades out and the game loop
// starts it with TakeSandboxRequest.
void QueueSandboxRequest(SandboxRequest request);
bool SandboxRequestPending();
std::optional<SandboxRequest> TakeSandboxRequest();
void BeginSandboxSession(const SandboxRequest &request);
bool SandboxActive();
// A co-op visit is running: the game must not write a save slot.
bool VisitActive();
const SandboxSession *ActiveSandbox();
// Records why the session is ending ("defeat", "ended", "left_dungeon", "loaded_save"); the first reason wins.
void NoteSandboxEnding(std::string_view reason);
// Ends the session: queues game.session.ended for every mod and asks the title to reopen the mode screen.
// Returns the reason used.
std::string EndSandboxSession(std::string_view fallback_reason, bool reopen_mode = true);
// The mode to reopen when the title screen next initialises, taken once.
std::optional<std::string> TakeTitleModeReturn();
Json SessionJson(bool in_title);

void ResetTitleModesForTests();

} // namespace moonmod

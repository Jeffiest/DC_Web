#include "presence.hpp"

#include <format>
#include <string>
#include <utility>

#include "dun/gameloop.hpp"
#include "editground.hpp"
#include "editmapscript.hpp"
#include "itemdata.hpp"
#include "main.hpp"
#include "savedata.hpp"

extern s32 mode;

namespace {

struct Character {
    const char *name;
    const char *image;
};

constexpr Character kCharacters[] = {
    {"Toan",   "toan"  },
    {"Xiao",   "xiao"  },
    {"Goro",   "goro"  },
    {"Ruby",   "ruby"  },
    {"Ungaga", "ungaga"},
    {"Osmond", "osmond"},
};

constexpr const char *kDungeons[DUNGEON_COUNT] = {
    "Divine Beast Cave",
    "Wise Owl Forest",
    "Shipwreck",
    "Sun and Moon Temple",
    "Moon Sea",
    "Gallery of Time",
    "Demon Shaft",
};

constexpr const char *kTowns[TOWN_COUNT] = {
    "Norune Village",
    "Matataki Village",
    "Queens",
    "Muska Lacka",
    "Yellow Drops",
};

DiscordActivity Playing(int chara, std::string state) {
    const Character &character = kCharacters[chara >= CHARA_TOAN && chara <= CHARA_OSMOND ? chara : CHARA_TOAN];
    return {std::format("Playing as {}", character.name), std::move(state), character.image, character.name};
}

DiscordActivity Away(std::string details) {
    return {std::move(details), {}, "dark_cloud", "Dark Cloud"};
}

bool Georama(int edit_mode) {
    switch (edit_mode) {
        case ED_MODE_GEORAMA:
        case ED_MODE_GEORAMA_MENU_INIT:
        case ED_MODE_GEORAMA_MENU:
        case ED_MODE_RETURN_MENU_GEORAMA:
            return true;
        default:
            return false;
    }
}

constexpr int kSettleTicks = 25;

DiscordActivity g_shown;
DiscordActivity g_pending;
int             g_pending_ticks = 0;

} // namespace

DiscordActivity PresenceDescribe(const PresenceState &state) {
    switch (state.mode) {
        case GAME_MODE_DUNGEON: {
            if (state.dungeon < 0 || state.dungeon >= DUNGEON_COUNT) {
                return Playing(state.chara, "In a dungeon");
            }
            std::string where = kDungeons[state.dungeon];
            if (state.choosing_floor) {
                where += " · Choosing a floor";
            } else if (state.floor >= 0 && state.dungeon == DUNGEON_GALLERY_OF_TIME) {
                where += std::format(" · {}{} Years Ago", state.back_floor ? "Back Floor, " : "",
                                     BtGetFloorLevel(state.floor));
            } else if (state.floor >= 0) {
                where += std::format(" · {}Floor {}", state.back_floor ? "Back " : "", state.floor + 1);
            }
            return Playing(state.chara, std::move(where));
        }
        case GAME_MODE_EDIT: {
            if (state.map_no < 0 || state.map_no >= TOWN_COUNT) {
                return Playing(state.chara, "Out in the world");
            }
            const char *town = kTowns[state.map_no];
            if (Georama(state.edit_mode)) {
                return Playing(state.chara, std::format("Rebuilding {}", town));
            }
            if (state.edit_mode == ED_MODE_FISHING) {
                return Playing(state.chara, std::format("Fishing in {}", town));
            }
            return Playing(state.chara, town);
        }
        case GAME_MODE_MENU:
            return Away("In the developer menu");
        case GAME_MODE_OPENING:
            return Away("Watching the opening");
        default:
            return Away("At the title screen");
    }
}

void PresenceTick() {
    switch (mode) {
        case GAME_MODE_LOADER:
        case GAME_MODE_SAVE:
        case GAME_MODE_UNUSED_4:
        case GAME_MODE_UNUSED_6:
        case GAME_MODE_UNUSED_8:
        case GAME_MODE_UNUSED_12:
            return;
    }
    PresenceState state;
    state.mode = mode;
    state.map_no = MapNo;
    state.edit_mode = GameMode;
    state.dungeon = selectMapNo;
    state.back_floor = BtUraDongeon != 0;
    state.choosing_floor = BtGameModeFlag == BT_GAME_MODE_ENTRANCE_MENU;
    if (SaveData != nullptr) {
        CDngStatusData *status = SaveData->GetDngStatus();
        state.floor = status->cur_floor;
        state.chara = status->cur_chara;
    }
    DiscordActivity activity = PresenceDescribe(state);
    if (activity != g_pending) {
        g_pending = std::move(activity);
        g_pending_ticks = 0;
    }
    if (++g_pending_ticks >= kSettleTicks && g_pending != g_shown) {
        g_shown = g_pending;
        DiscordSetActivity(g_shown);
    }
}

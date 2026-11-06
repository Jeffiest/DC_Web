// Co-op's shared doors (moonmod/coop_events.hpp), after Jeffiest/Chronicle's windows branch (MpEventPoll).

#include "moonmod/coop_events.hpp"

#include "btactstatus.hpp"
#include "btitem.hpp"
#include "dun/gameloop.hpp"
#include "gamemode.hpp"
#include "main.hpp"

namespace dc::game {

namespace {

bool InScript(int task) {
    return task == GAME_TASK_SCRIPT_START || task == GAME_TASK_SCRIPT_RUN || task == GAME_TASK_SCRIPT_ITEM_SELECT ||
           task == GAME_TASK_SCRIPT_RESTART;
}

// The script running now: what started it, whether it came from another player, and what it has done.
struct Running {
    bool               valid = false, remote = false, changes = false, leaves = false, sent = false;
    MoonModSharedEvent event;
};
Running g_running;
int     g_previous_task = 0;
bool    g_next_is_remote = false;

struct Waiting {
    bool               on = false;
    MoonModSharedEvent event;
    int                frames_left = 0;
};
Waiting g_waiting;
constexpr int kWaitFrames = 1800; // half a minute for the game to be free

std::vector<MoonModSharedEvent> g_out;

} // namespace

void MoonModCoopScriptOp(int kind) {
    if (!g_running.valid) return;
    if (kind == 2) g_running.leaves = true;
    if (kind == 1) g_running.changes = true;
}

void MoonModCoopEventsPoll() {
    if (mode != GAME_MODE_DUNGEON) {
        g_running = Running{};
        g_waiting.on = false;
        g_out.clear();
        return;
    }
    const int  task = gameTask;
    const bool in_script = InScript(task);
    if (in_script && !InScript(g_previous_task) && !g_running.valid) {
        g_running = Running{};
        g_running.valid = true;
        g_running.remote = g_next_is_remote;
        g_next_is_remote = false;
        MoonModSharedEvent &e = g_running.event;
        e.script = BtEventInfo.script_no != -1 ? BtEventInfo.script_no : BtEventInfo.running_script_no;
        e.mode = BtEventInfo.action_mode;
        e.ext = BtEventInfo.ext_memory;
        for (int i = 0; i < 3; ++i) {
            e.pos[i] = BtEventInfo.position[i];
            e.dir[i] = BtEventInfo.direction[i];
        }
    }
    // Sent once the script ends: by then it is known to have changed the floor and not to have left it.
    if (g_running.valid && (!in_script || g_running.leaves)) {
        if (!in_script && !g_running.remote && g_running.changes && !g_running.leaves && g_running.event.script >= 0 && g_out.size() < 8) {
            g_out.push_back(g_running.event);
        }
        if (!in_script) g_running = Running{};
    }
    g_previous_task = task;

    if (g_waiting.on) {
        if (--g_waiting.frames_left <= 0) {
            g_waiting.on = false;
        } else if (task == GAME_TASK_PLAY && BtGameModeFlag == BT_GAME_MODE_PLAY && BtActStatus.can_act != 0 &&
                   BtActStatus.movement_locked == 0 && UserStatus != nullptr) {
            const MoonModSharedEvent &e = g_waiting.event;
            BtEventInfo.script_no = e.script;
            BtEventInfo.ext_memory = e.ext;
            BtEventInfo.event_marker = 0;
            BtEventInfo.action_mode = e.mode;
            BtEventInfo.chara_help = 0;
            for (int i = 0; i < 3; ++i) {
                BtEventInfo.position[i] = e.pos[i];
                BtEventInfo.direction[i] = e.dir[i];
            }
            BtEventInfo.position[3] = 1.0f;
            g_next_is_remote = true;
            gameTask = GAME_TASK_SCRIPT_START;
            g_waiting.on = false;
        }
    }
}

std::vector<MoonModSharedEvent> MoonModCoopTakeEvents() {
    std::vector<MoonModSharedEvent> out;
    out.swap(g_out);
    return out;
}

bool MoonModCoopRunEvent(const MoonModSharedEvent &event) {
    if (g_waiting.on || mode != GAME_MODE_DUNGEON) return false;
    g_waiting = {true, event, kWaitFrames};
    return true;
}

bool MoonModCoopAutoItemSelect() {
    if (!(g_running.valid && g_running.remote)) return false;
    int pick = -1;
    if (BtEventInfo.item_select_filtered != 0 && BtEventInfo.item_select_list[0] != -1) pick = BtEventInfo.item_select_list[0];
    if (BtEventInfo.item_select_result != 0 && PortItemSelectResult != nullptr) PortItemSelectResult->i = pick;
    BtEventInfo.item_select_result = 0;
    return true;
}

} // namespace dc::game

// The walk-through exit room and the seamless floor change (moonmod/chronicle_adapter.hpp). Both are off
// (DngExitRoomEnabled, IsSeamlessDungeonEnabled): they are unfinished, and the normal continue/leave window is
// used.
#include <cmath>

#include "character.hpp"
#include "dun/gameloop.hpp"
#include "dungeonmap.hpp"
#include "framevu1.hpp"
#include "camerafollow.hpp"
#include "moonmod/chronicle_adapter.hpp"
#include "runscript.hpp"

namespace dc::game {}
using namespace dc::game;

namespace dc::game {
extern CFrameVu1 *CharaFrame;
} // namespace dc::game

namespace moonmod {

// Off until the walk-through exit room exists: when on, opening the exit skips the
// continue/leave window and stepping past the exit loads the next floor at once.
static bool s_seamless_dungeon_enabled = false;
static bool s_exit_door_opened = false;
static SeamlessAntechamberInfo s_antechamber_info;

bool IsSeamlessDungeonEnabled() {
    return s_seamless_dungeon_enabled;
}

void SetSeamlessDungeonEnabled(bool enabled) {
    s_seamless_dungeon_enabled = enabled;
}

void TriggerExitDoorPhysicalOpen() {
    s_exit_door_opened = true;
}

bool IsExitDoorPhysicallyOpened() {
    return s_exit_door_opened;
}

const SeamlessAntechamberInfo &GetSeamlessAntechamberInfo() {
    return s_antechamber_info;
}

void SetupSeamlessAntechamber(int exit_x, int exit_z) {
    s_antechamber_info.active = true;
    s_antechamber_info.exit_cell_x = exit_x;
    s_antechamber_info.exit_cell_z = exit_z;

    const float center_x = 160.0f * exit_x;
    const float center_z = 160.0f * exit_z;

    // Door A (Next Floor Portal): straight through along world +Z past door threshold
    s_antechamber_info.next_floor_portal_min[0] = center_x - 30.0f;
    s_antechamber_info.next_floor_portal_max[0] = center_x + 30.0f;
    s_antechamber_info.next_floor_portal_min[1] = -50.0f;
    s_antechamber_info.next_floor_portal_max[1] = 50.0f;
    s_antechamber_info.next_floor_portal_min[2] = center_z + 35.0f;
    s_antechamber_info.next_floor_portal_max[2] = center_z + 90.0f;

    // Door B (Village Portal): side door along world +X inside antechamber
    s_antechamber_info.village_portal_min[0] = center_x + 35.0f;
    s_antechamber_info.village_portal_max[0] = center_x + 90.0f;
    s_antechamber_info.village_portal_min[1] = -50.0f;
    s_antechamber_info.village_portal_max[1] = 50.0f;
    s_antechamber_info.village_portal_min[2] = center_z - 30.0f;
    s_antechamber_info.village_portal_max[2] = center_z + 30.0f;
}

int CheckSeamlessPortalCrossing(const float *pos) {
    if (!s_antechamber_info.active || pos == nullptr) return 0;

    // Check Door A: Next Floor (1)
    if (pos[0] >= s_antechamber_info.next_floor_portal_min[0] &&
        pos[0] <= s_antechamber_info.next_floor_portal_max[0] &&
        pos[1] >= s_antechamber_info.next_floor_portal_min[1] &&
        pos[1] <= s_antechamber_info.next_floor_portal_max[1] &&
        pos[2] >= s_antechamber_info.next_floor_portal_min[2] &&
        pos[2] <= s_antechamber_info.next_floor_portal_max[2]) {
        return 1;
    }

    // Check Door B: Village Exit (2)
    if (pos[0] >= s_antechamber_info.village_portal_min[0] &&
        pos[0] <= s_antechamber_info.village_portal_max[0] &&
        pos[1] >= s_antechamber_info.village_portal_min[1] &&
        pos[1] <= s_antechamber_info.village_portal_max[1] &&
        pos[2] >= s_antechamber_info.village_portal_min[2] &&
        pos[2] <= s_antechamber_info.village_portal_max[2]) {
        return 2;
    }

    return 0;
}

static RS_STACKDATA *s_exit_room_answer = nullptr;
static int s_exit_room_seed = -1;

static int s_pin_ticks = 0;
static float s_pin_pos[3] = {};

// Moves the player to a world position and holds them there for a second: floor
// collision is only gathered near the camera, which eases over after a jump.
void TeleportPlayer(float x, float z) {
    if (CharaFrame == nullptr) return;
    const float y = DngFloorHeight(NowDngMap, x, z);
    CharaMain.SetPosition(x, y, z);
    CharaFrame->SetPosition(x, y, z);
    if (NowCamera__3 != nullptr) NowCamera__3->SetFollow(x, y + 14.0f, z);
    s_pin_pos[0] = x;
    s_pin_pos[1] = y;
    s_pin_pos[2] = z;
    s_pin_ticks = 60;
}

bool TeleportPinned() {
    return s_pin_ticks > 0;
}

void StepTeleportPin() {
    if (s_pin_ticks <= 0 || CharaFrame == nullptr) return;
    --s_pin_ticks;
    CharaMain.SetPosition(s_pin_pos[0], s_pin_pos[1], s_pin_pos[2]);
    CharaFrame->SetPosition(s_pin_pos[0], s_pin_pos[1], s_pin_pos[2]);
}

bool ExitRoomPending() {
    return s_exit_room_answer != nullptr;
}

bool BeginExitRoom(RS_STACKDATA *escape_result) {
    if (escape_result == nullptr || !DngExitRoomEnabled || !DngExitRoomValid || NowDngMap == nullptr) return false;
    s_exit_room_answer = escape_result;
    s_exit_room_seed = NowDngMap->map_seed;
    if (CharaFrame == nullptr) return false;
    // Centre of the room's floor cell (CharaFrame holds the player's world position).
    TeleportPlayer(160.0f * (DngExitRoomX + 1), 160.0f * (DngExitRoomY + 1));
    std::printf("exit room: entered (answer pending)\n");
    return true;
}

void StepExitRoom() {
    StepTeleportPin();
    if (s_exit_room_answer == nullptr) return;
    if (!DngExitRoomValid || NowDngMap == nullptr || NowDngMap->map_seed != s_exit_room_seed) {
        // The floor changed some other way (escape powder, death): drop the question.
        s_exit_room_answer = nullptr;
        return;
    }
    if (CharaFrame == nullptr) return;
    const float *position = CharaFrame->position;
    const int col = static_cast<int>(std::floor((position[0] + 80.0f) / 160.0f));
    const int row = static_cast<int>(std::floor((position[2] + 80.0f) / 160.0f));
    if (col != DngExitRoomX + 1) return;
    int answer = 0;
    // North doorway (town stair beyond it) and south doorway (next-floor stair).
    if (row < DngExitRoomY || (row == DngExitRoomY && position[2] < 160.0f * DngExitRoomY - 30.0f)) {
        answer = kExitRoomAnswerTown;
    } else if (row > DngExitRoomY + 2 || (row == DngExitRoomY + 2 && position[2] > 160.0f * (DngExitRoomY + 2) + 30.0f)) {
        answer = kExitRoomAnswerNextFloor;
    }
    if (answer == 0) return;
    s_exit_room_answer->i = answer;
    s_exit_room_answer = nullptr;
    std::printf("exit room: answered %s\n", answer == kExitRoomAnswerTown ? "town" : "next floor");
}

void ResetExitRoomForTests() {
    s_exit_room_answer = nullptr;
    s_exit_room_seed = -1;
}

void ResetSeamlessDungeonForTests() {
    // Clears the opened-door and antechamber state only; the on/off switch stays as set.
    s_exit_door_opened = false;
    s_antechamber_info = SeamlessAntechamberInfo{};
}

} // namespace moonmod

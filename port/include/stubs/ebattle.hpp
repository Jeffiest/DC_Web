#pragma once

// The port's EdGetRXf, EdGetRYf and EyeCamera (port/src/ebattle.cpp) ask this unit's static
// check_key_mode whether the editor's input mode owns the pad, through a global forwarder; that also
// keeps clang from dropping the static as unused.

static int check_key_mode(int mode);

int PortEdCheckKeyMode(int mode) {
    return check_key_mode(mode);
}

// Only the town walk calls this collision seam. The port adds keyboard first-person velocity
// before the same retail floor/wall/event checks, without pretending the town is an interior.
struct MoveCheckInfo;
class CCPoly;
int PortEdMoveCheck(float *pos, float *velocity, float *out_pos, MoveCheckInfo *out_info, CCPoly *polys, int poly_num, int mode);
#define MoveCheck PortEdMoveCheck

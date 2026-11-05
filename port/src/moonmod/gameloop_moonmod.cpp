// MoonMod code that used to live in ps2/src/dun/gameloop.cpp: the swing-speed tuning motionDrive
// calls under PORT, and the monster model loader the adapter uses.

#include "dun/gameloop.hpp"

#include "btactstatus.hpp"
#include "dataalloc.hpp"
#include "dngstatusdata.hpp"
#include "monstorunit.hpp"
#include "userstatus.hpp"

namespace moonmod {
bool  ChronicleLoadMonsterModelForMod(dc::game::CMonstorUnit *unit, int native_model_no, int *out_slot);
float ChroniclePlayerAttackAnimSpeed();
} // namespace moonmod

namespace dc::game {

/* MoonMod player.tuning_set attack_anim_speed: Toan's sword swings (actions 0x24-0x28) play their
 * recovery, the part after the swing's last hit and sound frame, at the tuned speed. The hit frames
 * keep their one-frame steps, since ToanKey_Play tests them in half-frame windows, and the boost
 * lands short of the motion's end so the end window (end - 1 .. end) is still met. */
void MoonModTuneSwingMotion() {
    static int   boosted = 0;
    const float  scale = moonmod::ChroniclePlayerAttackAnimSpeed();
    float        speed = -1.0f;

    if (scale != 1.0f && CombatChara(UserStatus->cur_chara) == CHARA_TOAN && BtActStatus.action_on == 1 &&
        BtActStatus.action_no >= 0x24 && BtActStatus.action_no <= 0x28 &&
        BtActStatus.motion_no == BtActStatus.action_no && CharaMain.motion_type.motion_info != NULL) {
        static const float last_window[5] = {828.0f, 835.0f, 844.0f, 854.0f, 868.0f};
        const float        time = CharaMain.motion_type.state.time;
        const float        end = (float) CharaMain.motion_type.motion_info[BtActStatus.motion_no].end;
        const float        tail = last_window[BtActStatus.action_no - 0x24];

        if (time > tail && time < end - 2.0f) {
            speed = (end - 1.5f) - time;

            if (speed > scale) {
                speed = scale;
            }

            if (speed < 0.25f) {
                speed = -1.0f;
            }
        }
    }

    if (speed > 0.0f) {
        CharaMain.motion_speed = speed;
        boosted = 1;
    } else if (boosted != 0) {
        CharaMain.motion_speed = -1.0f;
        boosted = 0;
    }
}


} // namespace dc::game

using namespace dc::game;

bool moonmod::ChronicleLoadMonsterModelForMod(dc::game::CMonstorUnit *unit, int native_model_no, int *out_slot) {
    if (unit == nullptr || unit != NowMonstorUnit || out_slot == nullptr || GetMonstorTable(native_model_no) == nullptr ||
        unit->model_count < 0 || unit->model_count >= MONSTOR_MODEL_MAX || MonstorModelBuffer.base == nullptr ||
        MonstorModelBuffer.limit <= 0 || MonstorModelBuffer.used < 0 ||
        MonstorModelBuffer.used >= MonstorModelBuffer.limit) {
        return false;
    }
    // the escape and teleport light's model lives in the read buffer a model loads through: not until it has gone
    if (EscapeFlag != 0) return false;
    const int slot = unit->model_count;
    if (unit->SetupBaseModel(slot, native_model_no, 0x26, &MonstorModelBuffer) == 0 ||
        unit->model_count != slot + 1) {
        return false;
    }
    *out_slot = slot;
    return true;
}

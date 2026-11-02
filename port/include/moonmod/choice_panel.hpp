#pragma once

#include <functional>
#include <string>
#include <string_view>
#include <vector>

// A mod's choice panel (ui.choice_open), drawn and driven by src/moonmod/choice_panel_moonmod.cpp: the dungeon
// pauses on its next free moment, shows the options side by side as cards, and the player picks one with the pad
// or the mouse.
namespace dc::game {
struct MoonModChoiceLine {
    std::string text;
    int         tone = 0; // 0 plain, 1 good, 2 bad, 3 dim, 4 gold
};
struct MoonModChoiceOption {
    std::string                    title;
    std::string                    subtitle;
    std::vector<MoonModChoiceLine> lines;
};
struct MoonModChoice {
    std::string                      owner;
    std::string                      id;
    std::string                      title;
    std::string                      subtitle;
    std::vector<MoonModChoiceOption> options;
    int                              cursor = 0;
    bool                             cancel = false; // Circle (or a right click) backs out without a pick
    // The engine's own panels: the pick goes here instead of to a mod, and play resumes on this task (-1: play).
    std::function<void(int)>         done;
    int                              resume_task = -1;
};
// False when a panel is already open or waiting.
bool MoonModChoiceOpen(MoonModChoice choice);
// Takes away an owner's panel (its mod unloaded, or it closed it), resuming play if it was showing.
void MoonModChoiceCloseOwner(std::string_view owner);
// A panel is waiting for the dungeon to pause for it.
bool MoonModChoiceWaiting();
// A panel is open (waiting or showing): notices hold back meanwhile.
bool MoonModChoiceUp();
// The paused task's step: reads the pad and mouse; true once the panel has closed.
bool MoonModChoiceStep();
// The task the dungeon goes back to once the panel closes.
int  MoonModChoiceResumeTask(int play_task);
void MoonModChoiceDraw();
// A boss's entrance card (ui.boss_card): letterbox bars and its name across the screen for `frames`, the dungeon
// held still meanwhile.
void MoonModBossCard(const std::string &kicker, const std::string &name, const std::string &epithet, int frames);
void MoonModBossCardDraw();
// A town's step, each frame of its walking loop: reads the pad for a panel while one is open and holds the town's own
// controls meanwhile (the town goes on around it: only the party waits).
void MoonModChoiceStepTown(bool walking);
// Clears a panel left over when the dungeon ends.
void MoonModChoiceReset();
} // namespace dc::game

namespace moonmod {
// The pick (index from 0, or -1 for backing out), reported to the owner as ui.choice_made.
void ChronicleChoiceMade(const std::string &owner, const std::string &id, int index);
} // namespace moonmod

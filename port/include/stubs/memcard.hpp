#pragma once

// menu_save.cpp calls ExitSaveSelect, which this unit defines static: the PS2 link makes it global
// (object_fixups.json). It gets a global symbol under the name menu_save.cpp references.
static void ExitSaveSelect();

void PortExportExitSaveSelect() asm(PORT_ASM_NAME(_Z14ExitSaveSelectv));

void PortExportExitSaveSelect() {
    ExitSaveSelect();
}

// The port's SaveMenuFunc (port/src/memcard.cpp) keeps these steps of the save menu, which this
// unit defines static, so each gets a global symbol under its own name.

static int SaveMenuKeyFadeIn();
static int SaveMenuKeyFadeOut();
static int SaveMenuKeyModeSelect();
static int SaveMenuKeyCheckMcType();
static int SaveMenuKeyCheckMc();
static int SaveMenuKeyLoadConfig();

int PortExportSaveMenuKeyFadeIn() asm(PORT_ASM_NAME(_Z17SaveMenuKeyFadeInv));
int PortExportSaveMenuKeyFadeOut() asm(PORT_ASM_NAME(_Z18SaveMenuKeyFadeOutv));
int PortExportSaveMenuKeyModeSelect() asm(PORT_ASM_NAME(_Z21SaveMenuKeyModeSelectv));
int PortExportSaveMenuKeyCheckMcType() asm(PORT_ASM_NAME(_Z22SaveMenuKeyCheckMcTypev));
int PortExportSaveMenuKeyCheckMc() asm(PORT_ASM_NAME(_Z18SaveMenuKeyCheckMcv));
int PortExportSaveMenuKeyLoadConfig() asm(PORT_ASM_NAME(_Z21SaveMenuKeyLoadConfigv));

int PortExportSaveMenuKeyFadeIn() {
    return SaveMenuKeyFadeIn();
}

int PortExportSaveMenuKeyFadeOut() {
    return SaveMenuKeyFadeOut();
}

int PortExportSaveMenuKeyModeSelect() {
    return SaveMenuKeyModeSelect();
}

int PortExportSaveMenuKeyCheckMcType() {
    return SaveMenuKeyCheckMcType();
}

int PortExportSaveMenuKeyCheckMc() {
    return SaveMenuKeyCheckMc();
}

int PortExportSaveMenuKeyLoadConfig() {
    return SaveMenuKeyLoadConfig();
}

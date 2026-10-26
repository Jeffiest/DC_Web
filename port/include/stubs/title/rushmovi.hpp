#pragma once

// The PS2 link tells the four FaceChange(int) definitions of the opening scenes apart by renaming
// them per object (object_fixups.json); op_d.cpp calls this unit's as FaceChangeMovie.
#define FaceChange FaceChangeMovie

// This unit declares title.cpp's scene drawing and loading functions static, and the PS2 link
// binds them to title.cpp's global ones. Here the static ones forward to those.
void PortTitleDataLoad() asm(PORT_ASM_NAME(_Z8DataLoadv));
void PortTitleDrawProcA() asm(PORT_ASM_NAME(_Z9DrawProcAv));
void PortTitleDrawProcB() asm(PORT_ASM_NAME(_Z9DrawProcBv));
void PortTitleDrawProcC() asm(PORT_ASM_NAME(_Z9DrawProcCv));
void PortTitleDrawProcD() asm(PORT_ASM_NAME(_Z9DrawProcDv));
void PortTitleDrawProcE() asm(PORT_ASM_NAME(_Z9DrawProcEv));
void PortTitleDrawProcF() asm(PORT_ASM_NAME(_Z9DrawProcFv));
void PortTitleDrawProcG() asm(PORT_ASM_NAME(_Z9DrawProcGv));
void PortTitleDrawProcH() asm(PORT_ASM_NAME(_Z9DrawProcHv));
void PortTitleDrawProcI() asm(PORT_ASM_NAME(_Z9DrawProcIv));
void PortTitleDrawProcTitle() asm(PORT_ASM_NAME(_Z13DrawProcTitlev));

static void DataLoad() {
    PortTitleDataLoad();
}

static void DrawProcA() {
    PortTitleDrawProcA();
}

static void DrawProcB() {
    PortTitleDrawProcB();
}

static void DrawProcC() {
    PortTitleDrawProcC();
}

static void DrawProcD() {
    PortTitleDrawProcD();
}

static void DrawProcE() {
    PortTitleDrawProcE();
}

static void DrawProcF() {
    PortTitleDrawProcF();
}

static void DrawProcG() {
    PortTitleDrawProcG();
}

static void DrawProcH() {
    PortTitleDrawProcH();
}

static void DrawProcI() {
    PortTitleDrawProcI();
}

static void DrawProcTitle() {
    PortTitleDrawProcTitle();
}

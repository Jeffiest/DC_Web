#pragma once

// The MoonMod Georama piece registry (port/src/moonmod/editpartsinfo_moonmod.cpp): pieces a mod
// registers in plots CHRONICLE_GEORAMA_NATIVE_PART_COUNT.. CHRONICLE_GEORAMA_PART_CAPACITY - 1.
// ps2/include/editpartsinfo.hpp includes this under PORT.

#include "common.h"

struct EDITPARTS_INFO;
class CEditPartsInfo;
struct EDIT_PARTS_ATRA;
class CMapParts;

/** One registered data-only Georama variant; native slots are persisted by the PC adapter. */
struct CHRONICLE_MOD_GEORAMA_PIECE {
    int part_id;
    int template_part_id;
    int town_id;
    int stock;
    int materialized;
    char id[96];
    char owner[64];
    char name[48];
};

bool RegisterChronicleModGeoramaPiece(int part_id, int town_id, int template_part_id, int stock,
                                      const char *id, const char *owner, const char *name);
bool UnregisterChronicleModGeoramaPiece(int part_id);
// Gives a registered variant its own model: the part archive `part` inside the scene file `scene`
// (a retail gedit/<town>/scene.scn or a mod's gedit/mod/<name>.scn). Empty strings clear it.
bool SetChronicleModGeoramaPieceModel(int part_id, const char *scene, const char *part);
// Gives a registered variant its own footprint: width x height cells (1-32 each), one byte a cell row by row as the
// part definition file has them (0 empty; the template's values otherwise). cells NULL fills every cell with the
// template's most used non-empty value. width 0 goes back to the template's footprint.
bool SetChronicleModGeoramaPieceFootprint(int part_id, int width, int height, const u8 *cells);
// editloop.cpp: rebuilds a part's models from that archive; false leaves the template's models.
bool ChronicleLoadModPartModel(CMapParts *parts, const char *scene, const char *part);
bool GetChronicleModGeoramaPiece(int part_id, CHRONICLE_MOD_GEORAMA_PIECE *out_piece);
int FindChronicleModGeoramaPiece(const char *id);
EDIT_PARTS_ATRA *GetChronicleModGeoramaAtra(int town_id, int part_id);
void MaterializeChronicleModGeoramaPieces(int town_id, CMapParts *parts);
void ResetChronicleModGeoramaPiecesForTests();

// Hooks the CEditPartsInfo methods call: the materialized info of a mod plot (NULL when none), and
// the state a fresh Initialize / Load of the town table resets or puts back.
EDITPARTS_INFO *ChronicleModGeoramaPartsInfo(CEditPartsInfo *table, int index);
void ChronicleModGeoramaTableInitialized(CEditPartsInfo *table);
void ChronicleModGeoramaTableLoaded(CEditPartsInfo *table, int georama_no);

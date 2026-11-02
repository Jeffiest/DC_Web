// The MoonMod Georama piece registry that used to live in ps2/src/editpartsinfo.cpp, and retail's
// CEditPartsInfo lookups extended to the mod plots after the 24 native ones.

#include "editpartsinfo.hpp"

#include <cstdio>
#include <cstring>

#include "editatra.hpp"
#include "editpartsdata.hpp"
#include "mapparts.hpp"
#include "savedata.hpp"

namespace dc::game {

int CheckPartsInfo(EDITPARTS_INFO *info);

namespace {

CHRONICLE_MOD_GEORAMA_PIECE g_chronicle_mod_georama_pieces[CHRONICLE_GEORAMA_MOD_PART_COUNT] = {};
EDITPARTS_INFO g_chronicle_mod_georama_info[CHRONICLE_GEORAMA_MOD_PART_COUNT] = {};
EDIT_PARTS_ATRA g_chronicle_mod_georama_atra[CHRONICLE_GEORAMA_MOD_PART_COUNT] = {};
// The town and parts the variants were last materialized for: Load re-initializes the table after the town has
// materialized them, so it puts them back.
int        g_materialized_town = -1;
CMapParts *g_materialized_parts = NULL;
char g_chronicle_mod_georama_scene[CHRONICLE_GEORAMA_MOD_PART_COUNT][64] = {};
char g_chronicle_mod_georama_model[CHRONICLE_GEORAMA_MOD_PART_COUNT][32] = {};
constexpr int kModFootprintSide = 32;
struct ModFootprint {
    int  width;
    int  height;
    bool fill; // cells come from the template's most used value
    u8   cells[kModFootprintSide * kModFootprintSide];
};
ModFootprint g_chronicle_mod_georama_footprint[CHRONICLE_GEORAMA_MOD_PART_COUNT] = {};
EPARTS_INFO_HEADER g_chronicle_mod_georama_header[CHRONICLE_GEORAMA_MOD_PART_COUNT] = {};

int ModGeoramaPieceIndex(int part_id) {
    const int index = part_id - CHRONICLE_GEORAMA_NATIVE_PART_COUNT;
    return index >= 0 && index < CHRONICLE_GEORAMA_MOD_PART_COUNT ? index : -1;
}

} // namespace

bool RegisterChronicleModGeoramaPiece(int part_id, int town_id, int template_part_id, int stock,
                                      const char *id, const char *owner, const char *name) {
    const int index = ModGeoramaPieceIndex(part_id);
    if (index < 0 || town_id < -1 || town_id >= 5 || template_part_id < 0 ||
        template_part_id >= CHRONICLE_GEORAMA_NATIVE_PART_COUNT || stock < 1 || stock > 999 ||
        id == NULL || owner == NULL || name == NULL || id[0] == '\0' || owner[0] == '\0' || name[0] == '\0' ||
        std::strlen(id) >= sizeof(g_chronicle_mod_georama_pieces[index].id) ||
        std::strlen(owner) >= sizeof(g_chronicle_mod_georama_pieces[index].owner) ||
        std::strlen(name) >= sizeof(g_chronicle_mod_georama_pieces[index].name)) {
        return false;
    }

    for (int piece_index = 0; piece_index < CHRONICLE_GEORAMA_MOD_PART_COUNT; ++piece_index) {
        const CHRONICLE_MOD_GEORAMA_PIECE &other = g_chronicle_mod_georama_pieces[piece_index];
        if (piece_index != index && other.part_id != 0 && std::strcmp(other.id, id) == 0) return false;
    }

    CHRONICLE_MOD_GEORAMA_PIECE &piece = g_chronicle_mod_georama_pieces[index];
    if (piece.part_id != 0 && (std::strcmp(piece.id, id) != 0 || piece.town_id != town_id ||
                               piece.template_part_id != template_part_id || piece.stock != stock ||
                               std::strcmp(piece.owner, owner) != 0 || std::strcmp(piece.name, name) != 0)) {
        return false;
    }

    piece.part_id = part_id;
    piece.template_part_id = template_part_id;
    piece.town_id = town_id;
    piece.stock = stock;
    std::strcpy(piece.id, id);
    std::strcpy(piece.owner, owner);
    std::strcpy(piece.name, name);
    return true;
}

bool SetChronicleModGeoramaPieceModel(int part_id, const char *scene, const char *part) {
    const int index = ModGeoramaPieceIndex(part_id);
    if (index < 0 || scene == NULL || part == NULL || std::strlen(scene) >= sizeof(g_chronicle_mod_georama_scene[0]) ||
        std::strlen(part) >= sizeof(g_chronicle_mod_georama_model[0]) || (scene[0] == '\0') != (part[0] == '\0')) {
        return false;
    }
    std::strcpy(g_chronicle_mod_georama_scene[index], scene);
    std::strcpy(g_chronicle_mod_georama_model[index], part);
    return true;
}

bool SetChronicleModGeoramaPieceFootprint(int part_id, int width, int height, const u8 *cells) {
    const int index = ModGeoramaPieceIndex(part_id);
    if (index < 0 || width < 0 || width > kModFootprintSide || height < 0 || height > kModFootprintSide ||
        (width == 0) != (height == 0)) {
        return false;
    }
    ModFootprint &footprint = g_chronicle_mod_georama_footprint[index];
    std::memset(&footprint, 0, sizeof(footprint));
    footprint.width = width;
    footprint.height = height;
    footprint.fill = cells == NULL;
    if (cells != NULL) std::memcpy(footprint.cells, cells, (size_t) (width * height));
    return true;
}

bool UnregisterChronicleModGeoramaPiece(int part_id) {
    const int index = ModGeoramaPieceIndex(part_id);
    if (index < 0 || g_chronicle_mod_georama_pieces[index].part_id != part_id) return false;
    g_chronicle_mod_georama_scene[index][0] = '\0';
    g_chronicle_mod_georama_model[index][0] = '\0';
    std::memset(&g_chronicle_mod_georama_footprint[index], 0, sizeof(g_chronicle_mod_georama_footprint[index]));
    std::memset(&g_chronicle_mod_georama_pieces[index], 0, sizeof(g_chronicle_mod_georama_pieces[index]));
    std::memset(&g_chronicle_mod_georama_info[index], 0, sizeof(g_chronicle_mod_georama_info[index]));
    std::memset(&g_chronicle_mod_georama_atra[index], 0, sizeof(g_chronicle_mod_georama_atra[index]));
    return true;
}

bool GetChronicleModGeoramaPiece(int part_id, CHRONICLE_MOD_GEORAMA_PIECE *out_piece) {
    const int index = ModGeoramaPieceIndex(part_id);
    if (index < 0 || out_piece == NULL || g_chronicle_mod_georama_pieces[index].part_id != part_id) return false;
    *out_piece = g_chronicle_mod_georama_pieces[index];
    return true;
}

int FindChronicleModGeoramaPiece(const char *id) {
    if (id == NULL || id[0] == '\0') return -1;
    for (int index = 0; index < CHRONICLE_GEORAMA_MOD_PART_COUNT; ++index) {
        const CHRONICLE_MOD_GEORAMA_PIECE &piece = g_chronicle_mod_georama_pieces[index];
        if (piece.part_id != 0 && std::strcmp(piece.id, id) == 0) return piece.part_id;
    }
    return -1;
}

EDIT_PARTS_ATRA *GetChronicleModGeoramaAtra(int town_id, int part_id) {
    const int index = ModGeoramaPieceIndex(part_id);
    if (index < 0 || g_chronicle_mod_georama_pieces[index].part_id != part_id ||
        (g_chronicle_mod_georama_pieces[index].town_id >= 0 &&
         g_chronicle_mod_georama_pieces[index].town_id != town_id) ||
        g_chronicle_mod_georama_pieces[index].materialized == 0) {
        return NULL;
    }
    return &g_chronicle_mod_georama_atra[index];
}

void MaterializeChronicleModGeoramaPieces(int town_id, CMapParts *parts) {
    g_materialized_town = parts != NULL ? town_id : -1;
    g_materialized_parts = parts;
    for (int index = 0; index < CHRONICLE_GEORAMA_MOD_PART_COUNT; ++index) {
        CHRONICLE_MOD_GEORAMA_PIECE &piece = g_chronicle_mod_georama_pieces[index];
        piece.materialized = 0;
        std::memset(&g_chronicle_mod_georama_info[index], 0, sizeof(g_chronicle_mod_georama_info[index]));
        std::memset(&g_chronicle_mod_georama_atra[index], 0, sizeof(g_chronicle_mod_georama_atra[index]));
        if (piece.part_id == 0 || (piece.town_id >= 0 && piece.town_id != town_id) || parts == NULL) continue;

        CMapParts &source = parts[piece.template_part_id];
        EDITPARTS_INFO *source_info = EditPartsInfo.GetPartsInfo(piece.template_part_id);
        EDIT_PARTS_ATRA *source_atra = GetEditAtraPartsData(town_id, piece.template_part_id);
        if (source.handle < 0 || source.info == NULL || source_info == NULL || source_atra == NULL ||
            source.subtype != MAP_PARTS_SUBTYPE_NONE) {
            continue;
        }

        EDITPARTS_INFO &info = g_chronicle_mod_georama_info[index];
        info = *source_info;
        info.parts_no = piece.part_id;
        info.obtained = 1;
        info.placed = 0;
        info.stock = piece.stock;
        info.kind = 0;

        EDIT_PARTS_ATRA &atra = g_chronicle_mod_georama_atra[index];
        atra = *source_atra;
        atra.max = piece.stock;
        atra.kind = 0;

        ModFootprint &footprint = g_chronicle_mod_georama_footprint[index];
        if (footprint.width > 0 && source_info->header != NULL) {
            if (footprint.fill) {
                // The template's most used non-empty cell value stands for "part of the piece".
                int counts[256] = {};
                const EPARTS_INFO_HEADER *shape = source_info->header;
                for (int cell = 0; cell < shape->width * shape->height; ++cell) counts[shape->cell[cell]]++;
                int value = 1;
                for (int candidate = 1; candidate < 256; ++candidate) {
                    if (counts[candidate] > counts[value]) value = candidate;
                }
                std::memset(footprint.cells, value, sizeof(footprint.cells));
            }
            EPARTS_INFO_HEADER &header = g_chronicle_mod_georama_header[index];
            header = *source_info->header;
            header.width = footprint.width;
            header.height = footprint.height;
            header.cell = footprint.cells;
            info.header = &header;
            info.width = footprint.width;
            info.height = footprint.height;
        }

        std::memcpy(&parts[piece.part_id], &source, sizeof(CMapParts));
        parts[piece.part_id].parts_no = piece.part_id;
        parts[piece.part_id].handle = piece.part_id;
        parts[piece.part_id].info = &info;
        if (g_chronicle_mod_georama_scene[index][0] != '\0' &&
            !ChronicleLoadModPartModel(&parts[piece.part_id], g_chronicle_mod_georama_scene[index],
                                       g_chronicle_mod_georama_model[index])) {
            std::fprintf(stderr, "moonmod: Georama piece %s keeps its template model (%s in %s not loaded)\n", piece.id,
                         g_chronicle_mod_georama_model[index], g_chronicle_mod_georama_scene[index]);
        }
        piece.materialized = 1;
    }
}

void ResetChronicleModGeoramaPiecesForTests() {
    std::memset(g_chronicle_mod_georama_pieces, 0, sizeof(g_chronicle_mod_georama_pieces));
    std::memset(g_chronicle_mod_georama_info, 0, sizeof(g_chronicle_mod_georama_info));
    std::memset(g_chronicle_mod_georama_atra, 0, sizeof(g_chronicle_mod_georama_atra));
    std::memset(g_chronicle_mod_georama_scene, 0, sizeof(g_chronicle_mod_georama_scene));
    std::memset(g_chronicle_mod_georama_model, 0, sizeof(g_chronicle_mod_georama_model));
    std::memset(g_chronicle_mod_georama_footprint, 0, sizeof(g_chronicle_mod_georama_footprint));
    g_materialized_town = -1;
    g_materialized_parts = NULL;
}

EDITPARTS_INFO *ChronicleModGeoramaPartsInfo(CEditPartsInfo *table, int index) {
    if (table != &EditPartsInfo) return NULL;
    const int mod_index = ModGeoramaPieceIndex(index);
    if (mod_index < 0 || g_chronicle_mod_georama_pieces[mod_index].part_id != index ||
        g_chronicle_mod_georama_pieces[mod_index].materialized == 0) return NULL;
    return &g_chronicle_mod_georama_info[mod_index];
}

void ChronicleModGeoramaTableInitialized(CEditPartsInfo *table) {
    if (table == &EditPartsInfo) {
        for (int index = 0; index < CHRONICLE_GEORAMA_MOD_PART_COUNT; ++index) {
            g_chronicle_mod_georama_pieces[index].materialized = 0;
            std::memset(&g_chronicle_mod_georama_info[index], 0, sizeof(g_chronicle_mod_georama_info[index]));
            std::memset(&g_chronicle_mod_georama_atra[index], 0, sizeof(g_chronicle_mod_georama_atra[index]));
        }
    }
}

void ChronicleModGeoramaTableLoaded(CEditPartsInfo *table, int georama_no) {
    if (table == &EditPartsInfo && g_materialized_parts != NULL && g_materialized_town == georama_no) {
        MaterializeChronicleModGeoramaPieces(georama_no, g_materialized_parts);
    }
}

} // namespace dc::game

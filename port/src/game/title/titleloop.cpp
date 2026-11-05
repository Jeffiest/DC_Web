#include "common.h"

#include <libgraph.h>
#include <libpkt.h>
#include <libvu0.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdlib>
#include <cstring>

#include "app/gameloop.hpp"
#include "battle_globals.hpp"
#include "camera.hpp"
#include "camerafollow.hpp"
#include "character.hpp"
#include "dataalloc.hpp"
#include "dataread.hpp"
#include "dataset.hpp"
#include "editloop.hpp"
#include "frame.hpp"
#include "gfx/gfx.hpp"
#include "framevu1.hpp"
#include "gamemode.hpp"
#include "gamepad.hpp"
#include "main.hpp"
#include "mainselect.hpp"
#include "mathutil.hpp"
#include "mds.hpp"
#include "memcard.hpp"
#include "menu_save.hpp"
#include "mglib.hpp"
#include "object.hpp"
#include "rect.hpp"
#include "savedata.hpp"
#include "snd.hpp"
#include "sound.hpp"
#include "texture.hpp"
#include "title/cursol.hpp"
#include "title/dispfade.hpp"
#include "title/logo.hpp"
#include "title/op_a.hpp"
#include "title/op_b.hpp"
#include "title/op_c.hpp"
#include "title/rushmovi.hpp"
#include "title/scfader.hpp"
#include "title/script.hpp"
#include "title/sprite.hpp"
#include "title/title.hpp"
#include "title/title_draw.hpp"
#include "title/tisound.hpp"
#include "title/title_menu.hpp"
#include "title/titleloop.hpp"
#include "menu_draw.hpp"
#include "moonmod/item_icon_override.hpp"
#include "moonmod/runtime.hpp"
#include "moonmod/title_modes.hpp"
#include "moonmod/sky_screen.hpp"
#include "ui/framework.hpp"
#include "ui/menu_pointer.hpp"
#include "vutext.hpp"
#include "wind.hpp"

namespace dc::game {

using namespace dc::title;
using namespace dc::ui;

// TexManager.GetTexture takes the texture name as char *.
#pragma clang diagnostic ignored "-Wwritable-strings"

int    Fade1;
int    Fade2;
int    Fade3;
int    Fade4;
int    Wait;
int    opcnt;
int    keywait;
u_char brink;
int    brinkcnt;
int    EffCnt;

void TitleInit(int mode) {
    float scale = 750.0f;
    float far_z = 65535.0f;

    InitializeDataBuffer();
    SetDataBuffer(&VisualData, 200000);
    SetDataBuffer(&MotionData, 500000);
    SetDataBuffer(&TextureData, 300000);
    SetPacketReadBuffer(40000, 300000);

    MGSetRenderInfo(scale, 4.0f, far_z);
    Camera.SetRef(nullptr, 0.0f, 0.0f, 0.0f);
    Camera.SetPos(nullptr, 0.0f, 0.0f, 0.0f);
    FCamera.SetFollow(0.0f, 0.0f, 0.0f);
    FCamera.SetAngle(0.0f);
    FCamera.SetDistance(180.0f);
    FCamera.SetHeight(2.0f);
    MGSetBGColor(0.0f, 0.0f, 0.0f, 128.0f);
    setbilinear(1);

    LOADTEXTURE_INFO menu_textures[] = {
        {"#fukidashibase#640#" HALF_BUFFER_HEIGHT_STR "#4", 26, 0},
        {"#fontbase#512#256#1",                             26, 0},
        {"meswin/gaiji.img",                                26, 0},
        {"meswin/fuki256.img",                              26, 0},
        {"meswin/syst04.img",                               26, 0},
        {"",                                                0,  0}
    };

    TexManager.Initialize(16352);
    TexManager.LoadTextureBlock(-1, menu_textures, read_buffer);
    LoadFileMenuData("stayframe.img", read_buffer);
    TexManager.EnterFixTextureZ(reinterpret_cast<u_char *>(read_buffer));

    switch (LanguageCode) {
        case LANG_JAPANESE:
        case LANG_ENGLISH_US:
            LoadFile("titledat/title.pak", read_buffer, nullptr);
            break;
        case LANG_ENGLISH_UK:
            LoadFile("titledat/title_eu.pak", read_buffer, nullptr);
            break;
        case LANG_FRENCH:
            LoadFile("titledat/title_f.pak", read_buffer, nullptr);
            break;
        case LANG_GERMAN:
            LoadFile("titledat/title_g.pak", read_buffer, nullptr);
            break;
        case LANG_ITALIAN:
            LoadFile("titledat/title_i.pak", read_buffer, nullptr);
            break;
        case LANG_SPANISH:
            LoadFile("titledat/title_s.pak", read_buffer, nullptr);
            break;
        default:
            break;
    }

    LOADTEXTURE_INFO2 title_textures[] = {
        {"#frame_image#640#" HALF_BUFFER_HEIGHT_STR "#4", 0, 0},
        {"#frame_image#640#" HALF_BUFFER_HEIGHT_STR "#4", 1, 0},
        {nullptr,                                         0, 0},
        {nullptr,                                         1, 0},
        {nullptr,                                         1, 0},
        {nullptr,                                         1, 0},
        {nullptr,                                         1, 0},
        {nullptr,                                         1, 0},
        {nullptr,                                         1, 0},
        {nullptr,                                         1, 0},
        {nullptr,                                         0, 0}
    };

    title_textures[2].name = reinterpret_cast<char *>(GetPackFile(read_buffer, "bg.img", nullptr));
    title_textures[3].name = reinterpret_cast<char *>(GetPackFile(read_buffer, "title.img", nullptr));
    title_textures[4].name = reinterpret_cast<char *>(GetPackFile(read_buffer, "main.img", nullptr));
    title_textures[5].name = reinterpret_cast<char *>(GetPackFile(read_buffer, "pat01.img", nullptr));
    title_textures[6].name = reinterpret_cast<char *>(GetPackFile(read_buffer, "pat02.img", nullptr));
    title_textures[7].name = reinterpret_cast<char *>(GetPackFile(read_buffer, "start.img", nullptr));
    title_textures[8].name = reinterpret_cast<char *>(GetPackFile(read_buffer, "start3.img", nullptr));
    title_textures[9].name = reinterpret_cast<char *>(GetPackFile(read_buffer, "icon01.img", nullptr));
    TexManager.LoadTextureBlock(-1, title_textures);

    sceVu0FVECTOR pos;

    ObjectFrame3 = LoadMDSFile(GetPackFile(read_buffer, "sky.mds", nullptr), 2, 0);
    pos[0] = 0.0f;
    pos[1] = 0.0f;
    pos[2] = -28.0f;
    ObjectFrame3->SetPosition(pos);

    Cloud__2.LoadPackData(read_buffer, "cloud.cfg", &MotionData, nullptr);
    Cloud__2.motion_type.state.time = 100.0f;
    Cloud__2.motion_type.state.blend_step = 0.1f;
    Cloud__2.motion_type.state.motion_no = 0;
    Cloud__2.motion_type.state.playing_no = 0;
    Cloud__2.motion_type.state.blending = false;
    Cloud__2.motion_no = 0;
    Cloud__2.SetPosition(0.0f, 0.0f, -5.0f);

    Logo.LoadPackData(read_buffer, "logo.cfg", &MotionData, nullptr);
    Logo.motion_type.state.time = 1.0f;
    Logo.motion_type.state.blend_step = 0.1f;
    Logo.motion_type.state.motion_no = 0;
    Logo.motion_type.state.playing_no = 0;
    Logo.motion_type.state.blending = false;
    Logo.motion_no = 0;

    char *spark_names[9] = {
        "logo_p1.cfg", "logo_p2.cfg", "logo_p3.cfg", "logo_p4.cfg", "logo_p5.cfg",
        "logo_p6.cfg", "logo_p7.cfg", "logo_p8.cfg", "logo_p9.cfg"};

    for (int i = 0; i < 9; i++) {
        Spark[i].LoadPackData(read_buffer, spark_names[i], &MotionData, nullptr);
        Spark[i].motion_type.state.time = 1.0f;
        Spark[i].motion_type.state.blend_step = 0.1f;
        Spark[i].motion_type.state.motion_no = 0;
        Spark[i].motion_type.state.playing_no = 0;
        Spark[i].motion_type.state.blending = false;
        Spark[i].motion_no = 0;
        Spark[i].SetPosition(0.0f, 11.599f, -19.099f);
    }

    CSnd.SetReverb(0, 4, 5);
    CSnd.SetReverb(1, 0, 0);
    CSnd.LoadSoundFileFromPack("title.txt", read_buffer);
    CSnd.SetVol(MIDI_PORT_SE_TITLE, 256);
    CSnd.SetVol(MIDI_PORT_SE_DEFAULT, 256);
    CSnd.SetVol(MIDI_PORT_UNK_D, 256);
    CSnd.SetVol(MIDI_PORT_SE_SPECIAL, 256);
    CSnd.SQ_Play(MIDI_PORT_AMBIENT, 0);

    Fade1 = Fade2 = Fade3 = Fade4 = Wait = 0;
    opcnt = 0;
    keywait = 0;
    brinkcnt = 0;
    brink = 0;
    EffCnt = 0;
    CProcess.no = TITLE_STEP_FADE_IN;
    CFade.value = 0;

    CSprite.Init();
    CLogo.Init();
    CCursol.Init();

    if (mode == 1) {
        CCursol.select = TITLE_MENU_LOAD;
        CCursol.Set(316.0f);

        for (int i = 0; i < 10; i++) {
            CCursol.Move();
        }
    }

    GamePad.SetAutoRepeat(PAD_UP | PAD_DOWN | PAD_LEFT | PAD_RIGHT, 30, 9);
    GamePad.MenuModeOn(120);
}

namespace {
// A title menu row's screen y: laid out for NTSC's 448-line picture, lowered to stay centred in PAL.
constexpr int MenuY(int y) {
    return y + (SCREEN_HEIGHT - 448) / 2;
}

MenuPointer title_pointer;

// Mod title modes (moonmod/title_modes.hpp): their rows follow OPTION on the title menu, and choosing one
// shows the mode's own screen in these two steps. The values sit past retail's TITLE_STEP_EXIT.
constexpr int kTitleStepModMode = 40;
constexpr int kTitleStepModFadeOut = 41;

// The sky and clouds fade with the mode screen: out when a session starts, in when the title reopens on it.
bool TitleModeBackdropFading() {
    return CProcess.no == kTitleStepModFadeOut || (CProcess.no == kTitleStepModMode && CFade.Get(128) < 128);
}
// ---- Sky pages (moonmod::TitleSky): constellations of unlocks on a night sky ------------------------------------
// The HUD art the nodes use (itempack.img): the item box as each node's body, and the HUD's own icons.
struct SkyIconRect {
    const char *name;
    int         x, y, w, h;
};
constexpr SkyIconRect kSkyIcons[] = {
    {"heart", 0x40, 0xA0, 0x10, 0x10}, {"sword", 0x40, 0xB0, 0x10, 0x10}, {"flask", 0x64, 0x84, 0x12, 0x14},
    {"drop", 0x00, 0x48, 0x10, 0x14},  {"crystal", 0x00, 0xA0, 0x20, 0x20}, {"map", 0x20, 0xA0, 0x20, 0x20},
    {"shield", 0x00, 0x5C, 0x24, 0x24}, {"freeze", 194, 84, 62, 36},       {"stamina", 132, 120, 62, 36},
    {"poison", 194, 120, 62, 36},       {"curse", 194, 156, 62, 36},       {"goo", 132, 156, 62, 36},
};
constexpr SkyIconRect kSkyBox = {"box", 0x24, 0x5C, 0x24, 0x24};

// The HUD texture is a dungeon's; the title loads it the first time a sky page draws (and again if a mode change
// dropped it).
CTexture *SkyIcons() {
    if (CTexture *icons = TexManager.GetTexture("itempack", -1)) return icons;
    static std::vector<unsigned int> buffer;
    static int                       last_try = -1000;
    const int                        frame = static_cast<int>(moonmod::SharedRuntime().Frame());
    if (frame - last_try < 120) return nullptr;
    last_try = frame;
    buffer.assign(0x10000, 0);
    if (LoadFileMenuData(const_cast<char *>("itempack.img"), buffer.data()) <= 0) return nullptr;
    TexManager.EnterIMGFile(reinterpret_cast<u_char *>(buffer.data()), -1, 0, 0);
    return TexManager.GetTexture("itempack", -1);
}

// A dot never sits across the 640x480 frame's edge: on a wide window an untextured rect that reaches the edge is
// carried out to the window's (as a fade is), and a star there would show as a line across the side.
int SkyDotClear(int at, int size, int edge) {
    if (at <= edge && at + size > edge) return at + size / 2 < edge ? edge - size : edge + 1;
    if (at < edge && at + size >= edge) return edge - size - 1;
    return at;
}

void SkyDot(float x, float y, int size, spRGBA colour) {
    int left = static_cast<int>(x) - size / 2, top = static_cast<int>(y) - size / 2;
    left = SkyDotClear(SkyDotClear(left, size, 0), size, 640);
    top = SkyDotClear(SkyDotClear(top, size, 0), size, SCREEN_HEIGHT);
    set2DSpriteC4(Vif1Packet, CRect_i_(left, top, size, size), &colour, &colour, &colour, &colour);
}

// A dotted line; dots above `min_y` or below `max_y` are left out.
void SkyLine(float x0, float y0, float x1, float y1, spRGBA colour, float min_y = -1e9f, float max_y = 1e9f) {
    const float length = std::hypot(x1 - x0, y1 - y0);
    const int   steps = std::max(1, static_cast<int>(length / 5.0f));
    for (int i = 0; i <= steps; ++i) {
        const float t = static_cast<float>(i) / static_cast<float>(steps);
        const float y = y0 + (y1 - y0) * t;
        if (y < min_y || y > max_y) continue;
        SkyDot(x0 + (x1 - x0) * t, y, 2, colour);
    }
}

void SkyRing(float x, float y, float r, int size, spRGBA colour) {
    const int dots = std::max(12, static_cast<int>(r * 1.6f));
    for (int i = 0; i < dots; ++i) {
        const float a = static_cast<float>(i) * 6.2831853f / static_cast<float>(dots);
        SkyDot(x + std::cos(a) * r, y + std::sin(a) * r, size, colour);
    }
}

unsigned char SkyAlpha(int value, int alpha) { return static_cast<unsigned char>(std::clamp(value * alpha / 128, 0, 0x80)); }

// Text in the game's lettering in a given colour; gives back its width.
int SkyText(std::string_view text, float scale, unsigned char r, unsigned char g, unsigned char b, ui::Alignment align,
            int x, int y, int alpha, bool draw = true) {
    ui::Label label(text, ui::ColorWhite, align, true, scale);
    label.SetCustomRgb(r, g, b);
    if (draw) label.Draw(x, y, alpha);
    return label.GetWidth();
}

void SkyIcon(CTexture *icons, const std::string &name, float x, float y, int size, unsigned char alpha) {
    if (name.empty()) return;
    for (const SkyIconRect &icon : kSkyIcons) {
        if (name != icon.name) continue;
        if (icons == nullptr) return;
        const bool badge = icon.w > 50;
        const int  w = badge ? size + 6 : size;
        const int  h = badge ? (size + 6) * icon.h / icon.w : size;
        set2DSprite(Vif1Packet, icons, CRect_i_(static_cast<int>(x) - w / 2, static_cast<int>(y) - h / 2, w, h),
                    CRect_i_(icon.x, icon.y, icon.w, icon.h), alpha);
        return;
    }
    if (CTexture *image = HudImageTexture(name.c_str())) {
        set2DSprite(Vif1Packet, image, CRect_i_(static_cast<int>(x) - size / 2, static_cast<int>(y) - size / 2, size, size),
                    CRect_i_(0, 0, 32, 32), alpha);
    }
}

// What the last sky frame put where, for the mouse: the nodes, and the boxes a click acts on (a button in the
// hint row, the star map, or a constellation's name at the edge).
struct SkyHitNode {
    float       x, y, r;
    std::size_t index;
};
struct SkyHitBox {
    int                     x0, y0, x1, y1;
    moonmod::TitleModeInput input = moonmod::TitleModeInput::None;
    std::size_t             group = static_cast<std::size_t>(-1);
};
std::vector<SkyHitNode> sky_hit_nodes;
std::vector<SkyHitBox>  sky_hit_boxes;
MenuPointer             sky_pointer;

moonmod::TitleModeInput SkyTokenInput(std::string_view token) {
    if (token == "{cross}") return moonmod::TitleModeInput::Confirm;
    if (token == "{circle}") return moonmod::TitleModeInput::Back;
    if (token == "{square}") return moonmod::TitleModeInput::Square;
    if (token == "{triangle}") return moonmod::TitleModeInput::Map;
    if (token == "{L1}") return moonmod::TitleModeInput::L1;
    if (token == "{R1}") return moonmod::TitleModeInput::R1;
    if (token == "{L2}") return moonmod::TitleModeInput::L2;
    if (token == "{R2}") return moonmod::TitleModeInput::R2;
    return moonmod::TitleModeInput::None;
}

// A hint row: button tokens draw at the glyphs' own size (a label with tokens ignores its scale), the words between
// them at `scale`. Gives back the row's width; draws only when `draw` is set, and then records each button (and the
// word after a lone one) as a box the mouse can click.
int SkyHint(std::string_view hint, float scale, int x, int y, int alpha, bool draw) {
    int                     width = 0;
    std::size_t             at = 0;
    moonmod::TitleModeInput last_input = moonmod::TitleModeInput::None;
    while (at < hint.size()) {
        const bool        token = hint[at] == '{';
        std::size_t       end = at;
        if (token) {
            while (end < hint.size() && hint[end] == '{') {
                const std::size_t close = hint.find('}', end);
                if (close == std::string_view::npos) { end = hint.size(); break; }
                end = close + 1;
            }
        } else {
            end = std::min(hint.find('{', at), hint.size());
        }
        const std::string_view part = hint.substr(at, end - at);
        if (token) {
            std::size_t glyph_at = 0;
            int         count = 0;
            last_input = moonmod::TitleModeInput::None;
            while (glyph_at < part.size()) {
                const std::size_t      close = std::min(part.find('}', glyph_at), part.size() - 1);
                const std::string_view glyph = part.substr(glyph_at, close + 1 - glyph_at);
                ui::Label              label(glyph, ui::ColorWhite, ui::Alignment::Left, true, 1.0f);
                const int              w = label.GetWidth();
                if (draw) {
                    label.Draw(x + width, y, alpha);
                    last_input = SkyTokenInput(glyph);
                    if (last_input != moonmod::TitleModeInput::None) {
                        sky_hit_boxes.push_back({x + width, y - 2, x + width + w, y + 20, last_input});
                    }
                }
                width += w;
                glyph_at = close + 1;
                ++count;
            }
            if (count != 1) last_input = moonmod::TitleModeInput::None;
        } else {
            const int w = SkyText(part, scale, 214, 218, 232, ui::Alignment::Left, x + width, y, alpha, draw);
            if (last_input != moonmod::TitleModeInput::None) {
                sky_hit_boxes.push_back({x + width, y - 2, x + width + w, y + 20, last_input});
            }
            last_input = moonmod::TitleModeInput::None;
            width += w;
        }
        at = end;
    }
    return width;
}

void DrawTitleSky(const moonmod::TitleMode &mode, int alpha) {
    const moonmod::TitleSky &sky = *mode.sky;
    sky_hit_nodes.clear();
    sky_hit_boxes.clear();
    // The screen as it shows: wider than 640 on a wide window (Fill), so the sky spreads to its edges.
    const gfx::LogicalRect screen = gfx::VisibleLogicalRect(gfx::kMainTarget);
    const float left = std::min(0.0f, screen.x), right = std::max(640.0f, screen.x + screen.w);
    const float top_edge = std::min(0.0f, screen.y), bottom_edge = std::max(static_cast<float>(SCREEN_HEIGHT), screen.y + screen.h);
    const float width = right - left, centre_x = (left + right) / 2.0f;
    const int   l = static_cast<int>(left), r = static_cast<int>(right), t = static_cast<int>(top_edge);
    const int   b = static_cast<int>(bottom_edge), cx = static_cast<int>(centre_x);
    // The night: the title's sky darkened, with fixed stars.
    spRGBA night = {6, 8, 26, SkyAlpha(0x5C, alpha)};
    set2DSpriteC4(Vif1Packet, CRect_i_(l, t, r - l, b - t), &night, &night, &night, &night);
    unsigned int seed = 7;
    const int    star_count = static_cast<int>(150.0f * width / 640.0f);
    for (int i = 0; i < star_count; ++i) {
        seed = seed * 1103515245u + 12345u;
        const float sx = left + static_cast<float>((seed >> 8) % static_cast<unsigned int>(r - l));
        seed = seed * 1103515245u + 12345u;
        const float sy = top_edge + static_cast<float>((seed >> 8) % static_cast<unsigned int>(b - t));
        const int   size = (seed >> 20) % 5 == 0 ? 2 : 1;
        SkyDot(sx, sy, size, {230, 232, 255, SkyAlpha(0x30 + static_cast<int>((seed >> 24) % 0x40), alpha)});
    }
    CTexture *icons = SkyIcons();

    // The plate at the bottom: the cursor's node in full, and the buttons in a row of their own.
    const int   plate_w = std::min(r - l - 48, 780), plate_x = cx - plate_w / 2;
    const int   plate_h = 128, plate_top = b - plate_h - 6;
    const float sky_top = top_edge + 30.0f, sky_bottom = static_cast<float>(plate_top) - 30.0f;

    // The view: the cursor's constellation, eased towards; the map shows the whole sky.
    const moonmod::TitleSkyNode  &here = sky.nodes[sky.cursor];
    const moonmod::TitleSkyGroup &group = sky.groups[here.group];
    static float cam_x = 0.0f, cam_y = 0.0f;
    static bool  cam_set = false;
    float        min_x = here.x, max_x = here.x, min_y = here.y, max_y = here.y;
    for (const auto &node : sky.nodes) {
        min_x = std::min(min_x, node.x); max_x = std::max(max_x, node.x);
        min_y = std::min(min_y, node.y); max_y = std::max(max_y, node.y);
    }
    // In a constellation the view centres on it when it fits, and otherwise follows the cursor inside its bounds.
    float g_min_x = here.x, g_max_x = here.x, g_min_y = here.y, g_max_y = here.y;
    for (const auto &node : sky.nodes) {
        if (node.group != here.group) continue;
        g_min_x = std::min(g_min_x, node.x); g_max_x = std::max(g_max_x, node.x);
        g_min_y = std::min(g_min_y, node.y); g_max_y = std::max(g_max_y, node.y);
    }
    const auto follow = [](float lo, float hi, float at, float room_lo, float room_hi) {
        if (hi - lo <= room_lo + room_hi) return (lo + hi) / 2.0f + (room_lo - room_hi) / 2.0f;
        return std::clamp(at, lo + room_lo, hi - room_hi);
    };
    const float view_cy = (sky_top + 40.0f + sky_bottom) / 2.0f;
    const float room_x = width / 2.0f - 60.0f;
    const float target_x = sky.map ? (min_x + max_x) / 2.0f : follow(g_min_x, g_max_x, here.x, room_x, room_x);
    const float target_y = sky.map ? (min_y + max_y) / 2.0f
                                   : follow(g_min_y, g_max_y, here.y, view_cy - sky_top - 50.0f, sky_bottom - view_cy - 20.0f);
    if (!cam_set) {
        cam_x = target_x;
        cam_y = target_y;
        cam_set = true;
    }
    cam_x += (target_x - cam_x) * 0.18f;
    cam_y += (target_y - cam_y) * 0.18f;
    const float scale = sky.map ? std::min((width - 100.0f) / std::max(1.0f, max_x - min_x),
                                           (sky_bottom - sky_top - 70.0f) / std::max(1.0f, max_y - min_y))
                                : 1.0f;
    const auto  sx = [&](float x) { return (x - cam_x) * scale + centre_x; };
    const auto  sy = [&](float y) { return (y - cam_y) * scale + view_cy; };
    const auto  visible = [&](float x, float y) { return x > left - 60.0f && x < right + 60.0f && y > sky_top && y < sky_bottom + 20.0f; };

    // Links: dotted, bright where both ends are owned.
    for (const auto &node : sky.nodes) {
        const auto &c = sky.groups[node.group];
        for (std::size_t link : node.links) {
            const auto &other = sky.nodes[link];
            const bool  lit = node.state == 2 && other.state == 2;
            const float x0 = sx(other.x), y0 = sy(other.y), x1 = sx(node.x), y1 = sy(node.y);
            // A link shows only with a star of its own on screen: a stretch of line from stars out of sight
            // reads as a stray mark (and costs a sprite per dot).
            const auto on_screen = [&](float x, float y) { return x >= left && x <= right && y >= sky_top && y <= sky_bottom + 20.0f; };
            if (!on_screen(x0, y0) && !on_screen(x1, y1)) continue;
            // kept clear of the heading's band and the plate
            SkyLine(x0, y0, x1, y1, {c.r, c.g, c.b, SkyAlpha(lit ? 0x78 : 0x48, alpha)}, top_edge + 54.0f, static_cast<float>(plate_top) - 4.0f);
        }
    }
    // Captions (a path's name).
    if (!sky.map) {
        for (const auto &text : sky.texts) {
            if (!visible(sx(text.x), sy(text.y))) continue;
            if (sx(text.x) < left + 150.0f && sy(text.y) < top_edge + 80.0f) continue; // under the star map
            SkyText(text.text, 0.8f, text.r, text.g, text.b, ui::Alignment::Center, static_cast<int>(sx(text.x)),
                    static_cast<int>(sy(text.y)), alpha);
        }
    }
    // Nodes.
    for (std::size_t i = 0; i < sky.nodes.size(); ++i) {
        const auto &node = sky.nodes[i];
        const auto &c = sky.groups[node.group];
        const float x = sx(node.x), y = sy(node.y);
        if (!visible(x, y)) continue;
        sky_hit_nodes.push_back({x, y, sky.map ? 9.0f : (node.keystone ? 20.0f : 17.0f), i});
        if (sky.map) {
            SkyDot(x, y, i == sky.cursor ? 6 : 4,
                   node.state == 2 ? spRGBA{c.r, c.g, c.b, SkyAlpha(0x80, alpha)} : spRGBA{150, 150, 170, SkyAlpha(node.state ? 0x60 : 0x28, alpha)});
            if (i == sky.cursor) SkyRing(x, y, 7.0f, 2, {255, 255, 255, SkyAlpha(0x80, alpha)});
            continue;
        }
        const float r = node.keystone ? 17.0f : 14.0f;
        const bool  lit = node.state == 2 && node.on;
        if (lit) {
            SkyRing(x, y, r + 6.0f, 3, {c.r, c.g, c.b, SkyAlpha(0x14, alpha)});
            SkyRing(x, y, r + 3.0f, 3, {c.r, c.g, c.b, SkyAlpha(0x30, alpha)});
        }
        spRGBA ring = lit ? spRGBA{c.r, c.g, c.b, SkyAlpha(0x80, alpha)}
                    : node.state == 2 ? spRGBA{140, 140, 150, SkyAlpha(0x70, alpha)}
                    : node.state == 1 ? spRGBA{214, 214, 228, SkyAlpha(0x80, alpha)}
                                      : spRGBA{90, 90, 104, SkyAlpha(0x50, alpha)};
        if (node.keystone) {
            const spRGBA crown = node.state == 0 ? spRGBA{110, 100, 70, SkyAlpha(0x60, alpha)} : spRGBA{248, 206, 92, SkyAlpha(0x80, alpha)};
            for (int k = 0; k < 8; ++k) {
                const float a = static_cast<float>(k) * 0.7853982f;
                SkyDot(x + std::cos(a) * (r + 5.0f), y + std::sin(a) * (r + 5.0f), 4, crown);
            }
        }
        SkyRing(x, y, r, 2, ring);
        const int           box = node.keystone ? 26 : 21;
        const unsigned char body = SkyAlpha(node.state == 0 ? 0x38 : (node.state == 2 && !node.on ? 0x58 : 0x80), alpha);
        if (icons != nullptr) {
            set2DSprite(Vif1Packet, icons, CRect_i_(static_cast<int>(x) - box / 2, static_cast<int>(y) - box / 2, box, box),
                        CRect_i_(kSkyBox.x, kSkyBox.y, kSkyBox.w, kSkyBox.h), body);
        }
        SkyIcon(icons, node.icon, x, y, box - 7, body);
        // No label beside the node: the plate at the bottom names the one under the cursor.
    }
    if (sky.map) {
        for (const auto &g : sky.groups) {
            float gx = 0.0f, gy = -1e30f;
            int   count = 0;
            for (const auto &node : sky.nodes) {
                if (&sky.groups[node.group] != &g) continue;
                gx += node.x; gy = std::max(gy, node.y); ++count;
            }
            if (count == 0) continue;
            SkyText(g.name, 0.55f, g.r, g.g, g.b, ui::Alignment::Center, static_cast<int>(sx(gx / static_cast<float>(count))),
                    static_cast<int>(sy(gy)) + 6, alpha);
        }
    }
    // The hand on the cursor's node.
    {
        const auto &node = sky.nodes[sky.cursor];
        const float x = sx(node.x), y = sy(node.y);
        RECT        rect = {0, 0, 32, 32};
        setbilinear(0);
        CursorVibeCnt++;
        DrawObjectVibe(x - (node.keystone ? 17.0f : 14.0f) - 36.0f, y - 14.0f, TexManager.GetTexture("icon01", -1), rect, 128, alpha);
        setbilinear(1);
    }
    // The other constellations, named at the edge they lie beyond: eight slots round the screen, each naming the
    // nearest constellation in its direction (L2/R2 step through them, the star map shows the rest).
    if (!sky.map) {
        std::size_t best[8];
        float       best_d[8];
        for (int k = 0; k < 8; ++k) { best[k] = sky.groups.size(); best_d[k] = 1e30f; }
        for (std::size_t g = 0; g < sky.groups.size(); ++g) {
            if (g == here.group) continue;
            const float dx = sky.groups[g].x - cam_x, dy = sky.groups[g].y - cam_y;
            const int   k = (static_cast<int>(std::lround(std::atan2(dy, dx) / 0.7853982f)) + 8) % 8;
            const float d = dx * dx + dy * dy;
            if (d < best_d[k]) { best_d[k] = d; best[k] = g; }
        }
        // 0 right, 1 down-right, 2 down, 3 down-left, 4 left, 5 up-left (beside the star map), 6 up, 7 up-right
        const int low = plate_top - 24, mid = static_cast<int>(view_cy), high = t + 58;
        const int xs[8] = {r - 14, r - 14, cx, l + 14, l + 14, l + 124, cx, r - 14};
        const int ys[8] = {mid, low, low, low, mid, high, high, high};
        for (int k = 0; k < 8; ++k) {
            if (best[k] == sky.groups.size()) continue;
            const auto   &other = sky.groups[best[k]];
            const bool    to_left = k >= 3 && k <= 5, to_right = k <= 1 || k == 7;
            std::string   name = to_left ? "< " + other.name : (to_right ? other.name + " >" : other.name);
            ui::Alignment align = to_left ? ui::Alignment::Left : (to_right ? ui::Alignment::Right : ui::Alignment::Center);
            const int w = SkyText(name, 0.75f, other.r, other.g, other.b, align, xs[k], ys[k], alpha * 3 / 4);
            const int x0 = to_left ? xs[k] : (to_right ? xs[k] - w : xs[k] - w / 2);
            sky_hit_boxes.push_back({x0 - 4, ys[k] - 3, x0 + w + 4, ys[k] + 18, moonmod::TitleModeInput::None, best[k]});
        }
    }
    // Top: the heading, a star map of the whole sky, and the purse lines.
    ui::TrackerWindow::Draw(cx - 124, t + 4, 248, 40, alpha, ui::WindowPreset::BlueStone);
    ui::Label heading(mode.title, ui::ColorGold, ui::Alignment::Center, true, 1.15f, ui::FontWeight::Bold);
    heading.SetOutline(2.0f);
    heading.Draw(cx, t + 10, alpha);
    const float map_x = left + 16.0f, map_y = top_edge + 15.0f;
    ui::HelpBox::Draw(l + 8, t + 6, 108, 66, alpha);
    sky_hit_boxes.push_back({l + 8, t + 6, l + 116, t + 72, moonmod::TitleModeInput::Map});
    const float map_scale = std::min(92.0f / std::max(1.0f, max_x - min_x), 48.0f / std::max(1.0f, max_y - min_y));
    for (std::size_t i = 0; i < sky.nodes.size(); ++i) {
        const auto &node = sky.nodes[i];
        const auto &c = sky.groups[node.group];
        SkyDot(map_x + (node.x - min_x) * map_scale, map_y + (node.y - min_y) * map_scale, i == sky.cursor ? 3 : 2,
               node.state == 2 ? spRGBA{c.r, c.g, c.b, SkyAlpha(0x80, alpha)} : spRGBA{120, 120, 140, SkyAlpha(0x50, alpha)});
    }
    if (!sky.map) {
        // The view's frame on the star map, kept inside the map's box.
        const float x0 = std::clamp(map_x + (cam_x - width / 2.0f - min_x) * map_scale, left + 11.0f, left + 113.0f);
        const float x1 = std::clamp(map_x + (cam_x + width / 2.0f - min_x) * map_scale, left + 11.0f, left + 113.0f);
        const float y0 = std::clamp(map_y + (cam_y - (view_cy - sky_top) - min_y) * map_scale, top_edge + 9.0f, top_edge + 69.0f);
        const float y1 = std::clamp(map_y + (cam_y + (sky_bottom - view_cy) - min_y) * map_scale, top_edge + 9.0f, top_edge + 69.0f);
        const spRGBA frame = {248, 206, 92, SkyAlpha(0x60, alpha)};
        SkyLine(x0, y0, x1, y0, frame);
        SkyLine(x0, y1, x1, y1, frame);
        SkyLine(x0, y0, x0, y1, frame);
        SkyLine(x1, y0, x1, y1, frame);
    }
    static constexpr unsigned char kTopColours[3][3] = {{240, 238, 232}, {255, 214, 150}, {236, 110, 130}};
    for (std::size_t i = 0; i < sky.top.size() && i < 3; ++i) {
        SkyText(sky.top[i], 0.6f, kTopColours[i][0], kTopColours[i][1], kTopColours[i][2], ui::Alignment::Right, r - 8,
                t + 6 + static_cast<int>(i) * 14, alpha);
    }
    // Bottom: the cursor's node in full.
    const int inner_l = plate_x + 20, inner_r = plate_x + plate_w - 20;
    ui::TrackerWindow::Draw(plate_x, plate_top, plate_w, plate_h, alpha, ui::WindowPreset::BlueStone);
    const std::string &title = here.title.empty() ? here.label : here.title;
    ui::Label name(title, ui::ColorGold, ui::Alignment::Left, true, 0.95f, ui::FontWeight::Bold);
    name.Draw(inner_l, plate_top + 10, alpha);
    int after = inner_l + name.GetWidth() + 12;
    if (!here.rank.empty()) {
        SkyText(here.rank, 0.75f, 240, 238, 232, ui::Alignment::Left, after, plate_top + 13, alpha);
        after += 40;
    }
    if (!here.tag.empty()) SkyText(here.tag, 0.65f, group.r, group.g, group.b, ui::Alignment::Left, after, plate_top + 15, alpha);
    if (!here.cost.empty()) SkyText(here.cost, 0.65f, 240, 238, 232, ui::Alignment::Right, inner_r, plate_top + 14, alpha);
    const auto lines = ui::WrapText(here.text, inner_r - inner_l, 0.68f);
    for (std::size_t i = 0; i < lines.size() && i < 2; ++i) {
        SkyText(lines[i], 0.68f, 240, 238, 232, ui::Alignment::Left, inner_l, plate_top + 36 + static_cast<int>(i) * 15, alpha);
    }
    if (!here.needs.empty()) SkyText(here.needs, 0.6f, 200, 214, 240, ui::Alignment::Left, inner_l, plate_top + 68, alpha);
    // The buttons, centred on a row of their own along the plate's foot, shrunk if they would not fit.
    const std::string hint = mode.hint.empty()
                                 ? std::string("{dpad} Move  {L2}{R2} Sky  {cross} Buy  {square} On/off  {L1}{R1} Rank  {triangle} Map  {circle} Back")
                                 : mode.hint;
    float hint_scale = 0.62f;
    int   hint_w = SkyHint(hint, hint_scale, 0, 0, alpha, false);
    if (hint_w > inner_r - inner_l) {
        hint_scale = 0.5f;
        hint_w = SkyHint(hint, hint_scale, 0, 0, alpha, false);
    }
    SkyHint(hint, hint_scale, cx - hint_w / 2, plate_top + plate_h - 36, alpha * 7 / 8, true);
    // The mouse's hand, when the mouse has the screen.
    float hand_x = 0.0f, hand_y = 0.0f;
    if (MenuPointerHand(hand_x, hand_y)) {
        RECT rect = {0, 0, 32, 32};
        setbilinear(0);
        DrawObjectVibe(hand_x, hand_y, TexManager.GetTexture("icon01", -1), rect, 128, alpha);
        setbilinear(1);
    }
}

// The mouse on a sky page: the node under the hand takes the cursor, a left click buys it (or works the button,
// map or constellation name under the hand), a right click goes back, and the wheel turns through the
// constellations.
moonmod::TitleModeInput TakeSkyPointer(const moonmod::TitleSky &sky) {
    if (!sky_pointer.IsOpen()) sky_pointer.Open();
    sky_pointer.wide = true;
    const auto take = sky_pointer.Take(320.0f, 240.0f);
    MenuPointerShow(&sky_pointer);
    if (!take.pointing || !take.moved) return moonmod::TitleModeInput::None;
    if ((take.clicked & 2) != 0) return moonmod::TitleModeInput::Back;
    if (take.wheel != 0.0f) return take.wheel > 0.0f ? moonmod::TitleModeInput::L2 : moonmod::TitleModeInput::R2;
    const float px = sky_pointer.x, py = sky_pointer.y;
    const bool  click = (take.clicked & 1) != 0;
    for (const SkyHitBox &box : sky_hit_boxes) {
        if (px < static_cast<float>(box.x0) || px >= static_cast<float>(box.x1) || py < static_cast<float>(box.y0) ||
            py >= static_cast<float>(box.y1)) {
            continue;
        }
        if (!click) return moonmod::TitleModeInput::None;
        if (box.group < sky.groups.size()) {
            const std::size_t entry = moonmod::TitleSkyGroupEntry(sky, box.group);
            if (entry < sky.nodes.size() && moonmod::PointTitleSky(entry)) TiPlayVolSE(MIDI_PORT_UNK_D, 122, 24, 1.0f);
            return moonmod::TitleModeInput::None;
        }
        return box.input;
    }
    const SkyHitNode *under = nullptr;
    float             under_d = 0.0f;
    for (const SkyHitNode &node : sky_hit_nodes) {
        const float d = std::hypot(px - node.x, py - node.y);
        if (d <= node.r && (under == nullptr || d < under_d)) {
            under = &node;
            under_d = d;
        }
    }
    if (under == nullptr) return moonmod::TitleModeInput::None;
    if (moonmod::PointTitleSky(under->index)) TiPlayVolSE(MIDI_PORT_UNK_D, 122, 24, 1.0f);
    if (!click) return moonmod::TitleModeInput::None;
    // On the map a click closes it onto that node; on the sky it buys.
    return sky.map ? moonmod::TitleModeInput::Map : moonmod::TitleModeInput::Confirm;
}

// A mod title mode's screen on one plate, laid out from its content: heading, subtitle and info lines, the
// entries with the title's pointing hand beside the selected one, and the hint at the bottom.
void DrawTitleModeScreen(int alpha) {
    const moonmod::TitleMode *mode = moonmod::OpenTitleModeScreenMode();
    if (mode == nullptr || alpha <= 0) {
        return;
    }
    if (mode->sky && !mode->sky->nodes.empty() && !mode->sky->groups.empty()) {
        DrawTitleSky(*mode, alpha);
        return;
    }
    constexpr int kTop = 24;
    constexpr int kLineStep = 22;
    constexpr int kEntryStep = 26;
    int           y = kTop + 58;
    if (!mode->subtitle.empty()) {
        y += 34;
    }
    const int lines_top = y;
    y += static_cast<int>(mode->lines.size()) * kLineStep + 14;
    const int entries_top = y;
    y += static_cast<int>(mode->entries.size()) * kEntryStep + 6;
    const int hint_y = y;
    const int bottom = hint_y + 42;

    ui::TrackerWindow::Draw(60, MenuY(kTop), 520, bottom - kTop, alpha, ui::WindowPreset::BlueStone);
    ui::Label heading(mode->title, ui::ColorGold, ui::Alignment::Center, true, 1.5f, ui::FontWeight::Bold);
    heading.SetOutline(2.0f);
    heading.Draw(320, MenuY(kTop + 12), alpha);
    if (!mode->subtitle.empty()) {
        ui::Label(mode->subtitle, ui::ColorWhite, ui::Alignment::Center, true, 0.9f).Draw(320, MenuY(kTop + 58), alpha);
    }
    for (std::size_t i = 0; i < mode->lines.size(); ++i) {
        ui::Label(mode->lines[i], ui::ColorWhite, ui::Alignment::Center, true, 0.8f)
            .Draw(320, MenuY(lines_top + static_cast<int>(i) * kLineStep), alpha);
    }
    for (std::size_t i = 0; i < mode->entries.size(); ++i) {
        const auto &entry = mode->entries[i];
        const bool  selected = i == mode->cursor;
        const int   entry_y = MenuY(entries_top + static_cast<int>(i) * kEntryStep);
        const int   entry_alpha = !entry.enabled ? 48 : (selected ? 128 : 104);
        ui::Label   label(entry.label, selected ? ui::ColorGold : ui::ColorWhite, ui::Alignment::Center, true, 1.0f,
                          selected ? ui::FontWeight::Bold : ui::FontWeight::Normal);
        label.Draw(320, entry_y, entry_alpha * alpha / 128);
        if (selected) {
            const int width = label.GetWidth();
            RECT      rect = {0, 0, 32, 32};
            setbilinear(0);
            CursorVibeCnt++;
            DrawObjectVibe(static_cast<float>(320 - width / 2 - 34), static_cast<float>(entry_y - 4),
                           TexManager.GetTexture("icon01", -1), rect, 128, alpha);
            setbilinear(1);
        }
    }
    const std::string hint = mode->hint.empty() ? std::string("{cross} Choose    {circle} Back") : mode->hint;
    ui::Label(hint, ui::ColorWhite, ui::Alignment::Center, true, 0.7f).Draw(320, MenuY(hint_y), alpha * 3 / 4);
}

struct TitlePointerAction {
    bool start = false;
    bool confirm = false;
    bool back = false;
    bool moved = false;
    int  confirm_hover = -1; // the exit confirmation's button under the pointer
};

TitlePointerAction TakeTitlePointer() {
    TitlePointerAction action;
    if (CProcess.no == TITLE_STEP_OPTION_INIT || CProcess.no == TITLE_STEP_OPTION ||
        CProcess.no == TITLE_STEP_SAVE_INIT || CProcess.no == TITLE_STEP_SAVE ||
        CProcess.no == kTitleStepModMode || CProcess.no == kTitleStepModFadeOut) {
        // The child screen owns pointer capture, including a button held through the fade.
        title_pointer.Abandon();
        const moonmod::TitleMode *mode = moonmod::OpenTitleModeScreenMode();
        if (CProcess.no != kTitleStepModMode || mode == nullptr || !mode->sky) sky_pointer.Close();
        return action;
    }
    sky_pointer.Abandon(); // the title pointer takes the mouse over
    if (CProcess.no != TITLE_STEP_LOGO && CProcess.no != TITLE_STEP_MENU) {
        title_pointer.Close();
        return action;
    }
    if (!title_pointer.IsOpen()) {
        title_pointer.Open();
    }
    auto take = title_pointer.Take(225.0f, CProcess.no == TITLE_STEP_MENU ? CCursol.GetPos() : 312.0f);
    MenuPointerShow(&title_pointer);
    if (!take.pointing || !take.moved) {
        return action;
    }
    const float x = title_pointer.x;
    const float y = title_pointer.y - static_cast<float>(MenuY(0));
    if (CProcess.no == TITLE_STEP_LOGO) {
        action.start = (take.clicked & 1) && x >= 64 && x < 576 && y >= 296 && y < 360;
    } else if (title::menu::ConfirmOpen()) {
        action.confirm_hover = title::menu::ConfirmHit(title_pointer.x, title_pointer.y);
        action.confirm = (take.clicked & 1) != 0 && action.confirm_hover >= 0;
        action.back = (take.clicked & 2) != 0;
    } else {
        action.back = (take.clicked & 2) != 0;
        const int row = title::menu::RowAt(x, title_pointer.y);
        if (row >= 0) {
            if (CCursol.select != row) {
                CCursol.select = row;
                TiPlayVolSE(MIDI_PORT_UNK_D, 122, 24, 1.0f);
            }
            action.moved = true;
            action.confirm = (take.clicked & 1) != 0;
            opcnt = 0;
        }
    }
    return action;
}
} // namespace

int TitleLoop() {
    sceVu0FVECTOR pos;
    sceVu0FMATRIX matrix;

    FCamera.GetPos(pos);
    FCamera.Step(1);
    FCamera.GetCameraMatrix(matrix);
    MGSetViewMatrix(matrix, pos);
    Cloud__2.Step();
    const auto pointer = TakeTitlePointer();

    switch (CProcess.no) {
        case TITLE_STEP_FADE_IN:
            if (CFade.In()) {
                CProcess.no = TITLE_STEP_WAIT;
            }

            if (GamePad.Down(PAD_START) && EffCnt > 16) {
                CFade.Skip();
                CProcess.no = TITLE_STEP_LOGO;
            }

            if (EffCnt < 100) {
                EffCnt++;
            }

            break;

        case TITLE_STEP_RETURN_FADE_IN:
            if (CFade.In2()) {
                CProcess.no = TITLE_STEP_MENU;
            }

            break;

        case TITLE_STEP_WAIT:
            if (Wait < 120) {
                Wait++;
            } else {
                CProcess.no = TITLE_STEP_LOGO;
            }

            if (GamePad.Down(PAD_START) || pointer.start) {
                CProcess.no = TITLE_STEP_LOGO;
            }

            break;

        case TITLE_STEP_LOGO:
            if (CSprite.Se() == 0) {
                TiPlayVolSE(MIDI_PORT_SE_DEFAULT, 38, 21, 1.0f);
            }

            CSprite.Move();

            if (GamePad.Down(PAD_START) || pointer.start) {
                Logo.motion_type.state.time = 116.0f;
                Fade1 = 128;
                CSprite.x[0] = 700.0f;
            }

            if (CSprite.x[0] > 100.0f) {
                if (Logo.motion_type.state.time < 116.0f) {
                    for (int i = 0; i < 9; i++) {
                        Spark[i].Step();
                    }

                    Logo.Step();
                    CLogo.Fade();
                } else {
                    if (Fade1 < 128) {
                        Fade1++;
                    } else {
                        Fade2 = (Fade2 + 2) & 0x7f;
                        keywait++;
                        keywait = std::min(keywait, 500);
                    }

                    CLogo.Move();
                }

                if (Fade1 > 127 && (GamePad.Down(PAD_START) || pointer.start)) {
                    TiPlayVolSE(MIDI_PORT_UNK_D, 122, 25, 1.0f);
                    CProcess.no = TITLE_STEP_MENU;
                    opcnt = 0;
                }

                if (Fade1 > 127) {
                    if (opcnt > 1800) {
                        CCursol.select = TITLE_MENU_ATTRACT;
                        CProcess.no = TITLE_STEP_ATTRACT_FADE_OUT;
                    } else {
                        opcnt++;
                    }
                }
            }

            break;

        case TITLE_STEP_MENU:
            CLogo.Move();

            if (title::menu::ConfirmOpen()) {
                title::menu::StepConfirm(pointer.confirm_hover, pointer.confirm, pointer.back);
                break;
            }

            if (CCursol.Move() || pointer.moved || pointer.back) {
                if (GamePad.Down(PAD_UP)) {
                    CCursol.select--;
                    opcnt = 0;
                    TiPlayVolSE(MIDI_PORT_UNK_D, 122, 24, 1.0f);
                }

                if (GamePad.Down(PAD_DOWN)) {
                    CCursol.select++;
                    opcnt = 0;
                    TiPlayVolSE(MIDI_PORT_UNK_D, 122, 24, 1.0f);
                }

                if (CCursol.select < 0) {
                    CCursol.select = title::menu::ExitRow();
                }

                if (CCursol.select > title::menu::ExitRow()) {
                    CCursol.select = TITLE_MENU_NEW_GAME;
                }

                CCursol.Set(title::menu::HandY(CCursol.select));

                if (GamePad.Down(PAD_START) || GamePad.Down(PAD_CROSS) || pointer.confirm) {
                    if (CCursol.select == title::menu::ExitRow()) {
                        title::menu::OpenConfirm();
                    } else if (const int mode = title::menu::ModeAt(CCursol.select); mode >= 0) {
                        // A mod title mode: its screen replaces the logo and menu.
                        TiPlayVolSE(MIDI_PORT_SE_DEFAULT, 38, 20, 1.0f);
                        if (moonmod::OpenTitleModeScreen(static_cast<std::size_t>(mode))) {
                            CProcess.no = kTitleStepModMode;
                        }
                    } else {
                        TiPlayVolSE(MIDI_PORT_SE_DEFAULT, 38, 20, 1.0f);
                        CProcess.no = TITLE_STEP_MENU_BLINK;
                    }
                    opcnt = 0;
                }

                if (!title::menu::ConfirmOpen() && (GamePad.Down(PAD_CIRCLE) || pointer.back)) {
                    Fade1 = Fade2 = Fade3 = Fade4 = 0;
                    CSprite.Init();
                    CLogo.Init();
                    CCursol.Init();
                    CProcess.no = TITLE_STEP_LOGO;
                    opcnt = 0;
                }

                if (opcnt > 1800) {
                    CCursol.select = TITLE_MENU_ATTRACT;
                    CProcess.no = TITLE_STEP_ATTRACT_FADE_OUT;
                } else {
                    opcnt++;
                }
            }

            break;

        case TITLE_STEP_MENU_BLINK:
            CLogo.Move();
            brink = 1;
            brinkcnt++;

            if (brinkcnt > 60) {
                CProcess.no = TITLE_STEP_MENU_FADE_OUT;
            }

            break;

        case TITLE_STEP_MENU_FADE_OUT:
            brinkcnt++;

            if (CFade.Out()) {
                switch (CCursol.GetSelect()) {
                    case TITLE_MENU_NEW_GAME:
                        CProcess.no = TITLE_STEP_OPENING_BOOK_INIT;
                        break;
                    case TITLE_MENU_LOAD:
                        CProcess.no = TITLE_STEP_SAVE_INIT;
                        break;
                    case TITLE_MENU_OPTION:
                        CProcess.no = TITLE_STEP_OPTION_INIT;
                        break;
                    default:
                        break;
                }
            }

            break;

        case TITLE_STEP_ATTRACT_FADE_OUT:
            if (CFade.Out()) {
                SndStopAllSe();
                CSnd.Stop(MIDI_PORT_BGM);
                CSnd.StopVoice(0);
                CProcess.no = TITLE_STEP_EXIT;
            }

            break;

        case TITLE_STEP_OPENING_BOOK_INIT:
            SndStopAllSe();
            CSnd.Stop(MIDI_PORT_BGM);
            CSnd.StopVoice(0);
            CSnd.SetReverb(0, 4, 40);
            CSnd.SetVol(MIDI_PORT_SE_TITLE, 256);
            CSnd.SetVol(MIDI_PORT_SE_DEFAULT, 256);
            CSnd.SetVol(MIDI_PORT_UNK_D, 256);
            CSnd.SetVol(MIDI_PORT_SE_SPECIAL, 256);
            CSnd.SQ_Play(MIDI_PORT_BGM, 0);
            TiPlayVolSE(MIDI_PORT_SE_TITLE, 16, 24, 0.5f);
            {
                int book[2] = {2, 60};

                InitOpeningBook(reinterpret_cast<u_long128 *>(read_buffer), book);
            }
            CProcess.no = TITLE_STEP_OPENING_BOOK;
            break;

        case TITLE_STEP_OPENING_BOOK:
            if (OpeningBookKey()) {
                CProcess.no = TITLE_STEP_EXIT;
            }

            break;

        case TITLE_STEP_SAVE_INIT:
            InitMenuSave(SAVE_MENU_MODE_LOAD, 2, nullptr);
            CProcess.no = TITLE_STEP_SAVE;
            break;

        case TITLE_STEP_SAVE:
            switch (MenuSaveKey()) {
                case MENU_SAVE_RUNNING:
                    CFade.In();
                    break;
                case MENU_SAVE_LOADED:
                    MapJump(SaveData->GetMapNo(), -1);
                    app::GameResumeLoaded();
                    CProcess.no = TITLE_STEP_EXIT;
                    break;
                case MENU_SAVE_CLOSED:
                    brink = 0;
                    CFade.value = 0;
                    CProcess.no = TITLE_STEP_RETURN_FADE_IN;
                    break;
                default:
                    break;
            }

            break;

        case TITLE_STEP_OPTION_INIT:
            InitMenuOption(OPTION_OPEN_TITLE, 2, nullptr);
            CProcess.no = TITLE_STEP_OPTION;
            break;

        case TITLE_STEP_OPTION:
            CFade.In();

            if (MenuOptionKey()) {
                brink = 0;
                CFade.value = 0;
                CProcess.no = TITLE_STEP_RETURN_FADE_IN;
            }

            break;

        case kTitleStepModMode: {
            CFade.In();
            opcnt = 0;
            moonmod::TitleModeInput input = moonmod::TitleModeInput::None;
            if (GamePad.Down(PAD_UP)) {
                input = moonmod::TitleModeInput::Up;
            } else if (GamePad.Down(PAD_DOWN)) {
                input = moonmod::TitleModeInput::Down;
            } else if (GamePad.Down(PAD_LEFT)) {
                input = moonmod::TitleModeInput::Left;
            } else if (GamePad.Down(PAD_RIGHT)) {
                input = moonmod::TitleModeInput::Right;
            } else if (GamePad.Down(PAD_CROSS) || GamePad.Down(PAD_START)) {
                input = moonmod::TitleModeInput::Confirm;
            } else if (GamePad.Down(PAD_CIRCLE)) {
                input = moonmod::TitleModeInput::Back;
            } else if (GamePad.Down(PAD_SQUARE)) {
                input = moonmod::TitleModeInput::Square;
            } else if (GamePad.Down(PAD_L1)) {
                input = moonmod::TitleModeInput::L1;
            } else if (GamePad.Down(PAD_R1)) {
                input = moonmod::TitleModeInput::R1;
            } else if (GamePad.Down(PAD_L2)) {
                input = moonmod::TitleModeInput::L2;
            } else if (GamePad.Down(PAD_R2)) {
                input = moonmod::TitleModeInput::R2;
            } else if (GamePad.Down(PAD_TRIANGLE)) {
                input = moonmod::TitleModeInput::Map;
            }
            if (const moonmod::TitleMode *open = moonmod::OpenTitleModeScreenMode();
                input == moonmod::TitleModeInput::None && open != nullptr && open->sky && !open->sky->nodes.empty()) {
                input = TakeSkyPointer(*open->sky);
            }
            if (input == moonmod::TitleModeInput::Up || input == moonmod::TitleModeInput::Down ||
                input == moonmod::TitleModeInput::Left || input == moonmod::TitleModeInput::Right ||
                input == moonmod::TitleModeInput::L2 || input == moonmod::TitleModeInput::R2) {
                TiPlayVolSE(MIDI_PORT_UNK_D, 122, 24, 1.0f);
            } else if (input == moonmod::TitleModeInput::Confirm) {
                TiPlayVolSE(MIDI_PORT_SE_DEFAULT, 38, 20, 1.0f);
            }
            switch (moonmod::HandleTitleModeInput(input)) {
                case moonmod::TitleModeOutcome::Stay:
                    break;
                case moonmod::TitleModeOutcome::ReturnToTitle:
                    CProcess.no = TITLE_STEP_MENU;
                    if (CCursol.select >= title::menu::RowCount()) {
                        CCursol.select = TITLE_MENU_NEW_GAME;
                        CCursol.Set(title::menu::HandY(CCursol.select));
                    }
                    break;
                case moonmod::TitleModeOutcome::StartSession:
                    CProcess.no = kTitleStepModFadeOut;
                    break;
            }
            break;
        }

        case kTitleStepModFadeOut:
            if (CFade.Out()) {
                SndStopAllSe();
                CSnd.Stop(MIDI_PORT_BGM);
                CSnd.StopVoice(0);
                CCursol.select = moonmod::kTitleMenuSandbox;
                CProcess.no = TITLE_STEP_EXIT;
            }
            break;

        case TITLE_STEP_EXIT:
            SndStopAllSe();
            CSnd.Stop(MIDI_PORT_BGM);
            CSnd.StopVoice(0);
            return CCursol.GetSelect() + 1;

        default:
            break;
    }

    TitleDraw();
    CSnd.Step();
    return 0;
}

void TitleDraw() {
    sceVu0FVECTOR light0 = {2.4578f, 9.9294f, -2.8074f, 0.0f};
    sceVu0FVECTOR light1 = {4.6086f, -10.4028f, -0.8286f, 0.0f};
    sceVu0FVECTOR light2 = {0.0f, 0.0f, -10.0f, 0.0f};
    sceVu0FMATRIX light;
    sceVu0FMATRIX color = {
        {191.0f, 105.0f, 76.0f,  128.0f},
        {63.0f,  51.0f,  127.0f, 128.0f},
        {40.0f,  30.0f,  30.0f,  128.0f},
        {0.0f,   0.0f,   0.0f,   0.0f  }
    };
    sceVu0FVECTOR light0b = {0.0f, 6.0f, -8.0f, 0.0f};
    sceVu0FVECTOR light1b = {0.0f, 0.0f, 0.0f, 0.0f};
    sceVu0FVECTOR light2b = {0.0f, 0.0f, 0.0f, 0.0f};
    sceVu0FMATRIX lightb;
    sceVu0FMATRIX colorb = {
        {128.0f, 128.0f, 112.0f, 128.0f},
        {0.0f,   0.0f,   0.0f,   0.0f  },
        {0.0f,   0.0f,   0.0f,   0.0f  },
        {0.0f,   0.0f,   0.0f,   0.0f  }
    };
    sceVu0FVECTOR ambient = {0.0f, 0.0f, 0.0f, 100.0f};

    if (CProcess.no == TITLE_STEP_FADE_IN || (CProcess.no == TITLE_STEP_MENU_FADE_OUT && CCursol.GetSelect() == TITLE_MENU_NEW_GAME) || CProcess.no == TITLE_STEP_ATTRACT_FADE_OUT || TitleModeBackdropFading()) {
        ambient[3] = static_cast<float>(CFade.Get(128));
    } else {
        ambient[3] = 128.0f;
    }

    MGSetAmbient(ambient);

    sceVu0Normalize(light0, light0);
    sceVu0Normalize(light1, light1);
    sceVu0Normalize(light2, light2);
    sceVu0NormalLightMatrix(light, light0, light1, light2);
    MGSetPLight(light, color);

    if (CProcess.no != TITLE_STEP_OPENING_BOOK && CProcess.no != TITLE_STEP_EXIT) {
        static std::array<float, 4> rot = {0.0f, 0.0f, 0.0f, 0.0f};

        TexManager.ReloadTexture(GetVif1Packet(), 0);
        rot[1] += 0.001f;

        if (rot[1] > 3.14f) {
            rot[1] -= 6.28f;
        }

        ObjectFrame3->SetRotation(rot[0], rot[1], rot[2]);
        ObjectFrame3->SetScale(TitlePortBackdropScale(), 1.0f, TitlePortBackdropScale());
        MGDraw(ObjectFrame3);
    }

    if (CProcess.no != TITLE_STEP_OPENING_BOOK && CProcess.no != TITLE_STEP_EXIT) {
        TexManager.ReloadTexture(GetVif1Packet(), 1);

        if (CProcess.no == TITLE_STEP_FADE_IN || (CProcess.no == TITLE_STEP_MENU_FADE_OUT && CCursol.GetSelect() == TITLE_MENU_NEW_GAME) || CProcess.no == TITLE_STEP_ATTRACT_FADE_OUT || TitleModeBackdropFading()) {
            ambient[3] = static_cast<float>(CFade.Get(100));
        } else {
            ambient[3] = 100.0f;
        }

        MGSetAmbient(ambient);
        Cloud__2.SetScale(TitlePortBackdropScale(), TitlePortBackdropHeightScale(), 1.0f);
        Cloud__2.Draw();
    }

    if (CProcess.no != TITLE_STEP_FADE_IN || CFade.Get(128) >= 4) {
        sceGsTest test = {};

        MGSetGsTEST(&test);
        test.bits.date = 0;
        test.bits.ate = 0;
        MGSetGsTEST(&test);
        setbilinear(1);

        const TitleFogBand bands[] = {
            {CRect_i_(0, 0,   640, 85),  CRect_i_(1, 0,   639, 45), 112},
            {CRect_i_(0, 85,  640, 84),  CRect_i_(1, 42,  639, 45), 114},
            {CRect_i_(0, 169, 640, 84),  CRect_i_(1, 84,  639, 45), 114},
            {CRect_i_(0, 253, 640, 84),  CRect_i_(1, 126, 639, 45), 114},
            {CRect_i_(0, 336, 640, 105), CRect_i_(1, 167, 639, 57), 114},
            {CRect_i_(0, 441, 640, 39),  CRect_i_(1, 220, 639, 19), 114},
        };
        TitlePortFog(bands);

        MGSetGsTEST(nullptr);
    }

    {
        sceGsTex0 tex0;
        sceGsTex0 image;

        TitlePortCaptureFog();
        MGGetFBuffTex(&tex0);
        image = std::bit_cast<sceGsTex0>(TexManager.GetTexture("frame_image", -1)->tex0);
        tex0.PSM = 1;
        MGStretchMoveImage(&tex0, CRect_i_(0, 0, 10240, SCREEN_HALF_HEIGHT * 16), &image, CRect_i_(0, 0, 10240, SCREEN_HALF_HEIGHT * 16));
    }
    MGClearZBuffer(0);

    switch (CProcess.no) {
        case kTitleStepModMode:
        case kTitleStepModFadeOut:
            TexManager.ReloadTexture(GetVif1Packet(), 2);
            DrawTitleModeScreen(CFade.Get(128));
            break;

        case TITLE_STEP_LOGO:
        case TITLE_STEP_RETURN_FADE_IN:
        case TITLE_STEP_MENU:
        case TITLE_STEP_MENU_BLINK:
        case TITLE_STEP_MENU_FADE_OUT:
        case TITLE_STEP_ATTRACT_FADE_OUT:
            sceVu0Normalize(light0b, light0b);
            sceVu0Normalize(light1b, light1b);
            sceVu0Normalize(light2b, light2b);
            sceVu0NormalLightMatrix(lightb, light0b, light1b, light2b);
            MGSetPLight(lightb, colorb);
            CLogo.Sparkdraw(Logo.motion_type.state.time);
            CLogo.Draw();

            set2DSprite(GetVif1Packet(), TexManager.GetTexture("main", -1), CRect_i_(0, 62, 288, 170), CRect_i_(0, 0, 288, 170), static_cast<u_char>(CFade.Get(Fade4)));
            set2DSprite(GetVif1Packet(), TexManager.GetTexture("main", -1), CRect_i_(288, 112, 72, 190), CRect_i_(288, 50, 72, 190), static_cast<u_char>(CFade.Get(Fade4)));
            set2DSprite(GetVif1Packet(), TexManager.GetTexture("main", -1), CRect_i_(360, 142, 412, 160), CRect_i_(360, 80, 412, 160), static_cast<u_char>(CFade.Get(Fade4)));

            ambient[3] = static_cast<float>(CFade.Get(128));
            MGSetAmbient(ambient);

            if (CFade.Get(128) == 128) {
                CSprite.Draw();
            }

            // The mod title rows sit under OPTION, so the banner below the menu moves down past them, as far as
            // the frame allows (its lower rows are clear), in the logo step too so it does not jump.
            set2DSprite(GetVif1Packet(), TexManager.GetTexture("start", -1),
                        CRect_i_(64, std::min(MenuY(362) + title::menu::BannerShift(), SCREEN_HEIGHT - 48), 512, 64),
                        CRect_i_(0, 64, 512, 64), static_cast<u_char>(CFade.Get(Fade1)));

            if (CProcess.no == TITLE_STEP_LOGO) {
                set2DSprite(GetVif1Packet(), TexManager.GetTexture("start", -1), CRect_i_(64, MenuY(296), 512, 64), CRect_i_(0, 0, 512, 64), static_cast<u_char>(CFade.Get(Fade2)));
            } else {
                const int brightness = (brink != 0 && brinkcnt % 3 == 0) ? 32 : 128;

                // The painted rows (start3) have no Exit, so all four come from the font. None are
                // drawn on the way to the attract movie, whose number Exit's row shares.
                if (CProcess.no != TITLE_STEP_ATTRACT_FADE_OUT) {
                    title::menu::Draw(CCursol.GetSelect(), CFade.Get(brightness), CFade.Get(32));
                }

                if (CProcess.no != TITLE_STEP_ATTRACT_FADE_OUT && !title::menu::ConfirmOpen()) {
                    setbilinear(0);
                    CursorVibeCnt++;

                    RECT rect = {.x = 0, .y = 0, .width = 32, .height = 32};

                    float hand_x = 225.0f;
                    float hand_y = CCursol.GetPos();
                    MenuPointerHand(hand_x, hand_y);
                    DrawObjectVibe(hand_x, hand_y, TexManager.GetTexture("icon01", -1), rect, 128, CFade.Get(128));
                    setbilinear(1);
                }

                title::menu::DrawConfirm(CFade.Get(128));
                setbilinear(1);
            }

            break;

        case TITLE_STEP_OPENING_BOOK:
            TexManager.ReloadTexture(GetVif1Packet(), 2);
            OpeningBookDraw();
            break;

        case TITLE_STEP_SAVE:
            TexManager.ReloadTexture(GetVif1Packet(), 2);
            DrawMenuSave(nullptr);
            break;

        case TITLE_STEP_OPTION:
            TexManager.ReloadTexture(GetVif1Packet(), 2);
            DrawMenuOption();
            break;

        default:
            break;
    }

    if (CProcess.no == TITLE_STEP_LOGO && title_pointer.pointing) {
        float hand_x = 0.0f;
        float hand_y = 0.0f;
        if (MenuPointerHand(hand_x, hand_y)) {
            RECT rect = {.x = 0, .y = 0, .width = 32, .height = 32};
            setbilinear(0);
            DrawObjectVibe(hand_x, hand_y, TexManager.GetTexture("icon01", -1), rect, 128, 128);
            setbilinear(1);
        }
    }
    TitlePortFeedback(35);
}

// Back from a mod's sandbox session: open that mod's title mode screen instead of the logo.
void TitlePortReopenMode() {
    // Nothing from an earlier title visit stays open or queued.
    moonmod::CloseTitleModeScreen();
    const std::optional<std::string> id = moonmod::TakeTitleModeReturn();
    if (!id) {
        return;
    }
    const auto &modes = moonmod::TitleModes();
    for (std::size_t i = 0; i < modes.size(); ++i) {
        if (modes[i].id != *id || !moonmod::OpenTitleModeScreen(i)) {
            continue;
        }
        CCursol.select = title::menu::kRetailRowCount + static_cast<int>(i);
        CCursol.Set(title::menu::HandY(CCursol.select));
        // As if Start skipped the logo's animation, so Circle back to the menu finds it finished.
        Logo.motion_type.state.time = 116.0f;
        CSprite.x[0] = 700.0f;
        Fade1 = Fade2 = Fade3 = Fade4 = 128;
        CProcess.no = kTitleStepModMode;
        return;
    }
}

void MoonModDrawSkyPage(const moonmod::TitleMode &mode, int alpha) {
    if (mode.sky && !mode.sky->nodes.empty()) DrawTitleSky(mode, alpha);
}

} // namespace dc::game

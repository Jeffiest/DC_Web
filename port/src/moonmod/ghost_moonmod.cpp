// The death ghost (world.ghost_get / world.ghost_play). Every other frame on a dungeon floor the player's place, heading
// and motion go into a ring of five seconds. A mod reads it when the player falls and keeps it, and later plays it back:
// the hero's model, loaded into arenas of its own, walks those frames again, pale and half seen through, and fades
// when they end. Motions a mod appended to the hero (past the pack's own) play as the pack's standing motion.

#include "moonmod/ghost.hpp"

#include <algorithm>
#include <cmath>

#include "character.hpp"
#include "dataalloc.hpp"
#include "dataread.hpp"
#include "dun/gameloop.hpp"
#include "dungeonmap.hpp"
#include "gamemode.hpp"
#include "main.hpp"
#include "mglib.hpp"
#include "playable_chara.hpp"
#include "texture.hpp"
#include "userstatus.hpp"

namespace dc::game {

namespace {

constexpr std::array<const char *, 6> kHeroFiles = {
    "dun/mainchara/c01d.chr", "dun/mainchara/c04b.chr", "dun/mainchara/c06b.chr",
    "dun/mainchara/c05a.chr", "dun/mainchara/c10b.chr", "dun/mainchara/c18a.chr",
};
constexpr int kGhostTextureBlock = 0x63;

alignas(64) unsigned char g_file_storage[6 * 1024 * 1024];
alignas(64) unsigned char g_model_storage[8 * 1024 * 1024];
CCharacter     g_body;
CTexAnimeData  g_anime[64];
CDataAlloc2<1> g_models(-1);
int            g_loaded = -1;

// The ring of the player's frames on this floor.
std::array<MoonModGhostFrame, kMoonModGhostFrames> g_ring{};
int g_head = 0, g_count = 0, g_clock = 0, g_ring_seed = -1, g_ring_chara = -1;

// Playing back.
std::vector<MoonModGhostFrame> g_play;
float g_at = -1.0f; // index into g_play; < 0 when not playing
int   g_motion = -1, g_fade = 0;

bool InDungeon() { return mode == GAME_MODE_DUNGEON && UserStatus != nullptr && NowDngMap != nullptr && CharaMain.frame != nullptr; }

void Unload() {
    if (g_loaded < 0) return;
    TexManager.DeleteTextureBlock(kGhostTextureBlock);
    TexManager.CleanUpBuffer();
    TexManager.CleanUpTextureList();
    g_body.Initialize();
    g_loaded = -1;
    g_motion = -1;
}

bool Load(int chara) {
    Unload();
    const char *path = chara < 6 ? kHeroFiles[chara] : CharaPackPath(chara);
    int  size = 0;
    auto *pack = reinterpret_cast<unsigned int *>(g_file_storage);
    if (path == nullptr || LoadFile(const_cast<char *>(path), pack, &size) == 0 || size <= 0 || size > static_cast<int>(sizeof(g_file_storage))) {
        return false;
    }
    g_models.base = g_model_storage;
    g_models.used = 0;
    g_models.limit = static_cast<int>(sizeof(g_model_storage) >> 4);
    g_body.Initialize();
    for (CTexAnimeData &anime : g_anime) anime.Initialize();
    g_body.InitializeTexAnime(g_anime, 64);
    g_body.LoadPackData2(pack, const_cast<char *>("base.cfg"), &g_models, kGhostTextureBlock, &g_models, 0);
    if (g_body.frame == nullptr) return false;
    CFrameAttr attr;
    attr.fog_enable = true;
    attr.clip_enable = false;
    attr.program_option = 0;
    g_body.frame->SetAttr(attr, 1, 4);
    g_loaded = chara;
    return true;
}

void Place(const MoonModGhostFrame &f) {
    float p[4] = {f[0], f[1], f[2], 1.0f};
    g_body.frame->SetPosition(p);
    g_body.SetPosition(p);
    g_body.frame->SetRotation(0.0f, f[3], 0.0f);
    g_body.SetRotation(0.0f, f[3], 0.0f);
}

void Stop() {
    g_play.clear();
    g_at = -1.0f;
    g_fade = 0;
    Unload();
}

} // namespace

std::vector<MoonModGhostFrame> MoonModGhostRecorded(int &character) {
    character = g_count > 0 ? g_ring_chara : -1;
    std::vector<MoonModGhostFrame> out;
    out.reserve(g_count);
    for (int i = 0; i < g_count; ++i) out.push_back(g_ring[(g_head + i) % kMoonModGhostFrames]);
    return out;
}

bool MoonModGhostPlay(int character, const std::vector<MoonModGhostFrame> &frames) {
    if (character < 0 || frames.empty()) {
        Stop();
        return character < 0;
    }
    if (character >= CharaCount()) return false;
    g_play = frames;
    g_at = 0.0f;
    g_fade = 0;
    g_motion = -1;
    if (g_loaded != character) Unload();
    g_loaded = -2 - character; // loaded on the next dungeon frame
    return true;
}

bool MoonModGhostPlaying() { return g_at >= 0.0f; }

// Once a frame from the dungeon loop, after the player has moved.
void MoonModGhostStep() {
    if (!InDungeon()) {
        if (mode != GAME_MODE_DUNGEON) {
            Stop();
            g_count = 0;
        }
        return;
    }
    // Keep the player's frames; a new floor (or another hero) starts the ring again.
    const int chara = UserStatus->cur_chara;
    if (g_ring_seed != NowDngMap->map_seed || g_ring_chara != chara) {
        g_ring_seed = NowDngMap->map_seed;
        g_ring_chara = chara;
        g_head = g_count = 0;
    }
    if ((++g_clock & 1) == 0) {
        float at[4], facing[4];
        CharaMain.GetPosition(at);
        CharaMain.frame->GetRotation(facing);
        const MoonModGhostFrame f = {at[0], at[1], at[2], facing[1], static_cast<float>(CharaMain.motion_no)};
        if (g_count < kMoonModGhostFrames) {
            g_ring[(g_head + g_count++) % kMoonModGhostFrames] = f;
        } else {
            g_ring[g_head] = f;
            g_head = (g_head + 1) % kMoonModGhostFrames;
        }
    }

    if (g_at < 0.0f) return;
    if (g_loaded <= -2 && !Load(-2 - g_loaded)) {
        Stop();
        return;
    }
    const int last = static_cast<int>(g_play.size()) - 1;
    const int i = std::min(last, static_cast<int>(g_at));
    MoonModGhostFrame f = g_play[i];
    if (i < last) {
        const float t = g_at - static_cast<float>(i);
        for (int axis = 0; axis < 3; ++axis) f[axis] += (g_play[i + 1][axis] - f[axis]) * t;
    }
    Place(f);
    int motion = static_cast<int>(f[4]);
    if (motion < 0 || motion >= g_body.motion_end[0]) motion = 0;
    if (motion != g_motion) {
        g_body.motion_no = motion;
        g_body.motion_flags = 4;
        g_body.motion_speed = -1.0f;
        g_motion = motion;
    } else {
        g_body.motion_flags = 0;
    }
    g_body.Step();
    g_body.ClothStep(0);
    // Two frames kept per frame played back: the replay runs at the speed it was lived. The last pose holds, fading.
    if (i < last) {
        g_at += 0.5f;
    } else if (++g_fade > 60) {
        Stop();
    }
}

void MoonModGhostDraw(bool shadow) {
    if (g_at < 0.0f || g_loaded < 0 || !InDungeon() || shadow) return;
    float saved[4];
    MGGetAmbient(saved);
    const float alpha = 56.0f * (1.0f - static_cast<float>(g_fade) / 60.0f);
    float ambient[4] = {70.0f, 90.0f, 128.0f, alpha};
    MGSetAmbient(ambient);
    TexManager.ReloadTexture(Vif1Packet, kGhostTextureBlock);
    g_body.TextureAnime(kGhostTextureBlock);
    g_body.Draw();
    MGSetAmbient(saved);
}

} // namespace dc::game

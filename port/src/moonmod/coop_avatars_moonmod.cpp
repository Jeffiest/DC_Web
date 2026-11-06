// Co-op's remote players (moonmod/coop_avatars.hpp). Each avatar loads like the companion (its own file and model
// arenas, its own texture blocks, the weapon referenced to the hand) and is moved like the ghost (placed each frame),
// but from reports that come a few times a second: it eases toward the last one.

#include "moonmod/coop_avatars.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

#include "btmisc.hpp"
#include "character.hpp"
#include "dataalloc.hpp"
#include "dataread.hpp"
#include "dun/gameloop.hpp"
#include "editground.hpp"
#include "editloop.hpp"
#include "editmapscript.hpp"
#include "gamemode.hpp"
#include "itemdata.hpp"
#include "main.hpp"
#include "mglib.hpp"
#include "playable_chara.hpp"
#include "texture.hpp"
#include "ui/framework.hpp"
#include "userstatus.hpp"

namespace dc::game {

namespace {

constexpr std::array<const char *, 6> kHeroFiles = {
    "dun/mainchara/c01d.chr", "dun/mainchara/c04b.chr", "dun/mainchara/c06b.chr",
    "dun/mainchara/c05a.chr", "dun/mainchara/c10b.chr", "dun/mainchara/c18a.chr",
};
// Two blocks each, body and weapon: past the companion's (0x60, 0x61), the dungeon figure's (0x62) and the ghost's (0x63).
constexpr int    kFirstTextureBlock = 0x64;
constexpr size_t kFileBytes = 6 * 1024 * 1024;
constexpr size_t kModelBytes = 8 * 1024 * 1024;
// Past this from where it stands, an avatar jumps instead of easing (a warp, the stairs, a new floor).
constexpr float kJump = 160.0f;

struct Avatar {
    bool               wanted = false;
    MoonModAvatarState target;
    int                loaded_character = -1, loaded_weapon = -1;
    bool               placed = false;
    float              at[3] = {}, heading = 0.0f;
    int                motion = -1;
    std::unique_ptr<unsigned char[]> files, models;
    CCharacter         body, weapon;
    CTexAnimeData      anime[64];
    CDataAlloc2<1>     arena{-1};

    int BodyBlock(int slot) const { return kFirstTextureBlock + slot * 2; }
};

std::array<Avatar, kMoonModAvatars> g_avatars;

unsigned char *Aligned(unsigned char *raw) {
    return reinterpret_cast<unsigned char *>((reinterpret_cast<std::uintptr_t>(raw) + 63) & ~std::uintptr_t{63});
}

bool InDungeon() { return mode == GAME_MODE_DUNGEON && UserStatus != nullptr && CharaMain.frame != nullptr; }
bool InTown() {
    return mode == GAME_MODE_EDIT && MapNo >= 0 && MapNo < TOWN_COUNT && Chara != nullptr && Chara->frame != nullptr &&
           (GameMode == ED_MODE_WALK || GameMode == ED_MODE_TALK || GameMode == ED_MODE_EVENT || GameMode == ED_MODE_FISHING ||
            GameMode == ED_MODE_TIME_CHANGE || GameMode == ED_MODE_DOOR_OPEN);
}
bool InPlace() { return InDungeon() || InTown(); }
// Where the avatars are: a dungeon, or a town by its map. A new place loads them afresh (its own arenas and textures).
int Place() { return mode == GAME_MODE_DUNGEON ? 1000 : mode == GAME_MODE_EDIT ? MapNo : -1; }
int g_place = -1;

void PlayerAt(float out[4]) {
    if (mode == GAME_MODE_DUNGEON) {
        std::copy(CharaMain.pos, CharaMain.pos + 4, out);
    } else if (Chara != nullptr) {
        Chara->GetPosition(out);
    }
}

// A town hero walks with the town's motions (Toan's own town model numbers them 0-10); the avatars' models are the
// dungeon's, whose numbers the town maps them to (as party.town_set_active does for the others).
int TownMotion(int motion) {
    static constexpr int kMap[11] = {0, 1, 2, 0, 0, 34, 35, 31, 32, 0, 0};
    return motion >= 0 && motion < 11 ? kMap[motion] : motion;
}

void Unload(int slot) {
    Avatar &a = g_avatars[slot];
    if (a.loaded_character < 0) return;
    TexManager.DeleteTextureBlock(a.BodyBlock(slot));
    TexManager.DeleteTextureBlock(a.BodyBlock(slot) + 1);
    TexManager.CleanUpBuffer();
    TexManager.CleanUpTextureList();
    a.body.Initialize();
    a.weapon.Initialize();
    a.loaded_character = a.loaded_weapon = -1;
    a.motion = -1;
    a.placed = false;
    a.files.reset();
    a.models.reset();
}

unsigned int *ReadPack(const char *path, unsigned char *&at, unsigned char *end) {
    int  size = 0;
    auto *buffer = reinterpret_cast<unsigned int *>(at);
    if (path == nullptr || LoadFile(const_cast<char *>(path), buffer, &size) == 0 || size <= 0 || at + size > end) return nullptr;
    at += ((size >> 6) + 1) << 6;
    return buffer;
}

// A town's heroes carry no weapon.
int WeaponHere(const Avatar &a) { return mode == GAME_MODE_EDIT ? -1 : a.target.weapon; }

bool Load(int slot) {
    Avatar &a = g_avatars[slot];
    Unload(slot);
    const int chara = a.target.character;
    // The arenas, 64-byte aligned, taken only while the avatar is up.
    a.files.reset(new unsigned char[kFileBytes + 64]);
    a.models.reset(new unsigned char[kModelBytes + 64]);
    unsigned char *at = Aligned(a.files.get()), *end = at + kFileBytes;
    unsigned int  *pack = ReadPack(chara < 6 ? kHeroFiles[chara] : CharaPackPath(chara), at, end);
    if (pack == nullptr) {
        a.files.reset();
        a.models.reset();
        return false;
    }
    a.arena.base = Aligned(a.models.get());
    a.arena.used = 0;
    a.arena.limit = static_cast<int>(kModelBytes >> 4);
    a.body.Initialize();
    for (CTexAnimeData &anime : a.anime) anime.Initialize();
    a.body.InitializeTexAnime(a.anime, 64);
    a.body.LoadPackData2(pack, const_cast<char *>("base.cfg"), &a.arena, a.BodyBlock(slot), &a.arena, 0);
    if (a.body.frame == nullptr) {
        a.files.reset();
        a.models.reset();
        return false;
    }
    CFrameAttr attr;
    attr.fog_enable = true;
    attr.clip_enable = false;
    attr.program_option = 0;
    a.body.frame->SetAttr(attr, 1, 4);
    a.loaded_character = chara;
    a.loaded_weapon = -1;
    a.weapon.Initialize();
    const int weapon = WeaponHere(a);
    if (weapon >= ITEM_WEAPON_START) {
        std::array<char, 64> file{}, cfg{};
        BtGetWeaponNamePath3(file.data(), cfg.data(), weapon);
        const std::string path = std::string("commenu/weapon/") + file.data();
        if (unsigned int *weapon = file[0] != 0 ? ReadPack(path.c_str(), at, end) : nullptr) {
            a.weapon.LoadPackData3(weapon, cfg.data(), &a.arena, a.BodyBlock(slot) + 1, &a.arena, 1, 0);
            if (CFrame *hand = a.body.frame->SearchFrame(const_cast<char *>("weapon")); hand != nullptr && a.weapon.frame != nullptr) {
                a.weapon.SetPosition(0.0f, 0.0f, 0.0f);
                a.weapon.SetRotation(0.0f, 0.0f, 0.0f);
                a.weapon.frame->SetReference(hand);
            }
        }
    }
    // Loaded even without a weapon that fits: a weapon that won't load isn't tried again until it changes.
    a.loaded_weapon = weapon;
    a.placed = false;
    return true;
}

float Ease(float from, float to, float rate) { return from + (to - from) * rate; }

float TurnToward(float from, float to, float rate) {
    constexpr float kTau = 2.0f * std::numbers::pi_v<float>;
    float d = std::fmod(to - from, kTau);
    if (d > std::numbers::pi_v<float>) d -= kTau;
    if (d < -std::numbers::pi_v<float>) d += kTau;
    return from + d * rate;
}

} // namespace

bool MoonModAvatarSet(int slot, const MoonModAvatarState &state) {
    if (slot < 0 || slot >= kMoonModAvatars || state.character < 0 || state.character >= CharaCount()) return false;
    Avatar &a = g_avatars[slot];
    a.target = state;
    a.wanted = true;
    return true;
}

void MoonModAvatarClear(int slot) {
    for (int i = 0; i < kMoonModAvatars; ++i) {
        if (slot >= 0 && slot != i) continue;
        g_avatars[i].wanted = false;
        Unload(i);
    }
}

bool MoonModAvatarShown(int slot) {
    return slot >= 0 && slot < kMoonModAvatars && g_avatars[slot].wanted && g_avatars[slot].loaded_character >= 0;
}

bool MoonModAvatarAt(int slot, float out[3]) {
    if (!MoonModAvatarShown(slot) || !g_avatars[slot].placed || !InDungeon() || g_place != Place()) return false;
    std::copy(g_avatars[slot].at, g_avatars[slot].at + 3, out);
    return true;
}

// Once a frame from the dungeon loop, or a town's while walking.
void MoonModAvatarsStep() {
    if (Place() != g_place) {
        // Another place: the arenas go; the mod's wishes stay, for wherever comes next.
        for (int i = 0; i < kMoonModAvatars; ++i) Unload(i);
        g_place = Place();
    }
    if (!InPlace()) return;
    for (int slot = 0; slot < kMoonModAvatars; ++slot) {
        Avatar &a = g_avatars[slot];
        if (!a.wanted) continue;
        if (a.loaded_character != a.target.character || a.loaded_weapon != WeaponHere(a)) {
            if (!Load(slot)) {
                a.wanted = false; // a character that can't load isn't tried every frame
                continue;
            }
        }
        const float to[3] = {a.target.x, a.target.y, a.target.z};
        const float far = std::hypot(to[0] - a.at[0], to[2] - a.at[2]);
        if (!a.placed || far > kJump) {
            std::copy(to, to + 3, a.at);
            a.heading = a.target.heading;
            a.placed = true;
        } else {
            // About a fifth of the way each frame: a report every six frames lands just as the next one comes.
            for (int axis = 0; axis < 3; ++axis) a.at[axis] = Ease(a.at[axis], to[axis], 0.2f);
            a.heading = TurnToward(a.heading, a.target.heading, 0.3f);
        }
        float p[4] = {a.at[0], a.at[1], a.at[2], 1.0f};
        a.body.frame->SetPosition(p);
        a.body.SetPosition(p);
        a.body.frame->SetRotation(0.0f, a.heading, 0.0f);
        a.body.SetRotation(0.0f, a.heading, 0.0f);
        int motion = mode == GAME_MODE_EDIT ? TownMotion(a.target.motion) : a.target.motion;
        if (motion < 0 || motion >= a.body.motion_end[0]) motion = 0;
        if (motion != a.motion) {
            a.body.motion_no = motion;
            a.body.motion_flags = 4;
            a.body.motion_speed = -1.0f;
            a.motion = motion;
        } else {
            a.body.motion_flags = 0;
        }
        a.body.Step();
        a.body.ClothStep(0);
        if (a.weapon.frame != nullptr) a.weapon.Step();
    }
}

void MoonModAvatarsDraw(bool shadow) {
    if (!InPlace() || g_place != Place()) return;
    for (int slot = 0; slot < kMoonModAvatars; ++slot) {
        Avatar &a = g_avatars[slot];
        if (!a.wanted || a.loaded_character < 0 || a.body.frame == nullptr) continue;
        if (shadow) {
            a.body.DrawShadow();
            continue;
        }
        float      saved[4];
        const bool tinted = a.target.tint[0] >= 0.0f;
        if (tinted) {
            MGGetAmbient(saved);
            float ambient[4] = {a.target.tint[0], a.target.tint[1], a.target.tint[2], saved[3]};
            MGSetAmbient(ambient);
        }
        TexManager.ReloadTexture(Vif1Packet, a.BodyBlock(slot));
        a.body.TextureAnime(a.BodyBlock(slot));
        a.body.Draw();
        if (a.weapon.frame != nullptr) {
            TexManager.ReloadTexture(Vif1Packet, a.BodyBlock(slot) + 1);
            a.weapon.Draw();
        }
        if (tinted) MGSetAmbient(saved);
    }
}

void MoonModAvatarsDrawNames() {
    if (!InPlace() || g_place != Place()) return;
    float player[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    PlayerAt(player);
    for (int slot = 0; slot < kMoonModAvatars; ++slot) {
        const Avatar &a = g_avatars[slot];
        if (!a.wanted || !a.placed || a.loaded_character < 0 || a.target.name.empty()) continue;
        const float far = std::hypot(a.at[0] - player[0], a.at[2] - player[2]);
        if (far > 260.0f) continue;
        sceVu0FVECTOR head = {a.at[0], a.at[1] + 20.0f, a.at[2], 1.0f}; // just over the head (CharaHeight is 14-18)
        int           screen[4] = {};
        if (MGRotTransPers2D(screen, head, 0) == 0) continue;
        // Full nearby, gone toward where monsters stop being drawn (300).
        const int alpha = far < 160.0f ? 128 : static_cast<int>(128.0f * (260.0f - far) / 100.0f);
        ui::Label label(a.target.name, ui::ColorWhite, ui::Alignment::Center, true, 0.7f);
        if (a.target.tint[0] >= 0.0f) label.SetCustomRgb(150, 186, 255); // a guest's name in their blue
        label.SetOutline(2.0f, 10, 10, 20);
        label.Draw(screen[0], screen[1] - 30, alpha); // where the game's damage numbers rise from, above them
    }
}

} // namespace dc::game

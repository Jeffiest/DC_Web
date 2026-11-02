#include "common.h"
#include "dataread.hpp"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <limits>
#include <optional>
#include <print>
#include <set>
#include <string>
#include <string_view>
#include <utility>

#include "data/data_read.hpp"
#include "dcdata.hpp"
#include "playable_chara.hpp"
#include "localize/localize.hpp"
#include "moonmod/manager_ui.hpp"
#include "moonmod/native_resource_overrides.hpp"
#include "platform/arena.hpp"
#include "platform/load_work_profile.hpp"
#include "platform/paths.hpp"
#include "moonmod/manager_ui.hpp"
#include "moonmod/native_resource_overrides.hpp"

namespace dc::game {

using namespace dc::data;
using namespace dc::localize;

namespace fs = std::filesystem;

namespace {

u_int *packfile_buff;

std::array<BG_READ_INFO, 32> bg_read_info;

// One file inside a pack: offsets are bytes from the entry, and a first byte of zero ends the table.
struct PackEntry {
    char name[64];
    int  offset;
    int  size;
    int  next;
};

bool SameName(const char *name, std::string_view other) {
    std::size_t length = std::strlen(name);
    return length == other.size() && dcdata::FoldPath(std::string_view(name, length)) == dcdata::FoldPath(other);
}

PackEntry *PackEntryAt(u_int *pack, std::size_t capacity, int wanted) {
    if (pack == nullptr || wanted < 0) {
        return nullptr;
    }
    auto       *bytes = reinterpret_cast<unsigned char *>(pack);
    std::size_t offset = 0;
    for (int index = 0; offset <= capacity && capacity - offset >= sizeof(PackEntry); index++) {
        auto *entry = reinterpret_cast<PackEntry *>(bytes + offset);
        if (entry->name[0] == 0 || std::memchr(entry->name, '\0', sizeof(entry->name)) == nullptr ||
            std::cmp_less(entry->next, sizeof(PackEntry)) || static_cast<std::size_t>(entry->next) > capacity - offset) {
            return nullptr;
        }
        if (index == wanted) {
            return entry;
        }
        offset += static_cast<std::size_t>(entry->next);
    }
    return nullptr;
}

bool PackNameDuplicated(u_int *pack, std::size_t capacity, const PackEntry *entry) {
    for (int index = 0;; index++) {
        PackEntry *candidate = PackEntryAt(pack, capacity, index);
        if (candidate == nullptr) {
            return false;
        }
        if (candidate != entry && SameName(candidate->name, entry->name)) {
            return true;
        }
    }
}

u_int *PackEntryData(u_int *pack, std::size_t capacity, PackEntry *entry) {
    auto      *bytes = reinterpret_cast<unsigned char *>(pack);
    const auto offset = static_cast<std::size_t>(reinterpret_cast<unsigned char *>(entry) - bytes);
    if (std::cmp_less(entry->offset, sizeof(PackEntry)) || entry->size < 0 ||
        static_cast<std::size_t>(entry->offset) > capacity - offset ||
        static_cast<std::size_t>(entry->size) > capacity - offset - static_cast<std::size_t>(entry->offset)) {
        return nullptr;
    }
    return reinterpret_cast<u_int *>(reinterpret_cast<unsigned char *>(entry) + entry->offset);
}

// The member's offset from the start of the pack.
std::size_t PackEntryOffset(const u_int *pack, const PackEntry *entry) {
    const auto *pack_bytes = reinterpret_cast<const std::uint8_t *>(pack);
    const auto *entry_bytes = reinterpret_cast<const std::uint8_t *>(entry);
    return static_cast<std::size_t>(entry_bytes - pack_bytes) + static_cast<std::size_t>(entry->offset);
}

// Hands a file just read to the localization layer: a message file may be rewritten from the
// language's JSON in place (it fits the whole sectors DataReadLoadFile zeroed), and a pack is noted
// so its message entries can be told from GetPackFile.
void Localized(std::string_view path, void *buffer, int *size) {
    LocalizeLoaded(path, buffer);
    LocalizeFile(path, buffer, dcdata::SectorsFor(static_cast<std::uint64_t>(*size)) * dcdata::kSector, size);
}

// MoonFactory: a mod's file in place of the game's, or a new file the game's data does not have.

// SetupBaseModel reads monster assets into the shared dungeon read buffer. Keep package-backed
// identities below 2 MiB so reads remain bounded even while script execution has temporarily
// advanced the retail read-buffer pointer.
constexpr std::uintmax_t kVirtualMonsterAssetLimit = 2ull * 1024ull * 1024ull;
constexpr std::uintmax_t kVirtualMapAssetLimit = 8ull * 1024ull * 1024ull;
// A registered playable character's pack (playable_chara.hpp): a .chr override's limit.
constexpr std::uintmax_t kVirtualPlayerPackLimit = 16ull * 1024ull * 1024ull;
// A new floor pack (dun/<map>.cfg, dun/mpd_pack/<map>.mpd): BtMapJumpLoad stops at 6,400,000 bytes.
constexpr std::uintmax_t kVirtualFloorPackLimit = 6400000ull;

std::string_view StripDevice(std::string_view path) {
    const std::size_t colon = path.find(':');
    return colon == std::string_view::npos ? path : path.substr(colon + 1);
}

// Reads a mod's file whole, zeroing up to the end of its last sector as the drive did. Returns -1 when
// it cannot be read or its sectors exceed max_padded_size.
int ReadModFile(const fs::path &file, void *buffer, std::uintmax_t max_padded_size) {
    std::error_code      error;
    const std::uintmax_t size = fs::file_size(file, error);
    std::ifstream        stream(file, std::ios::binary);
    if (error || !stream || size > static_cast<std::uintmax_t>(std::numeric_limits<int>::max())) {
        return -1;
    }
    const std::uintmax_t padded = dcdata::SectorsFor(size) * dcdata::kSector;
    if (padded > max_padded_size) {
        return -1;
    }
    stream.read(static_cast<char *>(buffer), static_cast<std::streamsize>(size));
    if (std::cmp_not_equal(stream.gcount(), size)) {
        return -1;
    }
    std::memset(static_cast<char *>(buffer) + size, 0, static_cast<std::size_t>(padded - size));
    // The bytes are the mod's, not whatever retail asset an earlier load left here: its members are found
    // in them and its textures decode from them instead of being looked up as that asset's baked documents.
    DataReadTrackUnindexedFile(buffer, static_cast<std::size_t>(padded));
    return static_cast<int>(size);
}

// How far an override may grow past its retail file, by kind (DC1_PORT_SIZE_LIMITS.md). Each of these
// loaders reads into the free tail of an arena and advances it by the size it got, so the read only has
// to fit the block it lands in. 0: the kind keeps its retail size.
struct GrowthLimit {
    std::uintmax_t bytes;
    const char    *why;
};

GrowthLimit OverrideGrowthLimit(std::string_view path) {
    const auto ends = [path](std::string_view tail) { return path.ends_with(tail); };
    // The opening's text goes to a fixed 68,992-byte VisualData block (title/opening.cpp MesBuffer).
    if (path.starts_with("opdat/")) {
        return {ends(".mes") ? 68992u : 0u, "opening text buffer"};
    }
    if (ends(".mpd")) {
        return {6399999u, "BtMapJumpLoad's 6,400,000-byte map check"};
    }
    if (ends(".cfg")) {
        return {1024u * 1024u, ".cfg limit"};
    }
    if (ends(".chr")) {
        return {16u * 1024u * 1024u, ".chr limit"};
    }
    if (ends(".stb")) {
        return {4u * 1024u * 1024u, ".stb limit"};
    }
    if (ends(".mes")) {
        return {1024u * 1024u, ".mes limit"};
    }
    if (ends(".snd")) {
        return {16u * 1024u * 1024u, ".snd limit"};
    }
    if (ends(".scn")) {
        return {8u * 1024u * 1024u, "Georama scene limit"};
    }
    if (ends(".edt")) {
        return {64u * 1024u, "Georama layout buffer (editground.cpp)"};
    }
    return {0, "this kind is held to its retail size"};
}

// A refused replacement is also shown on screen, once per file, so a mod author sees it without the log.
void NoticeRefusedOverride(const std::string &path, const char *why) {
    static std::set<std::string> told;
    if (!told.insert(path).second) {
        return;
    }
    moonmod::ShowManagerNotice("Mod file not used: " + path + " (" + why + ")", 480);
}

// The mod's replacement for a retail file, if one may be used. One no larger than the retail file always
// is. A larger one is used when its kind may grow, it is under that kind's limit and its whole sectors fit
// the arena block the buffer is in; otherwise the retail file is read and the log names the limit.
std::optional<fs::path> UsableReplacement(const std::string &folded, const void *buffer) {
    const std::optional<fs::path> replacement = moonmod::FindNativeResourceOverride(folded);
    if (!replacement) {
        return std::nullopt;
    }
    const std::uintmax_t original_size = DataFileSize(folded);
    std::error_code      error;
    const std::uintmax_t replacement_size = fs::file_size(*replacement, error);
    if (error || replacement_size == 0 ||
        replacement_size > static_cast<std::uintmax_t>(std::numeric_limits<int>::max())) {
        std::println(stderr, "moonmod: ignored empty or unreadable native resource override for {}", folded);
        return std::nullopt;
    }
    if (replacement_size <= original_size) {
        return replacement;
    }
    const GrowthLimit    limit = OverrideGrowthLimit(folded);
    const std::uintmax_t padded = dcdata::SectorsFor(replacement_size) * dcdata::kSector;
    const std::uintmax_t room = ArenaRoomAt(buffer);
    if (limit.bytes == 0 || replacement_size > limit.bytes) {
        std::println(stderr, "moonmod: refused override for {}: {} bytes, retail {}, limit {} ({})", folded,
                     replacement_size, original_size, limit.bytes == 0 ? original_size : limit.bytes, limit.why);
        NoticeRefusedOverride(folded, "too big");
        return std::nullopt;
    }
    if (padded > room) {
        std::println(stderr,
                     "moonmod: refused override for {}: {} bytes ({} with sector padding), retail {}, "
                     "destination buffer has {} bytes",
                     folded, replacement_size, padded, original_size, room);
        NoticeRefusedOverride(folded, "does not fit its buffer");
        return std::nullopt;
    }
    std::println(stderr, "moonmod: override for {} grows {} -> {} bytes (limit {}, buffer room {})", folded,
                 original_size, replacement_size, limit.bytes, room);
    return replacement;
}

bool IsNameStem(std::string_view stem, bool allow_dash_dot) {
    return !stem.empty() && stem.size() <= 15 && std::ranges::all_of(stem, [allow_dash_dot](unsigned char c) {
        return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' ||
               (allow_dash_dot && (c == '-' || c == '.'));
    });
}

// A registered weapon's own model (weapons.register model = "<stem>"): commenu/weapon/<stem>.chr.
// A registered playable character's pack (e.g. Opar's dun/mainchara/c07d.chr), which no retail data has; read from
// the mod that supplies it, in the dungeon (LoadFile) and in the menu's character change (LoadFileBG).
bool IsVirtualPlayerPackPath(std::string_view path) {
    return IsPlayableCharaPack(path);
}

bool IsVirtualWeaponModelPath(std::string_view path) {
    constexpr std::string_view prefix = "commenu/weapon/";
    if (!path.starts_with(prefix)) {
        return false;
    }
    path.remove_prefix(prefix.size());
    return path.size() > 4 && path.size() <= 19 && !path.contains('/') && path.ends_with(".chr");
}

// A mod villager's own model (characters.register kind = "npc"): gedit/<town>/chara/<stem>.chr.
bool IsVirtualVillagerModelPath(std::string_view path) {
    constexpr std::string_view prefix = "gedit/";
    if (!path.starts_with(prefix)) {
        return false;
    }
    path.remove_prefix(prefix.size());
    const std::size_t town_end = path.find('/');
    if (town_end == std::string_view::npos || town_end == 0 || town_end > 8) {
        return false;
    }
    path.remove_prefix(town_end + 1);
    if (!path.starts_with("chara/")) {
        return false;
    }
    path.remove_prefix(6);
    return path.size() > 4 && path.size() <= 35 && !path.contains('/') && path.ends_with(".chr");
}

// dun/monstor/<stem>.chr or .stb.
bool IsVirtualMonsterAssetPath(std::string_view path) {
    constexpr std::string_view prefix = "dun/monstor/";
    if (!path.starts_with(prefix)) {
        return false;
    }
    path.remove_prefix(prefix.size());
    const std::size_t extension = path.rfind('.');
    if (path.contains('/') || extension == std::string_view::npos ||
        (path.substr(extension) != ".chr" && path.substr(extension) != ".stb")) {
        return false;
    }
    return IsNameStem(path.substr(0, extension), true);
}

// dun/<map>.cfg or dun/mpd_pack/<map>.mpd with a 1-15 character [a-z0-9_] map name.
bool IsVirtualFloorPackPath(std::string_view path) {
    std::string_view extension;
    if (path.starts_with("dun/mpd_pack/")) {
        path.remove_prefix(13);
        extension = ".mpd";
    } else if (path.starts_with("dun/")) {
        path.remove_prefix(4);
        extension = ".cfg";
    } else {
        return false;
    }
    if (!path.ends_with(extension)) {
        return false;
    }
    path.remove_suffix(4);
    return IsNameStem(path, false);
}

// gedit/mod/<name>.scn: a Georama variant's own scene file (see ChronicleLoadModPartModel).
bool IsVirtualGeoramaScenePath(std::string_view path) {
    if (!path.starts_with("gedit/mod/") || !path.ends_with(".scn")) {
        return false;
    }
    path.remove_prefix(10);
    path.remove_suffix(4);
    return IsNameStem(path, false);
}

// Loads a file the way LoadFile2 and LoadFileBG do, with the mods' files in play: a path alias reads its
// target, a replacement is read in place of the retail file, and a new file a mod registers is read when
// the data has none. background is LoadFileBG's narrower set of new files. localization_path is the name
// the localization layer sees.
bool LoadWithMods(std::string_view path, void *buffer, int *size, std::string &localization_path, bool background) {
    const std::string                folded = dcdata::FoldPath(StripDevice(path));
    const std::optional<std::string> alias = moonmod::FindNativeResourcePathAlias(folded);
    const std::optional<fs::path>    exact_override = moonmod::FindNativeResourceOverride(folded);
    localization_path = std::string(path);
    const bool native = DataFileExists(path);
    if (!native && !(alias && DataFileExists(*alias))) {
        if (buffer == nullptr || !exact_override) {
            return false;
        }
        std::uintmax_t limit = 0;
        if (IsVirtualPlayerPackPath(folded)) {
            limit = kVirtualPlayerPackLimit;
        } else if (IsVirtualVillagerModelPath(folded)) {
            // MoonMod: a town's character model can be a playable character's whole pack (Opar grown in a scene)
            limit = kVirtualPlayerPackLimit;
        } else if (background) {
            const bool model = IsVirtualWeaponModelPath(folded);
            limit = model ? kVirtualMonsterAssetLimit : (alias ? kVirtualMapAssetLimit : 0);
        } else if (IsVirtualMonsterAssetPath(folded) || IsVirtualWeaponModelPath(folded)) {
            limit = kVirtualMonsterAssetLimit;
        } else if (IsVirtualFloorPackPath(folded)) {
            limit = kVirtualFloorPackLimit;
        } else if (IsVirtualGeoramaScenePath(folded) || alias) {
            limit = kVirtualMapAssetLimit;
        }
        if (limit == 0) {
            return false;
        }
        *size = ReadModFile(*exact_override, buffer, limit);
        return *size >= 0;
    }
    if (alias && exact_override) {
        *size = ReadModFile(*exact_override, buffer, kVirtualMapAssetLimit);
        return *size >= 0;
    }
    if (native) {
        if (const std::optional<fs::path> replacement = UsableReplacement(folded, buffer)) {
            *size = ReadModFile(*replacement, buffer, std::numeric_limits<std::uintmax_t>::max());
            return *size >= 0;
        }
        return DataReadLoadFile(path, buffer, size);
    }
    localization_path = *alias;
    return DataReadLoadFile(*alias, buffer, size);
}

} // namespace

void InitReadBG() {
    for (BG_READ_INFO &info : bg_read_info) {
        info.busy = false;
    }
}

// The read happens here rather than a vertical sync later, so the slot is handed out already
// complete: issued (id) and done, as retail's ReadBG leaves it.
bool GameFileAvailable(std::string_view path) {
    const std::string folded = dcdata::FoldPath(StripDevice(path));
    if (DataFileExists(path)) return true;
    if (const std::optional<std::string> alias = moonmod::FindNativeResourcePathAlias(folded); alias && DataFileExists(*alias)) return true;
    return moonmod::FindNativeResourceOverride(folded).has_value();
}

int LoadFileBG(char *name, u_long128 *buffer, int *out_size) {
    const dc::platform::load_profile::Span profile(dc::platform::load_profile::Kind::BackgroundRead);
    if (out_size) {
        *out_size = 0;
    }
    if (!name || *name == 0) {
        return 0;
    }
    BG_READ_INFO *info = nullptr;
    for (BG_READ_INFO &slot : bg_read_info) {
        if (!slot.busy) {
            info = &slot;
            break;
        }
    }
    if (!info) {
        return 0;
    }
    if (reinterpret_cast<std::uintptr_t>(buffer) % 64 != 0) {
        printf("/*/*/*/*/not 64byte align at %p %s\n", static_cast<void *>(buffer), name);
    }
    int         size = 0;
    std::string localization_path;
    if (!LoadWithMods(name, buffer, &size, localization_path, true)) {
        return 0;
    }
    Localized(localization_path, buffer, &size);
    *std::format_to_n(info->name, sizeof(info->name) - 1, "{}", name).out = '\0';
    info->busy = true;
    info->id = 1;
    info->done = 1;
    info->buffer = buffer;
    info->size = size;
    info->sector = 0;
    info->sectors = static_cast<int>(dcdata::SectorsFor(size));
    if (out_size) {
        *out_size = size;
    }
    return 1;
}

BG_READ_INFO *GetReadBGFile(int index) {
    if (index < 0 || std::cmp_greater_equal(index, bg_read_info.size())) {
        return nullptr;
    }
    return bg_read_info[index].busy ? &bg_read_info[index] : nullptr;
}

void StartReadBG() {
    InitReadBG();
}

void ReadBG() {
}

int ReadBGSync() {
    for (const BG_READ_INFO &info : bg_read_info) {
        if (info.busy && (info.id == 0 || info.done == 0)) {
            return 1;
        }
    }
    return 0;
}

void BreakReadBG() {
    InitReadBG();
}

void InitCDFile() {
    DataReadIndex();
}

void InitMemoryFile() {
}

int LoadFile(char *path, void *buffer, int *out_size) {
    if (!LoadFile2(path, buffer, out_size, 0)) {
        std::print("File open error \"{}\"\n \n \n", path);
        __assert("etc.cpp", 753, "FALSE");
    }

    return 1;
}

int LoadFile2(char *path, void *buffer, int *out_size, int /*mode*/) {
    if (out_size) {
        *out_size = 0;
    }
    int         size = 0;
    std::string localization_path;
    if (!LoadWithMods(path, buffer, &size, localization_path, false)) {
        return 0;
    }
    Localized(localization_path, buffer, &size);
    if (out_size) {
        *out_size = size;
    }
    return 1;
}

// Only development builds wrote files, to the host PC's drive; the device and any drive letter
// after it are dropped so the path lands under save/host0/.
int WriteFile(char *path, void *buffer, int size) {
    std::string_view name = path;
    if (std::size_t colon = name.rfind(':'); colon != std::string_view::npos) {
        name = name.substr(colon + 1);
    }
    std::string relative(name);
    std::ranges::replace(relative, '\\', '/');
    relative.erase(0, relative.find_first_not_of('/'));
    if (!dcdata::IsSafeRelative(relative) || size < 0 || (buffer == nullptr && size != 0)) {
        return 0;
    }
    fs::path        target = PathsSaveRoot() / "host0" / fs::path(relative);
    std::error_code error;
    fs::create_directories(target.parent_path(), error);
    std::ofstream stream(target, std::ios::binary | std::ios::trunc);
    if (!stream) {
        return 0;
    }
    stream.write(static_cast<const char *>(buffer), size);
    return stream ? 1 : 0;
}

int LoadPackFile(char *path, u_int *buffer, int *out_size) {
    packfile_buff = buffer;

    if (!LoadFile2(path, buffer, out_size, 0)) {
        packfile_buff = nullptr;
        return 0;
    }

    return 1;
}

u_int *GetPackFile(char *name, int *out_size) {
    return GetPackFile(packfile_buff, name, out_size);
}

// A pack is looked up by the last component of a path, as retail does; a message file in it is
// given as the language's JSON has it.
u_int *GetPackFile(u_int *pack, char *name, int *out_size) {
    if (out_size != nullptr) {
        *out_size = 0;
    }
    if (pack == nullptr || name == nullptr) {
        return nullptr;
    }
    std::size_t capacity = 0;
    if (!DataReadPackCapacity(pack, capacity)) {
        return nullptr;
    }
    std::string_view basename = name;
    if (std::size_t slash = basename.rfind('/'); slash != std::string_view::npos) {
        basename = basename.substr(slash + 1);
    }
    for (int index = 0;; index++) {
        PackEntry *entry = PackEntryAt(pack, capacity, index);
        if (entry == nullptr) {
            return nullptr;
        }
        if (SameName(entry->name, basename)) {
            u_int *data = PackEntryData(pack, capacity, entry);
            if (data == nullptr) {
                return nullptr;
            }
            int size = entry->size;
            data = const_cast<u_int *>(LocalizePack(pack, basename, data, &size));
            DataReadTrackPackMember(pack, data, static_cast<std::size_t>(size), PackEntryOffset(pack, entry), entry->name,
                                    static_cast<std::size_t>(entry->size), index, PackNameDuplicated(pack, capacity, entry));
            if (out_size) {
                *out_size = size;
            }
            return data;
        }
    }
}

u_int *GetPackFile(u_int *pack, int index, char **out_name, int *out_size) {
    if (out_size != nullptr) {
        *out_size = 0;
    }
    std::size_t capacity = 0;
    if (!DataReadPackCapacity(pack, capacity)) {
        return nullptr;
    }
    PackEntry *entry = PackEntryAt(pack, capacity, index);
    if (entry == nullptr) {
        return nullptr;
    }
    u_int *data = PackEntryData(pack, capacity, entry);
    if (data == nullptr) {
        return nullptr;
    }
    int size = entry->size;
    data = const_cast<u_int *>(LocalizePack(pack, entry->name, data, &size));
    DataReadTrackPackMember(pack, data, static_cast<std::size_t>(size), PackEntryOffset(pack, entry), entry->name,
                            static_cast<std::size_t>(entry->size), index, PackNameDuplicated(pack, capacity, entry));
    if (out_name != nullptr) {
        *out_name = entry->name;
    }
    if (out_size != nullptr) {
        *out_size = size;
    }
    return data;
}

int GetPackFileExt(u_int *pack, char *extension, u_int **files, int max_files, int *sizes, char **names) {
    if (extension == nullptr || files == nullptr || max_files <= 0) {
        return 0;
    }
    int found = 0;
    for (int index = 0; found < max_files; index++) {
        char  *name = nullptr;
        int    size = 0;
        u_int *data = GetPackFile(pack, index, &name, &size);
        if (data == nullptr) {
            break;
        }
        std::string_view       full_name(name);
        const std::size_t      dot = full_name.rfind('.');
        const std::string_view found_extension = dot == std::string_view::npos ? full_name : full_name.substr(dot + 1);
        if (dcdata::FoldPath(found_extension) != dcdata::FoldPath(extension)) {
            continue;
        }
        files[found] = data;
        if (sizes != nullptr) {
            sizes[found] = size;
        }
        if (names != nullptr) {
            names[found] = name;
        }
        found++;
    }
    return found;
}

} // namespace dc::game

// Messages players leave for each other (moonmod/messages_moonmod.hpp).

#include "moonmod/messages_moonmod.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <fstream>
#include <map>
#include <random>
#include <sstream>
#include <system_error>

#include "moonmod/manifest.hpp"
#include "moonmod/symlinks.hpp"

namespace moonmod::messages {
namespace fs = std::filesystem;

namespace {

constexpr std::uintmax_t kMessageFileMax = 2048;
constexpr std::uintmax_t kRatingsFileMax = 256 * 1024;
constexpr std::size_t    kRatingsMax = 4000;

bool IsHex(std::string_view text, std::size_t length) {
    return text.size() == length && std::ranges::all_of(text, [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}

std::string RandomHex(std::size_t length) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::random_device    device;
    std::string           out;
    for (std::size_t i = 0; i < length; ++i) out.push_back(kDigits[device() & 15]);
    return out;
}

// A message id is "<author>-<8 hex>"; its file is that id with .json.
bool ValidId(std::string_view id) { return id.size() == 21 && id[12] == '-' && IsHex(id.substr(0, 12), 12) && IsHex(id.substr(13), 8); }

// A regular file directly in folder, read whole when it is no larger than limit.
bool ReadSmall(const fs::path &folder, const fs::path &path, std::uintmax_t limit, std::string &text) {
    std::error_code ec;
    const auto      status = fs::symlink_status(path, ec);
    if (ec || !fs::is_regular_file(status) || ContainsSymlinkBelow(path, folder)) return false;
    const std::uintmax_t size = fs::file_size(path, ec);
    if (ec || size > limit) return false;
    std::ifstream in(path, std::ios::binary);
    if (!in) return false;
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return text.size() <= limit;
}

// Written beside it first, then moved over it, so a sync service never carries half a file.
bool WriteWhole(const fs::path &folder, const fs::path &path, const std::string &text, std::string &error) {
    std::error_code ec;
    fs::create_directories(folder, ec);
    if (ec) {
        error = "the messages folder can't be made: " + ec.message();
        return false;
    }
    const fs::path temporary = fs::path(path).concat(".tmp");
    if (ContainsSymlinkBelow(path, folder) || ContainsSymlinkBelow(temporary, folder)) {
        error = "a link stands in the messages folder";
        return false;
    }
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out || !(out << text) || !out.flush()) {
            error = "the messages folder can't be written";
            return false;
        }
    }
    fs::rename(temporary, path, ec);
    if (ec) {
        fs::remove(temporary, ec);
        error = "the messages folder can't be written";
        return false;
    }
    return true;
}

fs::path RatingsPath(const fs::path &folder, std::string_view author) { return folder / (std::string(author) + ".ratings.json"); }

// One player's ratings file: {v = 1, ratings = {id = 1 | -1}}; anything malformed counts as no ratings.
std::map<std::string, int> ReadRatings(const fs::path &folder, const fs::path &path) {
    std::map<std::string, int> ratings;
    std::string                text;
    if (!ReadSmall(folder, path, kRatingsFileMax, text)) return ratings;
    const Json json = Json::parse(text, nullptr, false);
    if (!json.is_object() || !json.contains("ratings") || !json["ratings"].is_object()) return ratings;
    for (const auto &[id, value] : json["ratings"].items()) {
        if (ratings.size() >= kRatingsMax) break;
        if (!ValidId(id) || !value.is_number_integer()) continue;
        const int rating = value.get<int>();
        if (rating == 1 || rating == -1) ratings[id] = rating;
    }
    return ratings;
}

std::string Utf8(const fs::path &path) {
    const std::u8string text = path.u8string();
    return {text.begin(), text.end()};
}

} // namespace

Folder FolderFor(const fs::path &save_root) {
#ifdef _WIN32
    if (const wchar_t *value = ::_wgetenv(L"DC_MESSAGES_DIR"); value != nullptr && *value != L'\0') return {fs::path(value), true};
#else
    if (const char *value = std::getenv("DC_MESSAGES_DIR"); value != nullptr && *value != '\0') return {fs::path(value), true};
#endif
    const fs::path chosen = save_root / "moonmod" / "messages-folder.txt";
    std::string    text;
    if (ReadSmall(save_root, chosen, 4096, text)) {
        std::string line = text.substr(0, text.find_first_of("\r\n"));
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) line.pop_back();
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) line.erase(line.begin());
        const fs::path path(std::u8string(line.begin(), line.end()));
        if (!line.empty() && path.is_absolute()) return {path, true};
    }
    return {save_root / "moonmod" / "messages", false};
}

std::string Author(const fs::path &save_root, std::string &error) {
    const fs::path folder = save_root / "moonmod";
    const fs::path path = folder / "messages-author.txt";
    std::string    text;
    if (ReadSmall(save_root, path, 64, text)) {
        text = text.substr(0, text.find_first_of("\r\n"));
        if (IsHex(text, 12)) return text;
    }
    const std::string author = RandomHex(12);
    if (!WriteWhole(folder, path, author + "\n", error)) return {};
    return author;
}

bool ValidText(std::string_view text) {
    return !text.empty() && text.size() <= kTextMax && std::ranges::all_of(text, [](char c) { return c >= 0x20 && c <= 0x7E; }) &&
           text.find_first_not_of(' ') != std::string_view::npos;
}

bool ValidPlace(std::string_view place) {
    return !place.empty() && place.size() <= kPlaceMax && std::ranges::all_of(place, [](char c) {
               return (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == ':' || c == '_' || c == '.' || c == '-';
           });
}

bool Post(const fs::path &save_root, std::string_view mod_id, std::string_view place, std::string_view text, std::string &id,
          std::string &error) {
    if (!ValidPlace(place)) {
        error = "place is 1-64 characters of a-z, 0-9 and : _ . -";
        return false;
    }
    if (!ValidText(text)) {
        error = "text is 1-120 characters of printable ASCII";
        return false;
    }
    const std::string author = Author(save_root, error);
    if (author.empty()) return false;
    const Folder folder = FolderFor(save_root);
    int          own = 0;
    std::error_code ec;
    for (fs::directory_iterator it(folder.path, ec), end; !ec && it != end; it.increment(ec)) {
        const std::string name = Utf8(it->path().filename());
        if (name.size() == 26 && name.starts_with(author + "-") && name.ends_with(".json")) ++own;
    }
    if (own >= kOwnMax) {
        error = "this player keeps 300 messages at most: take one back first";
        return false;
    }
    id = author + "-" + RandomHex(8);
    const auto at = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
    const Json json = {{"v", 1}, {"id", id}, {"mod", std::string(mod_id)}, {"place", std::string(place)}, {"text", std::string(text)}, {"at", at}};
    return WriteWhole(folder.path, folder.path / (id + ".json"), json.dump(), error);
}

bool List(const fs::path &save_root, std::string_view mod_id, const std::vector<std::string> &places, std::vector<Message> &out,
          std::string &error) {
    out.clear();
    for (const std::string &place : places) {
        if (!ValidPlace(place)) {
            error = "each place is 1-64 characters of a-z, 0-9 and : _ . -";
            return false;
        }
    }
    const std::string author = Author(save_root, error);
    if (author.empty()) return false;
    const Folder               folder = FolderFor(save_root);
    std::vector<fs::path>      message_files;
    std::vector<fs::path>      rating_files;
    std::error_code            ec;
    int                        scanned = 0;
    for (fs::directory_iterator it(folder.path, ec), end; !ec && it != end && scanned < kScanMax; it.increment(ec), ++scanned) {
        const std::string name = Utf8(it->path().filename());
        if (name.size() == 26 && name.ends_with(".json") && ValidId(std::string_view(name).substr(0, 21))) {
            message_files.push_back(it->path());
        } else if (name.size() == 25 && name.ends_with(".ratings.json") && IsHex(std::string_view(name).substr(0, 12), 12)) {
            rating_files.push_back(it->path());
        }
    }
    for (const fs::path &path : message_files) {
        std::string text;
        if (!ReadSmall(folder.path, path, kMessageFileMax, text)) continue;
        const Json json = Json::parse(text, nullptr, false);
        if (!json.is_object()) continue;
        const auto string_field = [&json](const char *key) -> std::string {
            return json.contains(key) && json[key].is_string() ? json[key].get<std::string>() : std::string();
        };
        Message message;
        message.id = string_field("id");
        message.place = string_field("place");
        message.text = string_field("text");
        if (message.id != Utf8(path.stem()) || string_field("mod") != mod_id || !ValidPlace(message.place) || !ValidText(message.text)) continue;
        if (std::ranges::find(places, message.place) == places.end()) continue;
        if (json.contains("at") && json["at"].is_number_integer()) message.at = json["at"].get<std::int64_t>();
        message.mine = message.id.starts_with(author);
        out.push_back(std::move(message));
    }
    for (const fs::path &path : rating_files) {
        const std::string rater = Utf8(path.filename()).substr(0, 12);
        for (const auto &[id, rating] : ReadRatings(folder.path, path)) {
            const auto found = std::ranges::find(out, id, &Message::id);
            if (found == out.end() || id.starts_with(rater)) continue; // no one rates their own
            (rating > 0 ? found->good : found->poor) += 1;
            if (rater == author) found->rated = rating;
        }
    }
    std::ranges::sort(out, [](const Message &a, const Message &b) {
        const int score_a = a.good - a.poor, score_b = b.good - b.poor;
        return score_a != score_b ? score_a > score_b : a.at > b.at;
    });
    if (out.size() > static_cast<std::size_t>(kListMax)) out.resize(kListMax);
    return true;
}

bool Rate(const fs::path &save_root, std::string_view id, int rating, std::string &error) {
    if (!ValidId(id) || rating < -1 || rating > 1) {
        error = "expected a message id and a rating of 1, -1 or 0";
        return false;
    }
    const std::string author = Author(save_root, error);
    if (author.empty()) return false;
    if (id.starts_with(author)) {
        error = "a player doesn't rate their own message";
        return false;
    }
    const Folder               folder = FolderFor(save_root);
    const fs::path             path = RatingsPath(folder.path, author);
    std::map<std::string, int> ratings = ReadRatings(folder.path, path);
    if (rating == 0) {
        ratings.erase(std::string(id));
    } else {
        if (ratings.size() >= kRatingsMax && !ratings.contains(std::string(id))) {
            error = "this player has rated 4000 messages, the most kept";
            return false;
        }
        ratings[std::string(id)] = rating;
    }
    Json list = Json::object();
    for (const auto &[key, value] : ratings) list[key] = value;
    return WriteWhole(folder.path, path, Json{{"v", 1}, {"ratings", list}}.dump(), error);
}

bool Remove(const fs::path &save_root, std::string_view id, std::string &error) {
    if (!ValidId(id)) {
        error = "expected a message id";
        return false;
    }
    const std::string author = Author(save_root, error);
    if (author.empty()) return false;
    if (!id.starts_with(author)) {
        error = "a player takes back only their own messages";
        return false;
    }
    const Folder    folder = FolderFor(save_root);
    const fs::path  path = folder.path / (std::string(id) + ".json");
    std::error_code ec;
    if (ContainsSymlinkBelow(path, folder.path) || !fs::remove(path, ec) || ec) {
        error = "no such message of this player's";
        return false;
    }
    return true;
}

} // namespace moonmod::messages

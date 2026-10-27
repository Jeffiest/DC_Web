#pragma once

// Reads Dark Cloud's character configurations (.cfg) and cloth files (.clo) into the typed commands
// Kazarov's model converter takes, word for word as the game's own readers do: ReadInfo
// (chararead.cpp), CTextureAnime::LoadCFGFile (textureanime.cpp) and InitCloth (clothread.cpp).
// Header-only, like dcdata.hpp, so the port's tests can use it. It includes no game header.

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <format>
#include <string>
#include <string_view>
#include <vector>

namespace dcdata::profile {

enum class Arg {
    String,
    Integer,
    Float
};

struct CommandSpec {
    std::string_view name;
    std::vector<Arg> args;
};

// ReadInfo's command table, in its order.
inline const std::vector<CommandSpec> &CharacterCommands() {
    using enum Arg;
    static const std::vector<CommandSpec> commands = {
        {"VERTEX_ANIME",        {Integer}                        },
        {"SHADOW_VERTEX_ANIME", {Integer}                        },
        {"MODEL",               {String}                         },
        {"SHADOW_MODEL",        {String}                         },
        {"MOTION",              {Integer, String, String, String}},
        {"SHADOW_MOTION",       {String, String, String}         },
        {"KEY",                 {Integer, Integer, Float}        },
        {"KEY_START",           {Integer}                        },
        {"MOTION_END",          {}                               },
        {"CLOTH",               {String}                         },
        {"BODY_SIZE",           {Float, Float, Float}            },
        {"ALLOC_MDT",           {String}                         },
        {"ALLOC_DBUFF",         {String}                         },
        {"ALLOC_SHADOW_MDT",    {String}                         },
        {"ALLOC_SHADOW_DBUFF",  {String}                         },
        {"IMG",                 {Integer, String}                },
        {"IMG_END",             {}                               },
        {"FOOT",                {Float, Float}                   },
        {"EVENT",               {Float, Integer, Integer}        },
    };
    return commands;
}

// CTextureAnime::LoadCFGFile's tags, read from the same file by the script interpreter.
inline const std::vector<CommandSpec> &TextureAnimeCommands() {
    using enum Arg;
    static const std::vector<CommandSpec> commands = {
        {"TEX_ANIME",       {Integer, Integer}                                                                                    },
        {"TEX_ANIME_DATA",  {String, Integer, Integer, Integer, Integer, String, Integer, Integer, Integer, Integer}              },
        {"TEX_ANIME_DATA2",
         {String, Integer, Integer, Integer, Integer, String, Integer, Integer, Integer, Integer, Integer, Integer,
          Integer, Integer}                                                                                                       },
        {"TEX_SCROLL_DATA", {String, Integer, Integer, Integer, Integer, String, Integer, Integer, Float, Float, Integer, Integer}},
        {"TEX_ANIME_END",   {}                                                                                                    },
    };
    return commands;
}

// InitCloth's command table.
inline const std::vector<CommandSpec> &ClothCommands() {
    using enum Arg;
    static const std::vector<CommandSpec> commands = {
        {"SIZE",       {Integer, Integer}                                                                                 },
        {"FRAME",      {String}                                                                                           },
        {"NORMAL",     {Float}                                                                                            },
        {"FOLLOW",     {Float, Float, Float}                                                                              },
        {"K",          {Float, Float, Float}                                                                              },
        {"WINDEFFECT", {Float}                                                                                            },
        {"GRAVITY",    {Float, Float, Float}                                                                              },
        {"POLYDIV",    {String}                                                                                           },
        {"BOUND",      {String, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float, Float}},
    };
    return commands;
}

inline bool IsSpace(char c) {
    return c == ' ' || c == '\t' || c == '\n' || c == '\r';
}

// Blanks // and /* */ comments as the game's PreProcess does before reading a configuration.
inline std::string PreProcess(std::string_view raw) {
    std::string       text(raw);
    const std::size_t size = text.size();
    auto              at = [&](std::size_t pos) { return pos < size ? text[pos] : '\0'; };
    std::size_t       pos = 0;
    while (pos < size) {
        if (at(pos) == '/' && at(pos + 1) == '/') {
            while (pos < size && text[pos] != '\n' && text[pos] != '\r') {
                text[pos++] = ' ';
            }
        }
        if (at(pos) == '/' && at(pos + 1) == '*') {
            while (pos < size) {
                if (at(pos) == '*' && at(pos + 1) == '/') {
                    text[pos] = ' ';
                    text[pos + 1] = ' ';
                    break;
                }
                text[pos++] = ' ';
            }
            continue;
        }
        pos++;
    }
    return text;
}

// Shift-JIS leaves ASCII bytes as they are only outside double-byte characters, so a comment is
// walked character by character from its start.
inline std::vector<std::size_t> SjisCharacters(std::string_view text) {
    std::vector<std::size_t> starts;
    for (std::size_t i = 0; i < text.size();) {
        starts.push_back(i);
        const auto lead = static_cast<unsigned char>(text[i]);
        i += (lead >= 0x81 && lead <= 0x9F) || (lead >= 0xE0 && lead <= 0xFC) ? 2 : 1;
    }
    return starts;
}

// A space, a tab or an ideographic space.
inline bool IsBlankAt(std::string_view text, std::size_t at) {
    return text[at] == ' ' || text[at] == '\t' || text.substr(at, 2) == "\x81\x40";
}

// An ASCII or a full-width digit.
inline bool IsDigitAt(std::string_view text, std::size_t at) {
    const auto a = static_cast<unsigned char>(text[at]);
    const auto b = at + 1 < text.size() ? static_cast<unsigned char>(text[at + 1]) : 0;
    return (a >= '0' && a <= '9') || (a == 0x82 && b >= 0x4F && b <= 0x58);
}

// The name a KEY line's comment gives its action: the comment's text without the blanks around it
// and the action's index after it, as in `KEY 10, 20, 0.05 //立ち 0`. Empty without a comment, or
// with one that is only the index.
inline std::string KeyComment(std::string_view raw, std::size_t command_pos) {
    const std::size_t      line_end = std::min(raw.find_first_of("\r\n", command_pos), raw.size());
    const std::string_view line = raw.substr(command_pos, line_end - command_pos);
    const std::size_t      slash = line.find("//");
    if (slash == std::string_view::npos) {
        return {};
    }
    const std::string_view         comment = line.substr(slash + 2);
    const std::vector<std::size_t> chars = SjisCharacters(comment);
    std::size_t                    first = 0;
    std::size_t                    end = chars.size();
    while (first < end && IsBlankAt(comment, chars[first])) {
        first++;
    }
    while (end > first && IsBlankAt(comment, chars[end - 1])) {
        end--;
    }
    std::size_t digits = end;
    while (digits > first && IsDigitAt(comment, chars[digits - 1])) {
        digits--;
    }
    // A comment that is only the index names nothing.
    if (digits < end && (digits == first || IsBlankAt(comment, chars[digits - 1]))) {
        end = digits;
        while (end > first && IsBlankAt(comment, chars[end - 1])) {
            end--;
        }
    }
    if (first == end) {
        return {};
    }
    const std::size_t stop = end < chars.size() ? chars[end] : comment.size();
    return std::string(comment.substr(chars[first], stop - chars[first]));
}

// One typed command, as Kazarov's converter reads it. Strings go as their raw bytes, in hex; the
// converter decodes them from Shift-JIS as it does Dark Cloud 2's.
class CommandJson {
public:
    explicit CommandJson(std::string_view name) : out_(std::format(R"({{"command":"{}","args":[)", name)) {}

    void String(std::string_view bytes) {
        constexpr char kHex[] = "0123456789abcdef";
        std::string    hex;
        for (unsigned char c : bytes) {
            hex += kHex[c >> 4];
            hex += kHex[c & 0xF];
        }
        Arg(std::format(R"({{"type":"string","bytes":"{}"}})", hex));
    }

    void Integer(int value) { Arg(std::format(R"({{"type":"int","value":{}}})", value)); }

    void Float(float value) { Arg(std::format(R"({{"type":"float","value":{}}})", value)); }

    void None() { Arg(R"({"type":"none"})"); }

    void Invalid() { Arg(R"({"type":"invalid","value":null})"); }

    std::string Finish() const { return out_ + "]}"; }

private:
    void Arg(std::string_view json) {
        out_ += args_++ ? "," : "";
        out_ += json;
    }

    std::string out_;
    int         args_ = 0;
};

// One command a reader found, at the offset of its keyword, with its typed arguments as JSON.
struct Found {
    std::size_t pos;
    std::string json;
};

// The dialects of the game's two word readers: ReadInfo's and InitCloth's (chararead.cpp,
// clothread.cpp), and the script interpreter's (scriptinterpreter.cpp).
enum class Dialect {
    Character,
    Cloth,
    Interpreter
};

class WordReader {
public:
    WordReader(std::string_view text, Dialect dialect) : text_(text), dialect_(dialect) {}

    bool SkipSpace() {
        while (pos_ < text_.size() && IsSpace(text_[pos_])) {
            pos_++;
        }
        return pos_ < text_.size();
    }

    // The next command word, or false at the end of the file.
    bool Word(std::string &word, std::size_t &start) {
        if (!SkipSpace()) {
            return false;
        }
        start = pos_;
        word.clear();
        while (pos_ < text_.size() && !IsSpace(text_[pos_])) {
            word += text_[pos_++];
        }
        // ReadInfo takes a word cut short by the file's end; the others read nothing.
        if (pos_ == text_.size() && dialect_ != Dialect::Character) {
            return false;
        }
        pos_ = std::min(pos_ + 1, text_.size());
        return true;
    }

    // Reads a command's arguments; false at the end of the file, where the game runs nothing.
    bool Args(const std::vector<Arg> &types, CommandJson &json) {
        if (types.empty()) {
            return true;
        }
        if (!SkipSpace()) {
            return false;
        }
        for (Arg type : types) {
            if (!SkipSpace()) {
                return false;
            }
            std::string token;
            if (dialect_ == Dialect::Interpreter && text_[pos_] == ',') {
                pos_++;
                if (!SkipSpace()) {
                    return false;
                }
            }
            for (;;) {
                if (pos_ >= text_.size()) {
                    return false;
                }
                const char c = text_[pos_++];
                if (c == ',' || IsSpace(c)) {
                    break;
                }
                token += c;
            }
            if (!ReadArg(type, token, json)) {
                json.Invalid();
                return true;
            }
        }
        return true;
    }

private:
    bool ReadArg(Arg type, const std::string &token, CommandJson &json) const {
        const bool interpreter = dialect_ == Dialect::Interpreter;
        switch (type) {
            case Arg::String: {
                if (token.empty() || token[0] != '"') {
                    return false;
                }
                const std::size_t close = token.find('"', 1);
                if (close == std::string::npos) {
                    return false;
                }
                json.String(std::string_view(token).substr(1, close - 1));
                return true;
            }
            case Arg::Integer:
                for (char c : token) {
                    if ((c < '0' || c > '9') && !(interpreter && c == '-')) {
                        return false;
                    }
                }
                json.Integer(std::atoi(token.c_str()));
                return true;
            case Arg::Float:
                for (char c : token) {
                    if ((c < '0' || c > '9') && c != '.' && c != '-') {
                        return false;
                    }
                }
                json.Float(static_cast<float>(std::atof(token.c_str())));
                return true;
        }
        return false;
    }

    std::string_view text_;
    Dialect          dialect_;
    std::size_t      pos_ = 0;
};

// Reads every command of one table from a file, as the game's reader of that table would.
inline std::vector<Found> ReadCommands(std::string_view raw, std::string_view text,
                                       const std::vector<CommandSpec> &table, Dialect dialect) {
    std::vector<Found> found;
    WordReader         reader(text, dialect);
    std::string        word;
    std::size_t        start = 0;
    while (reader.Word(word, start)) {
        const auto spec = std::find_if(table.begin(), table.end(),
                                       [&](const CommandSpec &command) { return command.name == word; });
        if (spec == table.end()) {
            continue;
        }
        CommandJson json(spec->name);
        // KEY carries its comment's name first, as Dark Cloud 2's KEY does: KEY name, start, end, step.
        if (spec->name == "KEY") {
            const std::string name = KeyComment(raw, start);
            if (name.empty()) {
                json.None();
            } else {
                json.String(name);
            }
        }
        if (!reader.Args(spec->args, json)) {
            break;
        }
        found.push_back({start, json.Finish()});
    }
    return found;
}

inline bool EndsWithFolded(std::string_view text, std::string_view suffix) {
    return text.size() >= suffix.size() &&
           std::equal(suffix.begin(), suffix.end(), text.end() - suffix.size(), [](char a, char b) {
               return (a >= 'A' && a <= 'Z' ? a - 'A' + 'a' : a) == b;
           });
}

// The typed commands of a character configuration or cloth file, as JSON, or "null" for a file the
// game doesn't read as one: a .cfg with neither MODEL nor MOTION, or any other kind of file.
inline std::string Read(std::string_view alias, std::string_view data) {
    std::vector<Found> found;
    if (EndsWithFolded(alias, ".clo")) {
        found = ReadCommands(data, data, ClothCommands(), Dialect::Cloth);
    } else if (EndsWithFolded(alias, ".cfg")) {
        const std::string text = PreProcess(data);
        found = ReadCommands(data, text, CharacterCommands(), Dialect::Character);
        const bool character = std::any_of(found.begin(), found.end(), [](const Found &command) {
            return command.json.starts_with(R"({"command":"MODEL")") ||
                   command.json.starts_with(R"({"command":"MOTION")");
        });
        if (!character) {
            return "null";
        }
        std::vector<Found> textures = ReadCommands(data, text, TextureAnimeCommands(), Dialect::Interpreter);
        found.insert(found.end(), textures.begin(), textures.end());
        std::stable_sort(found.begin(), found.end(),
                         [](const Found &a, const Found &b) { return a.pos < b.pos; });
    } else {
        return "null";
    }
    std::string out = R"({"schema_version":1,"commands":[)";
    for (std::size_t i = 0; i < found.size(); i++) {
        out += i ? "," : "";
        out += found[i].json;
    }
    return out + "]}";
}

} // namespace dcdata::profile

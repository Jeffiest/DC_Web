#include "moonmod/title_modes.hpp"

#include <algorithm>
#include <cmath>
#include <initializer_list>
#include <string_view>
#include <utility>

#include "moonmod/runtime.hpp"



namespace moonmod {
namespace {

std::vector<TitleMode> g_title_modes;
std::optional<std::string> g_open_mode;       // qualified id of the mode whose screen is open
std::optional<SandboxRequest> g_sandbox_request;
std::optional<SandboxSession> g_sandbox;
std::string g_sandbox_ending;                 // the first reason noted for the ending session
std::optional<std::string> g_title_mode_return;
int g_ignored_backs = 0;                       // Circle presses on a sub-page the mod did not answer

ApiCallResult Invalid(std::string message) { return {ApiCallStatus::InvalidArgument, nullptr, std::move(message)}; }
ApiCallResult Failed(std::string message) { return {ApiCallStatus::Failed, nullptr, std::move(message)}; }

bool ValidLocalId(std::string_view id) {
    if (id.empty() || id.size() > 64) return false;
    return std::ranges::all_of(id, [](char c) {
        return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
    });
}

// Printable text without control characters, at most max_bytes. The title draws it with the game font.
bool ValidText(const Json &value, std::size_t max_bytes, bool allow_empty) {
    if (!value.is_string()) return false;
    const std::string &text = value.get_ref<const std::string &>();
    if ((!allow_empty && text.empty()) || text.size() > max_bytes) return false;
    return std::ranges::none_of(text, [](char c) { return static_cast<unsigned char>(c) < 0x20 || c == 0x7f; });
}

std::string Qualify(std::string_view owner, std::string_view local) {
    std::string id(owner);
    id.push_back(':');
    id.append(local);
    return id;
}

bool ReadColour(const Json &value, unsigned char &r, unsigned char &g, unsigned char &b) {
    if (!value.is_array() || value.size() != 3) return false;
    for (const Json &c : value) {
        if (!c.is_number_integer() || c.get<std::int64_t>() < 0 || c.get<std::int64_t>() > 255) return false;
    }
    r = static_cast<unsigned char>(value[0].get<int>());
    g = static_cast<unsigned char>(value[1].get<int>());
    b = static_cast<unsigned char>(value[2].get<int>());
    return true;
}

bool ReadCoordinate(const Json &object, const char *key, float &out) {
    if (!object.contains(key) || !object[key].is_number()) return false;
    const double value = object[key].get<double>();
    if (!std::isfinite(value) || value < -8000.0 || value > 8000.0) return false;
    out = static_cast<float>(value);
    return true;
}

bool OnlyFields(const Json &object, std::initializer_list<std::string_view> allowed) {
    for (auto field = object.begin(); field != object.end(); ++field) {
        if (std::ranges::find(allowed, field.key()) == allowed.end()) return false;
    }
    return true;
}

// A sky page: constellations (groups), nodes and captions. The cursor stays on the same node id across updates.
ApiCallResult ReadSky(const Json &request, const std::optional<TitleSky> &previous, std::optional<TitleSky> &out) {
    if (request.is_null()) {
        out.reset();
        return {ApiCallStatus::Ok, nullptr, {}};
    }
    if (!request.is_object() || !OnlyFields(request, {"groups", "nodes", "texts", "top", "cursor"})) {
        return Invalid("sky accepts only groups, nodes, texts, top and cursor");
    }
    TitleSky sky;
    const Json &groups = request.contains("groups") ? request["groups"] : Json::array();
    if (!groups.is_array() || groups.empty() || groups.size() > kTitleSkyGroupLimit) {
        return Invalid("sky groups must be an array of 1-16 constellations");
    }
    for (const Json &g : groups) {
        TitleSkyGroup group;
        if (!g.is_object() || !OnlyFields(g, {"id", "name", "colour", "x", "y"}) || !g.contains("id") ||
            !g["id"].is_string() || !ValidLocalId(g["id"].get_ref<const std::string &>()) ||
            !g.contains("name") || !ValidText(g["name"], kTitleModeLabelBytes, false) ||
            !ReadCoordinate(g, "x", group.x) || !ReadCoordinate(g, "y", group.y) ||
            (g.contains("colour") && !ReadColour(g["colour"], group.r, group.g, group.b))) {
            return Invalid("each sky group needs an id, a name of 1-32 bytes, x and y, and may give a colour {r, g, b}");
        }
        group.id = g["id"].get<std::string>();
        group.name = g["name"].get<std::string>();
        if (std::ranges::find(sky.groups, group.id, &TitleSkyGroup::id) != sky.groups.end()) return Invalid("sky group ids must be unique");
        sky.groups.push_back(std::move(group));
    }
    const Json &nodes = request.contains("nodes") ? request["nodes"] : Json::array();
    if (!nodes.is_array() || nodes.empty() || nodes.size() > kTitleSkyNodeLimit) return Invalid("sky nodes must be an array of 1-200 nodes");
    std::vector<std::vector<std::string>> link_ids;
    for (const Json &n : nodes) {
        TitleSkyNode node;
        if (!n.is_object() ||
            !OnlyFields(n, {"id", "group", "x", "y", "label", "rank", "icon", "title", "tag", "text", "needs", "cost", "state",
                            "on", "keystone", "links"}) ||
            !n.contains("id") || !n["id"].is_string() || !ValidLocalId(n["id"].get_ref<const std::string &>()) ||
            !n.contains("group") || !n["group"].is_string() || !ReadCoordinate(n, "x", node.x) || !ReadCoordinate(n, "y", node.y) ||
            !n.contains("label") || !ValidText(n["label"], kTitleModeLabelBytes, false)) {
            return Invalid("each sky node needs an id, a group, x, y and a label of 1-32 bytes");
        }
        node.id = n["id"].get<std::string>();
        node.label = n["label"].get<std::string>();
        const auto group = std::ranges::find(sky.groups, n["group"].get<std::string>(), &TitleSkyGroup::id);
        if (group == sky.groups.end()) return Invalid("sky node '" + node.id + "' names no group");
        node.group = static_cast<std::size_t>(group - sky.groups.begin());
        struct TextField { const char *key; std::string *into; std::size_t limit; };
        for (const TextField &field : {TextField{"rank", &node.rank, 12}, TextField{"icon", &node.icon, 240},
                                       TextField{"title", &node.title, kTitleModeTextBytes}, TextField{"tag", &node.tag, 32},
                                       TextField{"text", &node.text, 160}, TextField{"needs", &node.needs, 96},
                                       TextField{"cost", &node.cost, 48}}) {
            if (!n.contains(field.key)) continue;
            if (!ValidText(n[field.key], field.limit, true)) {
                return Invalid("sky node '" + node.id + "' field " + field.key + " is too long or not text");
            }
            *field.into = n[field.key].get<std::string>();
        }
        if (n.contains("state")) {
            const Json &state = n["state"];
            if (state == "locked") node.state = 0;
            else if (state == "open") node.state = 1;
            else if (state == "owned") node.state = 2;
            else return Invalid("a sky node's state is locked, open or owned");
        }
        for (const char *flag : {"on", "keystone"}) {
            if (!n.contains(flag)) continue;
            if (!n[flag].is_boolean()) return Invalid(std::string("a sky node's ") + flag + " must be a boolean");
            (std::string_view(flag) == "on" ? node.on : node.keystone) = n[flag].get<bool>();
        }
        std::vector<std::string> links;
        if (n.contains("links")) {
            if (!n["links"].is_array() || n["links"].size() > kTitleSkyLinkLimit) return Invalid("a sky node has at most 4 links");
            for (const Json &link : n["links"]) {
                if (!link.is_string()) return Invalid("sky links are node ids");
                links.push_back(link.get<std::string>());
            }
        }
        if (std::ranges::find(sky.nodes, node.id, &TitleSkyNode::id) != sky.nodes.end()) return Invalid("sky node ids must be unique");
        sky.nodes.push_back(std::move(node));
        link_ids.push_back(std::move(links));
    }
    for (std::size_t i = 0; i < sky.nodes.size(); ++i) {
        for (const std::string &link : link_ids[i]) {
            const auto target = std::ranges::find(sky.nodes, link, &TitleSkyNode::id);
            if (target == sky.nodes.end()) return Invalid("sky node '" + sky.nodes[i].id + "' links to an unknown node");
            sky.nodes[i].links.push_back(static_cast<std::size_t>(target - sky.nodes.begin()));
        }
    }
    if (request.contains("texts")) {
        const Json &texts = request["texts"];
        if (!texts.is_array() || texts.size() > kTitleSkyTextLimit) return Invalid("sky texts must be an array of at most 24 captions");
        for (const Json &t : texts) {
            TitleSkyText text;
            if (!t.is_object() || !OnlyFields(t, {"text", "x", "y", "colour"}) || !t.contains("text") ||
                !ValidText(t["text"], kTitleModeLabelBytes, false) || !ReadCoordinate(t, "x", text.x) ||
                !ReadCoordinate(t, "y", text.y) || (t.contains("colour") && !ReadColour(t["colour"], text.r, text.g, text.b))) {
                return Invalid("each sky text needs text of 1-32 bytes, x and y, and may give a colour");
            }
            text.text = t["text"].get<std::string>();
            sky.texts.push_back(std::move(text));
        }
    }
    if (request.contains("top")) {
        const Json &top = request["top"];
        if (!top.is_array() || top.size() > 3) return Invalid("sky top must be at most three lines");
        for (const Json &line : top) {
            if (!ValidText(line, kTitleModeTextBytes, true)) return Invalid("each sky top line must be at most 64 bytes of text");
            sky.top.push_back(line.get<std::string>());
        }
    }
    // The cursor: the node the mod names, else the node it was on, else the first.
    std::string cursor_id;
    if (request.contains("cursor")) {
        if (!request["cursor"].is_string()) return Invalid("sky cursor is a node id");
        cursor_id = request["cursor"].get<std::string>();
    } else if (previous && previous->cursor < previous->nodes.size()) {
        cursor_id = previous->nodes[previous->cursor].id;
    }
    const auto cursor = std::ranges::find(sky.nodes, cursor_id, &TitleSkyNode::id);
    sky.cursor = cursor == sky.nodes.end() ? 0 : static_cast<std::size_t>(cursor - sky.nodes.begin());
    sky.map = previous && previous->map;
    out = std::move(sky);
    return {ApiCallStatus::Ok, nullptr, {}};
}

// Reads the page fields shared by mode_register and mode_set into mode. Fields left out keep their value.
ApiCallResult ReadPage(const Json &request, TitleMode &mode) {
    for (auto field = request.begin(); field != request.end(); ++field) {
        const std::string &key = field.key();
        if (key != "id" && key != "label" && key != "title" && key != "subtitle" && key != "hint" &&
            key != "lines" && key != "entries" && key != "cursor" && key != "root" && key != "sky") {
            return Invalid("unsupported title mode field '" + key + "'");
        }
    }
    if (request.contains("label")) {
        if (!ValidText(request["label"], kTitleModeLabelBytes, false)) return Invalid("label must be 1-32 bytes of text");
        mode.label = request["label"].get<std::string>();
    }
    if (request.contains("title")) {
        if (!ValidText(request["title"], kTitleModeTitleBytes, false)) return Invalid("title must be 1-40 bytes of text");
        mode.title = request["title"].get<std::string>();
    }
    for (const char *key : {"subtitle", "hint"}) {
        if (!request.contains(key)) continue;
        if (!ValidText(request[key], kTitleModeTextBytes, true)) return Invalid(std::string(key) + " must be at most 64 bytes of text");
        (std::string_view(key) == "subtitle" ? mode.subtitle : mode.hint) = request[key].get<std::string>();
    }
    if (request.contains("lines")) {
        const Json &lines = request["lines"];
        if (!lines.is_array() || lines.size() > kTitleModeLineLimit) return Invalid("lines must be an array of at most 6 strings");
        std::vector<std::string> out;
        for (const Json &line : lines) {
            if (!ValidText(line, kTitleModeTextBytes, true)) return Invalid("each line must be at most 64 bytes of text");
            out.push_back(line.get<std::string>());
        }
        mode.lines = std::move(out);
    }
    if (request.contains("entries")) {
        const Json &entries = request["entries"];
        if (!entries.is_array() || entries.size() > kTitleModeEntryLimit) return Invalid("entries must be an array of at most 7 entries");
        std::vector<TitleModeEntry> out;
        for (const Json &entry : entries) {
            if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string() ||
                !ValidLocalId(entry["id"].get_ref<const std::string &>()) || entry["id"] == "back") {
                return Invalid("each entry needs an id of 1-64 letters, digits, '.', '_' or '-' (not 'back')");
            }
            for (auto field = entry.begin(); field != entry.end(); ++field) {
                if (field.key() != "id" && field.key() != "label" && field.key() != "enabled") {
                    return Invalid("entries accept only id, label and enabled");
                }
            }
            if (!entry.contains("label") || !ValidText(entry["label"], kTitleModeTextBytes, false)) {
                return Invalid("each entry needs a label of 1-64 bytes of text");
            }
            if (entry.contains("enabled") && !entry["enabled"].is_boolean()) return Invalid("entry enabled must be a boolean");
            const std::string id = entry["id"].get<std::string>();
            if (std::ranges::find(out, id, &TitleModeEntry::id) != out.end()) return Invalid("entry ids must be unique");
            out.push_back({id, entry["label"].get<std::string>(), entry.value("enabled", true)});
        }
        mode.entries = std::move(out);
        if (!request.contains("cursor")) mode.cursor = 0;
    }
    if (request.contains("cursor")) {
        if (!request["cursor"].is_number_integer() || request["cursor"].get<std::int64_t>() < 0) {
            return Invalid("cursor must be a 0-based entry index");
        }
        mode.cursor = static_cast<std::size_t>(request["cursor"].get<std::int64_t>());
    }
    if (mode.cursor >= mode.entries.size()) mode.cursor = mode.entries.empty() ? 0 : mode.entries.size() - 1;
    if (request.contains("root")) {
        if (!request["root"].is_boolean()) return Invalid("root must be a boolean");
        mode.root = request["root"].get<bool>();
    }
    if (request.contains("sky")) {
        std::optional<TitleSky> sky;
        if (ApiCallResult read = ReadSky(request["sky"], mode.sky, sky); read.status != ApiCallStatus::Ok) return read;
        mode.sky = std::move(sky);
    } else if (request.contains("entries")) {
        mode.sky.reset(); // a list page in place of the sky
    }
    return {ApiCallStatus::Ok, nullptr, {}};
}

TitleMode *FindMutable(std::string_view id, std::string_view owner) {
    for (TitleMode &mode : g_title_modes) {
        if (mode.id == id || (!owner.empty() && mode.owner == owner && mode.local_id == id)) return &mode;
    }
    return nullptr;
}

const Json *ReadRequest(const Json &args) {
    if (!args.is_array() || args.size() != 1 || !args[0].is_object()) return nullptr;
    return &args[0];
}

} // namespace

ApiCallResult RegisterTitleMode(std::string_view owner, const Json &args) {
    const Json *request = ReadRequest(args);
    if (request == nullptr || !request->contains("id") || !(*request)["id"].is_string()) {
        return Invalid("expected {id, label, title, subtitle?, hint?, lines?, entries?, cursor?, root?}");
    }
    const std::string local_id = (*request)["id"].get<std::string>();
    if (!ValidLocalId(local_id)) return Invalid("id must be 1-64 letters, digits, '.', '_' or '-'");
    if (!request->contains("label") || !request->contains("title")) return Invalid("a title mode needs a label and a title");
    const std::string id = Qualify(owner, local_id);
    if (TitleMode *existing = FindMutable(id, {})) {
        TitleMode updated = *existing;
        if (ApiCallResult read = ReadPage(*request, updated); read.status != ApiCallStatus::Ok) return read;
        *existing = std::move(updated);
        return {ApiCallStatus::Ok, TitleModeJson(*existing), {}};
    }
    if (g_title_modes.size() >= kTitleModeLimit) return Failed("the title menu has room for 2 mod title modes");
    TitleMode mode;
    mode.owner = std::string(owner);
    mode.id = id;
    mode.local_id = local_id;
    if (ApiCallResult read = ReadPage(*request, mode); read.status != ApiCallStatus::Ok) return read;
    g_title_modes.push_back(std::move(mode));
    return {ApiCallStatus::Ok, TitleModeJson(g_title_modes.back()), {}};
}

ApiCallResult SetTitleMode(std::string_view owner, const Json &args) {
    const Json *request = ReadRequest(args);
    if (request == nullptr || !request->contains("id") || !(*request)["id"].is_string()) {
        return Invalid("expected {id, title?, subtitle?, hint?, lines?, entries?, cursor?, root?, label?}");
    }
    TitleMode *mode = FindMutable((*request)["id"].get_ref<const std::string &>(), owner);
    if (mode == nullptr || mode->owner != owner) return Failed("no title mode with that id belongs to this mod");
    TitleMode updated = *mode;
    if (ApiCallResult read = ReadPage(*request, updated); read.status != ApiCallStatus::Ok) return read;
    *mode = std::move(updated);
    g_ignored_backs = 0;
    return {ApiCallStatus::Ok, TitleModeJson(*mode), {}};
}

ApiCallResult RemoveTitleMode(std::string_view owner, const Json &args) {
    const Json *request = ReadRequest(args);
    if (request == nullptr || !request->contains("id") || !(*request)["id"].is_string()) return Invalid("expected {id}");
    const TitleMode *mode = FindTitleMode((*request)["id"].get_ref<const std::string &>(), owner);
    if (mode == nullptr || mode->owner != owner) return Failed("no title mode with that id belongs to this mod");
    const std::string id = mode->id;
    if (g_open_mode == id) g_open_mode.reset();
    std::erase_if(g_title_modes, [&id](const TitleMode &m) { return m.id == id; });
    return {ApiCallStatus::Ok, {{"id", id}, {"removed", true}}, {}};
}

void RemoveTitleModesByOwner(std::string_view owner) {
    if (g_open_mode) {
        const TitleMode *open = FindTitleMode(*g_open_mode);
        if (open != nullptr && open->owner == owner) g_open_mode.reset();
    }
    std::erase_if(g_title_modes, [owner](const TitleMode &m) { return m.owner == owner; });
    if (g_sandbox_request && g_sandbox_request->owner == owner) g_sandbox_request.reset();
}

const std::vector<TitleMode> &TitleModes() { return g_title_modes; }

const TitleMode *FindTitleMode(std::string_view id, std::string_view owner) { return FindMutable(id, owner); }

Json TitleModeJson(const TitleMode &mode) {
    Json entries = Json::array();
    for (const TitleModeEntry &entry : mode.entries) {
        entries.push_back({{"id", entry.id}, {"label", entry.label}, {"enabled", entry.enabled}});
    }
    return {{"id", mode.id}, {"local_id", mode.local_id}, {"owner", mode.owner}, {"label", mode.label},
            {"title", mode.title}, {"subtitle", mode.subtitle}, {"hint", mode.hint}, {"lines", mode.lines},
            {"entries", std::move(entries)}, {"cursor", mode.cursor}, {"root", mode.root},
            {"open", g_open_mode == mode.id},
            {"sky", mode.sky ? Json{{"nodes", mode.sky->nodes.size()},
                                    {"cursor", mode.sky->nodes.empty() ? Json(nullptr) : Json(mode.sky->nodes[mode.sky->cursor].id)}}
                             : Json(nullptr)}};
}

std::size_t TitleSkyStep(const TitleSky &sky, std::size_t from, int dx, int dy) {
    if (from >= sky.nodes.size()) return from;
    const TitleSkyNode &here = sky.nodes[from];
    // A node joined to this one by a link (either way) is the likeliest aim, then one in the same constellation;
    // the cone widens to about 75 degrees for those and stays near 60 for the rest.
    const auto linked = [&](std::size_t i) {
        const auto &links = sky.nodes[i].links;
        return std::find(here.links.begin(), here.links.end(), i) != here.links.end() ||
               std::find(links.begin(), links.end(), from) != links.end();
    };
    std::size_t best = from;
    float       best_score = 0.0f;
    for (std::size_t i = 0; i < sky.nodes.size(); ++i) {
        if (i == from) continue;
        const float vx = sky.nodes[i].x - here.x;
        const float vy = sky.nodes[i].y - here.y;
        const float along = vx * static_cast<float>(dx) + vy * static_cast<float>(dy);
        if (along <= 0.5f) continue;
        const float across = std::fabs(vx * static_cast<float>(dy) - vy * static_cast<float>(dx));
        const bool  joined = linked(i), same = sky.nodes[i].group == here.group;
        if (across > along * (joined ? 3.7f : 1.7f)) continue;
        const float score = (along + across * 2.0f) * (joined ? 0.45f : (same ? 0.8f : 1.0f));
        if (best == from || score < best_score) {
            best = i;
            best_score = score;
        }
    }
    return best;
}

std::size_t TitleSkyGroupEntry(const TitleSky &sky, std::size_t group) {
    // The constellation's first node owned or open, else its first (its root comes first in the mod's list).
    std::size_t first = sky.nodes.size();
    for (std::size_t i = 0; i < sky.nodes.size(); ++i) {
        if (sky.nodes[i].group != group) continue;
        if (first == sky.nodes.size()) first = i;
        if (sky.nodes[i].state == 1) return i;
    }
    return first;
}

namespace {
std::size_t StepGroup(const TitleSky &sky, std::size_t from, int step) {
    if (sky.groups.empty() || from >= sky.nodes.size()) return from;
    const std::size_t count = sky.groups.size();
    std::size_t       group = sky.nodes[from].group;
    for (std::size_t tries = 0; tries < count; ++tries) {
        group = step > 0 ? (group + 1) % count : (group + count - 1) % count;
        const std::size_t entry = TitleSkyGroupEntry(sky, group);
        if (entry < sky.nodes.size()) return entry;
    }
    return from;
}
} // namespace

bool PointTitleSky(std::size_t node) {
    TitleMode *mode = g_open_mode ? FindMutable(*g_open_mode, {}) : nullptr;
    if (mode == nullptr || !mode->sky || node >= mode->sky->nodes.size() || mode->sky->cursor == node) return false;
    mode->sky->cursor = node;
    return true;
}

bool OpenTitleModeScreen(std::size_t index) {
    if (index >= g_title_modes.size()) return false;
    g_open_mode = g_title_modes[index].id;
    g_ignored_backs = 0;
    // Copies: the callback may register, change or remove modes, which can move g_title_modes.
    const std::string owner = g_title_modes[index].owner;
    const Json payload = {{"mode", g_title_modes[index].local_id}, {"qualified_id", g_title_modes[index].id},
                          {"frame", SharedRuntime().Frame()}};
    SharedRuntime().PublishToMod(owner, "game.title.opened", payload);
    return true;
}

const TitleMode *OpenTitleModeScreenMode() {
    return g_open_mode ? FindTitleMode(*g_open_mode) : nullptr;
}

void CloseTitleModeScreen() {
    g_open_mode.reset();
    g_sandbox_request.reset();
    g_ignored_backs = 0;
}

TitleModeOutcome HandleTitleModeInput(TitleModeInput input) {
    // A session the mod asked for outside an input (from game.tick or game.title.opened) starts first.
    if (g_sandbox_request) return TitleModeOutcome::StartSession;
    TitleMode *mode = g_open_mode ? FindMutable(*g_open_mode, {}) : nullptr;
    if (mode == nullptr) {
        CloseTitleModeScreen();
        return TitleModeOutcome::ReturnToTitle;
    }
    if (mode->sky) {
        TitleSky &sky = *mode->sky;
        const auto send = [&](std::string_view action) {
            const std::string owner = mode->owner;
            const Json payload = {{"mode", mode->local_id}, {"qualified_id", mode->id},
                                  {"entry", sky.nodes.empty() ? std::string() : sky.nodes[sky.cursor].id},
                                  {"action", action}, {"frame", SharedRuntime().Frame()}};
            SharedRuntime().PublishToMod(owner, "game.title.selected", payload);
        };
        switch (input) {
            case TitleModeInput::Up: sky.cursor = TitleSkyStep(sky, sky.cursor, 0, -1); return TitleModeOutcome::Stay;
            case TitleModeInput::Down: sky.cursor = TitleSkyStep(sky, sky.cursor, 0, 1); return TitleModeOutcome::Stay;
            case TitleModeInput::Left: sky.cursor = TitleSkyStep(sky, sky.cursor, -1, 0); return TitleModeOutcome::Stay;
            case TitleModeInput::Right: sky.cursor = TitleSkyStep(sky, sky.cursor, 1, 0); return TitleModeOutcome::Stay;
            case TitleModeInput::Map: sky.map = !sky.map; return TitleModeOutcome::Stay;
            case TitleModeInput::L2: sky.cursor = StepGroup(sky, sky.cursor, -1); return TitleModeOutcome::Stay;
            case TitleModeInput::R2: sky.cursor = StepGroup(sky, sky.cursor, 1); return TitleModeOutcome::Stay;
            case TitleModeInput::Confirm: send("buy"); break;
            case TitleModeInput::Square: send("toggle"); break;
            case TitleModeInput::L1: send("rank_down"); break;
            case TitleModeInput::R1: send("rank_up"); break;
            default: break;
        }
        if (input != TitleModeInput::Back) {
            if (g_sandbox_request) return TitleModeOutcome::StartSession;
            if (!g_open_mode || FindTitleMode(*g_open_mode) == nullptr) {
                CloseTitleModeScreen();
                return TitleModeOutcome::ReturnToTitle;
            }
            return TitleModeOutcome::Stay;
        }
    }
    switch (input) {
        case TitleModeInput::None:
        case TitleModeInput::Left:
        case TitleModeInput::Right:
        case TitleModeInput::Square:
        case TitleModeInput::L1:
        case TitleModeInput::R1:
        case TitleModeInput::Map:
        case TitleModeInput::L2:
        case TitleModeInput::R2:
            break;
        case TitleModeInput::Up:
            if (!mode->entries.empty()) mode->cursor = (mode->cursor + mode->entries.size() - 1) % mode->entries.size();
            break;
        case TitleModeInput::Down:
            if (!mode->entries.empty()) mode->cursor = (mode->cursor + 1) % mode->entries.size();
            break;
        case TitleModeInput::Confirm: {
            if (mode->entries.empty() || !mode->entries[mode->cursor].enabled) break;
            const std::string owner = mode->owner;
            const Json payload = {{"mode", mode->local_id}, {"qualified_id", mode->id},
                                  {"entry", mode->entries[mode->cursor].id}, {"index", mode->cursor},
                                  {"frame", SharedRuntime().Frame()}};
            SharedRuntime().PublishToMod(owner, "game.title.selected", payload);
            break;
        }
        case TitleModeInput::Back: {
            if (mode->root) {
                const std::string owner = mode->owner;
                const Json payload = {{"mode", mode->local_id}, {"qualified_id", mode->id}, {"entry", "back"},
                                      {"closed", true}, {"frame", SharedRuntime().Frame()}};
                CloseTitleModeScreen();
                SharedRuntime().PublishToMod(owner, "game.title.selected", payload);
                g_sandbox_request.reset(); // the screen is gone: nothing may start from it
                return TitleModeOutcome::ReturnToTitle;
            }
            // A sub-page: the mod answers Circle. If it ignores two in a row (a broken handler), Circle
            // leaves anyway so the player is never trapped on the screen.
            if (++g_ignored_backs > 2) {
                CloseTitleModeScreen();
                return TitleModeOutcome::ReturnToTitle;
            }
            const std::string owner = mode->owner;
            const Json payload = {{"mode", mode->local_id}, {"qualified_id", mode->id}, {"entry", "back"},
                                  {"closed", false}, {"frame", SharedRuntime().Frame()}};
            SharedRuntime().PublishToMod(owner, "game.title.selected", payload);
            break;
        }
    }
    if (g_sandbox_request) return TitleModeOutcome::StartSession;
    // The mod may have removed its own mode from inside the callback.
    if (!g_open_mode || FindTitleMode(*g_open_mode) == nullptr) {
        CloseTitleModeScreen();
        return TitleModeOutcome::ReturnToTitle;
    }
    return TitleModeOutcome::Stay;
}

void QueueSandboxRequest(SandboxRequest request) { g_sandbox_request = std::move(request); }
bool SandboxRequestPending() { return g_sandbox_request.has_value(); }

std::optional<SandboxRequest> TakeSandboxRequest() {
    std::optional<SandboxRequest> request = std::move(g_sandbox_request);
    g_sandbox_request.reset();
    return request;
}

void BeginSandboxSession(const SandboxRequest &request) {
    g_sandbox = SandboxSession{request.owner, request.mode_id, static_cast<int>(SharedRuntime().Frame())};
    g_sandbox_ending.clear();
    g_open_mode.reset();
    SharedRuntime().Publish("game.session.started",
                               {{"kind", "sandbox"}, {"owner", request.owner}, {"mode", request.mode_id},
                                {"floor", request.floor_id}, {"party_size", request.party_size},
                                {"frame", SharedRuntime().Frame()}});
}

bool SandboxActive() { return g_sandbox.has_value(); }
const SandboxSession *ActiveSandbox() { return g_sandbox ? &*g_sandbox : nullptr; }

void NoteSandboxEnding(std::string_view reason) {
    if (g_sandbox && g_sandbox_ending.empty()) g_sandbox_ending = std::string(reason);
}

std::string EndSandboxSession(std::string_view fallback_reason, bool reopen_mode) {
    if (!g_sandbox) return {};
    const std::string reason = g_sandbox_ending.empty() ? std::string(fallback_reason) : g_sandbox_ending;
    const SandboxSession ended = *g_sandbox;
    g_sandbox.reset();
    g_sandbox_ending.clear();
    if (reopen_mode && FindTitleMode(ended.mode_id) != nullptr) g_title_mode_return = ended.mode_id;
    // Published now, so mods hear the end before the title reopens the mode screen (game.title.opened).
    SharedRuntime().Publish("game.session.ended",
                               {{"kind", "sandbox"}, {"owner", ended.owner}, {"mode", ended.mode_id},
                                {"reason", reason}, {"frame", SharedRuntime().Frame()}});
    return reason;
}

std::optional<std::string> TakeTitleModeReturn() {
    std::optional<std::string> id = std::move(g_title_mode_return);
    g_title_mode_return.reset();
    return id;
}

Json SessionJson(bool in_title) {
    if (g_sandbox) {
        return {{"kind", "sandbox"}, {"owner", g_sandbox->owner}, {"mode", g_sandbox->mode_id},
                {"ending", g_sandbox_ending.empty() ? Json(nullptr) : Json(g_sandbox_ending)}};
    }
    return {{"kind", in_title ? "title" : "story"}, {"owner", nullptr}, {"mode", nullptr},
            {"open_mode", g_open_mode ? Json(*g_open_mode) : Json(nullptr)}};
}

void ResetTitleModesForTests() {
    g_ignored_backs = 0;
    g_title_modes.clear();
    g_open_mode.reset();
    g_sandbox_request.reset();
    g_sandbox.reset();
    g_sandbox_ending.clear();
    g_title_mode_return.reset();
}

} // namespace moonmod

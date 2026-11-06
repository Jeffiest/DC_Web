// Online play's session (moonmod/net_session.hpp): the relay's protocol over whatever socket the platform gives.

#include "moonmod/net_session.hpp"

#include <algorithm>
#include <cstdlib>

#include <nlohmann/json.hpp>

namespace moonmod::net {
namespace {

using Clock = std::chrono::steady_clock;
using Json = nlohmann::json;

// A socket with nothing on it is closed by proxies after a minute or two: a frame to oneself, which the relay drops,
// keeps it open.
constexpr auto kKeepAlive = std::chrono::seconds(25);
constexpr auto kConnectTimeout = std::chrono::seconds(20);

#ifndef DC_COOP_RELAY_DEFAULT
#define DC_COOP_RELAY_DEFAULT ""
#endif

} // namespace

std::string_view StateName(State state) {
    switch (state) {
        case State::Connecting: return "connecting";
        case State::Hosting: return "hosting";
        case State::Joined: return "joined";
        default: return "idle";
    }
}

bool ValidUtf8(std::string_view text) {
    for (std::size_t i = 0; i < text.size();) {
        const auto c = static_cast<unsigned char>(text[i]);
        int        more = 0;
        if (c < 0x80) more = 0;
        else if (c >= 0xC2 && c <= 0xDF) more = 1;
        else if ((c & 0xF0) == 0xE0) more = 2;
        else if (c >= 0xF0 && c <= 0xF4) more = 3;
        else return false;
        if (text.size() - i <= static_cast<std::size_t>(more)) return false;
        for (int k = 1; k <= more; ++k) {
            if ((static_cast<unsigned char>(text[i + k]) & 0xC0) != 0x80) return false;
        }
        i += static_cast<std::size_t>(more) + 1;
    }
    return true;
}

std::string Escape(std::string_view text) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    for (const char ch : text) {
        const auto c = static_cast<unsigned char>(ch);
        if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') {
            out.push_back(ch);
        } else {
            out.push_back('%');
            out.push_back(kHex[c >> 4]);
            out.push_back(kHex[c & 15]);
        }
    }
    return out;
}

bool ValidRelay(const std::string &url) {
    if (url.size() > 200 || url.find_first_of(" ?#\\\"'<>") != std::string::npos) return false;
    // wss:// anywhere; plain ws:// too, for a private server on a home network or a VPN (README.md in coop/relay).
    // Browsers refuse ws:// from an https page, so web players need wss://.
    if (url.starts_with("wss://")) return url.size() > 6;
    return url.starts_with("ws://") && url.size() > 5;
}

std::string RelayAddress(const std::string &asked) {
    std::string url = asked;
    if (url.empty()) {
        if (const char *env = std::getenv("DC_COOP_RELAY"); env != nullptr) url = env;
    }
    if (url.empty()) url = DC_COOP_RELAY_DEFAULT;
    while (url.ends_with('/')) url.pop_back();
    return url;
}

bool Session::Start(std::string_view owner, const std::string &relay, const std::string &code, const std::string &name,
                    const std::string &game, int version, std::string &error) {
    if (!factory_) {
        error = "this build of the game has no online play";
        return false;
    }
    if (state_ != State::Idle) {
        error = "a session is already open; leave it first";
        return false;
    }
    if (!ValidRelay(relay)) {
        error = relay.empty() ? "no relay address: this build has none, so pass one" : "the relay address must be wss://... or ws://...";
        return false;
    }
    std::string url = relay + (code.empty() ? std::string("/v1/host") : "/v1/join/" + code) + "?game=" + Escape(game) +
                      "&proto=" + std::to_string(version) + "&name=" + Escape(name);
    transport_ = factory_(url, error);
    if (!transport_) {
        if (error.empty()) error = "the relay can't be reached";
        return false;
    }
    owner_ = std::string(owner);
    relay_ = relay;
    code_ = code;
    id_ = -1;
    peers_.clear();
    events_.clear();
    state_ = State::Connecting;
    last_sent_ = Clock::now();
    return true;
}

void Session::End(const std::string &why) {
    if (transport_) {
        transport_->Close();
        transport_.reset();
    }
    if (state_ != State::Idle) Push({"closed", -1, why, {}});
    state_ = State::Idle;
    id_ = -1;
    peers_.clear();
}

void Session::Leave(std::string_view why) {
    End(std::string(why));
    // The mod that left asked for it: nothing waits to be read, and the session is anyone's again.
    events_.clear();
    owner_.clear();
    code_.clear();
}

void Session::Push(Event event) {
    if (events_.size() >= kQueueMax) {
        const auto message = std::ranges::find(events_, std::string("message"), &Event::kind);
        events_.erase(message != events_.end() ? message : events_.begin());
    }
    events_.push_back(std::move(event));
}

void Session::Control(const std::string &text) {
    const Json message = Json::parse(text, nullptr, false);
    if (!message.is_object() || !message.contains("t") || !message["t"].is_string()) return;
    const std::string t = message["t"].get<std::string>();
    const auto        number = [&message](const char *key) {
        return message.contains(key) && message[key].is_number_integer() ? message[key].get<int>() : -1;
    };
    const auto text_of = [&message](const char *key) {
        return message.contains(key) && message[key].is_string() ? message[key].get<std::string>() : std::string();
    };
    if (t == "welcome") {
        id_ = number("id");
        code_ = text_of("code");
        peers_.clear();
        if (message.contains("peers") && message["peers"].is_array()) {
            for (const Json &peer : message["peers"]) {
                if (peer.is_object() && peer.contains("id") && peer["id"].is_number_integer()) {
                    peers_.push_back({peer["id"].get<int>(), peer.value("name", std::string())});
                }
            }
        }
        state_ = id_ == 0 ? State::Hosting : State::Joined;
        Push({"connected", id_, code_, {}});
    } else if (t == "join") {
        const int id = number("id");
        std::erase_if(peers_, [id](const Peer &peer) { return peer.id == id; });
        peers_.push_back({id, text_of("name")});
        Push({"joined", id, {}, text_of("name")});
    } else if (t == "leave") {
        const int id = number("id");
        std::erase_if(peers_, [id](const Peer &peer) { return peer.id == id; });
        Push({"left", id, {}, {}});
    } else if (t == "error" || t == "closed") {
        const std::string reason = text_of("reason");
        End(reason.empty() ? std::string("the relay closed the room") : reason);
    }
}

void Session::Pump() {
    if (!transport_) return;
    Frame frame;
    while (transport_ && transport_->Poll(frame)) {
        switch (frame.kind) {
            case Frame::Kind::Open: break;
            case Frame::Kind::Text: Control(frame.data); break;
            case Frame::Kind::Binary:
                if (frame.data.size() >= 1 && frame.data.size() - 1 <= kMessageMax) {
                    std::string data = frame.data.substr(1);
                    if (ValidUtf8(data)) Push({"message", static_cast<unsigned char>(frame.data[0]), std::move(data), {}});
                }
                break;
            case Frame::Kind::Closed:
                End(frame.data.empty() ? std::string("the connection to the relay closed") : frame.data);
                return;
        }
    }
    if (!transport_) return;
    const auto now = Clock::now();
    if (state_ == State::Connecting && now - last_sent_ > kConnectTimeout) {
        End("the relay didn't answer");
    } else if (state_ != State::Connecting && id_ >= 0 && now - last_sent_ > kKeepAlive) {
        transport_->Send(std::string(1, static_cast<char>(id_)));
        last_sent_ = now;
    }
}

bool Session::Send(int to, std::string_view data, std::string &error) {
    Pump();
    if (state_ != State::Hosting && state_ != State::Joined) {
        error = state_ == State::Connecting ? "still connecting" : "not in a session";
        return false;
    }
    if (data.size() > kMessageMax || !ValidUtf8(data)) {
        error = "data must be text (UTF-8) of at most 16 KiB";
        return false;
    }
    if (to != kBroadcast && (to < 0 || to >= kPlayersMax || to == id_)) {
        error = "to must be another player's id, or all";
        return false;
    }
    std::string frame(1, static_cast<char>(to));
    frame.append(data);
    transport_->Send(frame);
    last_sent_ = Clock::now();
    return true;
}

std::vector<Event> Session::Receive(std::size_t max) {
    Pump();
    std::vector<Event> out;
    while (!events_.empty() && out.size() < max) {
        out.push_back(std::move(events_.front()));
        events_.pop_front();
    }
    if (state_ == State::Idle && events_.empty()) owner_.clear(); // a closed session read to its end is anyone's again
    return out;
}

#if !defined(__EMSCRIPTEN__) && !defined(_WIN32) && !defined(DC_HAVE_CURL)
// No socket on this build (no libcurl found): net.* says so.
TransportFactory PlatformTransport() { return {}; }
#endif

Session &Shared() {
    static Session session(PlatformTransport());
    return session;
}

} // namespace moonmod::net

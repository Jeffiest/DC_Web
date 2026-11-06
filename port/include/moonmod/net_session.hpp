#pragma once

// Online play (net.*): one session at a time, through the co-op relay (coop/relay/README.md). A player hosts a room or
// joins one by its five-letter code; the relay passes messages between the players' games and keeps no game state, so
// the host's game is the world and a mod decides what the messages mean. The web build talks to the relay with the
// browser's WebSocket, Windows with WinHTTP, Linux and macOS with a WebSocket of our own over libcurl's TLS connection
// (net_transport_*.cpp); a build without any of them reports net.* unsupported.

#include <chrono>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace moonmod::net {

inline constexpr std::size_t kMessageMax = 16 * 1024; // bytes of one message's data
inline constexpr std::size_t kQueueMax = 4096;        // events waiting for the mod; past it the oldest messages go
inline constexpr int         kPlayersMax = 4;
inline constexpr int         kBroadcast = 255;

// What a transport hands up: the socket opened, a text or binary frame came, or it closed (data: why).
struct Frame {
    enum class Kind { Open, Text, Binary, Closed } kind = Kind::Binary;
    std::string data;
};

// One WebSocket to the relay. Frames come in on any thread and wait in the transport until Poll takes them, on the
// game's thread; Send may be called from the game's thread while frames arrive.
class Transport {
public:
    virtual ~Transport() = default;
    virtual void Send(std::string_view bytes) = 0; // one binary frame
    virtual bool Poll(Frame &frame) = 0;
    virtual void Close() = 0;
};

// Opens a socket to url (wss://, or ws:// to this machine for testing), or says why it can't.
using TransportFactory = std::function<std::unique_ptr<Transport>(const std::string &url, std::string &error)>;
// The platform's: nullptr where the build has no network support.
TransportFactory PlatformTransport();

struct Peer {
    int         id = -1;
    std::string name;
};

// What net.receive hands a mod: connected (peer = its own id, data = the room code), joined, left (peer), message
// (peer = sender, data), closed (data = why the session ended).
struct Event {
    std::string kind;
    int         peer = -1;
    std::string data;
    std::string name;
};

enum class State { Idle, Connecting, Hosting, Joined };
std::string_view StateName(State state);

class Session {
public:
    explicit Session(TransportFactory factory) : factory_(std::move(factory)) {}

    // Opens a room (code empty) or joins one. game and version must match the room's: a room is one game at one
    // protocol version. False, with the reason, when the session can't start.
    bool Start(std::string_view owner, const std::string &relay, const std::string &code, const std::string &name,
               const std::string &game, int version, std::string &error);
    void Leave(std::string_view why = "left");
    // Takes what the transport received and keeps the socket alive; Receive and Send do it too.
    void Pump();
    // to: a peer id, or kBroadcast for everyone else. False with the reason when it can't go.
    bool Send(int to, std::string_view data, std::string &error);
    std::vector<Event> Receive(std::size_t max);

    State                    state() const { return state_; }
    const std::string       &owner() const { return owner_; }
    int                      id() const { return id_; }
    const std::string       &code() const { return code_; }
    const std::vector<Peer> &peers() const { return peers_; }
    const std::string       &relay() const { return relay_; }
    bool                     available() const { return static_cast<bool>(factory_); }

private:
    void Control(const std::string &text);
    void Push(Event event);
    void End(const std::string &why);

    TransportFactory                         factory_;
    std::unique_ptr<Transport>               transport_;
    State                                    state_ = State::Idle;
    std::string                              owner_;
    std::string                              relay_;
    std::string                              code_;
    int                                      id_ = -1;
    std::vector<Peer>                        peers_;
    std::deque<Event>                        events_;
    std::chrono::steady_clock::time_point    last_sent_{};
};

// The relay address for a call: the mod's, else DC_COOP_RELAY, else the build's (DC_COOP_RELAY_DEFAULT).
std::string RelayAddress(const std::string &asked);
bool        ValidRelay(const std::string &url);
bool        ValidUtf8(std::string_view text);
// Percent-encodes a query value.
std::string Escape(std::string_view text);

// The one session of this process, made with PlatformTransport the first time it's asked for.
Session &Shared();

} // namespace moonmod::net

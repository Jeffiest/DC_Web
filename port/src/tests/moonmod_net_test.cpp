// Online play's session (moonmod/net_session.hpp) over a scripted transport: the relay's protocol, the event queue
// and the checks on what a mod sends. The real sockets are tested against a relay by coop/relay/test.
#include <gtest/gtest.h>

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include "moonmod/net_session.hpp"

namespace {

using namespace moonmod::net;

struct Wire {
    std::string              url;
    std::deque<Frame>        incoming;
    std::vector<std::string> sent;
    bool                     closed = false;
};

class FakeTransport final : public Transport {
public:
    explicit FakeTransport(std::shared_ptr<Wire> wire) : wire_(std::move(wire)) {}
    void Send(std::string_view bytes) override { wire_->sent.emplace_back(bytes); }
    bool Poll(Frame &frame) override {
        if (wire_->incoming.empty()) return false;
        frame = wire_->incoming.front();
        wire_->incoming.pop_front();
        return true;
    }
    void Close() override { wire_->closed = true; }

private:
    std::shared_ptr<Wire> wire_;
};

TransportFactory Fake(std::shared_ptr<Wire> wire) {
    return [wire](const std::string &url, std::string &) -> std::unique_ptr<Transport> {
        wire->url = url;
        return std::make_unique<FakeTransport>(wire);
    };
}

std::vector<std::string> Kinds(const std::vector<Event> &events) {
    std::vector<std::string> out;
    for (const Event &e : events) out.push_back(e.kind);
    return out;
}

TEST(MoonModNet, HostingAsksTheRelayForARoomAndTakesItsWelcome) {
    auto        wire = std::make_shared<Wire>();
    Session     session(Fake(wire));
    std::string error;
    ASSERT_TRUE(session.Start("coop", "wss://relay.example", "", "Toan & co", "chronicle", 3, error)) << error;
    EXPECT_EQ(wire->url, "wss://relay.example/v1/host?game=chronicle&proto=3&name=Toan%20%26%20co");
    EXPECT_EQ(session.state(), State::Connecting);
    wire->incoming.push_back({Frame::Kind::Open, {}});
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":"welcome","id":0,"code":"AB3CD","host":0,"peers":[]})"});
    const auto events = session.Receive(10);
    ASSERT_EQ(Kinds(events), (std::vector<std::string>{"connected"}));
    EXPECT_EQ(events[0].peer, 0);
    EXPECT_EQ(events[0].data, "AB3CD");
    EXPECT_EQ(session.state(), State::Hosting);
    EXPECT_EQ(session.code(), "AB3CD");
    EXPECT_EQ(session.owner(), "coop");
}

TEST(MoonModNet, AGuestSeesPlayersComeAndGoAndTheirMessages) {
    auto        wire = std::make_shared<Wire>();
    Session     session(Fake(wire));
    std::string error;
    ASSERT_TRUE(session.Start("coop", "wss://relay.example", "AB3CD", "Xiao", "chronicle", 1, error));
    EXPECT_EQ(wire->url, "wss://relay.example/v1/join/AB3CD?game=chronicle&proto=1&name=Xiao");
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":"welcome","id":2,"code":"AB3CD","host":0,"peers":[{"id":0,"name":"Toan"},{"id":1,"name":"Goro"}]})"});
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":"join","id":3,"name":"Ruby"})"});
    wire->incoming.push_back({Frame::Kind::Binary, std::string("\x00", 1) + "P 1 2 3"});
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":"leave","id":1})"});
    wire->incoming.push_back({Frame::Kind::Binary, std::string("\x03", 1) + "\xFF not text"});
    const auto events = session.Receive(10);
    ASSERT_EQ(Kinds(events), (std::vector<std::string>{"connected", "joined", "message", "left"}));
    EXPECT_EQ(session.state(), State::Joined);
    EXPECT_EQ(session.id(), 2);
    EXPECT_EQ(events[1].name, "Ruby");
    EXPECT_EQ(events[2].peer, 0);
    EXPECT_EQ(events[2].data, "P 1 2 3");
    ASSERT_EQ(session.peers().size(), 2u); // Toan and Ruby; Goro left
    EXPECT_EQ(session.peers()[1].name, "Ruby");
}

TEST(MoonModNet, SendingFramesTheDataForTheRelayAndChecksIt) {
    auto        wire = std::make_shared<Wire>();
    Session     session(Fake(wire));
    std::string error;
    EXPECT_FALSE(session.Send(0, "early", error));
    ASSERT_TRUE(session.Start("coop", "wss://relay.example", "AB3CD", "", "chronicle", 1, error));
    EXPECT_FALSE(session.Send(0, "before the welcome", error));
    EXPECT_EQ(error, "still connecting");
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":"welcome","id":1,"code":"AB3CD","peers":[{"id":0,"name":"Toan"}]})"});
    session.Pump();
    ASSERT_TRUE(session.Send(0, "to the host", error)) << error;
    ASSERT_TRUE(session.Send(kBroadcast, "to all", error)) << error;
    ASSERT_EQ(wire->sent.size(), 2u);
    EXPECT_EQ(wire->sent[0], std::string("\x00", 1) + "to the host");
    EXPECT_EQ(wire->sent[1], "\xFF" "to all");
    EXPECT_FALSE(session.Send(1, "to myself", error));
    EXPECT_FALSE(session.Send(7, "nobody", error));
    EXPECT_FALSE(session.Send(0, std::string("\xC3\x28", 2), error));
    EXPECT_FALSE(session.Send(0, std::string(kMessageMax + 1, 'x'), error));
    EXPECT_TRUE(session.Send(0, std::string(kMessageMax, 'x'), error));
}

TEST(MoonModNet, ARefusalOrAClosedSocketEndsTheSessionWithItsReason) {
    auto        wire = std::make_shared<Wire>();
    Session     session(Fake(wire));
    std::string error;
    ASSERT_TRUE(session.Start("coop", "wss://relay.example", "ZZZZZ", "", "chronicle", 1, error));
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":"error","reason":"no room with that code"})"});
    auto events = session.Receive(10);
    ASSERT_EQ(Kinds(events), (std::vector<std::string>{"closed"}));
    EXPECT_EQ(events[0].data, "no room with that code");
    EXPECT_TRUE(wire->closed);
    EXPECT_EQ(session.state(), State::Idle);
    EXPECT_TRUE(session.owner().empty()); // read to its end, the session is anyone's again

    ASSERT_TRUE(session.Start("other", "wss://relay.example", "", "", "chronicle", 1, error));
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":"welcome","id":0,"code":"QQQQQ","peers":[]})"});
    wire->incoming.push_back({Frame::Kind::Closed, ""});
    events = session.Receive(10);
    ASSERT_EQ(Kinds(events), (std::vector<std::string>{"connected", "closed"}));
    EXPECT_EQ(events[1].data, "the connection to the relay closed");
}

TEST(MoonModNet, OneSessionAtATimeAndOnlyToARelay) {
    auto        wire = std::make_shared<Wire>();
    Session     session(Fake(wire));
    std::string error;
    EXPECT_FALSE(session.Start("coop", "https://relay.example", "", "", "chronicle", 1, error));
    EXPECT_FALSE(session.Start("coop", "ftp://relay.example", "", "", "chronicle", 1, error));
    EXPECT_FALSE(session.Start("coop", "", "", "", "chronicle", 1, error));
    ASSERT_TRUE(session.Start("coop", "ws://127.0.0.1:8787", "", "", "chronicle", 1, error)) << error;
    EXPECT_FALSE(session.Start("coop", "wss://relay.example", "", "", "chronicle", 1, error));
    session.Leave();
    EXPECT_EQ(session.state(), State::Idle);
    EXPECT_TRUE(session.Receive(10).empty());
    EXPECT_TRUE(session.Start("coop", "wss://relay.example", "", "", "chronicle", 1, error));
}

TEST(MoonModNet, ABuildWithoutASocketSaysSo) {
    Session     session(TransportFactory{});
    std::string error;
    EXPECT_FALSE(session.available());
    EXPECT_FALSE(session.Start("coop", "wss://relay.example", "", "", "chronicle", 1, error));
    EXPECT_EQ(error, "this build of the game has no online play");
}

TEST(MoonModNet, AFullQueueDropsTheOldestMessagesFirst) {
    auto        wire = std::make_shared<Wire>();
    Session     session(Fake(wire));
    std::string error;
    ASSERT_TRUE(session.Start("coop", "wss://relay.example", "", "", "chronicle", 1, error));
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":"welcome","id":0,"code":"AB3CD","peers":[]})"});
    for (std::size_t i = 0; i < kQueueMax + 10; ++i) wire->incoming.push_back({Frame::Kind::Binary, "\x01" + std::to_string(i)});
    session.Pump();
    std::vector<Event> events;
    for (;;) { // one answer holds only so much: the rest waits for the next
        const auto more = session.Receive(kQueueMax + 100);
        if (more.empty()) break;
        events.insert(events.end(), more.begin(), more.end());
    }
    ASSERT_EQ(events.size(), kQueueMax);
    EXPECT_EQ(events[0].kind, "connected"); // kept: only messages are dropped
    EXPECT_EQ(events[1].data, "11"); // the connected event takes a place too: eleven of the oldest messages went
    EXPECT_EQ(events.back().data, std::to_string(kQueueMax + 9));
}

TEST(MoonModNet, OnlyWellFormedUtf8PassesAndCutsKeepWholeCharacters) {
    EXPECT_FALSE(ValidUtf8("\xE0\x80\xAF"));     // a slash written the long way
    EXPECT_FALSE(ValidUtf8("\xED\xA0\x80"));     // half of a pair, not a character
    EXPECT_FALSE(ValidUtf8("\xF0\x80\x80\xAF")); // long again
    EXPECT_FALSE(ValidUtf8("\xF4\x90\x80\x80")); // past the last character
    EXPECT_TRUE(ValidUtf8("\xED\x9F\xBF \xEE\x80\x80 \xF4\x8F\xBF\xBF"));
    EXPECT_EQ(CutUtf8("abc", 8), "abc");
    EXPECT_EQ(CutUtf8("ab\xC3\xA9", 3), "ab"); // never half a character
    EXPECT_EQ(CutUtf8("ab\xC3\xA9", 4), "ab\xC3\xA9");
    EXPECT_TRUE(ValidUtf8(CutUtf8("\xF0\x9F\x8E\xAE\xF0\x9F\x8E\xAE", 6)));
    EXPECT_TRUE(ValidUtf8(CutUtf8("\xFF\xFE not text", 8))); // what isn't text comes out as text, or empty
}

TEST(MoonModNet, ARelaySendingNonsenseNeitherThrowsNorFillsTheGame) {
    auto        wire = std::make_shared<Wire>();
    Session     session(Fake(wire));
    std::string error;
    ASSERT_TRUE(session.Start("coop", "wss://relay.example", "AB3CD", "Xiao", "chronicle", 1, error));
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":"welcome","id":2,"code":"AB3CD","host":0,"peers":[{"id":0,"name":7},{"id":"x"},5,{"id":1,"name":"A name far longer than the relay ever lets one be"}]})"});
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":"join","id":3,"name":{"not":"a name"}})"});
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":"join","id":-4,"name":"Nobody"})"});
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":"join","id":"3"})"});
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":7})"});
    wire->incoming.push_back({Frame::Kind::Text, R"([1,2,3])"});
    wire->incoming.push_back({Frame::Kind::Text, "{not json"});
    wire->incoming.push_back({Frame::Kind::Binary, {}});
    std::vector<Event> events;
    EXPECT_NO_THROW(events = session.Receive(64));
    EXPECT_EQ(session.state(), State::Joined);
    for (const Peer &peer : session.peers()) {
        EXPECT_GE(peer.id, 0);
        EXPECT_LE(peer.name.size(), kNameMax);
        EXPECT_TRUE(ValidUtf8(peer.name));
    }
    for (const Event &e : events) EXPECT_TRUE(ValidUtf8(e.name));
    for (int i = 0; i < 500; ++i) wire->incoming.push_back({Frame::Kind::Text, R"({"t":"join","id":)" + std::to_string(10 + i) + "}"});
    EXPECT_NO_THROW(session.Receive(1000));
    EXPECT_LE(session.peers().size(), kPeersMax);
}

TEST(MoonModNet, OneAnswerNeverCarriesMoreThanAModCanTake) {
    auto        wire = std::make_shared<Wire>();
    Session     session(Fake(wire));
    std::string error;
    ASSERT_TRUE(session.Start("coop", "wss://relay.example", "", "", "chronicle", 1, error));
    wire->incoming.push_back({Frame::Kind::Text, R"({"t":"welcome","id":0,"code":"AB3CD","peers":[]})"});
    const std::string big(kMessageMax - 1, 'x');
    for (int i = 0; i < 20; ++i) wire->incoming.push_back({Frame::Kind::Binary, "\x01" + big});
    std::size_t messages = 0;
    for (int round = 0; round < 40 && messages < 20; ++round) {
        const auto  events = session.Receive(64);
        std::size_t bytes = 0;
        for (const Event &e : events) {
            bytes += e.data.size();
            if (e.kind == "message") ++messages;
        }
        EXPECT_LE(bytes, kReceiveBytes + kMessageMax);
        ASSERT_FALSE(events.empty()); // each answer makes headway
    }
    EXPECT_EQ(messages, 20u); // and none is lost for being held back
}

TEST(MoonModNet, ASocketsWaitingFramesAreBoundedAndKeepTheRelaysWord) {
    FrameQueue queue;
    queue.Push({Frame::Kind::Text, "welcome"});
    for (std::size_t i = 0; i < kTransportFrames + 50; ++i) queue.Push({Frame::Kind::Binary, std::to_string(i)});
    queue.Push({Frame::Kind::Closed, "bye"});
    EXPECT_LE(queue.size(), kTransportFrames + 2);
    Frame frame;
    ASSERT_TRUE(queue.Pop(frame));
    EXPECT_EQ(frame.kind, Frame::Kind::Text); // the relay's own word is never dropped
    ASSERT_TRUE(queue.Pop(frame));
    EXPECT_EQ(frame.kind, Frame::Kind::Binary);
    EXPECT_NE(frame.data, "0"); // the oldest messages went
    Frame last;
    while (queue.Pop(frame)) last = frame;
    EXPECT_EQ(last.kind, Frame::Kind::Closed);

    FrameQueue heavy;
    const std::string big(1024 * 1024, 'x');
    for (int i = 0; i < 20; ++i) heavy.Push({Frame::Kind::Binary, big});
    std::size_t bytes = 0;
    while (heavy.Pop(frame)) bytes += frame.data.size();
    EXPECT_LE(bytes, kTransportBytes);
    EXPECT_GT(bytes, 0u);
}

TEST(MoonModNet, Utf8AndEscaping) {
    EXPECT_TRUE(ValidUtf8("plain"));
    EXPECT_TRUE(ValidUtf8("\xE2\x9C\xA8 \xF0\x9F\x8E\xAE"));
    EXPECT_FALSE(ValidUtf8("\xE2\x9C"));
    EXPECT_FALSE(ValidUtf8("\xC0\xAF"));
    EXPECT_FALSE(ValidUtf8("\xF5\x80\x80\x80"));
    EXPECT_EQ(Escape("a b/c?\xC3\xA9"), "a%20b%2Fc%3F%C3%A9");
    EXPECT_TRUE(ValidRelay("wss://chronicle-coop-relay.example.workers.dev"));
    EXPECT_TRUE(ValidRelay("ws://192.168.1.20:7777")); // a private server at home
    EXPECT_FALSE(ValidRelay("ws://"));
    EXPECT_FALSE(ValidRelay("wss://relay.example/?inject"));
    EXPECT_EQ(RelayAddress("wss://relay.example//"), "wss://relay.example");
}

} // namespace

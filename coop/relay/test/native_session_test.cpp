// The PC transport (port/src/moonmod/net_transport_curl.cpp) and session against a running relay: a host and two
// guests, their messages, and the host leaving. Built and run by test/native.sh; RELAY_URL picks the relay.
#include "moonmod/net_session.hpp"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <thread>

using namespace moonmod::net;

static int passed = 0, failed = 0;
static void check(bool ok, const char *what) {
    std::printf("  %s %s\n", ok ? "ok  " : "FAIL", what);
    (ok ? passed : failed)++;
}

// Pumps until an event of that kind comes (or about five seconds go by).
static bool await(Session &s, const std::string &kind, Event *out = nullptr) {
    for (int i = 0; i < 250; ++i) {
        for (Event &e : s.Receive(64)) {
            if (e.kind == kind) {
                if (out) *out = e;
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(20));
    }
    return false;
}

int main() {
    const char *env = std::getenv("RELAY_URL");
    const std::string relay = env ? env : "ws://127.0.0.1:8787";
    Session host(PlatformTransport()), guest(PlatformTransport()), other(PlatformTransport()), stale(PlatformTransport());
    std::string error;
    check(host.available(), "this build has a socket");
    check(host.Start("coop", relay, "", "Toan", "chronicle", 1, error), "hosting starts");
    Event e;
    check(await(host, "connected", &e) && e.peer == 0 && e.data.size() == 5, "the host gets id 0 and a code");
    check(host.state() == State::Hosting, "and is hosting");
    const std::string code = host.code();
    check(guest.Start("coop", relay, code, "Xiao", "chronicle", 1, error), "a guest joins with the code");
    check(await(guest, "connected", &e) && e.peer == 1 && guest.peers().size() == 1 && guest.peers()[0].name == "Toan",
          "the guest is 1 and sees the host");
    check(await(host, "joined", &e) && e.peer == 1 && e.name == "Xiao", "the host hears it");
    check(host.Send(1, "hello Xiao \xE2\x9C\xA8", error), "the host sends to the guest");
    check(await(guest, "message", &e) && e.peer == 0 && e.data == "hello Xiao \xE2\x9C\xA8", "the guest gets it (UTF-8 intact)");
    std::string big(9000, 'x');
    check(guest.Send(0, big, error) && await(host, "message", &e) && e.data == big, "a 9000-byte message crosses (extended length)");
    check(!guest.Send(0, std::string("\xFF\xFE", 2), error), "data that isn't UTF-8 is refused");
    check(!guest.Send(1, "me", error), "a player can't send to itself");
    check(other.Start("coop", relay, code, "Goro", "chronicle", 2, error) && await(other, "closed", &e) &&
              e.data.find("version") != std::string::npos,
          "another version is turned away, with the reason");
    check(other.state() == State::Idle, "and that session is idle again");
    check(stale.Start("coop", relay, "ZZZZZ", "Ruby", "chronicle", 1, error) && await(stale, "closed", &e) &&
              e.data.find("no room") != std::string::npos,
          "an unknown code is turned away");
    host.Leave();
    check(await(guest, "closed", &e) && e.data.find("host") != std::string::npos, "the host leaving ends the guest's session");
    check(guest.state() == State::Idle, "the guest is idle");
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed > 0 ? 1 : 0;
}

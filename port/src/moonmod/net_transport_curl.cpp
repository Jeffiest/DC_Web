// Online play's socket on Linux and macOS (moonmod/net_session.hpp): libcurl makes the TLS connection (CONNECT_ONLY),
// and the WebSocket on top of it is ours, the handshake and RFC 6455 framing, so any libcurl does, not only one built
// with its own WebSocket support. One thread per socket does all the I/O; the game's thread only queues and takes.

#if defined(DC_HAVE_CURL) && !defined(__EMSCRIPTEN__) && !defined(_WIN32)

#include "moonmod/net_session.hpp"

#include <atomic>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <random>
#include <thread>

#include <curl/curl.h>

namespace moonmod::net {
namespace {

std::string Base64(const unsigned char *data, std::size_t size) {
    static constexpr char kTable[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    for (std::size_t i = 0; i < size; i += 3) {
        const unsigned v = (data[i] << 16) | ((i + 1 < size ? data[i + 1] : 0) << 8) | (i + 2 < size ? data[i + 2] : 0);
        out.push_back(kTable[(v >> 18) & 63]);
        out.push_back(kTable[(v >> 12) & 63]);
        out.push_back(i + 1 < size ? kTable[(v >> 6) & 63] : '=');
        out.push_back(i + 2 < size ? kTable[v & 63] : '=');
    }
    return out;
}

class CurlTransport final : public Transport {
public:
    CurlTransport(std::string origin, std::string target, std::string host) :
        origin_(std::move(origin)), target_(std::move(target)), host_(std::move(host)), thread_([this] { Run(); }) {}

    ~CurlTransport() override {
        Close();
        if (thread_.joinable()) thread_.join();
    }

    void Send(std::string_view bytes) override {
        const std::lock_guard lock(mutex_);
        outgoing_.emplace_back(Encode(0x2, bytes));
    }

    bool Poll(Frame &frame) override {
        const std::lock_guard lock(mutex_);
        if (incoming_.empty()) return false;
        frame = std::move(incoming_.front());
        incoming_.pop_front();
        return true;
    }

    void Close() override { stop_ = true; }

private:
    void Deliver(Frame::Kind kind, std::string data) {
        const std::lock_guard lock(mutex_);
        incoming_.push_back({kind, std::move(data)});
    }

    std::string Encode(int opcode, std::string_view payload) {
        std::string frame;
        frame.push_back(static_cast<char>(0x80 | opcode));
        const std::size_t size = payload.size();
        if (size < 126) {
            frame.push_back(static_cast<char>(0x80 | size));
        } else if (size < 65536) {
            frame.push_back(static_cast<char>(0x80 | 126));
            frame.push_back(static_cast<char>(size >> 8));
            frame.push_back(static_cast<char>(size & 255));
        } else {
            frame.push_back(static_cast<char>(0x80 | 127));
            for (int shift = 56; shift >= 0; shift -= 8) frame.push_back(static_cast<char>((static_cast<std::uint64_t>(size) >> shift) & 255));
        }
        unsigned char mask[4];
        for (unsigned char &m : mask) m = static_cast<unsigned char>(random_());
        frame.append(reinterpret_cast<const char *>(mask), 4);
        for (std::size_t i = 0; i < size; ++i) frame.push_back(static_cast<char>(payload[i] ^ mask[i % 4]));
        return frame;
    }

    // Waits until the socket can be read (or written), at most ms.
    bool Wait(bool write, int ms) {
        pollfd fd{static_cast<int>(socket_), static_cast<short>(write ? POLLOUT : POLLIN), 0};
        return poll(&fd, 1, ms) > 0;
    }

    bool SendAll(std::string_view bytes) {
        std::size_t done = 0;
        while (done < bytes.size()) {
            std::size_t sent = 0;
            const CURLcode result = curl_easy_send(curl_, bytes.data() + done, bytes.size() - done, &sent);
            if (result == CURLE_AGAIN) {
                Wait(true, 100);
                continue;
            }
            if (result != CURLE_OK) return false;
            done += sent;
        }
        return true;
    }

    // Reads what has come, into buffer_. False when the connection is gone.
    bool ReadSome() {
        char           chunk[16384];
        std::size_t    got = 0;
        const CURLcode result = curl_easy_recv(curl_, chunk, sizeof(chunk), &got);
        if (result == CURLE_AGAIN) return true;
        if (result != CURLE_OK || got == 0) return false;
        buffer_.append(chunk, got);
        return true;
    }

    bool Handshake() {
        unsigned char key[16];
        for (unsigned char &k : key) k = static_cast<unsigned char>(random_());
        const std::string request = "GET " + target_ + " HTTP/1.1\r\nHost: " + host_ +
                                    "\r\nUpgrade: websocket\r\nConnection: Upgrade\r\nSec-WebSocket-Version: 13\r\nSec-WebSocket-Key: " +
                                    Base64(key, sizeof(key)) + "\r\nUser-Agent: chronicle-coop\r\n\r\n";
        if (!SendAll(request)) return false;
        for (int waited = 0; waited < 150; ++waited) {
            if (const std::size_t end = buffer_.find("\r\n\r\n"); end != std::string::npos) {
                const std::string head = buffer_.substr(0, end);
                buffer_.erase(0, end + 4);
                if (head.rfind("HTTP/1.1 101", 0) == 0) return true;
                const std::size_t line = head.find("\r\n");
                failure_ = "the relay refused: " + head.substr(0, line);
                return false;
            }
            Wait(false, 100);
            if (!ReadSome()) return false;
        }
        failure_ = "the relay didn't answer";
        return false;
    }

    // Takes whole frames off buffer_. False on a close frame.
    bool Frames() {
        for (;;) {
            if (buffer_.size() < 2) return true;
            const auto     b0 = static_cast<unsigned char>(buffer_[0]);
            const auto     b1 = static_cast<unsigned char>(buffer_[1]);
            std::uint64_t  size = b1 & 0x7F;
            std::size_t    at = 2;
            if (size == 126) {
                if (buffer_.size() < 4) return true;
                size = (static_cast<unsigned char>(buffer_[2]) << 8) | static_cast<unsigned char>(buffer_[3]);
                at = 4;
            } else if (size == 127) {
                if (buffer_.size() < 10) return true;
                size = 0;
                for (int i = 0; i < 8; ++i) size = (size << 8) | static_cast<unsigned char>(buffer_[2 + i]);
                at = 10;
            }
            const bool masked = (b1 & 0x80) != 0;
            unsigned char mask[4] = {};
            if (masked) {
                if (buffer_.size() < at + 4) return true;
                std::memcpy(mask, buffer_.data() + at, 4);
                at += 4;
            }
            if (size > 1024 * 1024) return false;
            if (buffer_.size() < at + size) return true;
            std::string payload = buffer_.substr(at, static_cast<std::size_t>(size));
            buffer_.erase(0, at + static_cast<std::size_t>(size));
            if (masked) for (std::size_t i = 0; i < payload.size(); ++i) payload[i] = static_cast<char>(payload[i] ^ mask[i % 4]);
            const int  opcode = b0 & 0x0F;
            const bool fin = (b0 & 0x80) != 0;
            if (opcode == 0x8) {
                if (payload.size() > 2) failure_ = payload.substr(2);
                return false;
            }
            if (opcode == 0x9) {
                SendAll(Encode(0xA, payload));
                continue;
            }
            if (opcode == 0xA) continue;
            if (opcode == 0x1 || opcode == 0x2) {
                message_kind_ = opcode;
                message_ = std::move(payload);
            } else if (opcode == 0x0) {
                message_.append(payload);
            } else {
                continue;
            }
            if (fin) Deliver(message_kind_ == 0x1 ? Frame::Kind::Text : Frame::Kind::Binary, std::move(message_));
        }
    }

    void Run() {
        curl_ = curl_easy_init();
        if (curl_ == nullptr) {
            Deliver(Frame::Kind::Closed, "the network library didn't start");
            return;
        }
        curl_easy_setopt(curl_, CURLOPT_URL, origin_.c_str());
        curl_easy_setopt(curl_, CURLOPT_CONNECT_ONLY, 1L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT, 15L);
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        CURLcode result = curl_easy_perform(curl_);
        if (result == CURLE_OK) result = curl_easy_getinfo(curl_, CURLINFO_ACTIVESOCKET, &socket_);
        if (result != CURLE_OK) {
            Deliver(Frame::Kind::Closed, std::string("the relay can't be reached: ") + curl_easy_strerror(result));
        } else if (!Handshake()) {
            Deliver(Frame::Kind::Closed, failure_.empty() ? std::string("the relay closed the connection") : failure_);
        } else {
            Deliver(Frame::Kind::Open, {});
            bool alive = Frames();
            while (alive && !stop_) {
                std::deque<std::string> sending;
                {
                    const std::lock_guard lock(mutex_);
                    sending.swap(outgoing_);
                }
                for (const std::string &frame : sending) {
                    if (!SendAll(frame)) alive = false;
                }
                if (!alive) break;
                if (Wait(false, 15)) alive = ReadSome() && Frames();
            }
            if (stop_) {
                SendAll(Encode(0x8, "\x03\xE8"));
            } else {
                Deliver(Frame::Kind::Closed, failure_.empty() ? std::string("the connection to the relay closed") : failure_);
            }
        }
        curl_easy_cleanup(curl_);
        curl_ = nullptr;
    }

    std::string             origin_, target_, host_;
    std::mutex              mutex_;
    std::deque<Frame>       incoming_;
    std::deque<std::string> outgoing_;
    std::atomic<bool>       stop_{false};
    std::mt19937            random_{std::random_device{}()};
    CURL                   *curl_ = nullptr;
    curl_socket_t           socket_ = CURL_SOCKET_BAD;
    std::string             buffer_, message_, failure_;
    int                     message_kind_ = 0x2;
    std::thread             thread_; // last: it starts Run, which uses everything above
};

} // namespace

TransportFactory PlatformTransport() {
    static const bool ready = curl_global_init(CURL_GLOBAL_DEFAULT) == CURLE_OK;
    if (!ready) return {};
    return [](const std::string &url, std::string &error) -> std::unique_ptr<Transport> {
        const bool        secure = url.starts_with("wss://");
        const std::size_t start = secure ? 6 : 5;
        const std::size_t slash = url.find('/', start);
        const std::string authority = url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        const std::string target = slash == std::string::npos ? std::string("/") : url.substr(slash);
        if (authority.empty()) {
            error = "the relay address has no host";
            return nullptr;
        }
        return std::make_unique<CurlTransport>((secure ? "https://" : "http://") + authority, target, authority);
    };
}

} // namespace moonmod::net

#endif

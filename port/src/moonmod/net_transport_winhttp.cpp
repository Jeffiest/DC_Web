// Online play's socket on Windows (moonmod/net_session.hpp): WinHTTP's own WebSocket (Windows 8 and later), TLS and
// proxies as the system has them. One thread connects and receives; sends come from the game's thread under a lock
// (WinHTTP allows one send and one receive at a time on a socket).

#if defined(_WIN32) && !defined(__EMSCRIPTEN__)

#include "moonmod/net_session.hpp"

#include <atomic>
#include <mutex>
#include <thread>

#include <windows.h>
#include <winhttp.h>

namespace moonmod::net {
namespace {

std::wstring Wide(const std::string &text) {
    if (text.empty()) return {};
    const int    size = MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), nullptr, 0);
    std::wstring out(static_cast<std::size_t>(size), L'\0');
    MultiByteToWideChar(CP_UTF8, 0, text.data(), static_cast<int>(text.size()), out.data(), size);
    return out;
}

class WinHttpTransport final : public Transport {
public:
    WinHttpTransport(std::wstring host, INTERNET_PORT port, std::wstring path, bool secure) :
        host_(std::move(host)), port_(port), path_(std::move(path)), secure_(secure), thread_([this] { Run(); }) {}

    ~WinHttpTransport() override {
        Close();
        if (thread_.joinable()) thread_.join();
        if (session_ != nullptr) WinHttpCloseHandle(session_);
    }

    void Send(std::string_view bytes) override {
        const std::lock_guard lock(send_mutex_);
        if (socket_ != nullptr && !stop_) {
            WinHttpWebSocketSend(socket_, WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE, const_cast<char *>(bytes.data()),
                                 static_cast<DWORD>(bytes.size()));
        }
    }

    bool Poll(Frame &frame) override {
        const std::lock_guard lock(mutex_);
        if (incoming_.empty()) return false;
        frame = std::move(incoming_.front());
        incoming_.pop_front();
        return true;
    }

    void Close() override {
        if (stop_.exchange(true)) return;
        const std::lock_guard lock(send_mutex_);
        // Closing the handle ends the receive the thread waits in.
        if (socket_ != nullptr) {
            WinHttpWebSocketClose(socket_, WINHTTP_WEB_SOCKET_SUCCESS_CLOSE_STATUS, nullptr, 0);
            WinHttpCloseHandle(socket_);
            socket_ = nullptr;
        }
    }

private:
    void Deliver(Frame::Kind kind, std::string data) {
        const std::lock_guard lock(mutex_);
        incoming_.push_back({kind, std::move(data)});
    }

    void Fail(const char *what) {
        if (!stop_) Deliver(Frame::Kind::Closed, std::string(what) + " (error " + std::to_string(GetLastError()) + ")");
    }

    void Run() {
        session_ = WinHttpOpen(L"chronicle-coop", WINHTTP_ACCESS_TYPE_AUTOMATIC_PROXY, WINHTTP_NO_PROXY_NAME,
                               WINHTTP_NO_PROXY_BYPASS, 0);
        if (session_ == nullptr) {
            session_ = WinHttpOpen(L"chronicle-coop", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY, WINHTTP_NO_PROXY_NAME,
                                   WINHTTP_NO_PROXY_BYPASS, 0);
        }
        if (session_ == nullptr) return Fail("the network couldn't start");
        WinHttpSetTimeouts(session_, 15000, 15000, 15000, 0); // no timeout on receiving: a quiet room is fine
        HINTERNET connection = WinHttpConnect(session_, host_.c_str(), port_, 0);
        if (connection == nullptr) return Fail("the relay can't be reached");
        HINTERNET request = WinHttpOpenRequest(connection, L"GET", path_.c_str(), nullptr, WINHTTP_NO_REFERER,
                                               WINHTTP_DEFAULT_ACCEPT_TYPES, secure_ ? WINHTTP_FLAG_SECURE : 0);
        bool ok = request != nullptr && WinHttpSetOption(request, WINHTTP_OPTION_UPGRADE_TO_WEB_SOCKET, nullptr, 0) &&
                  WinHttpSendRequest(request, WINHTTP_NO_ADDITIONAL_HEADERS, 0, WINHTTP_NO_REQUEST_DATA, 0, 0, 0) &&
                  WinHttpReceiveResponse(request, nullptr);
        DWORD status = 0, size = sizeof(status);
        if (ok) {
            WinHttpQueryHeaders(request, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER, WINHTTP_HEADER_NAME_BY_INDEX,
                                &status, &size, WINHTTP_NO_HEADER_INDEX);
        }
        HINTERNET socket = ok && status == 101 ? WinHttpWebSocketCompleteUpgrade(request, 0) : nullptr;
        if (request != nullptr) WinHttpCloseHandle(request);
        WinHttpCloseHandle(connection);
        if (socket == nullptr) {
            if (ok) {
                if (!stop_) Deliver(Frame::Kind::Closed, "the relay refused (HTTP " + std::to_string(status) + ")");
                return;
            }
            return Fail("the relay can't be reached");
        }
        {
            const std::lock_guard lock(send_mutex_);
            if (stop_) {
                WinHttpCloseHandle(socket);
                return;
            }
            socket_ = socket;
        }
        Deliver(Frame::Kind::Open, {});
        std::string message;
        char        chunk[16384];
        while (!stop_) {
            DWORD                            read = 0;
            WINHTTP_WEB_SOCKET_BUFFER_TYPE   type{};
            const DWORD result = WinHttpWebSocketReceive(socket, chunk, sizeof(chunk), &read, &type);
            if (result != ERROR_SUCCESS) {
                if (!stop_) Deliver(Frame::Kind::Closed, "the connection to the relay closed");
                return;
            }
            if (type == WINHTTP_WEB_SOCKET_CLOSE_BUFFER_TYPE) {
                USHORT status_code = 0;
                char   reason[123] = {};
                DWORD  reason_size = 0;
                WinHttpWebSocketQueryCloseStatus(socket, &status_code, reason, sizeof(reason), &reason_size);
                if (!stop_) Deliver(Frame::Kind::Closed, std::string(reason, reason_size));
                return;
            }
            message.append(chunk, read);
            if (message.size() > 1024 * 1024) {
                if (!stop_) Deliver(Frame::Kind::Closed, "the relay sent too much");
                return;
            }
            if (type == WINHTTP_WEB_SOCKET_BINARY_MESSAGE_BUFFER_TYPE) {
                Deliver(Frame::Kind::Binary, std::move(message));
                message.clear();
            } else if (type == WINHTTP_WEB_SOCKET_UTF8_MESSAGE_BUFFER_TYPE) {
                Deliver(Frame::Kind::Text, std::move(message));
                message.clear();
            }
            // A *_FRAGMENT_BUFFER_TYPE: more of the same message follows.
        }
    }

    std::wstring       host_;
    INTERNET_PORT      port_;
    std::wstring       path_;
    bool               secure_;
    std::mutex         mutex_, send_mutex_;
    std::deque<Frame>  incoming_;
    std::atomic<bool>  stop_{false};
    HINTERNET          session_ = nullptr;
    HINTERNET          socket_ = nullptr;
    std::thread        thread_; // last: it starts Run, which uses everything above
};

} // namespace

TransportFactory PlatformTransport() {
    return [](const std::string &url, std::string &error) -> std::unique_ptr<Transport> {
        const bool        secure = url.starts_with("wss://");
        const std::size_t start = secure ? 6 : 5;
        const std::size_t slash = url.find('/', start);
        std::string       authority = url.substr(start, slash == std::string::npos ? std::string::npos : slash - start);
        const std::string path = slash == std::string::npos ? std::string("/") : url.substr(slash);
        INTERNET_PORT     port = secure ? INTERNET_DEFAULT_HTTPS_PORT : INTERNET_DEFAULT_HTTP_PORT;
        if (const std::size_t colon = authority.rfind(':'); colon != std::string::npos) {
            port = static_cast<INTERNET_PORT>(std::atoi(authority.c_str() + colon + 1));
            authority.resize(colon);
        }
        if (authority.empty()) {
            error = "the relay address has no host";
            return nullptr;
        }
        return std::make_unique<WinHttpTransport>(Wide(authority), port, Wide(path), secure);
    };
}

} // namespace moonmod::net

#endif

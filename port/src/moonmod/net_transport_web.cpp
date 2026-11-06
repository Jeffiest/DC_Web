// Online play's socket in the browser (moonmod/net_session.hpp): Emscripten's WebSocket API over the page's own
// WebSocket. The game runs on a pthread (PROXY_TO_PTHREAD); the socket lives on the page's main thread, whose callbacks
// queue frames under a lock for the game's thread to take, and sends are proxied to it by Emscripten.

#if defined(__EMSCRIPTEN__)

#include "moonmod/net_session.hpp"

#include <atomic>
#include <mutex>

#include <emscripten/html5.h>
#include <emscripten/websocket.h>

namespace moonmod::net {
namespace {

class WebTransport final : public Transport {
public:
    explicit WebTransport(EMSCRIPTEN_WEBSOCKET_T socket) : socket_(socket) {
        // The callbacks run on the page's main thread, the socket's own.
        emscripten_websocket_set_onopen_callback_on_thread(socket_, this, OnOpen, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
        emscripten_websocket_set_onmessage_callback_on_thread(socket_, this, OnMessage, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
        emscripten_websocket_set_onerror_callback_on_thread(socket_, this, OnError, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
        emscripten_websocket_set_onclose_callback_on_thread(socket_, this, OnClose, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
    }

    ~WebTransport() override {
        Close();
        // No callback may reach this object once it is gone.
        emscripten_websocket_set_onopen_callback_on_thread(socket_, nullptr, nullptr, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
        emscripten_websocket_set_onmessage_callback_on_thread(socket_, nullptr, nullptr, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
        emscripten_websocket_set_onerror_callback_on_thread(socket_, nullptr, nullptr, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
        emscripten_websocket_set_onclose_callback_on_thread(socket_, nullptr, nullptr, EM_CALLBACK_THREAD_CONTEXT_MAIN_RUNTIME_THREAD);
        emscripten_websocket_delete(socket_);
    }

    void Send(std::string_view bytes) override {
        if (open_) emscripten_websocket_send_binary(socket_, const_cast<char *>(bytes.data()), static_cast<uint32_t>(bytes.size()));
    }

    bool Poll(Frame &frame) override {
        const std::lock_guard lock(mutex_);
        if (incoming_.empty()) return false;
        frame = std::move(incoming_.front());
        incoming_.pop_front();
        return true;
    }

    void Close() override {
        if (!closed_) {
            closed_ = true;
            emscripten_websocket_close(socket_, 1000, "left");
        }
    }

private:
    void Deliver(Frame::Kind kind, std::string data) {
        const std::lock_guard lock(mutex_);
        incoming_.push_back({kind, std::move(data)});
    }

    static EM_BOOL OnOpen(int, const EmscriptenWebSocketOpenEvent *, void *user) {
        auto *self = static_cast<WebTransport *>(user);
        self->open_ = true;
        self->Deliver(Frame::Kind::Open, {});
        return EM_TRUE;
    }

    static EM_BOOL OnMessage(int, const EmscriptenWebSocketMessageEvent *event, void *user) {
        auto       *self = static_cast<WebTransport *>(user);
        std::string data(reinterpret_cast<const char *>(event->data), event->numBytes);
        // A text frame's bytes end with the terminating zero Emscripten adds.
        if (event->isText && !data.empty() && data.back() == '\0') data.pop_back();
        self->Deliver(event->isText ? Frame::Kind::Text : Frame::Kind::Binary, std::move(data));
        return EM_TRUE;
    }

    static EM_BOOL OnError(int, const EmscriptenWebSocketErrorEvent *, void *user) {
        auto *self = static_cast<WebTransport *>(user);
        if (!self->open_) self->Deliver(Frame::Kind::Closed, "the relay can't be reached");
        return EM_TRUE;
    }

    static EM_BOOL OnClose(int, const EmscriptenWebSocketCloseEvent *event, void *user) {
        auto *self = static_cast<WebTransport *>(user);
        self->open_ = false;
        self->Deliver(Frame::Kind::Closed, event->reason[0] != '\0' ? std::string(event->reason) : std::string());
        return EM_TRUE;
    }

    EMSCRIPTEN_WEBSOCKET_T socket_;
    std::mutex             mutex_;
    std::deque<Frame>      incoming_;
    std::atomic<bool>      open_{false};
    bool                   closed_ = false;
};

} // namespace

TransportFactory PlatformTransport() {
    if (!emscripten_websocket_is_supported()) return {};
    return [](const std::string &url, std::string &error) -> std::unique_ptr<Transport> {
        EmscriptenWebSocketCreateAttributes attributes;
        emscripten_websocket_init_create_attributes(&attributes);
        attributes.url = url.c_str();
        attributes.createOnMainThread = EM_TRUE;
        const EMSCRIPTEN_WEBSOCKET_T socket = emscripten_websocket_new(&attributes);
        if (socket <= 0) {
            error = "the browser couldn't open a socket to the relay";
            return nullptr;
        }
        auto transport = std::make_unique<WebTransport>(socket);
        // binaryType is arraybuffer, which emscripten_websocket_new sets.
        return transport;
    };
}

} // namespace moonmod::net

#endif

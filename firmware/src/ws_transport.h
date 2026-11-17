#pragma once

// WebSocketTransport (core/include/hubbo/integrations/websocket_transport.h)
// on ESP-IDF's esp_transport_ws, ws:// or wss://. wss:// runs over
// esp_transport_ssl and verifies the server against the ESP-IDF certificate
// bundle, hostname included (no pinning, no custom CA). Everything runs on the HA task:
// connect() and close() only record the request, and poll() does the work.
// A pending connect is made inside poll() with a blocking connect bounded
// by kConnectTimeoutMs (DNS included), longer than poll()'s timeout: only the
// HA task waits on it. Otherwise poll() waits at most its timeout for data.
// send() writes synchronously, bounded by kSendTimeoutMs.
//
// esp_transport_ws answers PING with PONG and handles CLOSE itself. Messages
// split into fragments are reassembled (in PSRAM, via the heap policy)
// up to kMaxMessageBytes. A bigger one drops the connection.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "hubbo/integrations/websocket_transport.h"

namespace hubbo::fw {

class EspWsTransport : public hubbo::integrations::WebSocketTransport {
public:
    static constexpr uint32_t kConnectTimeoutMs = 15000;  // DNS + TCP + TLS (3.4 s idle, 9 s under API load) + upgrade
    static constexpr uint32_t kSendTimeoutMs = 1000;
    static constexpr uint32_t kFrameReadTimeoutMs = 2000;  // rest of a frame already arriving
    static constexpr size_t kMaxMessageBytes = 4 * 1024 * 1024;

    EspWsTransport() = default;
    ~EspWsTransport() override;
    EspWsTransport(const EspWsTransport &) = delete;
    EspWsTransport &operator=(const EspWsTransport &) = delete;

    void setListener(Listener *listener) override { listener_ = listener; }
    bool connect(const std::string &url, const hubbo::integrations::ConnectOptions &options) override;
    bool send(std::string_view text) override;
    void close() override;
    void poll(uint32_t timeoutMs) override;

    // HA task: why the last connect failed ("" after a good one). Codes only.
    const std::string &lastError() const { return lastError_; }

private:
    enum class State : uint8_t { Idle, ConnectPending, Open, ClosePending };

    bool openNow();
    void readAvailable(uint32_t timeoutMs);
    void finish();  // tear down and deliver onClosed()
    void release();

    Listener *listener_ = nullptr;
    State state_ = State::Idle;
    std::string host_;
    std::string path_;
    uint16_t port_ = 0;
    bool tls_ = false;
    bool verifyTls_ = true;  // wss://: off encrypts without checking chain or name
    void *tcp_ = nullptr;  // esp_transport_handle_t: TCP, or SSL for wss://
    void *ws_ = nullptr;
    std::string message_;     // a text message being reassembled
    std::string sendBuffer_;  // send(): a writable copy for in-place masking
    std::string lastError_;
    bool inMessage_ = false;
};

}  // namespace hubbo::fw

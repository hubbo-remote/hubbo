#pragma once

#include <cstdint>
#include <string>
#include <string_view>

// The WebSocket port (ARCHITECTURE.md, Integrations). One
// text-frame WebSocket client connection, provided by each platform:
//   firmware  esp_transport_ws, driven from the HA task's poll();
//   hub-sim   an in-process fake Home Assistant (sim/src/fake_ha.h).
//
// Threading: the integration task (firmware: the HA task) owns the loop:
//
//     for (;;) {
//         transport.poll(client.hasWork() ? 0 : 50);  // may wait for I/O
//         client.poll(now);
//         adapter.service(now);
//     }
//
// Every method is called on that task, and every listener callback is
// delivered on it, from inside poll(). connect() and close() only start the
// work; the outcome arrives as onConnected()/onClosed(). Only poll() may
// wait: up to its timeout for I/O, and while a connection is being opened
// (TCP connect + WebSocket upgrade) up to 5 s. That wait is on the
// integration task only; the RF/engine task never calls into this port.
//
// A message larger than the platform can hold (firmware: about 4 MB) is
// dropped and the connection closed (onClosed()); the client then fails
// whatever was waiting for it.
namespace hubbo::integrations {

struct ConnectOptions {
    // wss:// only: check HA's certificate against the platform's CA bundle.
    // False accepts any certificate (a self-signed HA on the LAN).
    bool verifyTls = true;
};

class WebSocketTransport {
public:
    class Listener {
    public:
        virtual ~Listener() = default;
        virtual void onConnected() = 0;
        // One complete text message (fragments reassembled by the platform).
        // By value: the client may keep a large one (discovery) and parse it
        // in steps without copying.
        virtual void onText(std::string text) = 0;
        // The connection ended or a connect() failed. Exactly once per
        // connect() that returned true (after onConnected() or instead of it).
        virtual void onClosed() = 0;
    };

    virtual ~WebSocketTransport() = default;

    virtual void setListener(Listener *listener) = 0;
    // Start connecting to `url`: "ws://host:8123/api/websocket", or wss://
    // (TLS) for an https HA; the client passes it through as given. False if it
    // can't even start (bad URL, already connecting/open); no callback then.
    virtual bool connect(const std::string &url, const ConnectOptions &options) = 0;
    // Queue one text message. False if not connected or the send buffer is
    // full (the message is dropped; the caller fails what it was for).
    virtual bool send(std::string_view text) = 0;
    // Start closing; onClosed() follows (if a connection was open or opening).
    virtual void close() = 0;
    // Integration task, every loop: do pending I/O and deliver its events to
    // the listener. Waits at most `timeoutMs` for something to happen (0:
    // don't wait).
    virtual void poll(uint32_t timeoutMs) = 0;
};

}  // namespace hubbo::integrations

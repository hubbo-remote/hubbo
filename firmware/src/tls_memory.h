#pragma once

// mbedTLS's allocator for every TLS user (the HA WebSocket, the release
// feed, esp-mqtt): PSRAM first, since the prebuilt sdkconfig pins it to
// internal RAM and a session's buffers and handshake are tens of KB.
//
// Work on core 0 that never blocks starves IDLE0 (the task watchdog). A
// handshake is seconds of software ECC (no ECC hardware on the S3), and a
// fast download's reads never block either. Inside a YieldScope the task
// yields a tick once per 20 ms slice: at each mbedTLS allocation (about
// 2,400 per handshake) and wherever it calls yieldIfDue(). esp-mqtt's task
// opens no scope.

namespace hubbo::fw::tls {

// Once, at boot, before any TLS connection.
void useExternalMemory();

// Per task: several tasks may each hold one.
class YieldScope {
public:
    YieldScope();
    ~YieldScope();
    YieldScope(const YieldScope &) = delete;
    YieldScope &operator=(const YieldScope &) = delete;
};

// Inside a YieldScope: a tick if this task's slice is up.
void yieldIfDue();

}  // namespace hubbo::fw::tls

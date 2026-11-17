#include "ws_transport.h"

#include "tls_memory.h"

#include <esp_tls.h>
#include <esp_transport.h>
#include <esp_transport_tcp.h>
#include <esp_transport_ssl.h>
#include <esp_transport_ws.h>
#include <esp_crt_bundle.h>
#include <mbedtls/ssl.h>
#include <mbedtls/x509.h>

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace hubbo::fw {

namespace {

esp_transport_handle_t handle(void *p) { return static_cast<esp_transport_handle_t>(p); }

// "ws[s]://host[:port][/path]" -> parts. Port defaults to 80 (ws) or 443
// (wss), path to "/".
bool parseUrl(std::string_view url, std::string *host, uint16_t *port, std::string *path, bool *tls) {
    constexpr std::string_view kWs = "ws://";
    constexpr std::string_view kWss = "wss://";
    if (url.substr(0, kWs.size()) == kWs) {
        *tls = false;
        url.remove_prefix(kWs.size());
    } else if (url.substr(0, kWss.size()) == kWss) {
        *tls = true;
        url.remove_prefix(kWss.size());
    } else {
        return false;
    }
    const size_t slash = url.find('/');
    std::string_view authority = url.substr(0, slash);
    *path = slash == std::string_view::npos ? "/" : std::string(url.substr(slash));
    unsigned long portNumber = *tls ? 443 : 80;
    const size_t colon = authority.rfind(':');
    if (colon != std::string_view::npos) {
        const std::string digits(authority.substr(colon + 1));
        char *end = nullptr;
        portNumber = strtoul(digits.c_str(), &end, 10);
        if (digits.empty() || *end != '\0' || portNumber == 0 || portNumber > 65535) {
            return false;
        }
        authority = authority.substr(0, colon);
    }
    if (authority.empty()) {
        return false;
    }
    *host = std::string(authority);
    *port = static_cast<uint16_t>(portNumber);
    return true;
}

// verify_tls off: encrypted, but neither the chain nor the name is checked.
// The prebuilt esp-tls refuses a connection with no verification option
// (CONFIG_ESP_TLS_INSECURE is off), so this stands in for the bundle. The
// hostname still goes out as SNI, which name-routed proxies need.
esp_err_t attachNoVerify(void *conf) {
    mbedtls_ssl_conf_authmode(static_cast<mbedtls_ssl_config *>(conf), MBEDTLS_SSL_VERIFY_NONE);
    return ESP_OK;
}

}  // namespace

EspWsTransport::~EspWsTransport() {
    release();
}

// The WS layer doesn't own its TCP parent: both are destroyed.
void EspWsTransport::release() {
    if (ws_ != nullptr) {
        esp_transport_close(handle(ws_));
        esp_transport_destroy(handle(ws_));
        ws_ = nullptr;
    }
    if (tcp_ != nullptr) {
        esp_transport_destroy(handle(tcp_));
        tcp_ = nullptr;
    }
}

bool EspWsTransport::connect(const std::string &url, const hubbo::integrations::ConnectOptions &options) {
    if (state_ != State::Idle || !parseUrl(url, &host_, &port_, &path_, &tls_)) {
        return false;
    }
    verifyTls_ = options.verifyTls;
    state_ = State::ConnectPending;
    return true;
}

bool EspWsTransport::send(std::string_view text) {
    if (state_ != State::Open) {
        return false;
    }
    // esp_transport_ws masks the payload in place (and unmasks it after), so
    // it needs a writable copy: `text` may be read-only (a literal in flash
    // took a CacheError).
    sendBuffer_.assign(text.data(), text.size());
    const auto opcode = static_cast<ws_transport_opcodes_t>(WS_TRANSPORT_OPCODES_TEXT | WS_TRANSPORT_OPCODES_FIN);
    const int written = esp_transport_ws_send_raw(handle(ws_), opcode, sendBuffer_.data(),
                                                  static_cast<int>(sendBuffer_.size()), kSendTimeoutMs);
    sendBuffer_.clear();
    if (written != static_cast<int>(text.size())) {
        state_ = State::ClosePending;  // a half-written frame leaves the stream unusable
        return false;
    }
    return true;
}

void EspWsTransport::close() {
    if (state_ == State::ConnectPending || state_ == State::Open) {
        state_ = State::ClosePending;
    }
}

void EspWsTransport::poll(uint32_t timeoutMs) {
    switch (state_) {
        case State::Idle:
            return;
        case State::ConnectPending:
            if (openNow()) {
                state_ = State::Open;
                if (listener_ != nullptr) {
                    listener_->onConnected();
                }
            } else {
                finish();
            }
            return;
        case State::Open:
            readAvailable(timeoutMs);
            return;
        case State::ClosePending:
            finish();
            return;
    }
}

bool EspWsTransport::openNow() {
    lastError_.clear();
    esp_transport_handle_t tcp = tls_ ? esp_transport_ssl_init() : esp_transport_tcp_init();
    if (tcp != nullptr && tls_) {
        // Verifying: the bundle checks the chain, and esp-tls the hostname
        // (also sent as SNI) by default.
        esp_transport_ssl_crt_bundle_attach(tcp, verifyTls_ ? esp_crt_bundle_attach : attachNoVerify);
    }
    esp_transport_handle_t ws = tcp ? esp_transport_ws_init(tcp) : nullptr;
    if (ws == nullptr) {
        if (tcp != nullptr) {
            esp_transport_destroy(tcp);
        }
        return false;
    }
    tcp_ = tcp;
    ws_ = ws;
    esp_transport_ws_set_path(ws, path_.c_str());
    // Blocking: DNS, TCP connect, the TLS handshake and the HTTP upgrade,
    // bounded by the timeout.
    int connected;
    {
        tls::YieldScope yield;  // the handshake (tls_memory.h)
        connected = esp_transport_connect(ws, host_.c_str(), port_, kConnectTimeoutMs);
    }
    const int upgrade = esp_transport_ws_get_upgrade_request_status(ws);
    if (connected >= 0 && upgrade == 101) {
        return true;
    }
    // Which stage failed, for diagnostics (no secrets: codes only).
    char text[96];
    int tlsCode = 0, verifyFlags = 0;
    const esp_err_t error =
        esp_tls_get_and_clear_last_error(esp_transport_get_error_handle(ws), &tlsCode, &verifyFlags);
    // esp-tls keeps the mbedTLS code negated (positive); the bundle's
    // rejection is MBEDTLS_ERR_X509_CERT_VERIFY_FAILED, without flags.
    const int mbedtlsCode = tlsCode > 0 ? -tlsCode : tlsCode;
    if (upgrade > 0) {
        snprintf(text, sizeof(text), "upgrade refused: HTTP %d", upgrade);
    } else if (verifyFlags != 0 || mbedtlsCode == MBEDTLS_ERR_X509_CERT_VERIFY_FAILED) {
        snprintf(text, sizeof(text), "certificate rejected (mbedTLS -0x%x, verify flags 0x%x)", -mbedtlsCode,
                 verifyFlags);
    } else {
        snprintf(text, sizeof(text), "connect failed (%s, mbedTLS -0x%x)", esp_err_to_name(error), -mbedtlsCode);
    }
    lastError_ = text;
    return false;
}

void EspWsTransport::readAvailable(uint32_t timeoutMs) {
    esp_transport_handle_t ws = handle(ws_);
    // Up to a few frames per poll, so one poll() stays short.
    for (int frames = 0; frames < 8 && state_ == State::Open; frames++) {
        const int ready = esp_transport_poll_read(ws, frames == 0 ? static_cast<int>(timeoutMs) : 0);
        if (ready == 0) {
            return;
        }
        if (ready < 0) {
            state_ = State::ClosePending;
            return;
        }
        // The first read takes the frame header and the start of its payload.
        char head[512];
        int n = esp_transport_read(ws, head, sizeof(head), kFrameReadTimeoutMs);
        if (n < 0) {
            state_ = State::ClosePending;
            return;
        }
        const int opcode = esp_transport_ws_get_read_opcode(ws) & 0x0F;
        if (opcode == WS_TRANSPORT_OPCODES_CLOSE) {
            state_ = State::ClosePending;  // the transport already answered it
            return;
        }
        if (opcode != WS_TRANSPORT_OPCODES_TEXT && opcode != WS_TRANSPORT_OPCODES_CONT) {
            continue;  // PING/PONG (answered by the transport) or binary: ignored
        }
        if (opcode == WS_TRANSPORT_OPCODES_TEXT) {
            message_.clear();
            inMessage_ = true;
        } else if (!inMessage_) {
            continue;  // a continuation without a start
        }
        const size_t payload = static_cast<size_t>(esp_transport_ws_get_read_payload_len(ws));
        if (message_.size() + payload > kMaxMessageBytes) {
            state_ = State::ClosePending;
            return;
        }
        const size_t start = message_.size();
        message_.resize(start + payload);
        memcpy(message_.data() + start, head, static_cast<size_t>(n));
        size_t have = static_cast<size_t>(n);
        while (have < payload) {
            n = esp_transport_read(ws, message_.data() + start + have, static_cast<int>(payload - have),
                                   kFrameReadTimeoutMs);
            if (n <= 0) {
                state_ = State::ClosePending;
                return;
            }
            have += static_cast<size_t>(n);
        }
        if (esp_transport_ws_get_fin_flag(ws)) {
            inMessage_ = false;
            std::string text;
            text.swap(message_);
            if (listener_ != nullptr) {
                listener_->onText(std::move(text));
            }
        }
    }
}

void EspWsTransport::finish() {
    release();
    message_.clear();
    message_.shrink_to_fit();
    inMessage_ = false;
    state_ = State::Idle;
    if (listener_ != nullptr) {
        listener_->onClosed();
    }
}

}  // namespace hubbo::fw

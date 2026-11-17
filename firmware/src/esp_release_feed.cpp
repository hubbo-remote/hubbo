#include "esp_release_feed.h"

#include <Arduino.h>
#include <esp_crt_bundle.h>
#include <esp_http_client.h>
#include <mbedtls/pk.h>

#include <algorithm>
#include <cstring>
#include <functional>

#include "console.h"
#include "release_key.h"
#include "tls_memory.h"

namespace hubbo::fw {

using hubbo::release::FetchResult;

namespace {

constexpr int kMaxRedirects = 5;
constexpr size_t kReadBytes = 4096;
// GitHub's last redirect is a ~930-character URL; the default 512-byte send
// buffer can't hold its request line (measured: open fails).
constexpr int kSendBufferBytes = 2048;

bool isRedirect(int status) {
    return status == 301 || status == 302 || status == 303 || status == 307 || status == 308;
}

using Progress = std::function<void(uint8_t)>;

// The body of a 200 into `buffer`: `contentLength` bytes, or (chunked, -1)
// until the end, at most `capacity`.
bool readBody(esp_http_client_handle_t client, int64_t contentLength, uint8_t *buffer, size_t capacity,
              size_t *length, const std::function<bool()> &expired, const Progress &onProgress) {
    size_t got = 0;
    uint8_t lastPercent = 0;
    while (contentLength <= 0 || got < static_cast<size_t>(contentLength)) {
        if (expired()) {
            con.println("release: timed out");
            return false;
        }
        if (got == capacity) {  // chunked, and still coming
            con.printf("release: more than %u bytes\n", static_cast<unsigned>(capacity));
            return false;
        }
        const int n = esp_http_client_read(client, reinterpret_cast<char *>(buffer + got),
                                           std::min(kReadBytes, capacity - got));
        if (n < 0 || (n == 0 && !esp_http_client_is_complete_data_received(client))) {
            con.printf("release: read failed after %u bytes\n", static_cast<unsigned>(got));
            return false;
        }
        if (n == 0) {
            break;  // chunked, complete
        }
        got += n;
        tls::yieldIfDue();  // a fast feed's reads never block (tls_memory.h)
        if (onProgress && contentLength > 0) {
            const auto percent = static_cast<uint8_t>(uint64_t{got} * 100 / contentLength);
            if (percent != lastPercent) {
                lastPercent = percent;
                onProgress(percent);
            }
        }
    }
    *length = got;
    return true;
}

// GET `url` into `buffer`, following redirects, within `deadlineMs` of
// `startMs`. Ok: `*length` bytes. A body over `capacity` is Unreachable.
FetchResult get(const std::string &url, uint8_t *buffer, size_t capacity, size_t *length, uint32_t startMs,
                uint32_t deadlineMs, const Progress &onProgress) {
    tls::YieldScope yield;  // the handshakes and the reads (tls_memory.h)
    esp_http_client_config_t config{};
    config.url = url.c_str();
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.timeout_ms = EspReleaseFeed::kTimeoutMs;
    config.buffer_size_tx = kSendBufferBytes;
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (client == nullptr) {
        con.println("release: no memory for an HTTP client");
        return FetchResult::Unreachable;
    }
    const std::function<bool()> expired = [&] { return millis() - startMs >= deadlineMs; };
    bool ok = false;
    for (int hops = 0;; hops++) {
        if (expired()) {
            con.println("release: timed out");
            break;
        }
        const esp_err_t opened = esp_http_client_open(client, 0);
        if (opened != ESP_OK) {
            con.printf("release: connect failed (%s)\n", esp_err_to_name(opened));
            break;
        }
        const int64_t contentLength = esp_http_client_fetch_headers(client);
        const int status = esp_http_client_get_status_code(client);
        if (isRedirect(status) && hops < kMaxRedirects) {
            esp_http_client_flush_response(client, nullptr);
            if (esp_http_client_set_redirection(client) == ESP_OK) {
                continue;
            }
            con.printf("release: bad redirect (HTTP %d)\n", status);
        } else if (status != 200) {
            con.printf("release: HTTP %d\n", status);
        } else if (contentLength > 0 && static_cast<uint64_t>(contentLength) > capacity) {
            con.printf("release: %lld bytes, more than %u\n", static_cast<long long>(contentLength),
                       static_cast<unsigned>(capacity));
        } else {
            ok = readBody(client, contentLength, buffer, capacity, length, expired, onProgress);
        }
        break;
    }
    esp_http_client_cleanup(client);
    return ok ? FetchResult::Ok : FetchResult::Unreachable;
}

// ECDSA P-256 over SHA-256 of `message`, DER signature `sig`. `pem` is NUL-terminated.
bool verified(const char *pem, hubbo::auth::Hash &hash, const std::string &message, const uint8_t *sig,
              size_t sigLength) {
    mbedtls_pk_context key;
    mbedtls_pk_init(&key);
    bool ok = false;
    if (mbedtls_pk_parse_public_key(&key, reinterpret_cast<const unsigned char *>(pem), strlen(pem) + 1) != 0) {
        con.println("release: the public key doesn't parse");
    } else if (!mbedtls_pk_can_do(&key, MBEDTLS_PK_ECDSA)) {
        con.println("release: the public key isn't an EC key");
    } else {
        uint8_t digest[hubbo::auth::kSha256Size];
        hash.sha256(reinterpret_cast<const uint8_t *>(message.data()), message.size(), digest);
        ok = mbedtls_pk_verify(&key, MBEDTLS_MD_SHA256, digest, sizeof(digest), sig, sigLength) == 0;
    }
    mbedtls_pk_free(&key);
    return ok;
}

// The built-in release key, or a dev build's test key (release_key.h).
bool signedByRelease(hubbo::auth::Hash &hash, const std::string &message, const uint8_t *sig, size_t sigLength) {
    if (verified(release_key::kPem, hash, message, sig, sigLength)) {
        return true;
    }
#ifdef HUBBO_DEV_CONSOLE
    if (release_key::kTestPem != nullptr && verified(release_key::kTestPem, hash, message, sig, sigLength)) {
        con.println("release: signed with the dev build's test key");
        return true;
    }
#endif
    return false;
}

}  // namespace

FetchResult EspReleaseFeed::fetchManifest(std::string *manifest) {
    const uint32_t startMs = millis();
    // A release published between the two fetches pairs one manifest with
    // the other's signature: a mismatch is fetched once more before it counts.
    for (int attempt = 0;; attempt++) {
        std::string body(hubbo::release::kMaxManifestBytes, '\0');
        size_t bodyLength = 0;
        FetchResult r = get(base() + "/latest/download/manifest.json", reinterpret_cast<uint8_t *>(body.data()),
                            body.size(), &bodyLength, startMs, kManifestDeadlineMs, nullptr);
        if (r != FetchResult::Ok) {
            return r;
        }
        body.resize(bodyLength);
        uint8_t sig[kMaxSignatureBytes];
        size_t sigLength = 0;
        r = get(base() + "/latest/download/manifest.sig", sig, sizeof(sig), &sigLength, startMs, kManifestDeadlineMs,
                nullptr);
        if (r != FetchResult::Ok) {
            return r;
        }
        if (signedByRelease(hash_, body, sig, sigLength)) {
            *manifest = std::move(body);
            return FetchResult::Ok;
        }
        con.println("release: manifest.sig doesn't verify");
        if (attempt == 1) {
            return FetchResult::BadSignature;
        }
    }
}

FetchResult EspReleaseFeed::fetchImage(const hubbo::release::Manifest &manifest, uint8_t *buffer, size_t capacity,
                                       size_t *length) {
    const std::string url = base() + "/download/v" + manifest.version + "/hubbo-" + manifest.version + ".bin";
    return get(url, buffer, capacity, length, millis(), kImageDeadlineMs, onProgress_);
}

}  // namespace hubbo::fw

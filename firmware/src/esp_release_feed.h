#pragma once

// release::ReleaseFeed over esp_http_client (docs/design/firmware-updates.md):
// GitHub's latest release, over HTTPS with the certificate bundle, following
// its redirects (github.com answers 302 twice, then the asset host; no host
// is pinned). manifest.sig is checked with mbedTLS (ECDSA P-256 over SHA-256
// of manifest.json's exact bytes) against the built-in key (release_key.h).
//
// The update task only (it blocks on the network, one connection at a time):
// the handshake runs inside a tls::YieldScope, and a signature check is about
// 0.4 s of software ECC.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>

#include "hubbo/auth/crypto.h"
#include "hubbo/integrations/firmware_release.h"

namespace hubbo::fw {

class EspReleaseFeed : public hubbo::release::ReleaseFeed {
public:
    explicit EspReleaseFeed(hubbo::auth::Hash &hash) : hash_(hash) {}

    // Where the releases are: <base>/latest/download/manifest.json and .sig,
    // <base>/download/v<version>/hubbo-<version>.bin.
    static constexpr const char *kReleasesUrl = "https://github.com/maddox/hubbo/releases";
    static constexpr uint32_t kTimeoutMs = 10000;          // connect, and each read
    static constexpr uint32_t kManifestDeadlineMs = 30000;  // manifest.json and .sig together
    static constexpr uint32_t kImageDeadlineMs = 180000;    // the image (about 10 s at 250 KB/s)
    static constexpr size_t kMaxSignatureBytes = 160;       // DER ECDSA P-256 is at most 72

    hubbo::release::FetchResult fetchManifest(std::string *manifest) override;
    hubbo::release::FetchResult fetchImage(const hubbo::release::Manifest &manifest, uint8_t *buffer,
                                           size_t capacity, size_t *length) override;

    // fetchImage's progress, percent of the image, on this task as it changes.
    void setOnProgress(std::function<void(uint8_t percent)> onProgress) { onProgress_ = std::move(onProgress); }

#ifdef HUBBO_DEV_CONSOLE
    // The dev console's /debug/release: another feed, to test against a
    // local one; "" is GitHub's. Signatures still need the built-in key (or a
    // dev build's test key, release_key.h). Set it only while no fetch runs.
    void setTestFeed(std::string baseUrl) { baseUrl_ = std::move(baseUrl); }
#endif

private:
    std::string base() const { return baseUrl_.empty() ? std::string(kReleasesUrl) : baseUrl_; }

    hubbo::auth::Hash &hash_;
    std::function<void(uint8_t)> onProgress_;
    std::string baseUrl_;
};

}  // namespace hubbo::fw

#pragma once

// auth::Hash with SHA-256 on the ESP32-S3's accelerator (mbedTLS's port):
// about 10x the portable one on a 2 MB firmware image. HMAC and PBKDF2 stay
// the portable implementations (same results either way).

#include <mbedtls/sha256.h>

#include "hubbo/auth/crypto.h"

namespace hubbo::fw {

class EspHash : public hubbo::auth::SoftwareHash {
public:
    void sha256(const uint8_t *data, size_t length, uint8_t out[hubbo::auth::kSha256Size]) override {
        mbedtls_sha256(data, length, out, 0);
    }
};

}  // namespace hubbo::fw

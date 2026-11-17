#pragma once

#include <cstddef>

// The release signing public key (ECDSA P-256, PEM), built in from
// firmware/release-key.pub.pem by scripts/release_key.py.
namespace hubbo::fw::release_key {

extern const char kPem[];  // NUL-terminated; mbedtls_pk_parse_public_key takes sizeof including the NUL
// A local test key: dev-console builds made with HUBBO_TEST_RELEASE_KEY_FILE
// only, else null. Only HUBBO_DEV_CONSOLE code reads it.
extern const char *const kTestPem;

}  // namespace hubbo::fw::release_key

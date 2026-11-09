#pragma once

// auth::SecretStore on NVS, namespace "hubbo-secret" (admin record, API token
// hashes; later integration credentials). NVS writes one entry atomically
// (CRC'd; a power cut keeps the old value), which is the per-record contract.
//
// "Can't read" is never "doesn't exist": only ESP_ERR_NVS_NOT_FOUND (a
// missing key, or the namespace never created: a fresh hub) is NotFound.
// Every other error is Unavailable, and health() reports it, so a broken NVS
// can never look like "no admin yet".
//
// Not encrypted: both NVS encryption schemes need eFuse burning or flash
// encryption, which this MVP doesn't do (README "Storage").
//
// Called from the HTTP task only (and once at boot before it starts).

#include <string>
#include <string_view>
#include <vector>

#include "hubbo/auth/secret_store.h"

namespace hubbo::fw {

class NvsSecretStore : public hubbo::auth::SecretStore {
public:
    static constexpr const char *kNamespace = "hubbo-secret";

    hubbo::storage::Health health() const override;
    hubbo::auth::SecretRead read(std::string_view key, std::string *value) const override;
    bool write(std::string_view key, std::string_view value) override;
    bool erase(std::string_view key) override;
    hubbo::auth::SecretRead keys(std::string_view prefix, std::vector<std::string> *out) const override;
};

}  // namespace hubbo::fw

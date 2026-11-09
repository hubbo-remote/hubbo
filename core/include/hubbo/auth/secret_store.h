#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>
#include <vector>

#include "hubbo/storage/record_store.h"
#include "hubbo/storage/storage.h"

// Where secrets live: the admin password hash, API token hashes and (later)
// integration credentials such as the Home Assistant token. Never the
// ConfigService JSON, backups, logs or API responses.
//
// A small key-value port so the firmware can implement it directly on NVS
// (ideally an encrypted NVS namespace; see docs/security.md):
//   - keys are at most kMaxSecretKeyLength bytes of [a-z0-9_] (NVS limit 15);
//   - values are strings up to kMaxSecretValueLength bytes;
//   - write()/erase() are durable when they return true;
//   - keys() lists keys with a prefix (NVS: nvs_entry_find in the namespace).
//
// "Can't read" is never "doesn't exist" (docs/design/persistence-and-
// contracts.md): read() and keys() say which. On NVS:
//   ESP_ERR_NVS_NOT_FOUND            -> NotFound
//   any other error, or no namespace -> Unavailable (and health() too)
// A store that couldn't read its contents must refuse writes, so nothing it
// didn't read is ever overwritten.
//
// Keys used by the core (auth/credentials.cpp):
//   admin            "<username>|<password hash>" (one key: setup is atomic)
//   tok_<8-char id>  one API token record
namespace hubbo::auth {

constexpr size_t kMaxSecretKeyLength = 15;
constexpr size_t kMaxSecretValueLength = 512;

// Keys follow storage::isValidKey (15 chars of [a-z0-9_], the NVS limit).

enum class SecretRead : uint8_t {
    Ok,           // found; value filled
    NotFound,     // positively absent
    Unavailable,  // couldn't read (IO error, medium down): unknown, NOT absent
    Corrupt,      // the store's data exists but can't be decoded
};
const char *secretReadName(SecretRead result);  // "ok", "not_found", "unavailable", "corrupt"

class SecretStore {
public:
    virtual ~SecretStore() = default;
    virtual storage::Health health() const = 0;
    virtual SecretRead read(std::string_view key, std::string *value) const = 0;
    // Durable on true. False when the store isn't writable, including when
    // its contents couldn't be read.
    virtual bool write(std::string_view key, std::string_view value) = 0;
    virtual bool erase(std::string_view key) = 0;  // true if gone afterwards
    // Ok and fills `out`, or Unavailable/Corrupt (out untouched).
    virtual SecretRead keys(std::string_view prefix, std::vector<std::string> *out) const = 0;
    // Re-read the medium (after it came back). No-op for stores that read
    // through on every call, such as NVS.
    virtual void reload() {}
    // The medium's own health, even while health() reports a failed read as
    // Unavailable. Credentials::refresh() retries at once when it changes.
    virtual storage::Health mediumHealth() const { return health(); }
};

// In-memory store for hub-sim and tests. Same key/value rules as NVS. The
// public fields inject failures.
class MemorySecretStore : public SecretStore {
public:
    storage::Health health() const override { return healthState; }
    SecretRead read(std::string_view key, std::string *value) const override;
    bool write(std::string_view key, std::string_view value) override;
    bool erase(std::string_view key) override;
    SecretRead keys(std::string_view prefix, std::vector<std::string> *out) const override;

    bool failWrites = false;                                 // e.g. flash full
    storage::Health healthState = storage::Health::Available;  // Unavailable: reads and writes fail

private:
    std::map<std::string, std::string, std::less<>> values_;
};

// Secrets as one record ("secrets", a flat JSON object) in the record store
// (docs/design/persistence-and-contracts.md): every write saves the whole map
// as a new generation, so a crash mid-save keeps the previous version and an
// unacknowledged write never loads. hub-sim keeps it under --data, so
// credentials survive a restart like the hub's NVS. The payload is plaintext;
// on the board use NVS (ideally encrypted) behind the same SecretStore port.
//
// Writes are refused unless the record loaded (or is positively absent):
// saving the whole map after a failed load would replace secrets it never
// read.
class StorageSecretStore : public SecretStore {
public:
    static constexpr const char *kRecordName = "secrets";

    explicit StorageSecretStore(storage::RecordStore &records);

    storage::Health health() const override;
    SecretRead read(std::string_view key, std::string *value) const override;
    bool write(std::string_view key, std::string_view value) override;
    bool erase(std::string_view key) override;
    SecretRead keys(std::string_view prefix, std::vector<std::string> *out) const override;
    void reload() override;
    storage::Health mediumHealth() const override { return records_.health(); }

    SecretRead loadState() const { return loaded_; }  // Ok (record), NotFound (none), Unavailable, Corrupt

private:
    // Whole map as the generation the store says comes next; false (nothing
    // saved) on failure, including a store that isn't fully Available
    // (ReadOnly after a failed invalidation: no write is trusted there).
    bool save();
    bool writable() const { return loaded_ == SecretRead::Ok || loaded_ == SecretRead::NotFound; }

    storage::RecordStore &records_;
    SecretRead loaded_ = SecretRead::Unavailable;
    MemorySecretStore memory_;
};

}  // namespace hubbo::auth

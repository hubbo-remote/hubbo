#pragma once

#include <cstddef>
#include <map>
#include <string>
#include <string_view>

#include "hubbo/storage/record_store.h"

// Small, non-secret runtime state that should survive a reboot (today: the
// active activity id, the hub's name). Keys follow storage::isValidKey
// (fits NVS); values are short strings (at most kMaxStateValueLength bytes).
//
// Implementations:
//   RecordStateStore  one record per key in a RecordStore (hub-sim --data).
//   MemoryStateStore  in memory (tests, hub-sim without --data).
//   (firmware)        NVS.
//
// Never called on the RF/engine hot path: writes come from a delayed
// persister on the config/HTTP side (see engine/active_persister.h).
namespace hubbo::storage {

constexpr size_t kMaxStateValueLength = 128;  // keys: storage::isValidKey

class StateStore {
public:
    virtual ~StateStore() = default;
    // Ok (value filled), NotFound (positively absent), or StorageFailed /
    // Corrupt (unreadable: unknown, never the same as absent).
    virtual ErrorCode get(std::string_view key, std::string *value) const = 0;
    // False on failure (invalid key, too long, medium error). May be slow
    // (flash); never call it from the engine task.
    virtual bool set(std::string_view key, std::string_view value) = 0;
    // Removes the key, so get() is NotFound. False on failure; removing an
    // absent key is true. Same tasks as set().
    virtual bool remove(std::string_view key) = 0;
};

class MemoryStateStore : public StateStore {
public:
    ErrorCode get(std::string_view key, std::string *value) const override;
    bool set(std::string_view key, std::string_view value) override;
    bool remove(std::string_view key) override;

    int writes() const { return writes_; }
    bool failWrites = false;  // test hook
    bool failReads = false;   // test hook: get() -> StorageFailed

private:
    std::map<std::string, std::string, std::less<>> values_;
    int writes_ = 0;
};

// One record per key in a RecordStore (hub-sim --data; tests).
class RecordStateStore : public StateStore {
public:
    explicit RecordStateStore(RecordStore &store) : store_(store) {}
    ErrorCode get(std::string_view key, std::string *value) const override;
    bool set(std::string_view key, std::string_view value) override;
    bool remove(std::string_view key) override;

private:
    RecordStore &store_;
};

}  // namespace hubbo::storage

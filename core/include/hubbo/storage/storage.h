#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <string_view>

#include "hubbo/domain/error.h"

// The storage port: whole-file reads and writes, which is all the record
// store (A/B slots, no renames) needs. Names are flat file names
// ([A-Za-z0-9._-], no directories).
//
// Implementations:
//   StdioStorage   C stdio under a root directory. On the host that's a
//                  normal directory; on the ESP32 it's a LittleFS mount
//                  (e.g. "/littlefs"), which the VFS exposes to stdio.
//   MemoryStorage  in-memory, with fault injection for power-loss tests.
//
// Called only from the config task, never on button presses or activity
// switches.
namespace hubbo::storage {

// Whether a medium can be used. Unavailable: not mounted / IO failure.
// ReadOnly: reads work, writes fail. Nothing falls back to RAM.
enum class Health : uint8_t { Available, Unavailable, ReadOnly };
const char *healthName(Health health);  // "available", "unavailable", "read_only"

bool isValidFileName(std::string_view name);

// Keys of small durable records (state, secrets): [a-z0-9_], 1..15 bytes
// (NVS's key limit). The one validator for every key-value store.
constexpr size_t kMaxKeyLength = 15;
bool isValidKey(std::string_view key);

class Storage {
public:
    virtual ~Storage() = default;
    // NotFound, TooLong (larger than maxBytes) or StorageFailed.
    virtual ErrorCode read(const std::string &name, std::string *out, size_t maxBytes) = 0;
    // Creates or truncates, writes everything and flushes to the medium.
    virtual ErrorCode write(const std::string &name, std::string_view data) = 0;
    // Ok if it doesn't exist.
    virtual ErrorCode remove(const std::string &name) = 0;
    virtual Health health() const { return Health::Available; }

    // Trust belongs to the medium (design part 6, "one owner per medium"):
    // a record store that couldn't invalidate a slot after a failed save
    // distrusts the medium, and every record store on it then reports
    // ReadOnly until reboot. Wrappers (hub-sim's SwitchableStorage) carry
    // their own flag, so all stores must share the same Storage object.
    void distrust() { untrusted_.store(true); }
    bool trusted() const { return !untrusted_.load(); }

protected:
    void trustAgain() { untrusted_.store(false); }  // a power cycle (MemoryStorage::reboot)

private:
    std::atomic<bool> untrusted_{false};
};

class StdioStorage : public Storage {
public:
    explicit StdioStorage(std::string root);  // existing directory, no trailing '/'
    Health health() const override;  // Unavailable if the root isn't a writable directory

    ErrorCode read(const std::string &name, std::string *out, size_t maxBytes) override;
    ErrorCode write(const std::string &name, std::string_view data) override;
    ErrorCode remove(const std::string &name) override;

private:
    std::string path(const std::string &name) const { return root_ + "/" + name; }
    std::string root_;
};

class MemoryStorage : public Storage {
public:
    ErrorCode read(const std::string &name, std::string *out, size_t maxBytes) override;
    ErrorCode write(const std::string &name, std::string_view data) override;
    ErrorCode remove(const std::string &name) override;
    Health health() const override { return health_; }
    void setHealth(Health health) { health_ = health; }
    // Read-error injection: reads of `name` fail with StorageFailed (an IO
    // error, distinct from NotFound) until cleared.
    void failReads(const std::string &name, bool fail = true) {
        if (fail) {
            failingReads_[name] = true;
        } else {
            failingReads_.erase(name);
        }
    }

    // Power-loss simulation: after `operations` more mutating calls succeed,
    // the next write keeps only its first `tornBytes` bytes and it and every
    // later mutation fail (the "power" is gone). -1 disables.
    void crashAfter(int operations, size_t tornBytes = 0) {
        crashAfter_ = operations;
        tornBytes_ = tornBytes;
    }
    bool crashed() const { return crashed_; }
    void reboot() {  // power comes back: a new boot trusts the medium again
        crashed_ = false;
        crashAfter_ = -1;
        trustAgain();
    }

    std::map<std::string, std::string> &files() { return files_; }
    int mutations() const { return mutations_; }
    int reads() const { return reads_; }  // read() calls, failed ones included

private:
    bool powerLost();
    std::map<std::string, std::string> files_;
    int crashAfter_ = -1;
    size_t tornBytes_ = 0;
    bool crashed_ = false;
    int mutations_ = 0;
    int reads_ = 0;
    Health health_ = Health::Available;
    std::map<std::string, bool> failingReads_;
};

}  // namespace hubbo::storage

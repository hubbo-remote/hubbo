#pragma once

// A Storage whose health hub-sim can override, to exercise the
// storage-unavailable and read-only paths (serve --storage-unavailable,
// POST /sim/storage). Host-only.

#include "host_guard.h"
#include "hubbo/storage/storage.h"

namespace hubbo::sim {

class SwitchableStorage : public storage::Storage {
public:
    explicit SwitchableStorage(storage::Storage &inner) : inner_(inner) {}

    void setHealth(storage::Health health) { health_ = health; }

    storage::Health health() const override {
        return health_ != storage::Health::Available ? health_ : inner_.health();
    }
    ErrorCode read(const std::string &name, std::string *out, size_t maxBytes) override {
        if (health() == storage::Health::Unavailable) return ErrorCode::StorageFailed;
        return inner_.read(name, out, maxBytes);
    }
    ErrorCode write(const std::string &name, std::string_view data) override {
        if (health() != storage::Health::Available) return ErrorCode::StorageFailed;
        return inner_.write(name, data);
    }
    ErrorCode remove(const std::string &name) override {
        if (health() != storage::Health::Available) return ErrorCode::StorageFailed;
        return inner_.remove(name);
    }

private:
    storage::Storage &inner_;
    storage::Health health_ = storage::Health::Available;
};

}  // namespace hubbo::sim

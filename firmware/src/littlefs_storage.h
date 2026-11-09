#pragma once

// storage::Storage on the LittleFS mount: core's stdio implementation (the
// VFS exposes LittleFS to stdio; StdioStorage fsyncs every write), with
// health taken from the mount itself. Unmounted means Unavailable, so the
// record store fails every save instead of pretending.

#include "flash_fs.h"
#include "hubbo/storage/storage.h"

namespace hubbo::fw {

class LittleFsStorage : public hubbo::storage::StdioStorage {
public:
    LittleFsStorage() : StdioStorage(flash_fs::kMountPoint) {}
    hubbo::storage::Health health() const override {
        return flash_fs::mounted() ? hubbo::storage::Health::Available : hubbo::storage::Health::Unavailable;
    }
};

}  // namespace hubbo::fw

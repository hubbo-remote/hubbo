#pragma once

// All firmware text output goes through `con` (a Print), never Serial
// directly:
//   - USB serial is non-blocking: a 4 KB TX buffer and a 0 ms TX timeout
//     (begin()), so a slow or absent USB host never stalls the RF loop;
//     output that doesn't fit is dropped;
//   - dev builds (HUBBO_DEV_CONSOLE) also keep the last kRecentBytes in RAM
//     for GET /debug/log, since a PoE-powered board has no serial at all.

#include <Print.h>

#include <cstddef>
#include <string>

namespace hubbo::fw {

class Console : public Print {
public:
    static constexpr size_t kRecentBytes = 8192;

    // Before anything prints.
    void begin(unsigned long baud);

    size_t write(uint8_t c) override { return write(&c, 1); }
    size_t write(const uint8_t *buffer, size_t size) override;
    using Print::write;

#ifdef HUBBO_DEV_CONSOLE
    // The last kRecentBytes of output, oldest first. Any task.
    std::string recent() const;
#endif
};

extern Console con;

}  // namespace hubbo::fw

#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

#include "hubbo/domain/limits.h"

// Identifier rules. Ids are stable machine identifiers, never display names.
//
//   id          ^[a-z][a-z0-9_]{0,31}$                       integration, device, activity
//   command id  ^[a-z][a-z0-9_]{0,31}(\.[a-z][a-z0-9_]{0,31})?$
//
// These match api/openapi.yaml. The API additionally requires the prefixes
// it mints ("dev_", "act_"); the core treats ids as opaque.
//
// "system" is the reserved integration id of the built-in system actions.
namespace hubbo {

bool isValidId(std::string_view id);
bool isValidCommandId(std::string_view id);
bool isValidName(std::string_view name);  // 1..kMaxNameLength bytes, no control chars

constexpr const char *kSystemIntegrationId = "system";

// Fixed-capacity, always NUL-terminated string for runtime structs that must
// not allocate. assign() refuses (returns false, leaves it empty) rather than
// truncating, so an over-long value can never alias a shorter valid one.
template <size_t N>
class FixedString {
public:
    FixedString() { data_[0] = '\0'; }

    bool assign(std::string_view value) {
        if (value.size() > N) {
            clear();
            return false;
        }
        std::memcpy(data_, value.data(), value.size());
        data_[value.size()] = '\0';
        size_ = static_cast<uint8_t>(value.size());
        return true;
    }

    void clear() {
        data_[0] = '\0';
        size_ = 0;
    }

    const char *c_str() const { return data_; }
    size_t size() const { return size_; }
    bool empty() const { return size_ == 0; }
    std::string_view view() const { return std::string_view(data_, size_); }

    bool operator==(std::string_view other) const { return view() == other; }
    bool operator!=(std::string_view other) const { return view() != other; }

private:
    static_assert(N < 256, "size is stored in a byte");
    char data_[N + 1];
    uint8_t size_ = 0;
};

using IdString = FixedString<limits::kMaxIdLength>;

}  // namespace hubbo

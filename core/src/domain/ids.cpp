#include "hubbo/domain/ids.h"

namespace hubbo {

namespace {

bool isLowerAscii(char c) { return c >= 'a' && c <= 'z'; }
bool isDigitAscii(char c) { return c >= '0' && c <= '9'; }

}  // namespace

bool isValidId(std::string_view id) {
    if (id.empty() || id.size() > limits::kMaxIdLength || !isLowerAscii(id[0])) {
        return false;
    }
    for (char c : id) {
        if (!isLowerAscii(c) && !isDigitAscii(c) && c != '_') {
            return false;
        }
    }
    return true;
}

bool isValidCommandId(std::string_view id) {
    // One optional dot: "<segment>" or "<domain>.<segment>", each a valid id.
    if (id.empty() || id.size() > limits::kMaxCommandIdLength) {
        return false;
    }
    const size_t dot = id.find('.');
    if (dot == std::string_view::npos) {
        return isValidId(id);
    }
    return isValidId(id.substr(0, dot)) && isValidId(id.substr(dot + 1));
}

bool isValidName(std::string_view name) {
    if (name.empty() || name.size() > limits::kMaxNameLength) {
        return false;
    }
    for (char c : name) {
        const auto u = static_cast<unsigned char>(c);
        if (u < 0x20 || u == 0x7F) {
            return false;
        }
    }
    return true;
}

}  // namespace hubbo

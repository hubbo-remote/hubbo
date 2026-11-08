#include "hubbo/auth/csrf.h"

#include "hubbo/auth/crypto.h"

namespace hubbo::auth {

namespace {

std::string_view trim(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t')) s.remove_suffix(1);
    return s;
}

}  // namespace

bool isJsonContentType(std::string_view contentType) {
    const size_t semicolon = contentType.find(';');
    return asciiEqualsIgnoreCase(trim(contentType.substr(0, semicolon)), "application/json");
}

bool originAllowed(std::string_view origin, std::string_view host) {
    if (origin.empty()) {
        return true;
    }
    std::string_view rest;
    if (origin.substr(0, 7) == "http://") {
        rest = origin.substr(7);
    } else if (origin.substr(0, 8) == "https://") {
        rest = origin.substr(8);
    } else {
        return false;  // includes "null"
    }
    // An origin is scheme://host[:port] with nothing after it.
    if (rest.empty() || rest.find('/') != std::string_view::npos || host.empty()) {
        return false;
    }
    return asciiEqualsIgnoreCase(rest, trim(host));
}

}  // namespace hubbo::auth

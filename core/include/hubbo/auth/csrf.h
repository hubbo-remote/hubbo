#pragma once

#include <string_view>

// The browser request checks that stand in for a CSRF token (with the
// SameSite=Strict session cookie):
//
// 1. Unsafe requests with a body must be `Content-Type: application/json`
//    (parameters allowed). HTML forms cannot send that cross-origin without a
//    CORS preflight, which the hub never approves (no CORS headers at all).
// 2. If an `Origin` header is present it must name this hub (same host and
//    port as the `Host` header). Absent Origin is allowed: non-browser clients
//    (curl, agents) don't send it, and browsers always send it on cross-origin
//    POSTs.
namespace hubbo::auth {

// `contentType` names `mediaType` (case-insensitive, parameters ignored).
bool isMediaType(std::string_view contentType, std::string_view mediaType);

// `origin` is the Origin header ("" if absent), `host` the Host header.
// "null" origins are rejected.
bool originAllowed(std::string_view origin, std::string_view host);

}  // namespace hubbo::auth

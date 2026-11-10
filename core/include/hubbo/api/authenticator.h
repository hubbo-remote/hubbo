#pragma once

#include <cstdint>
#include <string_view>

#include "hubbo/auth/credentials.h"
#include "hubbo/auth/session.h"
#include "hubbo/http/route.h"

// The production RequestAuthenticator: turns a request's credentials into an
// auth::RequestContext for the router's policy check.
//
//   Authorization: Bearer hbt_...   -> Token principal (if the token is live)
//   Cookie: hubbo_session=...       -> Admin principal (if the session is
//                                      live)
//   anything else / invalid         -> Anonymous (the policy answers 401)
//
// A bearer header wins over a cookie, so a script that also carries a stale
// browser cookie is judged by its token. hub-sim's no-auth mode uses its own
// authenticator (sim/src/noauth.h) instead.
namespace hubbo::api {

constexpr const char *kSessionCookie = "hubbo_session";

// Clocks the API needs, injected by the platform.
class Clock {
public:
    virtual ~Clock() = default;
    virtual uint64_t monotonicMs() const = 0;  // uptime, never goes back
    virtual int64_t unixSeconds() const = 0;   // wall clock, -1 if unknown (no SNTP yet)
};

// Value of cookie `name` in a Cookie header, or empty.
std::string_view cookieValue(std::string_view cookieHeader, std::string_view name);

class Authenticator : public http::RequestAuthenticator {
public:
    Authenticator(auth::Credentials &credentials, auth::SessionStore &sessions, const Clock &clock)
        : credentials_(credentials), sessions_(sessions), clock_(clock) {}

    auth::RequestContext authenticate(const http::Request &request) override;

private:
    auth::Credentials &credentials_;
    auth::SessionStore &sessions_;
    const Clock &clock_;
};

}  // namespace hubbo::api

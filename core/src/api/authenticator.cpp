#include "hubbo/api/authenticator.h"

#include "hubbo/auth/token.h"

namespace hubbo::api {

std::string_view cookieValue(std::string_view header, std::string_view name) {
    while (!header.empty()) {
        const size_t semi = header.find(';');
        std::string_view pair = header.substr(0, semi);
        while (!pair.empty() && pair.front() == ' ') pair.remove_prefix(1);
        const size_t eq = pair.find('=');
        if (eq != std::string_view::npos && pair.substr(0, eq) == name) return pair.substr(eq + 1);
        if (semi == std::string_view::npos) break;
        header.remove_prefix(semi + 1);
    }
    return {};
}

auth::RequestContext Authenticator::authenticate(const http::Request &request) {
    auth::RequestContext context;
    context.credentialsUnavailable = credentials_.state() == auth::CredentialState::Unavailable;
    // Can't read the credentials: accept none (a token or session can't be
    // checked against a store that can't be read). The policy answers 503.
    if (context.credentialsUnavailable) return context;
    if (request.headers == nullptr) return context;

    const std::string_view authorization = request.headers->header("Authorization");
    if (!authorization.empty()) {
        const std::string_view token = auth::bearerToken(authorization);
        if (!token.empty() && credentials_.authenticateToken(token, clock_.unixSeconds()) != nullptr) {
            context.principal = auth::Principal::token();
        }
        return context;  // a bad Authorization header never falls back to the cookie
    }

    const std::string_view id = cookieValue(request.headers->header("Cookie"), kSessionCookie);
    if (id.empty()) return context;
    const auth::SessionAuth session = sessions_.authenticate(id, clock_.monotonicMs());
    if (session.valid) {
        context.principal = auth::Principal::admin();
        context.sessionExpiresInMs = session.expiresInMs;
    }
    return context;
}

}  // namespace hubbo::api

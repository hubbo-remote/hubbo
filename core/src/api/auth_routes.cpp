// /auth/* and the admin account/token routes.

#include "hubbo/auth/token.h"
#include "hubbo/config/service.h"
#include "hubbo/domain/limits.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

namespace {

using Kind = Field::Kind;

std::string sessionCookie(const http::Request &request, std::string_view id) {
    std::string cookie = std::string(kSessionCookie) + "=" + std::string(id) + "; Path=/; HttpOnly; SameSite=Strict";
    if (id.empty()) cookie += "; Max-Age=0";
    if (request.secure) cookie += "; Secure";
    return cookie;
}

std::string_view requestSessionId(const http::Request &request) {
    return request.headers ? cookieValue(request.headers->header("Cookie"), kSessionCookie) : std::string_view();
}

// A new session (login, setup): the cookie and the Session body.
void startSession(ApiContext &ctx, const http::Request &request, http::Response &out, int status) {
    std::string id;
    if (!ctx.sessions.create(ctx.clock.monotonicMs(), &id)) {
        sendError(out, 500, "internal_error", "No entropy for a session id");
        return;
    }
    JsonDocument doc;
    doc["authenticated"] = true;
    doc["role"] = "admin";
    doc["username"] = ctx.credentials.username();
    doc["expires_in_s"] = auth::kSessionLifetimeMs / 1000;
    doc["setup_required"] = false;
    doc["auth_state"] = auth::credentialStateName(ctx.credentials.state());
    doc["auth_mode"] = authMode(ctx);
    out.headers.emplace_back("Set-Cookie", sessionCookie(request, id));
    out.headers.emplace_back("Cache-Control", "no-store");
    sendJson(out, status, doc);
}

void sendCredentialError(http::Response &out, auth::CredentialResult result, const char *passwordPath = "/password") {
    switch (result) {
        case auth::CredentialResult::InvalidUsername:
            sendDetail(out, "/username", "pattern", "Username must be 1-32 of A-Z a-z 0-9 _ . -");
            return;
        case auth::CredentialResult::WeakPassword:
            sendDetail(out, passwordPath, "weak_password",
                       "Password must be " + std::to_string(auth::kMinPasswordLength) + "-" +
                           std::to_string(auth::kMaxPasswordLength) + " bytes");
            return;
        case auth::CredentialResult::InvalidName:
            sendDetail(out, "/name", "invalid_name", issueMessage(ErrorCode::InvalidName));
            return;
        case auth::CredentialResult::AlreadyExists:
            sendError(out, 409, "setup_already_done", "The admin account already exists");
            return;
        case auth::CredentialResult::NoAdmin:
            sendError(out, 409, "setup_required", "Create the admin account first");
            return;
        case auth::CredentialResult::Unavailable:
            sendError(out, 503, "storage_unavailable",
                      auth::decisionMessage(auth::Decision::CredentialsUnavailable));
            return;
        case auth::CredentialResult::LimitReached:
            sendError(out, 409, "limit_reached", "Revoke a token first (16 at most)");
            return;
        case auth::CredentialResult::NotFound: sendError(out, 404, "not_found", "No such token"); return;
        default: sendError(out, 500, "internal_error", "Could not save credentials"); return;
    }
}

// {username, password}, as setup and login take them.
bool checkCredentials(const JsonDocument &doc, ValidationReport *report) {
    return checkObject(doc.as<JsonVariantConst>(),
                       {{"username", Kind::String, true, auth::kMaxUsernameLength},
                        {"password", Kind::String, true, auth::kMaxPasswordLength}},
                       report);
}

void putToken(JsonObject t, const auth::StoredToken &info) {
    t["id"] = "tok_" + info.id;
    t["name"] = info.name;
    t["hint"] = "hbt_" + info.id + "_";
    putTimestamp(t, "created_at", info.createdAt);
    putTimestamp(t, "last_used_at", info.lastUsedAt);
}

}  // namespace

void addAuthRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1/auth/setup", auth::Operation::SetupAdmin, [&ctx](auto &request, auto &, auto &out) {
        // Only a positively empty store takes a new admin (decided before the body is parsed).
        if (const auto guard = ctx.credentials.setupGuard(); guard != auth::CredentialResult::Ok) {
            return sendCredentialError(out, guard);
        }
        JsonDocument doc;
        if (!readJson(request, out, doc)) return;
        ValidationReport report;
        if (!checkCredentials(doc, &report)) {
            sendIssues(out, report, "Send {username, password}");
            return;
        }
        const auto result = ctx.credentials.setupAdmin(doc["username"].as<const char *>(),
                                                       doc["password"].as<const char *>());
        if (result != auth::CredentialResult::Ok) {
            sendCredentialError(out, result);
            return;
        }
        startSession(ctx, request, out, 201);
    });

    sink.add("/api/v1/auth/login", auth::Operation::Login, [&ctx](auto &request, auto &, auto &out) {
        JsonDocument doc;
        if (!readJson(request, out, doc)) return;
        ValidationReport report;
        if (!checkCredentials(doc, &report)) {
            sendIssues(out, report, "Send {username, password}");
            return;
        }
        // Only Ok can check a password. Never answer 401 when the truth is
        // "can't read the account" (Unavailable).
        if (const auto guard = ctx.credentials.adminGuard(); guard != auth::CredentialResult::Ok) {
            return sendCredentialError(out, guard);
        }
        if (!ctx.credentials.verifyLogin(doc["username"].as<const char *>(), doc["password"].as<const char *>())) {
            sendError(out, 401, "invalid_credentials", "Wrong username or password");
            return;
        }
        startSession(ctx, request, out, 200);
    });

    sink.add("/api/v1/auth/logout", auth::Operation::Logout, [&ctx](auto &request, auto &, auto &out) {
        const std::string_view id = requestSessionId(request);
        if (!id.empty()) ctx.sessions.remove(id);
        out.headers.emplace_back("Set-Cookie", sessionCookie(request, ""));
        sendEmpty(out, 204);
    });

    sink.add("/api/v1/auth/session", auth::Operation::GetSession, [&ctx](auto &request, auto &, auto &out) {
        JsonDocument doc;
        const auth::Principal &who = request.context.principal;
        const bool simulator = who.credential == auth::Credential::SimulatorBypass;
        doc["authenticated"] = who.role != auth::Role::Anonymous;
        if (who.role == auth::Role::Admin) {
            doc["role"] = "admin";
            doc["username"] = simulator ? std::string("simulator") : ctx.credentials.username();
        } else if (who.role == auth::Role::Token) {
            doc["role"] = "token";
        } else {
            doc["role"] = nullptr;
        }
        if (who.credential == auth::Credential::SessionCookie) {
            doc["expires_in_s"] = request.context.sessionExpiresInMs / 1000;
        }
        // Only a positively empty store asks for setup; unreadable is not empty.
        doc["setup_required"] = !simulator && ctx.credentials.state() == auth::CredentialState::Fresh;
        doc["auth_state"] = auth::credentialStateName(ctx.credentials.state());
        doc["config_state"] = config::configStateName(ctx.config.state());
        doc["auth_mode"] = authMode(ctx);
        out.headers.emplace_back("Cache-Control", "no-store");
        sendJson(out, 200, doc);
    });

    sink.add("/api/v1/admin/account/password", auth::Operation::ChangeAdminPassword,
             [&ctx](auto &request, auto &, auto &out) {
                 JsonDocument doc;
                 if (!readJson(request, out, doc)) return;
                 ValidationReport report;
                 if (!checkObject(doc.as<JsonVariantConst>(),
                                  {{"current_password", Kind::String, true, auth::kMaxPasswordLength},
                                   {"new_password", Kind::String, true, auth::kMaxPasswordLength}},
                                  &report)) {
                     sendIssues(out, report, "Send {current_password, new_password}");
                     return;
                 }
                 const auto result = ctx.credentials.changePassword(doc["current_password"].as<const char *>(),
                                                                    doc["new_password"].as<const char *>());
                 if (result == auth::CredentialResult::WrongPassword) {
                     sendError(out, 401, "invalid_credentials", "Current password is wrong");
                     return;
                 }
                 if (result != auth::CredentialResult::Ok) {
                     sendCredentialError(out, result, "/new_password");
                     return;
                 }
                 ctx.sessions.removeAllExcept(requestSessionId(request));
                 sendEmpty(out, 204);
             });

    sink.add("/api/v1/admin/tokens", auth::Operation::ListTokens, [&ctx](auto &, auto &, auto &out) {
        JsonDocument doc;
        JsonArray tokens = doc["tokens"].to<JsonArray>();
        for (const auto &info : ctx.credentials.tokens()) putToken(tokens.add<JsonObject>(), info);
        sendJson(out, 200, doc);
    });

    sink.add("/api/v1/admin/tokens", auth::Operation::CreateToken, [&ctx](auto &request, auto &, auto &out) {
        JsonDocument doc;
        if (!readJson(request, out, doc)) return;
        ValidationReport report;
        if (!checkObject(doc.as<JsonVariantConst>(), {{"name", Kind::String, true, limits::kMaxNameLength}}, &report)) {
            sendIssues(out, report, "Send {name}");
            return;
        }
        auth::IssuedToken issued;
        const auto result = ctx.credentials.createToken(doc["name"].as<const char *>(), ctx.clock.unixSeconds(), &issued);
        if (result != auth::CredentialResult::Ok) {
            sendCredentialError(out, result);
            return;
        }
        JsonDocument body;
        putToken(body.to<JsonObject>(), ctx.credentials.tokens().back());
        body["token"] = issued.plaintext;
        out.headers.emplace_back("Cache-Control", "no-store");
        sendJson(out, 201, body);
    });

    sink.add("/api/v1/admin/tokens/{token_id}", auth::Operation::RevokeToken, [&ctx](auto &, auto &params, auto &out) {
        const std::string_view id = params[0];
        if (id.substr(0, 4) != "tok_") {
            sendError(out, 404, "not_found", "No such token");
            return;
        }
        const auto result = ctx.credentials.revokeToken(id.substr(4));
        if (result != auth::CredentialResult::Ok) {
            sendCredentialError(out, result);
            return;
        }
        sendEmpty(out, 204);
    });
}

}  // namespace hubbo::api

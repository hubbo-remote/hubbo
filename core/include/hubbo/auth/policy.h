#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Authorization policy for every /api/v1 operation.
//
// The table in policy.cpp is the data: operationId (as in api/openapi.yaml),
// HTTP method, requirement, CSRF exemption and body limit. api/tools/validate.py
// (run by api/lint.sh) fails if it disagrees with the spec's x-hubbo-role,
// x-hubbo-csrf-exempt and x-hubbo-max-body. Every route handler calls authorize() first, before
// reading the body; nothing else decides who may call what.
//
//   Public        anyone (login, logout, session)
//   SetupOnly     anyone, but only while no admin account exists
//   TokenOrAdmin  API token or admin session
//   AdminOnly     admin session only
//
// Cookie-authenticated unsafe requests (POST/PUT/DELETE) also need a valid
// CSRF token, except login and setup (no session yet; those rely on the
// JSON-only content type and the Origin check, see csrf.h).
//
// Simulator bypass: only a build with HUBBO_SIMULATOR_NO_AUTH defined can
// create a SimulatorBypass principal; firmware builds refuse to compile with
// it (see the #error below).
#if defined(HUBBO_SIMULATOR_NO_AUTH) && (defined(ESP_PLATFORM) || defined(ARDUINO))
#error "HUBBO_SIMULATOR_NO_AUTH must never be defined for ESP32 builds"
#endif

namespace hubbo::auth {

enum class Role : uint8_t { Anonymous, Token, Admin };

enum class Requirement : uint8_t { Public, SetupOnly, TokenOrAdmin, AdminOnly };

enum class Method : uint8_t { Get, Post, Put, Delete };

// Exempt: a cookie may make this unsafe request without a CSRF token
// (login and setup, which run before a session exists).
enum class Csrf : uint8_t { Check, Exempt };

enum class Operation : uint8_t {
    GetStatus,
    GetRemoteStatus,
    ListRemoteButtons,
    SetupAdmin,
    Login,
    Logout,
    GetSession,
    ListDevices,
    GetDevice,
    GetDeviceCapabilities,
    ExecuteCommand,
    ListActivities,
    CreateActivity,
    ValidateActivity,
    ActivitiesOff,
    GetActivity,
    ReplaceActivity,
    DeleteActivity,
    ActivateActivity,
    GetGlobalBindings,
    ReplaceGlobalBindings,
    ListIntegrations,
    GetIntegrationConfig,
    ConfigureIntegration,
    DiscoverIntegration,
    StartIntegrationPairing,
    ForgetIntegrationPairing,
    RegisterDevice,
    UpdateDevice,
    DeleteDevice,
    GetRemotePairing,
    StartRemotePairing,
    CancelRemotePairing,
    ChangeAdminPassword,
    ListTokens,
    CreateToken,
    RevokeToken,
    DownloadBackup,
    RestoreBackup,
    FactoryReset,
    GetFirmwareStatus,
    UpdateFirmware,
    Count
};

constexpr size_t kOperationCount = static_cast<size_t>(Operation::Count);

struct OperationInfo {
    Operation operation;
    const char *id;  // OpenAPI operationId
    Method method;
    Requirement requirement;
    Csrf csrf;
    uint32_t maxBody;  // largest JSON body accepted (x-hubbo-max-body); 0 = takes no JSON body
};

const OperationInfo &operationInfo(Operation op);
// The largest JSON body any operation accepts (today restore, 640 KiB: kMaxConfigFileBytes + 128 KiB).
// Transports must accept bodies at least this large before routing (the
// router then applies each operation's own limit); a smaller transport cap
// would make some operations, e.g. restoring the hub's own backup, fail.
// Firmware image uploads are streamed separately and not covered.
uint32_t maxRequestBody();
bool operationFromId(std::string_view id, Operation *out);

enum class Credential : uint8_t { None, SessionCookie, BearerToken, SimulatorBypass };

// Who is calling. Built by the HTTP layer after checking the cookie (via
// SessionStore::authenticate) or the bearer token (via findToken). A request that
// presents an invalid credential is Anonymous, not an error.
struct Principal {
    Role role = Role::Anonymous;
    Credential credential = Credential::None;

    static Principal anonymous() { return {}; }
    static Principal admin() { return {Role::Admin, Credential::SessionCookie}; }
    static Principal token() { return {Role::Token, Credential::BearerToken}; }
#ifdef HUBBO_SIMULATOR_NO_AUTH
    // "AUTH DISABLED — SIMULATOR": every request is a simulated admin.
    static Principal simulator() { return {Role::Admin, Credential::SimulatorBypass}; }
#endif
};

constexpr bool kSimulatorNoAuthBuild =
#ifdef HUBBO_SIMULATOR_NO_AUTH
    true;
#else
    false;
#endif

struct RequestContext {
    Principal principal;
    bool csrfValid = false;    // X-Hubbo-CSRF matched the session (cookie only)
    uint64_t sessionExpiresInMs = 0;  // cookie only: until the session expires
    bool adminExists = true;   // an admin account is known to exist (credentials loaded Ok)
    // The credential store couldn't be read: nobody can be authenticated, and
    // saying 401 would tell a valid user their credentials are wrong.
    bool credentialsUnavailable = false;
};

enum class Decision : uint8_t {
    Allow,
    Unauthenticated,  // 401 unauthenticated
    Forbidden,        // 403 forbidden (e.g. API token on an admin operation)
    CsrfFailed,       // 403 csrf_failed
    SetupClosed,      // 409 setup_already_done
    CredentialsUnavailable,  // 503 storage_unavailable: credentials can't be checked right now
};

Decision authorize(Operation op, const RequestContext &context);
// The same decision for a route outside the API table (hub-sim's /sim/*
// tools use AdminOnly + Check with their own method).
Decision authorize(Requirement requirement, Csrf csrf, Method method, const RequestContext &context);

int httpStatus(Decision decision);
const char *decisionCode(Decision decision);  // API error code; "" for Allow
const char *decisionMessage(Decision decision);  // human text for the error body

bool isUnsafe(Method method);

}  // namespace hubbo::auth

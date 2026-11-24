#pragma once

#include <cstddef>
#include <cstdint>
#include <string_view>

// Authorization policy for every /api/v1 operation.
//
// The table in policy.cpp is the data: operationId (as in api/openapi.yaml),
// HTTP method, requirement, body limit and body type. api/tools/validate.py
// (run by api/lint.sh) fails if it disagrees with the spec's x-hubbo-role,
// x-hubbo-max-body and request content type. The router calls authorize()
// for every route; nothing else decides who may call what.
//
//   Public        anyone (setup, login, logout, session; setup itself only
//                 succeeds while the credentials are `fresh`)
//   TokenOrAdmin  API token or admin session
//   AdminOnly     admin session only
//
// There's no CSRF token: the session cookie is SameSite=Strict, bodies must
// be JSON, and the router refuses a foreign Origin (see csrf.h).
//
// Simulator bypass: only a build with HUBBO_SIMULATOR_NO_AUTH defined can
// create a SimulatorBypass principal; firmware builds refuse to compile with
// it (see the #error below).
#if defined(HUBBO_SIMULATOR_NO_AUTH) && (defined(ESP_PLATFORM) || defined(ARDUINO))
#error "HUBBO_SIMULATOR_NO_AUTH must never be defined for ESP32 builds"
#endif

namespace hubbo::auth {

enum class Role : uint8_t { Anonymous, Token, Admin };

enum class Requirement : uint8_t { Public, TokenOrAdmin, AdminOnly };

enum class Method : uint8_t { Get, Post, Put, Delete };

// What an operation's body is: JSON (`application/json`), or raw bytes
// (`application/octet-stream`: the firmware image).
enum class Body : uint8_t { Json, Binary };
const char *mediaType(Body body);  // the Content-Type it must be sent with

enum class Operation : uint8_t {
    GetStatus,
    ListActivityRuns,
    GetRemoteStatus,
    ListRemoteButtons,
    GetRemoteLog,
    SetupAdmin,
    Login,
    Logout,
    GetSession,
    ListDevices,
    GetDevice,
    GetDeviceCapabilities,
    ExecuteCommand,
    ListProfiles,
    GetProfile,
    ListKinds,
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
    RunMacro,
    GetSmartButtons,
    ReplaceSmartButtons,
    GetSmartButtonActions,
    ListIntegrations,
    GetIntegrationConfig,
    ConfigureIntegration,
    DiscoverIntegration,
    GetDiscovery,
    CancelDiscovery,
    RequestEntityDetails,
    GetEntityDetails,
    StartDevicePairing,
    GetDevicePairing,
    CancelDevicePairing,
    ForgetDevicePairing,
    RegisterDevice,
    UpdateDevice,
    DeleteDevice,
    GetRemoteConfig,
    SetRemoteEndpoint,
    GetRemotePairing,
    StartRemotePairing,
    CancelRemotePairing,
    ChangeAdminPassword,
    ListTokens,
    CreateToken,
    RevokeToken,
    DownloadBackup,
    RestoreBackup,
    ResetConfiguration,
    FactoryReset,
    GetFirmwareStatus,
    CheckFirmwareRelease,
    InstallFirmwareRelease,
    UpdateFirmware,
    GetHub,
    UpdateHub,
    RestartHub,
    GetApiIndex,
    GetOpenApi,
    GetAgentGuide,
    Count
};

constexpr size_t kOperationCount = static_cast<size_t>(Operation::Count);

struct OperationInfo {
    Operation operation;
    const char *id;  // OpenAPI operationId
    Method method;
    Requirement requirement;
    uint32_t maxBody;  // largest body accepted (x-hubbo-max-body); 0 = takes no body
    Body body = Body::Json;
};

const OperationInfo &operationInfo(Operation op);
// The largest JSON body any operation accepts (today restore, 640 KiB:
// kMaxConfigFileBytes + 128 KiB): the cap for unknown routes and host-only
// tools. The firmware image (Body::Binary, the OTA slot size) is the one
// exception; transports size each read from Router::bodyLimit(), which
// returns the matched operation's own limit.
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
    uint64_t sessionExpiresInMs = 0;  // cookie only: until the session expires
    // The credential store couldn't be read: nobody can be authenticated, and
    // saying 401 would tell a valid user their credentials are wrong.
    bool credentialsUnavailable = false;
};

enum class Decision : uint8_t {
    Allow,
    Unauthenticated,  // 401 unauthenticated
    Forbidden,        // 403 forbidden (e.g. API token on an admin operation)
    CredentialsUnavailable,  // 503 storage_unavailable: credentials can't be checked right now
};

Decision authorize(Operation op, const RequestContext &context);
// The same decision for a route outside the API table (hub-sim's /sim/*
// tools use AdminOnly).
Decision authorize(Requirement requirement, const RequestContext &context);

int httpStatus(Decision decision);
const char *decisionCode(Decision decision);  // API error code; "" for Allow
const char *decisionMessage(Decision decision);  // human text for the error body

bool isUnsafe(Method method);

}  // namespace hubbo::auth

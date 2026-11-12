#include "hubbo/auth/policy.h"

#include "hubbo/config/json.h"

namespace hubbo::auth {

namespace {

// One source (design part 4): a hub must restore any backup or blocked-raw
// export it produced, so the restore limit comes from the config size limit.
// The largest config the hub saves is kMaxConfigFileBytes; a Backup of it is
// the same sections with a small wrapper (format, dates, board, excluded:
// < 1 KiB) and no per-integration revisions, and a blocked-raw export is the
// record itself. 128 KiB of headroom covers the wrapper and a file that was
// pretty-printed or re-indented by an editor. api/lint.sh evaluates this
// expression to compare it with x-hubbo-max-body.
constexpr uint32_t kMaxRestoreBodyBytes = config::kMaxConfigFileBytes + 128 * 1024;

// The board's OTA slot (default_16MB.csv: app0/app1 are 0x640000 each). The
// port checks the real slot size again; this only bounds the upload.
constexpr uint32_t kMaxFirmwareImageBytes = 6400 * 1024;

// Keep in operation-enum order. api/lint.sh parses the table rows below
// (operation, id, method, requirement, max body bytes, optional body type)
// and compares them with api/openapi.yaml (x-hubbo-role, x-hubbo-max-body;
// 16 KiB for a JSON body without one; 0 = no body; Body::Binary for an
// application/octet-stream body).
constexpr OperationInfo kOperations[] = {
    {Operation::GetStatus, "getStatus", Method::Get, Requirement::TokenOrAdmin, 0},
    {Operation::GetRemoteStatus, "getRemoteStatus", Method::Get, Requirement::TokenOrAdmin, 0},
    {Operation::ListRemoteButtons, "listRemoteButtons", Method::Get, Requirement::TokenOrAdmin, 0},
    {Operation::SetupAdmin, "setupAdmin", Method::Post, Requirement::Public, 1024},
    {Operation::Login, "login", Method::Post, Requirement::Public, 1024},
    {Operation::Logout, "logout", Method::Post, Requirement::Public, 0},
    {Operation::GetSession, "getSession", Method::Get, Requirement::Public, 0},
    {Operation::ListDevices, "listDevices", Method::Get, Requirement::TokenOrAdmin, 0},
    {Operation::GetDevice, "getDevice", Method::Get, Requirement::TokenOrAdmin, 0},
    {Operation::GetDeviceCapabilities, "getDeviceCapabilities", Method::Get, Requirement::TokenOrAdmin, 0},
    {Operation::ExecuteCommand, "executeCommand", Method::Post, Requirement::TokenOrAdmin, 2048},
    {Operation::ListProfiles, "listProfiles", Method::Get, Requirement::AdminOnly, 0},
    {Operation::GetProfile, "getProfile", Method::Get, Requirement::AdminOnly, 0},
    {Operation::ListKinds, "listKinds", Method::Get, Requirement::AdminOnly, 0},
    {Operation::ListActivities, "listActivities", Method::Get, Requirement::TokenOrAdmin, 0},
    {Operation::CreateActivity, "createActivity", Method::Post, Requirement::TokenOrAdmin, 32768},
    {Operation::ValidateActivity, "validateActivity", Method::Post, Requirement::TokenOrAdmin, 32768},
    {Operation::ActivitiesOff, "activitiesOff", Method::Post, Requirement::TokenOrAdmin, 0},
    {Operation::GetActivity, "getActivity", Method::Get, Requirement::TokenOrAdmin, 0},
    {Operation::ReplaceActivity, "replaceActivity", Method::Put, Requirement::TokenOrAdmin, 32768},
    {Operation::DeleteActivity, "deleteActivity", Method::Delete, Requirement::TokenOrAdmin, 0},
    {Operation::ActivateActivity, "activateActivity", Method::Post, Requirement::TokenOrAdmin, 0},
    {Operation::GetGlobalBindings, "getGlobalBindings", Method::Get, Requirement::TokenOrAdmin, 0},
    {Operation::ReplaceGlobalBindings, "replaceGlobalBindings", Method::Put, Requirement::TokenOrAdmin, 32768},
    {Operation::ListIntegrations, "listIntegrations", Method::Get, Requirement::TokenOrAdmin, 0},
    {Operation::GetIntegrationConfig, "getIntegrationConfig", Method::Get, Requirement::AdminOnly, 0},
    {Operation::ConfigureIntegration, "configureIntegration", Method::Put, Requirement::AdminOnly, 8192},
    {Operation::DiscoverIntegration, "discoverIntegration", Method::Post, Requirement::AdminOnly, 0},
    {Operation::GetDiscovery, "getDiscovery", Method::Get, Requirement::AdminOnly, 0},
    {Operation::CancelDiscovery, "cancelDiscovery", Method::Delete, Requirement::AdminOnly, 0},
    {Operation::RequestEntityDetails, "requestEntityDetails", Method::Post, Requirement::AdminOnly, 0},
    {Operation::GetEntityDetails, "getEntityDetails", Method::Get, Requirement::AdminOnly, 0},
    {Operation::StartDevicePairing, "startDevicePairing", Method::Post, Requirement::AdminOnly, 0},
    {Operation::GetDevicePairing, "getDevicePairing", Method::Get, Requirement::AdminOnly, 0},
    {Operation::CancelDevicePairing, "cancelDevicePairing", Method::Delete, Requirement::AdminOnly, 0},
    {Operation::ForgetDevicePairing, "forgetDevicePairing", Method::Delete, Requirement::AdminOnly, 0},
    {Operation::RegisterDevice, "registerDevice", Method::Post, Requirement::AdminOnly, 4096},
    {Operation::UpdateDevice, "updateDevice", Method::Put, Requirement::AdminOnly, 4096},
    {Operation::DeleteDevice, "deleteDevice", Method::Delete, Requirement::AdminOnly, 0},
    {Operation::GetRemoteConfig, "getRemoteConfig", Method::Get, Requirement::AdminOnly, 0},
    {Operation::SetRemoteEndpoint, "setRemoteEndpoint", Method::Put, Requirement::AdminOnly, 1024},
    {Operation::GetRemotePairing, "getRemotePairing", Method::Get, Requirement::AdminOnly, 0},
    {Operation::StartRemotePairing, "startRemotePairing", Method::Post, Requirement::AdminOnly, 0},
    {Operation::CancelRemotePairing, "cancelRemotePairing", Method::Delete, Requirement::AdminOnly, 0},
    {Operation::ChangeAdminPassword, "changeAdminPassword", Method::Put, Requirement::AdminOnly, 1024},
    {Operation::ListTokens, "listTokens", Method::Get, Requirement::AdminOnly, 0},
    {Operation::CreateToken, "createToken", Method::Post, Requirement::AdminOnly, 1024},
    {Operation::RevokeToken, "revokeToken", Method::Delete, Requirement::AdminOnly, 0},
    {Operation::DownloadBackup, "downloadBackup", Method::Get, Requirement::AdminOnly, 0},
    {Operation::RestoreBackup, "restoreBackup", Method::Post, Requirement::AdminOnly, kMaxRestoreBodyBytes},
    {Operation::ResetConfiguration, "resetConfiguration", Method::Post, Requirement::AdminOnly, 0},
    {Operation::FactoryReset, "factoryReset", Method::Post, Requirement::AdminOnly, 0},
    {Operation::GetFirmwareStatus, "getFirmwareStatus", Method::Get, Requirement::AdminOnly, 0},
    {Operation::UpdateFirmware, "updateFirmware", Method::Post, Requirement::AdminOnly, kMaxFirmwareImageBytes,
     Body::Binary},
};

static_assert(sizeof(kOperations) / sizeof(kOperations[0]) == kOperationCount,
              "kOperations must list every Operation");

constexpr bool tableInEnumOrder() {
    for (size_t i = 0; i < kOperationCount; i++) {
        if (static_cast<size_t>(kOperations[i].operation) != i) return false;
    }
    return true;
}
static_assert(tableInEnumOrder(), "kOperations must be in Operation order");

}  // namespace

const OperationInfo &operationInfo(Operation op) {
    const size_t index = static_cast<size_t>(op);
    return kOperations[index < kOperationCount ? index : 0];
}

uint32_t maxRequestBody() {
    uint32_t largest = 0;
    for (const auto &info : kOperations) {
        if (info.body == Body::Json && info.maxBody > largest) largest = info.maxBody;
    }
    return largest;
}

bool operationFromId(std::string_view id, Operation *out) {
    for (const auto &info : kOperations) {
        if (id == info.id) {
            *out = info.operation;
            return true;
        }
    }
    return false;
}

bool isUnsafe(Method method) { return method != Method::Get; }

const char *mediaType(Body body) { return body == Body::Binary ? "application/octet-stream" : "application/json"; }

Decision authorize(Operation op, const RequestContext &context) {
    if (static_cast<size_t>(op) >= kOperationCount) {
        return Decision::Forbidden;
    }
    const OperationInfo &info = kOperations[static_cast<size_t>(op)];
    return authorize(info.requirement, context);
}

Decision authorize(Requirement requirement, const RequestContext &context) {
    const Principal &who = context.principal;

    // Role and credential must agree; anything else is treated as anonymous.
    // A simulator bypass principal is a simulated admin, but only in a build
    // where it can legitimately exist; anywhere else it is forged.
    const bool simulator = who.credential == Credential::SimulatorBypass;
    const bool consistent =
        (who.role == Role::Admin && who.credential == Credential::SessionCookie) ||
        (who.role == Role::Token && who.credential == Credential::BearerToken) ||
        (who.role == Role::Anonymous && who.credential == Credential::None) ||
        (who.role == Role::Admin && simulator && kSimulatorNoAuthBuild);
    const Role role = consistent ? who.role : Role::Anonymous;

    if (requirement == Requirement::Public) return Decision::Allow;
    if (role == Role::Anonymous) {
        return context.credentialsUnavailable ? Decision::CredentialsUnavailable : Decision::Unauthenticated;
    }
    return requirement == Requirement::AdminOnly && role != Role::Admin ? Decision::Forbidden : Decision::Allow;
}

int httpStatus(Decision decision) {
    switch (decision) {
        case Decision::Allow: return 200;
        case Decision::Unauthenticated: return 401;
        case Decision::Forbidden: return 403;
        case Decision::CredentialsUnavailable: return 503;
    }
    return 403;
}

const char *decisionCode(Decision decision) {
    switch (decision) {
        case Decision::Allow: return "";
        case Decision::Unauthenticated: return "unauthenticated";
        case Decision::Forbidden: return "forbidden";
        case Decision::CredentialsUnavailable: return "storage_unavailable";
    }
    return "forbidden";
}

const char *decisionMessage(Decision decision) {
    switch (decision) {
        case Decision::Allow: return "";
        case Decision::Unauthenticated: return "Log in or send an API token";
        case Decision::Forbidden: return "This operation needs the admin session";
        case Decision::CredentialsUnavailable:
            return "Stored credentials couldn't be read at boot; reboot the hub (or wipe it)";
    }
    return "Not allowed";
}

}  // namespace hubbo::auth

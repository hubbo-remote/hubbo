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

// Keep in operation-enum order. api/lint.sh parses the table rows below
// (operation, id, method, requirement, csrf, max JSON body bytes) and compares
// them with api/openapi.yaml (x-hubbo-role, x-hubbo-csrf-exempt,
// x-hubbo-max-body; 16 KiB for a JSON body without one; 0 = no JSON body).
constexpr OperationInfo kOperations[] = {
    {Operation::GetStatus, "getStatus", Method::Get, Requirement::TokenOrAdmin, Csrf::Check, 0},
    {Operation::GetRemoteStatus, "getRemoteStatus", Method::Get, Requirement::TokenOrAdmin, Csrf::Check, 0},
    {Operation::ListRemoteButtons, "listRemoteButtons", Method::Get, Requirement::TokenOrAdmin, Csrf::Check, 0},
    {Operation::SetupAdmin, "setupAdmin", Method::Post, Requirement::SetupOnly, Csrf::Exempt, 1024},
    {Operation::Login, "login", Method::Post, Requirement::Public, Csrf::Exempt, 1024},
    {Operation::Logout, "logout", Method::Post, Requirement::Public, Csrf::Check, 0},
    {Operation::GetSession, "getSession", Method::Get, Requirement::Public, Csrf::Check, 0},
    {Operation::ListDevices, "listDevices", Method::Get, Requirement::TokenOrAdmin, Csrf::Check, 0},
    {Operation::GetDevice, "getDevice", Method::Get, Requirement::TokenOrAdmin, Csrf::Check, 0},
    {Operation::GetDeviceCapabilities, "getDeviceCapabilities", Method::Get, Requirement::TokenOrAdmin, Csrf::Check, 0},
    {Operation::ExecuteCommand, "executeCommand", Method::Post, Requirement::TokenOrAdmin, Csrf::Check, 2048},
    {Operation::ListActivities, "listActivities", Method::Get, Requirement::TokenOrAdmin, Csrf::Check, 0},
    {Operation::CreateActivity, "createActivity", Method::Post, Requirement::TokenOrAdmin, Csrf::Check, 32768},
    {Operation::ValidateActivity, "validateActivity", Method::Post, Requirement::TokenOrAdmin, Csrf::Check, 32768},
    {Operation::ActivitiesOff, "activitiesOff", Method::Post, Requirement::TokenOrAdmin, Csrf::Check, 0},
    {Operation::GetActivity, "getActivity", Method::Get, Requirement::TokenOrAdmin, Csrf::Check, 0},
    {Operation::ReplaceActivity, "replaceActivity", Method::Put, Requirement::TokenOrAdmin, Csrf::Check, 32768},
    {Operation::DeleteActivity, "deleteActivity", Method::Delete, Requirement::TokenOrAdmin, Csrf::Check, 0},
    {Operation::ActivateActivity, "activateActivity", Method::Post, Requirement::TokenOrAdmin, Csrf::Check, 0},
    {Operation::GetGlobalBindings, "getGlobalBindings", Method::Get, Requirement::TokenOrAdmin, Csrf::Check, 0},
    {Operation::ReplaceGlobalBindings, "replaceGlobalBindings", Method::Put, Requirement::TokenOrAdmin, Csrf::Check, 32768},
    {Operation::ListIntegrations, "listIntegrations", Method::Get, Requirement::TokenOrAdmin, Csrf::Check, 0},
    {Operation::GetIntegrationConfig, "getIntegrationConfig", Method::Get, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::ConfigureIntegration, "configureIntegration", Method::Put, Requirement::AdminOnly, Csrf::Check, 8192},
    {Operation::DiscoverIntegration, "discoverIntegration", Method::Post, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::GetDiscovery, "getDiscovery", Method::Get, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::CancelDiscovery, "cancelDiscovery", Method::Delete, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::RequestEntityDetails, "requestEntityDetails", Method::Post, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::GetEntityDetails, "getEntityDetails", Method::Get, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::StartIntegrationPairing, "startIntegrationPairing", Method::Post, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::GetIntegrationPairing, "getIntegrationPairing", Method::Get, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::ForgetIntegrationPairing, "forgetIntegrationPairing", Method::Delete, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::RegisterDevice, "registerDevice", Method::Post, Requirement::AdminOnly, Csrf::Check, 4096},
    {Operation::UpdateDevice, "updateDevice", Method::Put, Requirement::AdminOnly, Csrf::Check, 4096},
    {Operation::DeleteDevice, "deleteDevice", Method::Delete, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::GetRemoteConfig, "getRemoteConfig", Method::Get, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::SetRemoteEndpoint, "setRemoteEndpoint", Method::Put, Requirement::AdminOnly, Csrf::Check, 1024},
    {Operation::GetRemotePairing, "getRemotePairing", Method::Get, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::StartRemotePairing, "startRemotePairing", Method::Post, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::CancelRemotePairing, "cancelRemotePairing", Method::Delete, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::ChangeAdminPassword, "changeAdminPassword", Method::Put, Requirement::AdminOnly, Csrf::Check, 1024},
    {Operation::ListTokens, "listTokens", Method::Get, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::CreateToken, "createToken", Method::Post, Requirement::AdminOnly, Csrf::Check, 1024},
    {Operation::RevokeToken, "revokeToken", Method::Delete, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::DownloadBackup, "downloadBackup", Method::Get, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::RestoreBackup, "restoreBackup", Method::Post, Requirement::AdminOnly, Csrf::Check, kMaxRestoreBodyBytes},
    {Operation::FactoryReset, "factoryReset", Method::Post, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::GetFirmwareStatus, "getFirmwareStatus", Method::Get, Requirement::AdminOnly, Csrf::Check, 0},
    {Operation::UpdateFirmware, "updateFirmware", Method::Post, Requirement::AdminOnly, Csrf::Check, 0},
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
        if (info.maxBody > largest) largest = info.maxBody;
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

Decision authorize(Operation op, const RequestContext &context) {
    if (static_cast<size_t>(op) >= kOperationCount) {
        return Decision::Forbidden;
    }
    const OperationInfo &info = kOperations[static_cast<size_t>(op)];
    return authorize(info.requirement, info.csrf, info.method, context);
}

Decision authorize(Requirement requirement, Csrf csrf, Method method, const RequestContext &context) {
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

    // CSRF: a cookie session's unsafe requests must carry the token. Checked
    // before the role so a forged cross-site request learns nothing.
    if (consistent && who.credential == Credential::SessionCookie && isUnsafe(method) && csrf == Csrf::Check &&
        !context.csrfValid) {
        return Decision::CsrfFailed;
    }

    switch (requirement) {
        case Requirement::Public:
            return Decision::Allow;
        case Requirement::SetupOnly:
            return context.adminExists ? Decision::SetupClosed : Decision::Allow;
        case Requirement::TokenOrAdmin:
            if (role != Role::Anonymous) return Decision::Allow;
            return context.credentialsUnavailable ? Decision::CredentialsUnavailable : Decision::Unauthenticated;
        case Requirement::AdminOnly:
            if (role == Role::Anonymous) {
                return context.credentialsUnavailable ? Decision::CredentialsUnavailable : Decision::Unauthenticated;
            }
            return role == Role::Admin ? Decision::Allow : Decision::Forbidden;
    }
    return Decision::Forbidden;
}

int httpStatus(Decision decision) {
    switch (decision) {
        case Decision::Allow: return 200;
        case Decision::Unauthenticated: return 401;
        case Decision::Forbidden: return 403;
        case Decision::CsrfFailed: return 403;
        case Decision::SetupClosed: return 409;
        case Decision::CredentialsUnavailable: return 503;
    }
    return 403;
}

const char *decisionCode(Decision decision) {
    switch (decision) {
        case Decision::Allow: return "";
        case Decision::Unauthenticated: return "unauthenticated";
        case Decision::Forbidden: return "forbidden";
        case Decision::CsrfFailed: return "csrf_failed";
        case Decision::SetupClosed: return "setup_already_done";
        case Decision::CredentialsUnavailable: return "storage_unavailable";
    }
    return "forbidden";
}

const char *decisionMessage(Decision decision) {
    switch (decision) {
        case Decision::Allow: return "";
        case Decision::Unauthenticated: return "Log in or send an API token";
        case Decision::Forbidden: return "This operation needs the admin session";
        case Decision::CsrfFailed: return "Missing or wrong X-Hubbo-CSRF header";
        case Decision::SetupClosed: return "The admin account already exists";
        case Decision::CredentialsUnavailable: return "Stored credentials can't be read right now; try again later";
    }
    return "Not allowed";
}

}  // namespace hubbo::auth

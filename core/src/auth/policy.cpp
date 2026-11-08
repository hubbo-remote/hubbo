#include "hubbo/auth/policy.h"

namespace hubbo::auth {

namespace {

// Keep in operation-enum order. api/lint.sh parses the
// table rows below (operation, id, method, requirement, csrf) and compares them
// with api/openapi.yaml.
constexpr OperationInfo kOperations[] = {
    {Operation::GetStatus, "getStatus", Method::Get, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::GetRemoteStatus, "getRemoteStatus", Method::Get, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::ListRemoteButtons, "listRemoteButtons", Method::Get, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::SetupAdmin, "setupAdmin", Method::Post, Requirement::SetupOnly, Csrf::Exempt},
    {Operation::Login, "login", Method::Post, Requirement::Public, Csrf::Exempt},
    {Operation::Logout, "logout", Method::Post, Requirement::Public, Csrf::Check},
    {Operation::GetSession, "getSession", Method::Get, Requirement::Public, Csrf::Check},
    {Operation::ListDevices, "listDevices", Method::Get, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::GetDevice, "getDevice", Method::Get, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::GetDeviceCapabilities, "getDeviceCapabilities", Method::Get, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::ExecuteCommand, "executeCommand", Method::Post, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::ListActivities, "listActivities", Method::Get, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::CreateActivity, "createActivity", Method::Post, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::ValidateActivity, "validateActivity", Method::Post, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::ActivitiesOff, "activitiesOff", Method::Post, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::GetActivity, "getActivity", Method::Get, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::ReplaceActivity, "replaceActivity", Method::Put, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::DeleteActivity, "deleteActivity", Method::Delete, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::ActivateActivity, "activateActivity", Method::Post, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::GetGlobalBindings, "getGlobalBindings", Method::Get, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::ReplaceGlobalBindings, "replaceGlobalBindings", Method::Put, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::ListIntegrations, "listIntegrations", Method::Get, Requirement::TokenOrAdmin, Csrf::Check},
    {Operation::GetIntegrationConfig, "getIntegrationConfig", Method::Get, Requirement::AdminOnly, Csrf::Check},
    {Operation::ConfigureIntegration, "configureIntegration", Method::Put, Requirement::AdminOnly, Csrf::Check},
    {Operation::DiscoverIntegration, "discoverIntegration", Method::Post, Requirement::AdminOnly, Csrf::Check},
    {Operation::StartIntegrationPairing, "startIntegrationPairing", Method::Post, Requirement::AdminOnly, Csrf::Check},
    {Operation::ForgetIntegrationPairing, "forgetIntegrationPairing", Method::Delete, Requirement::AdminOnly, Csrf::Check},
    {Operation::RegisterDevice, "registerDevice", Method::Post, Requirement::AdminOnly, Csrf::Check},
    {Operation::UpdateDevice, "updateDevice", Method::Put, Requirement::AdminOnly, Csrf::Check},
    {Operation::DeleteDevice, "deleteDevice", Method::Delete, Requirement::AdminOnly, Csrf::Check},
    {Operation::GetRemotePairing, "getRemotePairing", Method::Get, Requirement::AdminOnly, Csrf::Check},
    {Operation::StartRemotePairing, "startRemotePairing", Method::Post, Requirement::AdminOnly, Csrf::Check},
    {Operation::CancelRemotePairing, "cancelRemotePairing", Method::Delete, Requirement::AdminOnly, Csrf::Check},
    {Operation::ChangeAdminPassword, "changeAdminPassword", Method::Put, Requirement::AdminOnly, Csrf::Check},
    {Operation::ListTokens, "listTokens", Method::Get, Requirement::AdminOnly, Csrf::Check},
    {Operation::CreateToken, "createToken", Method::Post, Requirement::AdminOnly, Csrf::Check},
    {Operation::RevokeToken, "revokeToken", Method::Delete, Requirement::AdminOnly, Csrf::Check},
    {Operation::DownloadBackup, "downloadBackup", Method::Get, Requirement::AdminOnly, Csrf::Check},
    {Operation::RestoreBackup, "restoreBackup", Method::Post, Requirement::AdminOnly, Csrf::Check},
    {Operation::GetFirmwareStatus, "getFirmwareStatus", Method::Get, Requirement::AdminOnly, Csrf::Check},
    {Operation::UpdateFirmware, "updateFirmware", Method::Post, Requirement::AdminOnly, Csrf::Check},
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
    if (consistent && who.credential == Credential::SessionCookie && isUnsafe(info.method) &&
        info.csrf == Csrf::Check && !context.csrfValid) {
        return Decision::CsrfFailed;
    }

    switch (info.requirement) {
        case Requirement::Public:
            return Decision::Allow;
        case Requirement::SetupOnly:
            return context.adminExists ? Decision::SetupClosed : Decision::Allow;
        case Requirement::TokenOrAdmin:
            return role == Role::Anonymous ? Decision::Unauthenticated : Decision::Allow;
        case Requirement::AdminOnly:
            if (role == Role::Anonymous) return Decision::Unauthenticated;
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
    }
    return "forbidden";
}

}  // namespace hubbo::auth

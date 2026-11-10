#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

#include "hubbo/auth/csrf.h"
#include "hubbo/auth/policy.h"

using namespace hubbo::auth;

namespace {

RequestContext ctx(Principal p, bool csrf = true, bool adminExists = true) {
    RequestContext c;
    c.principal = p;
    c.csrfValid = csrf;
    c.adminExists = adminExists;
    return c;
}

Decision as(Principal p, Operation op, bool csrf = true) { return authorize(op, ctx(p, csrf)); }

Operation op(const char *id) {
    Operation out = Operation::Count;
    EXPECT_TRUE(operationFromId(id, &out)) << id;
    return out;
}

}  // namespace

TEST(AuthPolicy, TableIsConsistent) {
    std::set<std::string> ids;
    for (size_t i = 0; i < kOperationCount; i++) {
        const auto &info = operationInfo(static_cast<Operation>(i));
        EXPECT_EQ(static_cast<size_t>(info.operation), i);
        EXPECT_TRUE(ids.insert(info.id).second) << "duplicate " << info.id;
        Operation back;
        ASSERT_TRUE(operationFromId(info.id, &back));
        EXPECT_EQ(back, info.operation);
    }
    Operation unused;
    EXPECT_FALSE(operationFromId("callService", &unused));
    EXPECT_FALSE(operationFromId("", &unused));
}

// The full allow/deny matrix, written out by hand from ARCHITECTURE.md
// "Security" rather than derived from the table, so a table edit that
// widens access fails here.
TEST(AuthPolicy, RoleMatrix) {
    const char *tokenAllowed[] = {
        "getStatus", "getRemoteStatus", "listRemoteButtons", "listDevices", "getDevice",
        "getDeviceCapabilities", "executeCommand", "listActivities", "createActivity",
        "validateActivity", "activitiesOff", "getActivity", "replaceActivity", "deleteActivity",
        "activateActivity", "getGlobalBindings", "replaceGlobalBindings", "listIntegrations",
    };
    const char *adminOnly[] = {
        "getIntegrationConfig", "configureIntegration", "discoverIntegration", "getDiscovery", "cancelDiscovery",
        "requestEntityDetails", "getEntityDetails",
        "startIntegrationPairing", "getIntegrationPairing", "forgetIntegrationPairing", "registerDevice", "updateDevice",
        "deleteDevice", "getRemoteConfig", "setRemoteEndpoint", "getRemotePairing", "startRemotePairing", "cancelRemotePairing",
        "changeAdminPassword", "listTokens", "createToken", "revokeToken", "downloadBackup",
        "restoreBackup", "factoryReset", "getFirmwareStatus", "updateFirmware",
    };
    const char *publicOps[] = {"login", "logout", "getSession"};

    size_t covered = 1;  // setupAdmin, tested separately
    for (const char *id : tokenAllowed) {
        EXPECT_EQ(as(Principal::anonymous(), op(id)), Decision::Unauthenticated) << id;
        EXPECT_EQ(as(Principal::token(), op(id)), Decision::Allow) << id;
        EXPECT_EQ(as(Principal::admin(), op(id)), Decision::Allow) << id;
        covered++;
    }
    for (const char *id : adminOnly) {
        EXPECT_EQ(as(Principal::anonymous(), op(id)), Decision::Unauthenticated) << id;
        EXPECT_EQ(as(Principal::token(), op(id)), Decision::Forbidden) << id;
        EXPECT_EQ(as(Principal::admin(), op(id)), Decision::Allow) << id;
        covered++;
    }
    for (const char *id : publicOps) {
        EXPECT_EQ(as(Principal::anonymous(), op(id)), Decision::Allow) << id;
        EXPECT_EQ(as(Principal::token(), op(id)), Decision::Allow) << id;
        EXPECT_EQ(as(Principal::admin(), op(id)), Decision::Allow) << id;
        covered++;
    }
    EXPECT_EQ(covered, kOperationCount) << "a new operation needs a row in this matrix";
}

TEST(AuthPolicy, EveryAdminPathIsAdminOnly) {
    // Defence in depth for the matrix above: nothing a token can reach
    // manages devices, integrations, credentials, pairing, backups or firmware.
    for (size_t i = 0; i < kOperationCount; i++) {
        const auto &info = operationInfo(static_cast<Operation>(i));
        const Decision d = as(Principal::token(), info.operation);
        const std::string id = info.id;
        const bool sensitive = id.find("Token") != std::string::npos ||
                               id.find("Integration") != std::string::npos ||
                               id.find("Pairing") != std::string::npos ||
                               id.find("Backup") != std::string::npos ||
                               id.find("Firmware") != std::string::npos ||
                               id.find("Password") != std::string::npos ||
                               id == "registerDevice" || id == "updateDevice" || id == "deleteDevice";
        if (sensitive && id != "listIntegrations") {
            EXPECT_NE(d, Decision::Allow) << id;
        }
    }
}

TEST(AuthPolicy, CsrfRequiredForCookieUnsafeRequests) {
    // Cookie + unsafe + no CSRF -> refused, even for admin-only operations.
    EXPECT_EQ(as(Principal::admin(), Operation::CreateActivity, false), Decision::CsrfFailed);
    EXPECT_EQ(as(Principal::admin(), Operation::RegisterDevice, false), Decision::CsrfFailed);
    EXPECT_EQ(as(Principal::admin(), Operation::Logout, false), Decision::CsrfFailed);
    EXPECT_EQ(as(Principal::admin(), Operation::UpdateFirmware, false), Decision::CsrfFailed);
    // Safe methods don't need it.
    EXPECT_EQ(as(Principal::admin(), Operation::ListDevices, false), Decision::Allow);
    EXPECT_EQ(as(Principal::admin(), Operation::DownloadBackup, false), Decision::Allow);
    // Login and setup run before a session exists.
    EXPECT_EQ(as(Principal::admin(), Operation::Login, false), Decision::Allow);
    // Bearer tokens are not ambient credentials: no CSRF.
    EXPECT_EQ(as(Principal::token(), Operation::CreateActivity, false), Decision::Allow);
}

TEST(AuthPolicy, SetupOnlyBeforeAdminExists) {
    EXPECT_EQ(authorize(Operation::SetupAdmin, ctx(Principal::anonymous(), false, false)),
              Decision::Allow);
    EXPECT_EQ(authorize(Operation::SetupAdmin, ctx(Principal::anonymous(), false, true)),
              Decision::SetupClosed);
    EXPECT_EQ(authorize(Operation::SetupAdmin, ctx(Principal::admin(), true, true)),
              Decision::SetupClosed);
    EXPECT_EQ(authorize(Operation::SetupAdmin, ctx(Principal::token(), true, true)),
              Decision::SetupClosed);
    // No admin yet doesn't open anything else.
    EXPECT_EQ(authorize(Operation::RegisterDevice, ctx(Principal::anonymous(), false, false)),
              Decision::Unauthenticated);
}

TEST(AuthPolicy, InconsistentPrincipalsAreAnonymous) {
    // A token role arriving via a cookie, or admin via bearer, is a bug or a
    // forgery; it gets no access.
    EXPECT_EQ(as({Role::Admin, Credential::BearerToken}, Operation::RegisterDevice),
              Decision::Unauthenticated);
    EXPECT_EQ(as({Role::Token, Credential::SessionCookie}, Operation::ListDevices),
              Decision::Unauthenticated);
    EXPECT_EQ(as({Role::Admin, Credential::None}, Operation::ListDevices), Decision::Unauthenticated);
    EXPECT_EQ(authorize(Operation::Count, ctx(Principal::admin())), Decision::Forbidden);
}

TEST(AuthPolicy, SimulatorBypassOnlyInSimulatorBuilds) {
    const Principal forged{Role::Admin, Credential::SimulatorBypass};
    if (kSimulatorNoAuthBuild) {
        EXPECT_EQ(as(forged, Operation::RegisterDevice, false), Decision::Allow);
    } else {
        // Native test and firmware builds: the bypass credential gets exactly
        // what an anonymous caller gets.
        for (size_t i = 0; i < kOperationCount; i++) {
            const auto o = static_cast<Operation>(i);
            EXPECT_EQ(as(forged, o, false), as(Principal::anonymous(), o, false))
                << operationInfo(o).id;
        }
        EXPECT_EQ(as(forged, Operation::RegisterDevice, false), Decision::Unauthenticated);
    }
}

TEST(AuthPolicy, MaxRequestBodyCoversEveryOperation) {
    // Transports size their body buffers from this; it must cover restore.
    EXPECT_EQ(maxRequestBody(), operationInfo(Operation::RestoreBackup).maxBody);
    for (size_t i = 0; i < kOperationCount; i++) {
        EXPECT_LE(operationInfo(static_cast<Operation>(i)).maxBody, maxRequestBody());
    }
}

TEST(AuthPolicy, DecisionMapping) {
    EXPECT_EQ(httpStatus(Decision::Unauthenticated), 401);
    EXPECT_EQ(httpStatus(Decision::Forbidden), 403);
    EXPECT_EQ(httpStatus(Decision::CsrfFailed), 403);
    EXPECT_EQ(httpStatus(Decision::SetupClosed), 409);
    EXPECT_STREQ(decisionCode(Decision::CsrfFailed), "csrf_failed");
    EXPECT_STREQ(decisionCode(Decision::SetupClosed), "setup_already_done");
}

TEST(AuthCsrf, JsonContentType) {
    EXPECT_TRUE(isJsonContentType("application/json"));
    EXPECT_TRUE(isJsonContentType("Application/JSON; charset=utf-8"));
    EXPECT_FALSE(isJsonContentType("text/plain"));
    EXPECT_FALSE(isJsonContentType("application/x-www-form-urlencoded"));
    EXPECT_FALSE(isJsonContentType("multipart/form-data; boundary=x"));
    EXPECT_FALSE(isJsonContentType("application/jsonp"));
    EXPECT_FALSE(isJsonContentType(""));
}

TEST(AuthCsrf, Origin) {
    EXPECT_TRUE(originAllowed("", "hubbo.local"));
    EXPECT_TRUE(originAllowed("http://hubbo.local", "hubbo.local"));
    EXPECT_TRUE(originAllowed("https://192.168.1.56:8443", "192.168.1.56:8443"));
    EXPECT_TRUE(originAllowed("http://HUBBO.local", "hubbo.local"));
    EXPECT_FALSE(originAllowed("http://evil.example", "hubbo.local"));
    EXPECT_FALSE(originAllowed("http://hubbo.local.evil.example", "hubbo.local"));
    EXPECT_FALSE(originAllowed("http://hubbo.local:8080", "hubbo.local"));
    EXPECT_FALSE(originAllowed("null", "hubbo.local"));
    EXPECT_FALSE(originAllowed("file://", "hubbo.local"));
    EXPECT_FALSE(originAllowed("http://hubbo.local/path", "hubbo.local"));
    EXPECT_FALSE(originAllowed("http://hubbo.local", ""));
}

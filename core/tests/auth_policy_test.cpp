#include <gtest/gtest.h>

#include <cstring>
#include <set>
#include <string>

#include "hubbo/auth/csrf.h"
#include "hubbo/auth/policy.h"

using namespace hubbo::auth;

namespace {

RequestContext ctx(Principal p) {
    RequestContext c;
    c.principal = p;
    return c;
}

Decision as(Principal p, Operation op) { return authorize(op, ctx(p)); }

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

// The full allow/deny matrix, written out by hand from docs/security.md
// rather than derived from the table, so a table edit that
// widens access fails here.
TEST(AuthPolicy, RoleMatrix) {
    const char *tokenAllowed[] = {
        "getStatus", "listActivityRuns", "getRemoteStatus", "listRemoteButtons", "getRemoteLog", "listDevices", "getDevice",
        "getDeviceCapabilities", "executeCommand", "listActivities", "createActivity",
        "validateActivity", "activitiesOff", "getActivity", "replaceActivity", "deleteActivity",
        "activateActivity", "getGlobalBindings", "replaceGlobalBindings", "runMacro", "getSmartButtons",
        "replaceSmartButtons", "getSmartButtonActions", "listIntegrations",
    };
    const char *adminOnly[] = {
        "listProfiles", "getProfile", "listKinds",
        "getIntegrationConfig", "configureIntegration", "discoverIntegration", "getDiscovery", "cancelDiscovery",
        "requestEntityDetails", "getEntityDetails",
        "startDevicePairing", "getDevicePairing", "cancelDevicePairing", "forgetDevicePairing", "registerDevice", "updateDevice",
        "deleteDevice", "getRemoteConfig", "setRemoteEndpoint", "getRemotePairing", "startRemotePairing", "cancelRemotePairing",
        "changeAdminPassword", "listTokens", "createToken", "revokeToken", "downloadBackup",
        "restoreBackup", "resetConfiguration", "factoryReset", "getFirmwareStatus", "checkFirmwareRelease",
        "installFirmwareRelease", "updateFirmware",
        "getHub", "updateHub", "restartHub",
    };
    // setupAdmin's handler refuses it unless the credentials are fresh.
    // The API's own description (index, spec, guide) reveals nothing about the home.
    const char *publicOps[] = {"setupAdmin", "login", "logout", "getSession", "getApiIndex", "getOpenApi",
                               "getAgentGuide"};

    size_t covered = 0;
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
                               id.find("Hub") != std::string::npos ||
                               id == "registerDevice" || id == "updateDevice" || id == "deleteDevice";
        if (sensitive && id != "listIntegrations") {
            EXPECT_NE(d, Decision::Allow) << id;
        }
    }
}

TEST(AuthPolicy, UnreadableCredentialsAre503NotA401) {
    RequestContext c;
    c.credentialsUnavailable = true;
    EXPECT_EQ(authorize(Operation::ListDevices, c), Decision::CredentialsUnavailable);
    EXPECT_EQ(authorize(Operation::RegisterDevice, c), Decision::CredentialsUnavailable);
    EXPECT_EQ(authorize(Operation::GetSession, c), Decision::Allow);
    EXPECT_EQ(httpStatus(Decision::CredentialsUnavailable), 503);
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
        EXPECT_EQ(as(forged, Operation::RegisterDevice), Decision::Allow);
    } else {
        // Native test and firmware builds: the bypass credential gets exactly
        // what an anonymous caller gets.
        for (size_t i = 0; i < kOperationCount; i++) {
            const auto o = static_cast<Operation>(i);
            EXPECT_EQ(as(forged, o), as(Principal::anonymous(), o))
                << operationInfo(o).id;
        }
        EXPECT_EQ(as(forged, Operation::RegisterDevice), Decision::Unauthenticated);
    }
}

TEST(AuthPolicy, MaxRequestBodyCoversEveryJsonOperation) {
    // The cap for unknown routes: it must cover restore. The firmware image
    // is the one (binary) exception, bounded by the OTA slot.
    EXPECT_EQ(maxRequestBody(), operationInfo(Operation::RestoreBackup).maxBody);
    for (size_t i = 0; i < kOperationCount; i++) {
        const auto &info = operationInfo(static_cast<Operation>(i));
        if (info.operation == Operation::UpdateFirmware) continue;
        EXPECT_EQ(info.body, Body::Json) << info.id;
        EXPECT_LE(info.maxBody, maxRequestBody()) << info.id;
    }
    const auto &update = operationInfo(Operation::UpdateFirmware);
    EXPECT_EQ(update.body, Body::Binary);
    EXPECT_EQ(update.maxBody, 0x640000u);  // the board's OTA slot
}

TEST(AuthPolicy, DecisionMapping) {
    EXPECT_EQ(httpStatus(Decision::Unauthenticated), 401);
    EXPECT_EQ(httpStatus(Decision::Forbidden), 403);
    EXPECT_STREQ(decisionCode(Decision::Forbidden), "forbidden");
    EXPECT_STREQ(decisionCode(Decision::CredentialsUnavailable), "storage_unavailable");
}

TEST(AuthCsrf, JsonContentType) {
    EXPECT_TRUE(isMediaType("application/json", mediaType(Body::Json)));
    EXPECT_TRUE(isMediaType("Application/JSON; charset=utf-8", mediaType(Body::Json)));
    EXPECT_FALSE(isMediaType("text/plain", mediaType(Body::Json)));
    EXPECT_FALSE(isMediaType("application/x-www-form-urlencoded", mediaType(Body::Json)));
    EXPECT_FALSE(isMediaType("multipart/form-data; boundary=x", mediaType(Body::Json)));
    EXPECT_FALSE(isMediaType("application/jsonp", mediaType(Body::Json)));
    EXPECT_FALSE(isMediaType("", mediaType(Body::Json)));
    EXPECT_TRUE(isMediaType("application/octet-stream", mediaType(Body::Binary)));
    EXPECT_FALSE(isMediaType("application/json", "application/octet-stream"));
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

// Hubbo firmware for the Waveshare ESP32-S3-ETH: wiring, boot and the main
// loop. Task plan in README "Tasks":
//   loopTask, core 1  RF + engine: radio FIFO -> RemoteInput -> Engine ->
//                     BleHidAdapter -> NimBLE notify. Serial console.
//   httpd,    core 0  API handlers, ConfigService, and everything posted to
//                     it with HttpServer::post()/every().
#include <Arduino.h>
#include <ESPmDNS.h>
#include <esp_heap_caps.h>
#include <esp_system.h>

#include <atomic>
#include <cstdio>
#include <memory>
#include <string>

#include "ble_hid.h"
#include "board.h"
#include "diagnostics.h"
#include "engine_runtime.h"
#include "esp_clock.h"
#include "esp_random_source.h"
#include "ethernet_link.h"
#include "flash_fs.h"
#include "http_server.h"
#include "hubbo/api/api.h"
#include "hubbo/api/authenticator.h"
#include "hubbo/auth/credentials.h"
#include "hubbo/auth/crypto.h"
#include "hubbo/auth/rate_limit.h"
#include "hubbo/auth/session.h"
#include "hubbo/config/boot.h"
#include "hubbo/config/service.h"
#include "hubbo/domain/adapter.h"
#include "hubbo/domain/config.h"
#include "hubbo/domain/error.h"
#include "hubbo/engine/active_persister.h"
#include "hubbo/engine/engine.h"
#include "hubbo/http/route.h"
#include "hubbo/integrations/ble_hid.h"
#include "hubbo/remote/button.h"
#include "hubbo/remote/input.h"
#include "hubbo/storage/record_store.h"
#include "hubbo/storage/state_store.h"
#include "littlefs_storage.h"
#include "nvs_secret_store.h"
#include "ota.h"
#include "remote_radio.h"

#ifndef HUBBO_BLE_NAME
#define HUBBO_BLE_NAME "Hubbo"
#endif

using hubbo::remote::RemoteEvent;
using namespace hubbo::fw;
using diag::logf;

namespace {

constexpr uint32_t kStatusIntervalMs = 30000;
constexpr uint32_t kPublishIntervalMs = 250;

char hostname[24];

// --- RF/engine side (declaration order = construction order) -----------------

BleHid ble;
diag::TimedHidTransport timedHid(ble);
hubbo::integrations::BleHidAdapter bleAdapter(hubbo::kBleIntegrationId, timedHid);
hubbo::AdapterSet adapters;
hubbo::Engine::Store configStore;
hubbo::Engine engine(configStore, adapters);
EngineRuntime runtime(engine, adapters);

// Bumped on every activity change (engine task); the persister tick only
// wakes httpd when this moved or a write is still pending.
std::atomic<uint32_t> activityChanges{1};  // 1: tick once at boot

// The remote event being dispatched. It is logged just before the first
// line the engine produces for it (or right after dispatch), so formatting
// never runs between the radio read and the HID report.
struct {
    bool pending = false;
    RemoteEvent event;
} eventToLog;

void flushEventLog() {
    if (!eventToLog.pending) {
        return;
    }
    eventToLog.pending = false;
    const RemoteEvent &e = eventToLog.event;
    logf("event %lu %s %s%s", static_cast<unsigned long>(e.atMs), hubbo::remote::buttonName(e.button),
         hubbo::remote::eventKindName(e.kind), e.timedOut ? " timedOut" : "");
}

class EngineLog : public hubbo::EngineObserver {
public:
    void onActionDispatched(const hubbo::DispatchRecord &r) override {
        flushEventLog();
        const char *command = r.action.command != nullptr ? r.action.command->id : "?";
        const bool ok = r.result.error == hubbo::ErrorCode::Ok;
        logf("action %lu %s %s %s -> %s%s%s", static_cast<unsigned long>(r.atMs), r.integrationId,
             r.target[0] ? r.target : "-", command, hubbo::execStatusName(r.result.status), ok ? "" : " ",
             ok ? "" : hubbo::errorCodeName(r.result.error));
    }
    void onSequenceFinished(const hubbo::SequenceReport &r) override {
        flushEventLog();
        if (r.outcome != hubbo::SequenceOutcome::Completed) {
            logf("sequence %s %s: %s (action %u)", hubbo::sequenceOriginName(r.info.origin),
                 hubbo::sequenceOutcomeName(r.outcome), hubbo::errorCodeName(r.error), r.actionIndex);
        }
    }
    void onActivityChanged(const char *activityId, uint32_t nowMs) override {
        flushEventLog();
        activityChanges.fetch_add(1, std::memory_order_release);
        logf("activity %lu %s", static_cast<unsigned long>(nowMs), activityId[0] ? activityId : "(off)");
    }
    void onTransitionRejected(hubbo::ErrorCode error, const char *activityId, uint32_t) override {
        flushEventLog();
        logf("transition to %s rejected: %s", activityId[0] ? activityId : "(off)", hubbo::errorCodeName(error));
    }
    void onConfigChanged(uint32_t revision, uint32_t) override {
        logf("config revision %lu active", static_cast<unsigned long>(revision));
    }
};
EngineLog engineLog;

RemoteEvent lastEvent;  // for the API's remote status
bool hasLastEvent = false;

class EventTap : public hubbo::remote::EventSink {
public:
    void onRemoteEvent(const RemoteEvent &event) override {
        diag::eventEmitted(event.kind);
        lastEvent = event;
        hasLastEvent = true;
        eventToLog = {true, event};
        engine.onRemoteEvent(event);
        flushEventLog();
    }
};
EventTap eventTap;
hubbo::remote::RemoteInput input(eventTap);

class InputFeeder : public PayloadSink {
public:
    void onRadioPayload(const uint8_t *bytes, uint8_t length, uint8_t pipe, uint32_t readUs) override {
        if (diag::rawLog()) {
            diag::logPayload(bytes, length, pipe);
        }
        diag::beginPayload(readUs);
        input.onPayload(bytes, length, millis());
        diag::endPayload();
    }
    void onListeningStopped() override { input.releaseAll(millis()); }
};
InputFeeder feeder;
RemoteRadio radio(feeder);
EthernetLink ethernet;

// --- Config/HTTP side ----------------------------------------------------------
// Built at boot on this task; after httpServer.begin() only the httpd task
// touches it (directly in handlers, or through httpServer.post()/every()).

EspRandomSource randomSource;
hubbo::auth::SoftwareHash hash;
NvsSecretStore secretStore;
EspClock apiClock;
hubbo::auth::Credentials credentials(secretStore, hash, randomSource);
hubbo::auth::SessionStore sessions(randomSource);
hubbo::api::Authenticator authenticator(credentials, sessions, apiClock);
hubbo::auth::LoginRateLimiter loginLimiter;
// Durable records ("config", "active") as slot files on LittleFS. If the
// filesystem isn't mounted the store reports Unavailable and every save
// fails; nothing falls back to RAM. Secrets stay in NVS (nvs_secret_store.h).
LittleFsStorage fsStorage;
hubbo::storage::SlotRecordStore records(fsStorage, hubbo::config::kMaxConfigFileBytes);
hubbo::storage::RecordStateStore stateStore(records);
std::unique_ptr<hubbo::config::ConfigService> configService;
hubbo::ActivePersister activePersister(stateStore);
hubbo::config::LoadReport bootConfig;  // for the console; the API reads live state
hubbo::auth::CredentialState bootCredentials = hubbo::auth::CredentialState::Unavailable;
size_t bootTokens = 0;  // credentials belong to httpd after boot
// Read at boot: secretStore.health() opens NVS, which the RF task must not.
hubbo::storage::Health bootSecretsHealth = hubbo::storage::Health::Unavailable;
std::unique_ptr<hubbo::api::Api> api;
hubbo::http::Router router;
HttpServer httpServer;

// Deliberate reboots (`reboot`, `fs format`, OTA) are requests. This task
// never waits on another one (it alone polls the engine, which httpd may be
// waiting on): it releases held keys itself, stops reading the radio, and
// posts the rest to httpd, which writes out the debounced active activity
// and then restarts. The engine keeps being polled until then.
std::atomic<const char *> rebootReason{nullptr};  // set from any task
bool rebooting = false;                             // loopTask: keys released
bool restartPosted = false;

void releaseOutputs() {
    const uint32_t nowMs = millis();
    input.releaseAll(nowMs);
    adapters.releaseAll(nowMs);
}

void flushAndRestart() {
    activePersister.flush(engine.activeActivityIdShared(),
                          configService && configService->state() == hubbo::config::ConfigState::Ok);
    Serial.println("restarting");
    Serial.flush();
    esp_restart();
}

// Any task. `why` must be a string literal.
void requestReboot(const char *why) {
    const char *expected = nullptr;
    rebootReason.compare_exchange_strong(expected, why);
}

// From loop(): release keys here (they're this task's), then hand the
// restart to httpd, retrying if its queue is full. Without a running server
// nothing else uses the persister, so do it here.
void pollReboot() {
    constexpr uint32_t kRestartDeadlineMs = 5000;
    static uint32_t requestedAtMs = 0;
    const char *why = rebootReason.load(std::memory_order_acquire);
    if (why == nullptr) {
        return;
    }
    if (!rebooting) {
        rebooting = true;
        requestedAtMs = millis();
        Serial.printf("reboot requested (%s)\n", why);
        releaseOutputs();
        if (!httpServer.running()) {
            flushAndRestart();
        }
        restartPosted = httpServer.post(&flushAndRestart);
        Serial.println(restartPosted ? "reboot: flush queued on httpd"
                                     : "reboot: couldn't queue the flush; restarting at the deadline");
    }
    // httpd busy or the job lost: don't leave the remote dead. Restart
    // without the flush (at worst the last seconds of active-activity state).
    if (millis() - requestedAtMs >= kRestartDeadlineMs) {
        Serial.println("reboot: deadline passed, restarting without the flush");
        Serial.flush();
        esp_restart();
    }
}

void onOtaWritten() {
    requestReboot("ota");
}

// --- Boot ----------------------------------------------------------------------

const char *resetReasonName(esp_reset_reason_t reason) {
    switch (reason) {
        case ESP_RST_POWERON: return "power-on";
        case ESP_RST_SW: return "software";
        case ESP_RST_PANIC: return "panic";
        case ESP_RST_INT_WDT: return "interrupt watchdog";
        case ESP_RST_TASK_WDT: return "task watchdog";
        case ESP_RST_WDT: return "other watchdog";
        case ESP_RST_BROWNOUT: return "brownout";
        case ESP_RST_DEEPSLEEP: return "deep sleep";
        case ESP_RST_EXT: return "external pin";
        case ESP_RST_USB: return "usb";
        default: return "other";
    }
}

void printStorage() {
    Serial.printf("fs: %s, %lu of %lu KB used\n",
                  flash_fs::mounted() ? "mounted" : "NOT MOUNTED (storage unavailable; `fs format` to fix)",
                  static_cast<unsigned long>(flash_fs::usedBytes() >> 10),
                  static_cast<unsigned long>(flash_fs::totalBytes() >> 10));
}

void mountFilesystem() {
    switch (flash_fs::mount()) {
        case flash_fs::MountResult::Mounted: break;
        case flash_fs::MountResult::FormattedBlank: Serial.println("fs: blank partition, formatted"); break;
        case flash_fs::MountResult::FormattedOnRequest: Serial.println("fs: formatted as requested"); break;
        case flash_fs::MountResult::FormatRefusedMounted:
        case flash_fs::MountResult::FormatRefused:
            Serial.println("fs: !!! FORMAT REQUESTED BUT NOT DONE: the request flag in NVS couldn't be cleared, so"
                           " formatting would repeat every boot. Storage left as it was. Check NVS.");
            break;
        case flash_fs::MountResult::Failed:
            Serial.println("fs: MOUNT FAILED; not blank, so not formatted (`fs format` to erase)");
            break;
    }
    printStorage();
}

void printConfigState(const hubbo::config::LoadReport &r) {
    Serial.printf("config: %s", hubbo::config::configStateName(r.state));
    if (r.generation != 0) {
        Serial.printf(", generation %llu", static_cast<unsigned long long>(r.generation));
    }
    if (r.migratedFrom >= 0) {
        Serial.printf(", migrated from schema %d", r.migratedFrom);
    }
    if (r.reason != hubbo::ErrorCode::Ok) {
        Serial.printf(", %s", hubbo::errorCodeName(r.reason));
    }
    Serial.println();
}

void printCredentialState() {
    Serial.printf("auth: %s, secrets %s", hubbo::auth::credentialStateName(bootCredentials),
                  hubbo::storage::healthName(bootSecretsHealth));
    switch (bootCredentials) {
        case hubbo::auth::CredentialState::Fresh: Serial.println(" (no admin yet: first-run setup open)"); break;
        case hubbo::auth::CredentialState::Ok:
            Serial.printf(", %u API token(s)\n", static_cast<unsigned>(bootTokens));
            break;
        case hubbo::auth::CredentialState::Unavailable:
            Serial.println(" (SECRETS UNREADABLE: login and setup refused)");
            break;
        case hubbo::auth::CredentialState::Corrupt:
            Serial.println(" (SECRETS CORRUPT: login and setup refused)");
            break;
    }
}

// The one boot path (core's bootHub): config, then credentials, then the
// active activity, restored without its startup only when the config is Ok
// and the saved value is readable. Runs in setup(), before the first radio
// read, so the button map is live from the start.
void bootState() {
    configService = std::make_unique<hubbo::config::ConfigService>(records, adapters, configStore);
    const hubbo::BootReport report = hubbo::bootHub(hubbo::ConfigSpec{}, *configService, &credentials, engine,
                                                    activePersister, millis());

    bootConfig = report.config;
    printConfigState(bootConfig);
    const auto &config = configService->config();
    switch (bootConfig.state) {
        case hubbo::config::ConfigState::Fresh:
            Serial.println("config: no saved config; starting empty (no activities)");
            break;
        case hubbo::config::ConfigState::Ok:
            Serial.printf("config: %u activit%s, %u device(s)\n", static_cast<unsigned>(config.activities.size()),
                          config.activities.size() == 1 ? "y" : "ies", static_cast<unsigned>(config.devices.size()));
            break;
        case hubbo::config::ConfigState::Blocked:
            Serial.println("config: BLOCKED; nothing runs and edits are refused until a restore or reset");
            for (const auto &issue : bootConfig.issues.issues()) {
                Serial.printf("  %s at %s\n", hubbo::errorCodeName(issue.code), issue.path.c_str());
            }
            break;
        case hubbo::config::ConfigState::StorageUnavailable:
            Serial.println("config: STORAGE UNAVAILABLE; nothing runs and edits are refused (`fs format`)");
            break;
    }

    bootCredentials = report.credentials;
    bootTokens = credentials.tokens().size();
    bootSecretsHealth = secretStore.health();
    printCredentialState();

    switch (report.active) {
        case hubbo::ActiveRestore::Restored:
            Serial.printf("activity: restored %s (no startup)\n", report.activityId.c_str());
            break;
        case hubbo::ActiveRestore::NoneSaved:
            Serial.println("activity: none saved");
            break;
        case hubbo::ActiveRestore::Unknown:
            Serial.printf("activity: unknown (%s%s%s); none active, saved value kept\n",
                          hubbo::errorCodeName(report.reason), report.activityId.empty() ? "" : ", saved ",
                          report.activityId.c_str());
            break;
    }
}

void startApi() {
    hubbo::api::HubInfo info;
    info.board = board::kName;
    info.hostname = hostname;
    api = std::make_unique<hubbo::api::Api>(
        hubbo::api::ApiContext{credentials, sessions, loginLimiter, apiClock, *configService, runtime, info});
    hubbo::api::registerApiRoutes(router, *api);
    httpServer.begin(router, &authenticator);

    // From now on NVS writes requested on this task (Hub endpoint, BLE bond
    // deletion) run on httpd: the RF task never touches NVS.
    radio.setPersistRunner([](const RadioEndpoint &endpoint, bool erase) {
        if (!httpServer.post([endpoint, erase] { RemoteRadio::persistEndpoint(endpoint, erase); })) {
            Serial.println("radio: couldn't queue saving the Hub endpoint; it won't survive a reboot");
        }
    });
    ble.setOffload([](std::function<void()> job) { return httpServer.post(std::move(job)); });

    // httpd housekeeping, once a second while there's something to do:
    //  - the active activity, saved 5 s after it settles (on a change or
    //    while a write is pending);
    //  - ConfigService::refresh(), which retries a load that failed on
    //    storage once the medium is back (never overwriting a record), while
    //    the config is storage_unavailable.
    static std::atomic<uint32_t> seenChanges{0};
    static std::atomic<bool> writePending{false};
    static std::atomic<bool> configNeedsRefresh{false};
    configNeedsRefresh.store(bootConfig.state == hubbo::config::ConfigState::StorageUnavailable);
    const bool ticking = httpServer.every(
        1000,
        [] {
            if (configNeedsRefresh.load(std::memory_order_acquire)) {
                const hubbo::config::ConfigState state = configService->refresh(apiClock.monotonicMs());
                configNeedsRefresh.store(state == hubbo::config::ConfigState::StorageUnavailable,
                                         std::memory_order_release);
                if (state != hubbo::config::ConfigState::StorageUnavailable) {
                    Serial.printf("config: storage back, now %s\n", hubbo::config::configStateName(state));
                }
            }
            seenChanges.store(activityChanges.load(std::memory_order_acquire), std::memory_order_release);
            activePersister.tick(engine.activeActivityIdShared(), millis(),
                                 configService->state() == hubbo::config::ConfigState::Ok);
            writePending.store(activePersister.pending(), std::memory_order_release);
        },
        [] {
            return configNeedsRefresh.load(std::memory_order_acquire) ||
                   writePending.load(std::memory_order_acquire) ||
                   activityChanges.load(std::memory_order_acquire) != seenChanges.load(std::memory_order_acquire);
        });
    if (!ticking) {
        Serial.println("httpd: housekeeping timer failed; the active activity won't be saved");
    }
}

// --- Running -----------------------------------------------------------------

// A new address: Host allow-list and mDNS on httpd (MDNS.begin can block for
// tens of ms, and the router belongs to httpd); OTA here (cheap, and its
// handle() runs on this task).
void onAddressChanged() {
    httpServer.post([ip = std::string(ethernet.ipText())] {
        static bool mdnsStarted = false;
        if (!mdnsStarted && MDNS.begin(hostname)) {
            mdnsStarted = true;
            MDNS.addService("http", "tcp", HttpServer::kPort);
        }
        const std::string name(hostname);
        router.setAllowedHosts({ip, name, name + ".local"});
    });
    ota::begin(hostname, &releaseOutputs, &onOtaWritten);  // no-op if disabled or already started
}

// Status for the API, copied for the HTTP task a few times a second.
void publishRuntime() {
    static RuntimeStatus s;  // keeps the cached BLE peer between publishes
    s.linkUp = ethernet.up();
    memcpy(s.ip, ethernet.ipText(), sizeof(s.ip));
    const RadioEndpoint ep = radio.endpoint();
    s.remotePaired = ep.isValid();
    s.channel = ep.isValid() ? ep.channel : -1;
    s.lastPacketAtMs = radio.payloadCount() ? radio.lastPayloadMs() : 0;
    s.held = input.held();
    s.hasLastEvent = hasLastEvent;
    s.lastEvent = lastEvent;
    s.badChecksums = input.badChecksumCount();
    s.unknownCodes = input.unknownCodeCount();
    s.queueOverflows = engine.executor().stats().rejected;
    const LatencyStats::Summary latency = diag::hidSummary();
    s.latencyCount = latency.count;
    s.p50Ms = latency.p50 / 1000.0f;
    s.p95Ms = latency.p95 / 1000.0f;
    s.p99Ms = latency.p99 / 1000.0f;
    const auto bleState = static_cast<uint8_t>(ble.state());
    if (bleState != s.bleState) {
        ble.peerAddress(s.blePeer, sizeof(s.blePeer));  // allocates; only on change
        s.bleState = bleState;
    }
    runtime.publish(s);
}

// --- Serial console ------------------------------------------------------------

void printMemory() {
    Serial.printf("mem: heap free %lu min %lu largest %lu | internal free %lu min %lu | psram free %lu"
                  " | loop stack left %lu\n",
                  static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMinFreeHeap()),
                  static_cast<unsigned long>(ESP.getMaxAllocHeap()),
                  static_cast<unsigned long>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned long>(heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL)),
                  static_cast<unsigned long>(ESP.getFreePsram()),
                  static_cast<unsigned long>(uxTaskGetStackHighWaterMark(nullptr)));
}

void printStatus() {
    const uint32_t nowMs = millis();
    const RadioEndpoint ep = radio.endpoint();
    Serial.printf("status: up %lu s\n", static_cast<unsigned long>(nowMs / 1000));
    if (radio.state() == RemoteRadio::State::Scanning) {
        Serial.printf("  radio: scanning (channel %u)\n", radio.scanChannel());
    } else {
        Serial.printf("  radio: %s", RemoteRadio::stateName(radio.state()));
        if (ep.isValid()) {
            Serial.printf(" channel %u address 0x%010llX", ep.channel, static_cast<unsigned long long>(ep.address));
        }
        Serial.printf(", %lu payloads, last %lu ms ago, %lu bad checksums, %lu unknown codes\n",
                      static_cast<unsigned long>(radio.payloadCount()),
                      static_cast<unsigned long>(radio.payloadCount() ? nowMs - radio.lastPayloadMs() : 0),
                      static_cast<unsigned long>(input.badChecksumCount()),
                      static_cast<unsigned long>(input.unknownCodeCount()));
    }
    const auto &es = engine.stats();
    const auto &xs = engine.executor().stats();
    Serial.printf("  engine: %s, %lu events, %lu unbound, %lu dispatched, %lu transitions;"
                  " sequences %lu ok %lu failed %lu rejected %lu timeouts\n",
                  engine.hasActiveActivity() ? engine.activeActivityId() : "(off)",
                  static_cast<unsigned long>(es.events), static_cast<unsigned long>(es.unbound),
                  static_cast<unsigned long>(es.dispatched), static_cast<unsigned long>(es.transitions),
                  static_cast<unsigned long>(xs.completed), static_cast<unsigned long>(xs.failed),
                  static_cast<unsigned long>(xs.rejected), static_cast<unsigned long>(xs.timeouts));
    Serial.printf("  eth: %s %s, http %s (%lu requests), ota %s\n", EthernetLink::stateName(ethernet.state()),
                  ethernet.ipText(), httpServer.running() ? "up" : "down",
                  static_cast<unsigned long>(httpServer.requestCount()), ota::enabled() ? "enabled" : "disabled");
    char peer[18];
    ble.peerAddress(peer, sizeof(peer));
    Serial.printf("  ble: %s", BleHid::stateName(ble.state()));
    if (peer[0] != '\0') {
        Serial.printf(" peer %s", peer);
    }
    if (ble.state() == BleHid::State::Pairing) {
        Serial.printf(" (%lu s left)", static_cast<unsigned long>(ble.pairingSecondsLeft(nowMs)));
    }
    Serial.printf(", notify failures %lu\n", static_cast<unsigned long>(ble.notifyFailures()));
    Serial.print("  at boot ");  // live state: GET /api/v1/status
    printConfigState(bootConfig);
    Serial.print("  at boot ");
    printCredentialState();
    Serial.print("  ");
    printMemory();
    diag::printLatency();
}

void printHelp() {
    Serial.println(
        "commands (activities and devices are driven through the API):\n"
        "  status                 radio / engine / eth / ble / memory / latency\n"
        "  scan [seconds]         learn a Harmony Hub (press its pair button)\n"
        "  scan stop              stop scanning\n"
        "  radio forget           erase the saved Hub endpoint\n"
        "  pair [seconds]         open the BLE pairing window\n"
        "  pair stop              close it\n"
        "  ble forget             delete BLE bonds\n"
        "  raw on|off             print raw radio payloads (distorts latency)\n"
        "  lat [reset]            latency summary, or clear it\n"
        "  mem                    memory and filesystem\n"
        "  fs format              ERASE the config filesystem (reboots, formats at boot)\n"
        "  wdt                    hang the loop to test the watchdog (reboots)\n"
        "  reboot");
}

void runCommand(char *line) {
    const uint32_t nowMs = millis();
    char *cmd = strtok(line, " \t");
    char *arg1 = strtok(nullptr, " \t");
    if (cmd == nullptr) {
        return;
    }
    auto is = [](const char *a, const char *b) { return a != nullptr && strcmp(a, b) == 0; };
    auto seconds = [&](uint32_t fallbackMs) {
        const uint32_t s = arg1 ? strtoul(arg1, nullptr, 10) : 0;
        return s ? s * 1000 : fallbackMs;
    };

    if (is(cmd, "help") || is(cmd, "?")) {
        printHelp();
    } else if (is(cmd, "status")) {
        printStatus();
    } else if (is(cmd, "scan") && is(arg1, "stop")) {
        radio.stopScan();
    } else if (is(cmd, "scan")) {
        if (!radio.startScan(nowMs, seconds(RemoteRadio::kDefaultScanTimeoutMs))) {
            Serial.println("radio: can't scan now (not ready or already scanning)");
        }
    } else if (is(cmd, "radio") && is(arg1, "forget")) {
        radio.forgetEndpoint();
    } else if (is(cmd, "pair") && is(arg1, "stop")) {
        ble.stopPairing();
        Serial.println("ble: pairing window closed");
    } else if (is(cmd, "pair")) {
        if (!ble.startPairing(nowMs, seconds(BleHid::kDefaultPairingWindowMs))) {
            Serial.println("ble: not started");
        }
    } else if (is(cmd, "ble") && is(arg1, "forget")) {
        ble.forget();
    } else if (is(cmd, "raw")) {
        diag::setRawLog(is(arg1, "on"));
        Serial.printf("raw %s\n", diag::rawLog() ? "on" : "off");
    } else if (is(cmd, "lat") && is(arg1, "reset")) {
        diag::resetLatency();
        Serial.println("latency reset");
    } else if (is(cmd, "lat")) {
        diag::printLatency();
    } else if (is(cmd, "mem")) {
        printMemory();
        // Filesystem usage walks flash: httpd prints it.
        if (!httpServer.post(&printStorage)) {
            Serial.println("fs: busy, try again");
        }
    } else if (is(cmd, "fs") && is(arg1, "format")) {
        // Never live: a config save may be running on httpd. The request is
        // an NVS write, so httpd records it; the format runs at next boot.
        const bool posted = httpServer.post([] {
            if (flash_fs::requestFormatAtBoot()) {
                requestReboot("fs format: the format runs at boot");
            } else {
                Serial.println("fs: couldn't record the format request; not rebooting");
            }
        });
        if (!posted) {
            Serial.println("fs: busy, try again");
        }
    } else if (is(cmd, "wdt")) {
        Serial.println("hanging the loop; the task watchdog should reboot in ~5 s");
        Serial.flush();
        for (;;) {
        }
    } else if (is(cmd, "reboot")) {
        requestReboot("console");
    } else {
        Serial.printf("unknown command: %s (try `help`)\n", cmd);
    }
}

void pollSerial() {
    static char line[64];
    static size_t length = 0;
    while (Serial.available() > 0) {
        const int c = Serial.read();
        if (c == '\r' || c == '\n') {
            if (length > 0) {
                line[length] = '\0';
                runCommand(line);
                length = 0;
            }
        } else if (length < sizeof(line) - 1) {
            line[length++] = static_cast<char>(c);
        }
    }
}

}  // namespace

void setup() {
    Serial.begin(115200);
    // Give USB CDC up to 1 s to attach so the boot log isn't lost; on PoE
    // (no USB) this times out.
    while (!Serial && millis() < 1000) {
    }
    snprintf(hostname, sizeof(hostname), "hubbo-%012llX", static_cast<unsigned long long>(ESP.getEfuseMac()));

    Serial.printf("\nHubbo on %s (%s)\n", board::kName, hostname);
    Serial.printf("reset reason: %s\n", resetReasonName(esp_reset_reason()));
    Serial.printf("chip %s rev %u, %u cores, %lu MHz, flash %lu MB, psram %lu KB\n", ESP.getChipModel(),
                  ESP.getChipRevision(), ESP.getChipCores(), static_cast<unsigned long>(ESP.getCpuFreqMHz()),
                  static_cast<unsigned long>(ESP.getFlashChipSize() >> 20),
                  static_cast<unsigned long>(ESP.getPsramSize() >> 10));
    Serial.printf("sdk %s\n", ESP.getSdkVersion());

    mountFilesystem();
    if (!radio.begin()) {
        Serial.println("radio: failed; check wiring and the 10 uF capacitor. Continuing without it.");
    }
    if (ble.begin(HUBBO_BLE_NAME)) {
        randomSource.setEntropyReady(true);  // RF is on: esp_fill_random is now secure
    } else {
        Serial.println("ble: failed. Continuing without it; logins and tokens are refused (no RNG entropy).");
    }
    if (adapters.add(bleAdapter) != hubbo::ErrorCode::Ok) {
        Serial.println("engine: BLE adapter registration failed");
    }
    if (!engine.ready()) {
        Serial.println("engine: no config reader slot; remote input is inert");
    }
    engine.setObserver(&engineLog);
    bootState();
    ethernet.begin(hostname);
    startApi();
    Serial.printf("ota: %s\n", ota::enabled() ? "compiled in, starts when Ethernet is up"
                                              : "disabled (no HUBBO_OTA_PASSWORD at build)");

    printMemory();
    enableLoopWDT();
    Serial.println("ready; type `help`");
    diag::resetLatency();
}

void loop() {
    diag::loopTick();

    // Hot path first: drain the radio (events dispatch straight into the
    // engine), fire due Hold / release timeouts, then run the engine's
    // mailbox, waits, completions and config swaps.
    // While a reboot is pending the radio is no longer read (no new holds),
    // but the engine keeps running: httpd may be waiting on it.
    const bool stopping = rebooting;
    if (!stopping) {
        radio.loop(millis());
    }
    const uint32_t nowMs = millis();
    if (!stopping) {
        input.poll(nowMs);
    }
    engine.poll(nowMs);
    pollReboot();

    ble.loop(nowMs);
    ethernet.loop(nowMs);
    if (ethernet.takeAddressChange()) {
        onAddressChanged();
    }
    ota::loop();

    static uint32_t lastPublishMs = 0;
    if (nowMs - lastPublishMs >= kPublishIntervalMs) {
        lastPublishMs = nowMs;
        publishRuntime();
    }

    diag::printLogs();
    pollSerial();

    static uint32_t lastStatusMs = 0;
    if (nowMs - lastStatusMs >= kStatusIntervalMs) {
        lastStatusMs = nowMs;
        printStatus();
    }
}

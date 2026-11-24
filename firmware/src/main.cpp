// Hubbo firmware for the Waveshare ESP32-S3-ETH: wiring, boot and the main
// loop. Task plan in README "Tasks":
//   loopTask, core 1  RF + engine: radio FIFO -> RemoteInput -> Engine ->
//                     BleHidAdapter -> NimBLE notify. Serial console.
//   httpd,    core 0  API handlers, ConfigService, and everything posted to
//                     it with HttpServer::post()/every().
#include <Arduino.h>
#include <esp_heap_caps.h>
#include <esp_system.h>
#include <esp_timer.h>
#include <atomic>
#include <cstdio>
#include <cstring>
#include <functional>
#include <memory>
#include <new>
#include <string>
#include <string_view>
#include <vector>

#include "console.h"
#include "ble_hid.h"
#include "commands.h"
#include "board.h"
#include "dev_console.h"
#include "diagnostics.h"
#include "engine_runtime.h"
#include "esp_clock.h"
#include "esp_hash.h"
#include "esp_random_source.h"
#include "ethernet_link.h"
#include "factory_reset.h"
#include "firmware_releases.h"
#include "firmware_update.h"
#include "flash_fs.h"
#include "integration_task.h"
#include "ir_transmitter.h"
#include "http_server.h"
#include "hub_name.h"
#include "image_marker.h"
#include "hubbo/api/api.h"
#include "hubbo/api/remote_port.h"
#include "hubbo/api/transitions.h"
#include "hubbo/api/authenticator.h"
#include "hubbo/auth/credentials.h"
#include "hubbo/auth/crypto.h"
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
#include "hubbo/integrations/ha_adapter.h"
#include "hubbo/integrations/ha_client.h"
#include "hubbo/integrations/ir.h"
#include "hubbo/integrations/mqtt_bridge.h"
#include "hubbo/remote/button.h"
#include "hubbo/remote/input.h"
#include "hubbo/remote/log.h"
#include "hubbo/storage/record_store.h"
#include "hubbo/storage/state_store.h"
#include "littlefs_storage.h"
#include "mqtt_transport.h"
#include "nvs_secret_store.h"
#include "ota.h"
#include "update_lock.h"
#include "remote_radio.h"
#include "restart.h"
#include "ble_host_runtime.h"
#include "remote_runtime.h"
#include "tls_memory.h"
#include "ws_transport.h"

using hubbo::remote::RemoteEvent;
using namespace hubbo::fw;
using diag::logf;

namespace {

constexpr uint32_t kStatusIntervalMs = 30000;
constexpr uint32_t kPublishIntervalMs = 250;

HubName hubName;  // Bluetooth and mDNS names, live; DHCP and OTA at boot
RmtIrTransmitter irTransmitter;  // DRAM: its interrupt writes into it

// --- RF/engine side (declaration order = construction order) -----------------

BleHid ble;
diag::TimedHidTransport timedHid(ble);
#ifdef HUBBO_REMOTE_LOG
// The remote log (dev builds, docs/design/remote-log.md): allocated in PSRAM
// in setup(); null if that fails.
class PortMuxLogLock : public hubbo::remote::LogLock {
public:
    void lock() override { portENTER_CRITICAL(&mux_); }
    void unlock() override { portEXIT_CRITICAL(&mux_); }

private:
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
PortMuxLogLock remoteLogLock;
hubbo::remote::RemoteLog *remoteLog = nullptr;
diag::LoggedHidTransport loggedHid(timedHid);
hubbo::integrations::HidTransport &hid = loggedHid;
#else
hubbo::integrations::HidTransport &hid = timedHid;
#endif
hubbo::integrations::BleHidAdapter bleAdapter(hubbo::kBleIntegrationId, hid);
// Home Assistant: the transport and client live on the HA task; the adapter
// bridges the engine task (execute/poll) and the HA task (service).
EspWsTransport haTransport;
// The client's clock: read fresh at every poll and callback (the same time
// base as poll()'s nowMs), so a slow TLS connect doesn't age its deadlines.
uint64_t haNowMs() {
    return static_cast<uint64_t>(esp_timer_get_time() / 1000);
}
hubbo::integrations::HomeAssistantClient haClient(haTransport, haNowMs);
hubbo::integrations::HomeAssistantAdapter haAdapter(hubbo::api::kHaIntegrationId, haClient);
// IR: the adapter runs on the engine task; the transmitter's own task sends.
hubbo::integrations::IrAdapter irAdapter(hubbo::kIrIntegrationId, irTransmitter);
hubbo::AdapterSet adapters;
hubbo::Engine::Store configStore;
hubbo::Engine engine(configStore, adapters);
EngineRuntime runtime(engine, adapters, haClient);

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
        logAction(r.atMs, r.integrationId, r.target, r.action.command, "", r.result);
    }
    // A Pending action's outcome (e.g. a wait_connected that timed out), when
    // it failed; successes stay quiet.
    void onActionCompleted(const hubbo::CompletionRecord &r) override {
        if (r.result.error != hubbo::ErrorCode::Ok) {
            logAction(r.atMs, r.integrationId, r.target, r.command, " done", r.result);
        }
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

private:
    // `action <ms> <integration> <target> <command>[ done] -> <status>[ <error>]`
    static void logAction(uint32_t atMs, const char *integrationId, const char *target,
                          const hubbo::CommandSpec *command, const char *suffix, const hubbo::ExecResult &result) {
        flushEventLog();
        const bool ok = result.error == hubbo::ErrorCode::Ok;
        logf("action %lu %s %s %s%s -> %s%s%s", static_cast<unsigned long>(atMs), integrationId,
             target[0] ? target : "-", command != nullptr ? command->id : "?", suffix,
             hubbo::execStatusName(result.status), ok ? "" : " ", ok ? "" : hubbo::errorCodeName(result.error));
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
    void onRadioPayload(const uint8_t *bytes, uint8_t length, uint8_t pipe, uint32_t readUs,
                        hubbo::remote::Signal signal) override {
        if (diag::rawLog()) {
            diag::logPayload(bytes, length, pipe);
        }
        diag::beginPayload(readUs);
        input.onPayload(bytes, length, millis(), signal);
        diag::endPayload();
    }
    void onListeningStopped() override { input.releaseAll(millis()); }
};
InputFeeder feeder;
RemoteRadio radio(feeder);
RemoteRuntime remoteRuntime(radio);  // the admin API's view of the radio
BleHostRuntime bleHosts(ble);   // the admin API's view of the Bluetooth hosts
// Forgets the hosts of Bluetooth devices the saved config no longer has
// (each saved config, the one loaded at boot included).
hubbo::integrations::BleHostReconciler bleReconciler(bleHosts);
EthernetLink ethernet;

// --- Config/HTTP side ----------------------------------------------------------
// Built at boot on this task; after httpServer.begin() only the httpd task
// touches it (directly in handlers, or through httpServer.post()/every()).

EspRandomSource randomSource;
EspHash hash;  // hardware SHA-256
NvsSecretStore secretStore;
EspClock apiClock;
hubbo::auth::Credentials credentials(secretStore, hash, randomSource);
hubbo::auth::SessionStore sessions(randomSource);
hubbo::api::Authenticator authenticator(credentials, sessions, apiClock);
// Durable records ("config", "active") as slot files on LittleFS. If the
// filesystem isn't mounted the store reports Unavailable and every save
// fails; nothing falls back to RAM. Secrets stay in NVS (nvs_secret_store.h).
LittleFsStorage fsStorage;
hubbo::storage::SlotRecordStore records(fsStorage, hubbo::config::kMaxConfigFileBytes);
hubbo::storage::RecordStateStore stateStore(records);
// The Ethernet MAC (efuse), read once: the default hub name's suffix and
// the MQTT node id.
struct EthernetMac {
    uint8_t bytes[6];
    EthernetMac() { EthernetLink::mac(bytes); }
};
const EthernetMac ethernetMac;
// The hub's name ("hub_name"). A hub never renamed is "Hubbo XXXX", from the
// last two bytes of the Ethernet MAC.
hubbo::config::HubSettings hubSettings(stateStore, hubbo::config::defaultHubName(ethernetMac.bytes));
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
IntegrationTask haTask;
IntegrationTask mqttTask;  // the MQTT bridge (below)

// Loop task priority (Arduino's default is 1). Above every task without a
// core affinity (the W5500 driver's RX task runs at 15, and at 1 it took
// core 1 for 10-17 ms at a time), so those run on core 0 and core 1 belongs
// to the radio loop. Tasks pinned to core 1 above it (IPC, Arduino network
// events) still preempt it, briefly.
constexpr UBaseType_t kLoopPriority = 16;

// Ethernet starts from a short task pinned to core 0, so the W5500's GPIO
// and SPI interrupts are installed there rather than on the loop's core.
void beginEthernetOnCore0() {
    // Static: on a timeout the task may still finish after setup() moves on.
    static struct {
        SemaphoreHandle_t done;
        UBaseType_t stackLeft;
    } init{xSemaphoreCreateBinary(), 0};
    constexpr uint32_t kStackBytes = 4096;
    constexpr uint32_t kWaitMs = 10000;
    const bool started =
        init.done != nullptr &&
        xTaskCreatePinnedToCore(
            [](void *) {
                ethernet.begin(hubName.bootHostname());
                init.stackLeft = uxTaskGetStackHighWaterMark(nullptr);
                xSemaphoreGive(init.done);
                vTaskDelete(nullptr);
            },
            "eth_init", kStackBytes, nullptr, 5, nullptr, 0) == pdPASS;
    if (!started) {
        con.println("eth: couldn't start the core-0 init task; starting Ethernet on this core");
        ethernet.begin(hubName.bootHostname());
        return;
    }
    // Never block boot on it: the loop and its watchdog aren't running yet.
    // On a timeout the task may still be inside ETH.begin(), so it isn't
    // called a second time; Ethernet comes up if that call ever finishes.
    if (xSemaphoreTake(init.done, pdMS_TO_TICKS(kWaitMs)) != pdTRUE) {
        con.println("eth: init still running after 10 s; continuing boot without waiting");
        return;
    }
    con.printf("eth: init stack %lu of %lu bytes unused\n", static_cast<unsigned long>(init.stackLeft),
               static_cast<unsigned long>(kStackBytes));
}

// Before a restart. The console's TX timeout is 0 (never wait for the USB
// host), and with that HWCDC's flush() drops what's queued instead of sending
// it, so the last lines before the reboot were lost.
void flushSerial() {
    Serial.setTxTimeoutMs(200);
    Serial.flush();
}

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

// Every deliberate restart ends here (any task): the loop's stats survive it
// (diag::keepLoopStatsAcrossRestart), so a stall before it can be read later.
void restartNow() {
    diag::keepLoopStatsAcrossRestart(rebootReason.load(std::memory_order_acquire));
    flushSerial();
    esp_restart();
}

// Normally on httpd, queued behind the handler that asked for the reboot,
// so its response has been handed to lwIP: first a moment for lwIP to put it
// on the wire before the restart cuts the connection.
void flushAndRestart() {
    constexpr uint32_t kDrainMs = 100;
    vTaskDelay(pdMS_TO_TICKS(kDrainMs));
    activePersister.flush(engine.activeActivityIdShared(),
                          configService && configService->state() == hubbo::config::ConfigState::Ok);
    con.println("restarting");
    restartNow();
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
        con.printf("reboot requested (%s)\n", why);
        releaseOutputs();
        if (!httpServer.running()) {
            flushAndRestart();
        }
        restartPosted = httpServer.post(&flushAndRestart);
        con.println(restartPosted ? "reboot: flush queued on httpd"
                                     : "reboot: couldn't queue the flush; restarting at the deadline");
    }
    // httpd busy or the job lost: don't leave the remote dead. Restart
    // without the flush (at worst the last seconds of active-activity state).
    if (millis() - requestedAtMs >= kRestartDeadlineMs) {
        con.println("reboot: deadline passed, restarting without the flush");
        restartNow();
    }
}

void onOtaWritten() {
    requestReboot("ota");
}

FirmwareUpdate firmwareUpdate(&requestReboot, hash);  // updates from the web admin
// Online updates: checks, release installs and the new image's trial.
FirmwareReleases releases(firmwareUpdate, stateStore, hubSettings, ethernet, hash,
                          {[](std::function<void()> job) { return httpServer.post(std::move(job)); },
                           &requestReboot});
FactoryReset factoryReset(&requestReboot);           // POST /admin/factory-reset
Restart restart(&requestReboot);                     // POST /admin/restart

// --- MQTT: Hubbo as a Home Assistant device (docs/design/mqtt-discovery.md) ---
// The bridge and its transport run on the mqtt task. Its hooks only read
// shared state or post to httpd: transitions and the restart go through the
// same calls as the API's routes, on the API's task.

// Runs `job` on httpd, or logs why it couldn't.
void postFromMqtt(const char *what, std::function<void()> job) {
    if (!httpServer.post(std::move(job))) {
        con.printf("mqtt: %s dropped (httpd queue full)\n", what);
    }
}

EspMqttTransport mqttTransport;
hubbo::integrations::MqttBridge mqttBridge(
    mqttTransport, hubbo::integrations::mqttNodeId(ethernetMac.bytes),
    hubbo::integrations::MqttBridge::Hooks{
        [] { return activityChanges.load(std::memory_order_acquire); },
        [] { return engine.activeActivityIdShared(); },
        [] {
            hubbo::integrations::MqttDeviceInfo info;
            info.version = image::version();
            info.board = board::kName;
            info.ip = runtime.ip();
            static const std::string mac = hubbo::integrations::mqttMacText(ethernetMac.bytes);
            info.mac = mac;
            info.hostname = hubSettings.identity().shownHostname();
            info.uptimeS = static_cast<uint32_t>(apiClock.monotonicMs() / 1000);
            return info;
        },
        [](const std::string &activityId) {
            postFromMqtt("transition", [activityId] {
                const hubbo::api::TransitionOutcome outcome =
                    hubbo::api::runTransition(*configService, runtime, activityId);
                if (outcome.status != hubbo::api::TransitionStatus::Done) {
                    con.printf("mqtt: transition to %s refused (%s)\n", activityId.empty() ? "(off)" : activityId.c_str(),
                               outcome.status == hubbo::api::TransitionStatus::Busy ? "engine busy" : "config blocked");
                }
            });
        },
        [] {
            postFromMqtt("restart", [] {
                const hubbo::api::RestartOutcome outcome = hubbo::api::requestHubRestart(&restart, &firmwareUpdate);
                if (outcome != hubbo::api::RestartOutcome::Accepted) {
                    con.printf("mqtt: restart refused (%s)\n", outcome == hubbo::api::RestartOutcome::UpdateInProgress
                                                                   ? "a firmware update is running"
                                                                   : "unavailable");
                }
            });
        },
    });

// Each saved config (the one loaded at boot included): Bluetooth hosts the
// config no longer has are forgotten, then the bridge gets the activities.
hubbo::integrations::MqttActivitySync mqttActivities(mqttBridge);
hubbo::config::ConfigFanOut configListeners{&bleReconciler, &mqttActivities};

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

void mountFilesystem() {
    switch (flash_fs::mount()) {
        case flash_fs::MountResult::Mounted: break;
        case flash_fs::MountResult::FormattedBlank: con.println("fs: blank partition, formatted"); break;
        case flash_fs::MountResult::FormattedOnRequest: con.println("fs: formatted as requested"); break;
        case flash_fs::MountResult::FormatRefused:
            con.println("fs: !!! FORMAT REQUESTED BUT NOT DONE: the request flag in NVS couldn't be cleared, so"
                           " formatting would repeat every boot. Storage left as it was. Check NVS.");
            break;
        case flash_fs::MountResult::Failed:
            con.println("fs: MOUNT FAILED; not blank, so not formatted (`fs format` to erase)");
            break;
    }
    commands::printStorage();
}

// The saved name (or the default) for Bluetooth, mDNS, DHCP and OTA, and
// renames from the API applied live (hub_name.h).
void loadHubName() {
    const hubbo::ErrorCode loaded = hubSettings.load();
    const hubbo::config::HubIdentity identity = hubSettings.identity();
    hubName.begin(identity);
    mqttBridge.setHubName(identity.name);
    hubSettings.onChanged([](const hubbo::config::HubIdentity &renamed) {
        hubName.apply(renamed);
        mqttBridge.setHubName(renamed.name);
    });
    con.printf("name: \"%s\", hostname %s\n", identity.name.c_str(), identity.hostname.c_str());
    if (loaded != hubbo::ErrorCode::Ok && loaded != hubbo::ErrorCode::NotFound) {
        con.printf("name: saved name unreadable (%s); using the default\n", hubbo::errorCodeName(loaded));
    }
}

void printConfigState(const hubbo::config::LoadReport &r) {
    con.printf("config: %s", hubbo::config::configStateName(r.state));
    if (r.generation != 0) {
        con.printf(", generation %llu", static_cast<unsigned long long>(r.generation));
    }
    if (r.migratedFrom >= 0) {
        con.printf(", migrated from schema %d", r.migratedFrom);
    }
    if (r.reason != hubbo::ErrorCode::Ok) {
        con.printf(", %s", hubbo::errorCodeName(r.reason));
    }
    con.println();
}

void printCredentialState() {
    con.printf("auth: %s, secrets %s", hubbo::auth::credentialStateName(bootCredentials),
                  hubbo::storage::healthName(bootSecretsHealth));
    switch (bootCredentials) {
        case hubbo::auth::CredentialState::Fresh: con.println(" (no admin yet: first-run setup open)"); break;
        case hubbo::auth::CredentialState::Ok:
            con.printf(", %u API token(s)\n", static_cast<unsigned>(bootTokens));
            break;
        case hubbo::auth::CredentialState::Unavailable:
            con.println(" (SECRETS UNREADABLE: login and setup refused)");
            break;
    }
}

// For `status`: config and credentials as found at boot (live state: GET
// /api/v1/status).
void printBootState() {
    con.print("  at boot ");
    printConfigState(bootConfig);
    con.print("  at boot ");
    printCredentialState();
}

// The one boot path (core's bootHub): config, then credentials, then the
// active activity, restored without its startup only when the config is Ok
// and the saved value is readable. Runs in setup(), before the first radio
// read, so the button map is live from the start.
void bootState() {
    configService = std::make_unique<hubbo::config::ConfigService>(records, adapters, configStore);
    configService->setListener(&configListeners);
    const hubbo::BootReport report = hubbo::bootHub(hubbo::factoryDefaultConfig(), *configService, &credentials, engine,
                                                    activePersister, millis());

    bootConfig = report.config;
    printConfigState(bootConfig);
    const auto &config = configService->config();
    switch (bootConfig.state) {
        case hubbo::config::ConfigState::Fresh:
            con.println("config: no saved config; starting empty (no activities)");
            break;
        case hubbo::config::ConfigState::Ok:
            con.printf("config: %u activit%s, %u device(s)\n", static_cast<unsigned>(config.activities.size()),
                          config.activities.size() == 1 ? "y" : "ies", static_cast<unsigned>(config.devices.size()));
            break;
        case hubbo::config::ConfigState::Blocked:
            con.println("config: BLOCKED; nothing runs and edits are refused until a restore or reset");
            for (const auto &issue : bootConfig.issues.issues()) {
                con.printf("  %s at %s\n", hubbo::errorCodeName(issue.code), issue.path.c_str());
            }
            break;
        case hubbo::config::ConfigState::StorageUnavailable:
            con.println("config: STORAGE UNAVAILABLE; nothing runs and edits are refused (`fs format`)");
            break;
    }

    bootCredentials = report.credentials;
    bootTokens = credentials.tokens().size();
    bootSecretsHealth = secretStore.health();
    printCredentialState();

    switch (report.active) {
        case hubbo::ActiveRestore::Restored:
            con.printf("activity: restored %s (no startup)\n", report.activityId.c_str());
            break;
        case hubbo::ActiveRestore::NoneSaved:
            con.println("activity: none saved");
            break;
        case hubbo::ActiveRestore::Unknown:
            con.printf("activity: unknown (%s%s%s); none active, saved value kept\n",
                          hubbo::errorCodeName(report.reason), report.activityId.empty() ? "" : ", saved ",
                          report.activityId.c_str());
            break;
    }
}

// The HA client's settings at boot (core's helper: the "ha_main" URL, none
// while it's missing or disabled, and the token from NVS). Later changes come
// through the API. The token is never logged.
void configureHomeAssistant() {
    const hubbo::auth::SecretRead read =
        hubbo::api::applyHomeAssistantSettings(*configService, &secretStore, haClient);
    const hubbo::IntegrationSpec *integration = configService->findIntegration(hubbo::api::kHaIntegrationId);
    const bool hasUrl = integration != nullptr && integration->enabled &&
                        !hubbo::api::homeAssistantSocketUrl(integration->url).empty();
    con.printf("ha: %s, token %s\n", hasUrl ? "configured" : "not configured", hubbo::auth::secretReadName(read));
}

// The bridge's settings at boot (core's helper: "mqtt_main" and the password
// from NVS); the API applies later changes. The password is never logged.
void configureMqtt() {
    const hubbo::auth::SecretRead read = hubbo::api::applyMqttSettings(*configService, &secretStore, mqttBridge);
    const hubbo::IntegrationSpec *integration = configService->findIntegration(hubbo::kMqttIntegrationId);
    const bool enabled = integration != nullptr && integration->enabled;
    con.printf("mqtt: %s, node %s, password %s\n",
               !enabled ? "disabled" : integration->host.empty() ? "no broker set" : "configured",
               mqttBridge.nodeId().c_str(), hubbo::auth::secretReadName(read));
}

// The mqtt task: the transport's wait is the pass's sleep. Each change of
// the bridge's state is logged once ("mqtt: connected").
void startMqtt() {
    const bool started = mqttTask.start("mqtt", [] {
        static const char *logged = nullptr;
        mqttTransport.poll(IntegrationTask::kSocketWaitMs);
        mqttBridge.poll(apiClock.monotonicMs());
        const char *state = mqttBridge.state();
        if (logged == nullptr || strcmp(state, logged) != 0) {
            logged = state;
            const std::string detail = mqttBridge.detail();
            con.printf("mqtt: %s%s%s\n", state, detail.empty() ? "" : ": ", detail.c_str());
        }
        return false;
    });
    if (!started) {
        con.println("mqtt: task failed to start; MQTT is unavailable");
    }
}

void startApi() {
    hubbo::api::HubInfo info;
    info.version = image::version();
    info.board = board::kName;
    hubbo::api::ApiContext context{credentials, sessions, apiClock, *configService, runtime, info};
    context.remote = &remoteRuntime;
    context.bleHosts = &bleHosts;
    context.firmware = &firmwareUpdate;
    context.releases = &releases;
    context.factoryReset = &factoryReset;
    context.hub = &hubSettings;
#ifdef HUBBO_REMOTE_LOG
    context.remoteLog = remoteLog;
#endif
    context.restart = &restart;
    context.mqtt = &mqttBridge;
    context.homeAssistant = &haClient;
    context.secrets = &secretStore;
    api = std::make_unique<hubbo::api::Api>(std::move(context));
    api->setYield([] { vTaskDelay(1); });  // discovery normalization on httpd, every 32 candidates
    hubbo::api::registerApiRoutes(router, *api);
    httpServer.begin(router, &authenticator);

    // From now on NVS writes requested on this task (Hub endpoint, BLE bond
    // deletion) run on httpd: the RF task never touches NVS.
    radio.setPersistRunner([](const RadioEndpoint &endpoint, bool erase) {
        if (!httpServer.post([endpoint, erase] { RemoteRadio::persistEndpoint(endpoint, erase); })) {
            con.println("radio: couldn't queue saving the Hub endpoint; it won't survive a reboot");
        }
    });
    ble.setOffload([](std::function<void()> job) { return httpServer.post(std::move(job)); });
    dev_console::begin({httpServer, runtime, irTransmitter, releases, firmwareUpdate});
    if (!httpServer.serveWebAdmin()) {  // last: /api and /debug match first
        con.println("http: web admin handler failed to register");
    }

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
                    con.printf("config: storage back, now %s\n", hubbo::config::configStateName(state));
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
        con.println("httpd: housekeeping timer failed; the active activity won't be saved");
    }
}

// --- Running -----------------------------------------------------------------

// A new address: mDNS on httpd (its calls wait on the mDNS task, and
// renames run there too); OTA here (cheap, and its handle() runs on this
// task).
void onAddressChanged() {
    httpServer.post([] { hubName.startMdns(hubSettings, HttpServer::kPort); });
    ota::begin(hubName.bootHostname(), &releaseOutputs, &onOtaWritten);  // no-op if disabled or already started
}

// A rename's Bluetooth half (hub_name.h): BleHid is this task's.
void pollHubName() {
    static HubNameString name;
    if (hubName.takeBleName(name)) {
        ble.setName(name.view());
    }
}

// Status for the API, copied for the HTTP task a few times a second.
void publishRuntime() {
    RuntimeStatus s;
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
    const uint32_t nowMs = millis();
    runtime.publish(s);
    remoteRuntime.publish(nowMs);
    bleHosts.publish(nowMs);
}

}  // namespace

void setup() {
    con.begin(115200);
    // Give USB CDC up to 1 s to attach so the boot log isn't lost; on PoE
    // (no USB) this times out.
    while (!Serial && millis() < 1000) {
    }
    con.printf("\nHubbo %s on %s\n", image::version(), board::kName);
    con.printf("reset reason: %s\n", resetReasonName(esp_reset_reason()));
    diag::printPreviousRun();
    con.printf("chip %s rev %u, %u cores, %lu MHz, flash %lu MB, psram %lu KB\n", ESP.getChipModel(),
                  ESP.getChipRevision(), ESP.getChipCores(), static_cast<unsigned long>(ESP.getCpuFreqMHz()),
                  static_cast<unsigned long>(ESP.getFlashChipSize() >> 20),
                  static_cast<unsigned long>(ESP.getPsramSize() >> 10));
    con.printf("sdk %s\n", ESP.getSdkVersion());

    commands::begin({radio, input, engine, ble, ethernet, httpServer, irTransmitter, &printBootState, &requestReboot,
                     &flushSerial});
    factory_reset::wipeIfRequested();  // POST /admin/factory-reset before this restart, or a gate build
    mountFilesystem();
    loadHubName();
    if (!radio.begin(hubSettings.radioPower())) {
        con.println("radio: failed; check wiring and the 10 uF capacitor. Continuing without it.");
    }
    if (ble.begin(hubSettings.identity().name.c_str())) {
        randomSource.setEntropyReady(true);  // RF is on: esp_fill_random is now secure
    } else {
        con.println("ble: failed. Continuing without it; logins and tokens are refused (no RNG entropy).");
    }
    if (adapters.add(bleAdapter) != hubbo::ErrorCode::Ok) {
        con.println("engine: BLE adapter registration failed");
    }
    if (adapters.add(haAdapter) != hubbo::ErrorCode::Ok) {
        con.println("engine: Home Assistant adapter registration failed");
    }
    if (adapters.add(irAdapter) != hubbo::ErrorCode::Ok) {
        con.println("engine: IR adapter registration failed");
    }
    if (!irTransmitter.begin()) {  // IR commands then fail adapter_unavailable
        con.println("ir: task failed to start; IR is unavailable");
    }
    if (!engine.ready()) {
        con.println("engine: no config reader slot; remote input is inert");
    }
    engine.setObserver(&engineLog);
    // The run history (~14 KB) lives in PSRAM; it's set once, before the loop.
    if (void *mem = heap_caps_malloc(sizeof(hubbo::RunHistory), MALLOC_CAP_SPIRAM)) {
        engine.setRunHistory(new (mem) hubbo::RunHistory());
    } else {
        con.println("engine: no PSRAM for the run history; runs aren't recorded");
    }
#ifdef HUBBO_REMOTE_LOG
    if (void *mem = heap_caps_malloc(sizeof(hubbo::remote::RemoteLog), MALLOC_CAP_SPIRAM)) {
        remoteLog = new (mem) hubbo::remote::RemoteLog(remoteLogLock);
        input.setLog(remoteLog);
        engine.setRemoteLog(remoteLog);
        diag::setRemoteLog(remoteLog);
        con.printf("remote log: on, %u KB in PSRAM\n", static_cast<unsigned>(sizeof(hubbo::remote::RemoteLog) >> 10));
    } else {
        con.println("remote log: no PSRAM; off");
    }
#endif
    // The config lives for the whole uptime: parse it into PSRAM, keeping
    // internal RAM for lwIP and the drivers started below (prebuilt rule).
    commands::printMemory("boot: before config ");
    heap_caps_malloc_extmem_enable(0);
    bootState();
    heap_caps_malloc_extmem_enable(CONFIG_SPIRAM_MALLOC_ALWAYSINTERNAL);
    commands::printMemory("boot: after config ");
    runtime.setConfig(configService.get());
    runtime.setBleHosts(&bleHosts);
    runtime.setMqtt(&mqttBridge);
    runtime.setIrTransmitter(&irTransmitter);
    update_lock::setRebootPending([] { return rebootReason.load(std::memory_order_acquire) != nullptr; });
    beginEthernetOnCore0();
    startApi();
    tls::useExternalMemory();  // mbedTLS from PSRAM first (tls_memory.h)
    configureHomeAssistant();
    haClient.setYield([] { vTaskDelay(1); });
    const bool haStarted = haTask.start("ha", [] {
        // A connect blocks here for up to 15 s (TLS); the client reads its
        // clock (haNowMs) fresh, so its deadlines start after it.
        haTransport.poll(haClient.hasWork() ? 0 : IntegrationTask::kSocketWaitMs);
        haClient.poll(haNowMs());
        haAdapter.service();
        return haClient.hasWork();
    });
    if (!haStarted) {
        con.println("ha: task failed to start; Home Assistant is unavailable");
    }
    configureMqtt();
    con.printf("ota: %s\n", ota::enabled() ? "compiled in, starts when Ethernet is up"
                                              : "disabled (no HUBBO_OTA_PASSWORD at build)");

    // From here on every malloc/new tries PSRAM first, then internal RAM (the
    // prebuilt sdkconfig only does that for blocks of 4 KB and up). A big
    // restore's small allocations otherwise use up internal RAM. Drivers,
    // all started above, ask for internal or DMA memory explicitly. See README.
    heap_caps_malloc_extmem_enable(0);
    startMqtt();  // after: esp-mqtt's client and buffers go to PSRAM
    releases.boot(bootConfig.state != hubbo::config::ConfigState::StorageUnavailable);  // loaded: ok, blocked or fresh
    if (!releases.start()) {
        con.println("update: task failed to start; no update checks");
    }

    commands::printMemory();
    enableLoopWDT();
    vTaskPrioritySet(nullptr, kLoopPriority);
    con.println("ready; type `help`");
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
        radio.setPower(hubSettings.radioPower());  // a change from PUT /admin/hub/settings
        radio.loop(millis());
    }
    const uint32_t nowMs = millis();
    if (!stopping) {
        input.poll(nowMs);
        remoteRuntime.poll(nowMs);
        bleHosts.poll(nowMs);
    }
    engine.poll(nowMs);
    pollReboot();

    pollHubName();
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
    commands::poll();
    dev_console::poll();

    static uint32_t lastStatusMs = 0;
    if (nowMs - lastStatusMs >= kStatusIntervalMs) {
        lastStatusMs = nowMs;
        commands::printStatus();
    }
}

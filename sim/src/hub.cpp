#include "hub.h"

#include "hubbo/domain/trigger.h"
#include "hubbo/remote/packet.h"

namespace hubbo::sim {

namespace {

constexpr uint32_t kKeepaliveMs = 40;  // the remote's hold packet interval
constexpr uint32_t kPersistTickMs = 100;

// Any valid non-button payload keeps RemoteInput's release timeout alive.
// These bytes are arbitrary (not captured from a remote); only "not a button
// packet" matters.
constexpr uint8_t kKeepalive[5] = {0x00, 0x4F, 0x00, 0x00, 0xB1};

}  // namespace

SimHub::SimHub(storage::Storage *storage, storage::StateStore *state, bool realHomeAssistant)
    : realHomeAssistant_(realHomeAssistant),
      fakes_(realHomeAssistant ? &ha_.adapter : nullptr),
      storage_(storage != nullptr ? storage : &memory_),
      records_(*storage_, config::kMaxConfigFileBytes),
      service_(records_, fakes_.set, store_),
      recordState_(records_),
      state_(state != nullptr ? *state : recordState_),
      persister_(state_) {
    engine_.setObserver(this);
    fakes_.hid.pairing = &blePairing_;
    resetBle();
    factoryReset_ = freshFactoryResetPort();
}

config::LoadReport SimHub::boot(const ConfigSpec &defaults, auth::Credentials *credentials) {
    boot_ = bootHub(defaults, service_, credentials, engine_, persister_, nowMs_);  // the firmware's boot path too
    return boot_.config;
}

ValidationReport SimHub::load(const ConfigSpec &config) {
    config::Outcome outcome = service_.restore(config);
    if (!outcome.ok() && outcome.issues.ok()) outcome.issues.add(outcome.code, "");
    engine_.poll(nowMs_);  // pick it up now
    return outcome.issues;
}

void SimHub::stopEverything() {
    held_ = 0;
    input_.releaseAll(nowMs_);
    if (engine_.hasActiveActivity()) engine_.off(nowMs_);
    forgetActive();
    remote_ = api::SimulatedRemote();
}

api::SimulatedFactoryReset SimHub::freshFactoryResetPort() {
    api::SimulatedFactoryReset port;
    port.erase = [this] {
        if (onFactoryReset) onFactoryReset();
    };
    return port;
}

ValidationReport SimHub::factoryReset(const ConfigSpec &config) {
    stopEverything();  // nothing stays held across a reset
    fakes_.homeAssistant.resetFaults();
    ha_.server.reset();
    ha_.client.reconnect();  // at once, not after the backoff; stale entities forgotten
    resetBle();
    fakes_.hid.setConnected(true);
    firmware_ = api::SimulatedFirmwareUpdate();
    factoryReset_ = freshFactoryResetPort();
    return load(config);
}

void SimHub::wipe(auth::Credentials *credentials) {
    stopEverything();
    blePairing_ = api::SimulatedBlePairing();  // unbonded
    // The board formats the filesystem; here the config record's slots
    // (SlotRecordStore's "<name>.a"/".b") go.
    for (const char slot : {'a', 'b'}) {
        (void)storage_->remove(std::string(config::ConfigService::kRecordName) + "." + slot);
    }
    boot(ConfigSpec{}, credentials);  // the firmware's first boot and defaults
    engine_.poll(nowMs_);
}

void SimHub::advanceFirmware(uint32_t ms) {
    const api::FirmwareUpdateState before = firmware_.status().state;
    firmware_.advance(ms);  // write, then the simulated reboot
    const bool updating = before == api::FirmwareUpdateState::Writing || before == api::FirmwareUpdateState::Rebooting;
    if (!updating || firmware_.status().state != api::FirmwareUpdateState::Idle) return;
    // Idle after Writing or Rebooting only comes from a finished reboot.
    if (onFirmwareRebooted) onFirmwareRebooted();
}

void SimHub::resetBle() {
    blePairing_ = api::SimulatedBlePairing();
    blePairing_.bonded(kBondedHost);  // the fixture world: the Apple TV is paired
}

void SimHub::press(remote::Button button) {
    held_ |= remote::buttonBit(button);
    sendGroup(remote::buttonInfo(button).group);
}

void SimHub::release(remote::Button button) {
    held_ &= ~remote::buttonBit(button);
    sendGroup(remote::buttonInfo(button).group);
}

void SimHub::tap(remote::Button button, uint32_t holdMs) {
    press(button);
    advance(holdMs);
    release(button);
}

void SimHub::sendGroup(uint8_t group) {
    uint8_t packet[remote::kButtonPacketLength];
    remote::encodePacket(group, held_ & remote::groupMask(group), packet);
    input_.onPayload(packet, sizeof(packet), nowMs_);
    lastPacketMs_ = nowMs_;
    anyPacket_ = true;
    engine_.poll(nowMs_);
}

void SimHub::onRemoteEvent(const remote::RemoteEvent &event) {
    lastEvent_ = event;
    hasLastEvent_ = true;
    engine_.onRemoteEvent(event);
}

void SimHub::advance(uint32_t ms, bool silent) {
    for (uint32_t i = 0; i < ms; i++) {
        nowMs_++;
        if (!silent && held_ != 0 && nowMs_ - lastPacketMs_ >= kKeepaliveMs) {
            input_.onPayload(kKeepalive, sizeof(kKeepalive), nowMs_);
            lastPacketMs_ = nowMs_;
        }
        input_.poll(nowMs_);
        serviceIntegrations();
        engine_.poll(nowMs_);
        // The config side remembers the active activity (delayed write),
        // ticking like the firmware's HTTP task does, not every ms.
        if (nowMs_ % kPersistTickMs == 0) {
            service_.refresh(nowMs_);  // recover a load that failed on storage
            persister_.tick(engine_.activeActivityIdShared(), nowMs_,
                            service_.state() == config::ConfigState::Ok);
        }
    }
    remote_.advance(ms);      // scan timeout
    blePairing_.advance(ms);  // pairing window timeout
    advanceFirmware(ms);
    factoryReset_.advance(ms);  // the restart, then its erase
    // The radio gave up on them; the simulated thumb lets go too.
    if (silent && input_.held() == 0) held_ = 0;
}

void SimHub::serviceIntegrations() {
    do {
        ha_.transport.poll(0);
        ha_.client.poll(nowMs_);
        ha_.adapter.service();
        ha_.transport.poll(0);
    } while (ha_.client.hasWork());
}

void SimHub::logCommand(std::string text) {
    commands_.push_back({nowMs_, std::move(text)});
    if (onLog) onLog("command", commands_.back());
}

void SimHub::logEvent(std::string text) {
    events_.push_back({nowMs_, std::move(text)});
    if (onLog) onLog("event", events_.back());
}

void SimHub::onActionDispatched(const DispatchRecord &record) {
    const bool activate = record.action.op == ActionOp::Activate;
    std::string line = record.integrationId;
    line += ' ';
    line += (!activate && record.target[0] != '\0') ? record.target : "-";
    line += ' ';
    line += record.action.command != nullptr ? record.action.command->id : "?";
    if (activate) {
        line += std::string(" activity_id=") + record.target;  // the param holds an index
    } else if (record.action.command != nullptr) {
        const std::string params = formatParams(*record.action.command, record.action.params, record.action.paramsPresent);
        if (!params.empty()) line += ' ' + params;
    }
    line += " -> ";
    line += execStatusName(record.result.status);
    if (record.result.status == ExecStatus::Failed) {
        line += std::string(" ") + errorCodeName(record.result.error);
    }
    logCommand(std::move(line));
}

void SimHub::onSequenceFinished(const SequenceReport &report) {
    std::string line = sequenceOriginName(report.info.origin);
    line += ' ';
    line += report.info.activityId.empty() ? "-" : report.info.activityId.c_str();
    if (report.info.origin == SequenceOrigin::Binding || report.info.origin == SequenceOrigin::GlobalBinding) {
        line += ' ' + formatTrigger(report.info.trigger);
    }
    line += ' ';
    line += sequenceOutcomeName(report.outcome);
    if (report.outcome != SequenceOutcome::Completed) {
        line += std::string(" ") + errorCodeName(report.error) + "@" + std::to_string(report.actionIndex);
    }
    logEvent(std::move(line));
}

void SimHub::onActivityChanged(const char *activityId, uint32_t) {
    logEvent(std::string("activity ") + (activityId[0] != '\0' ? activityId : "none"));
}

void SimHub::onTransitionRejected(ErrorCode error, const char *activityId, uint32_t) {
    std::string line = std::string("transition_rejected ") + errorCodeName(error);
    if (activityId[0] != '\0') line += std::string(" ") + activityId;
    logEvent(std::move(line));
}

void SimHub::onConfigChanged(uint32_t revision, uint32_t) { logEvent("config revision=" + std::to_string(revision)); }

void SimHub::onUnbound(const remote::RemoteEvent &event) {
    logEvent("unbound " + formatTrigger(Trigger{event.button, event.kind}));
}

}  // namespace hubbo::sim

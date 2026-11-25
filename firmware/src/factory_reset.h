#pragma once

// The factory reset (POST /admin/factory-reset) and the gate wipe share one
// wipe: erase all of NVS (the login, API tokens, the HA token, BLE bonds, the
// Hub endpoint), then request
// flash_fs's format at boot, which mountFilesystem() runs next. OTA keeps
// working: its password hash is compiled in.
//
// The wipe runs at boot, early in setup(), before anything opens NVS or the
// filesystem and before the radio starts. The API only records the request
// (one NVS flag, on httpd) and asks for a normal reboot, which releases held
// keys and sends the 202 first. So a reset that's been answered 202 happens
// even if the restart is forced (the reboot deadline) or the power drops.

#include "hubbo/api/factory_reset_port.h"

namespace hubbo::fw {

namespace factory_reset {

// Boot only, early in setup(), before NVS or the filesystem is used: wipes
// if a factory reset was requested before this restart, or on every boot in
// a HUBBO_WIPE_ON_BOOT gate build. Logs "WIPE: <why>; ...".
void wipeIfRequested();

}  // namespace factory_reset

class FactoryReset : public hubbo::api::FactoryResetPort {
public:
    // `reboot` requests the restart (any task; a string literal).
    explicit FactoryReset(void (*reboot)(const char *why)) : reboot_(reboot) {}

    // httpd. False while ArduinoOTA or a web update holds the owner flag,
    // another reboot is pending, or the request can't be saved.
    bool requestFactoryReset() override;

private:
    void (*reboot_)(const char *);
};

}  // namespace hubbo::fw

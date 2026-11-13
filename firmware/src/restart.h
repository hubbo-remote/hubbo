#pragma once

// POST /admin/restart (api::RestartPort): a plain restart, through the
// normal reboot path (held keys released on the loop, then the flush and
// restart queued on httpd, so it runs after this response has been sent).

#include "hubbo/api/restart_port.h"

namespace hubbo::fw {

class Restart : public hubbo::api::RestartPort {
public:
    // `reboot` requests the restart (any task; a string literal).
    explicit Restart(void (*reboot)(const char *why)) : reboot_(reboot) {}

    // httpd. True: the hub restarts after this response (a repeat is true
    // too). False while ArduinoOTA or a web update holds the owner flag, or
    // another reboot is pending. Once true, no update can start.
    bool requestRestart() override;

private:
    void (*reboot_)(const char *);
};

}  // namespace hubbo::fw

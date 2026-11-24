#pragma once

#include <cstdint>

// One update at a time: ArduinoOTA (ota.h), a web update
// (firmware_update.h), a factory reset (factory_reset.h) and a restart
// (restart.h) each claim this owner flag first. While anyone holds it, or a
// reboot is pending, nobody else starts writing the slot.

namespace hubbo::fw::update_lock {

// Reboot: a restart or factory reset, held until the restart. Either one
// asking while the other is pending is accepted, and both happen at once.
enum class Owner : uint8_t { Idle, Web, Arduino, Reboot };

// Any task. True if `who` now holds the flag, or already did (a holder's
// repeat claim succeeds even with a reboot pending). Otherwise false once a
// reboot is pending (setRebootPending), so nothing starts writing the slot
// after an update has finished.
bool claim(Owner who);
// Whether claim(who) would succeed now, without claiming.
bool claimable(Owner who);
void release(Owner who);  // no-op unless `who` holds it
bool holds(Owner who);
// Once at boot, before anything claims.
void setRebootPending(bool (*pending)());

}  // namespace hubbo::fw::update_lock

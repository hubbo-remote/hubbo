#include "factory_reset.h"

#include <Preferences.h>
#include <nvs_flash.h>

#include "console.h"
#include "flash_fs.h"
#include "ota.h"

namespace hubbo::fw {

namespace {
constexpr const char *kPrefsNamespace = "hubbo_reset";
constexpr const char *kPrefsRequested = "requested";
}  // namespace

namespace factory_reset {

namespace {

void wipe(const char *why) {
    const esp_err_t nvs = nvs_flash_erase();
    const esp_err_t init = nvs_flash_init();
    const bool format = flash_fs::requestFormatAtBoot();  // writes NVS: after the erase
    con.printf("WIPE: %s; nvs erase %s, init %s, fs format %s\n", why, esp_err_to_name(nvs),
               esp_err_to_name(init), format ? "requested" : "NOT requested");
}

}  // namespace

void wipeIfRequested() {
#ifdef HUBBO_WIPE_ON_BOOT
    wipe("factory reset build");  // gate builds only (scripts/wipe_on_boot.py)
#else
    Preferences prefs;
    if (!prefs.begin(kPrefsNamespace, true)) {
        return;  // namespace never created: never requested
    }
    const bool requested = prefs.getUChar(kPrefsRequested, 0) != 0;
    prefs.end();
    if (requested) {
        wipe("factory reset");  // the erase clears the request too
    }
#endif
}

}  // namespace factory_reset

bool FactoryReset::requestFactoryReset() {
    // Held until the restart: no update can start after this. A repeat
    // request, or one after a restart was accepted, finds it held already;
    // a failed save then leaves it held (only httpd claims Reboot, so the
    // check and the claim can't race).
    const bool alreadyHeld = ota::holds(ota::Owner::Reboot);
    if (!ota::claim(ota::Owner::Reboot)) {
        return false;  // an update is writing, or another reboot is pending
    }
    Preferences prefs;
    const bool saved = prefs.begin(kPrefsNamespace, false) && prefs.putUChar(kPrefsRequested, 1) == 1;
    prefs.end();
    if (!saved) {
        if (!alreadyHeld) {
            ota::release(ota::Owner::Reboot);
        }
        con.println("factory reset: couldn't save the request");
        return false;
    }
    con.println("factory reset: requested; wiping at the restart");
    reboot_("factory reset");
    return true;
}

}  // namespace hubbo::fw

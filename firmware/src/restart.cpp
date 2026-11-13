#include "restart.h"

#include "console.h"
#include "ota.h"

namespace hubbo::fw {

bool Restart::requestRestart() {
    if (!ota::claim(ota::Owner::Reboot)) {
        return false;  // an update is writing, or another reboot is pending
    }
    con.println("restart: requested");
    reboot_("restart");
    return true;
}

}  // namespace hubbo::fw

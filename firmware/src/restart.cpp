#include "restart.h"

#include "console.h"
#include "update_lock.h"

namespace hubbo::fw {

bool Restart::requestRestart() {
    if (!update_lock::claim(update_lock::Owner::Reboot)) {
        return false;  // an update is writing, or another reboot is pending
    }
    con.println("restart: requested");
    reboot_("restart");
    return true;
}

}  // namespace hubbo::fw

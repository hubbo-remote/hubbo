#include "update_lock.h"

#include <atomic>

namespace hubbo::fw::update_lock {
namespace {
std::atomic<Owner> currentOwner{Owner::Idle};
bool (*rebootPending)() = nullptr;
}  // namespace

bool claimable(Owner who) {
    const Owner holder = currentOwner.load();
    if (holder == who) {
        return true;
    }
    return holder == Owner::Idle && !(rebootPending != nullptr && rebootPending());
}

bool claim(Owner who) {
    if (!claimable(who)) {
        return false;
    }
    Owner expected = Owner::Idle;
    return currentOwner.compare_exchange_strong(expected, who) || expected == who;
}

bool holds(Owner who) {
    return currentOwner.load() == who;
}

void release(Owner who) {
    Owner expected = who;
    currentOwner.compare_exchange_strong(expected, Owner::Idle);
}

void setRebootPending(bool (*pending)()) {
    rebootPending = pending;
}

}  // namespace hubbo::fw::update_lock

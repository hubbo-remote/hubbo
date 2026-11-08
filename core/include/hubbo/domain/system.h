#pragma once

#include "hubbo/domain/params.h"

// Built-in "system" integration. It has no devices (device_id must be null)
// and is executed by the core itself, not by an adapter:
//
//   wait     {duration_ms: int 0..60000}   non-blocking pause in a sequence
//   activate {activity_id: activity_ref}   switch activity (last in a binding)
//   off      {}                            run current shutdown, clear (last in a binding)
//
// activate/off are only allowed as the last action of a binding (they end the
// current context) and never inside startup/shutdown, which keeps transitions
// from chaining.
namespace hubbo {

enum class SystemCommand : uint8_t { Wait, Activate, Off };

CommandCatalog systemCatalog();  // indices match SystemCommand

}  // namespace hubbo

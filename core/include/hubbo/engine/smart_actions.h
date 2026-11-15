#pragma once

#include <vector>

#include "hubbo/domain/config.h"
#include "hubbo/domain/params.h"

// What a smart-button slot does, derived from its device
// (docs/design/smart-buttons.md, "The actions, by type"). The compiler
// builds the compiled slots from it; the API shows it read-only.
namespace hubbo {

// One press: actions for a tap, for Hold firing, and for the Up after a
// Hold (never after a tap).
struct SmartPressActions {
    std::vector<ActionSpec> tap;
    std::vector<ActionSpec> hold;
    std::vector<ActionSpec> release;
};

struct SmartSlotActions {
    SmartPressActions button;  // the slot's own button
    SmartPressActions plus;    // + while this slot is selected
    SmartPressActions minus;   // - while this slot is selected
};

// The slot's actions for `device` by its kind, each only if `catalog` (the
// device's commands) offers `<entity domain>.<service>`: a light without
// brightness gets no +/-, a cover without a position opens and closes. Any
// other kind, or a device that isn't Home Assistant, gets none.
SmartSlotActions smartSlotActions(const DeviceSpec &device, const CommandCatalog &catalog);

}  // namespace hubbo

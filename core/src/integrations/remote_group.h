#pragma once

// The `remote` command group a profile device offers (Bluetooth and IR):
// press {command, hold_ms?}, press_down {command} (released by press_up),
// press_up {command}. Internal to the integrations.

#include <cstdint>
#include <vector>

#include "hubbo/domain/limits.h"
#include "hubbo/domain/params.h"

namespace hubbo::integrations::detail {

struct RemoteGroup {
    std::vector<const char *> names;  // the `command` options: the profile's command names, in order
    ParamSpec pressParams[2];
    ParamSpec heldParams[1];

    // Fills specs[0..2] (press, press_down, press_up) once `names` is set.
    // `data` is the adapter's per-catalog pointer; `pressTimeoutMs` press's
    // executor backstop.
    void fill(CommandSpec *specs, const void *data, uint32_t pressTimeoutMs) {
        const auto count = static_cast<uint16_t>(names.size());
        heldParams[0] = ParamSpec::enumeration("command", names.data(), count).described("Remote command");
        pressParams[0] = heldParams[0];
        pressParams[1] = ParamSpec::integer("hold_ms", 1, static_cast<int32_t>(limits::kMaxHoldMs), false)
                             .described(nullptr, "ms");
        using E = CommandEffect;
        specs[0] = {"press", pressParams, 2, E::Momentary, nullptr, kGroup, data, pressTimeoutMs};
        specs[1] = {"press_down", heldParams, 1, E::Hold, "press_up", kGroup, data, 0};
        specs[2] = {"press_up", heldParams, 1, E::Momentary, nullptr, kGroup, data, 0};
    }

    static constexpr const char *kGroup = "remote";
};

}  // namespace hubbo::integrations::detail

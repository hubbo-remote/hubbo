#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "hubbo/domain/params.h"

// Source (uncompiled) configuration model. Field names mirror the public
// JSON (ARCHITECTURE.md "Data model"):
//
//   Action    {integration_id, device_id|null, command_id, params}
//   Activity  {id, name, startup: Action[], shutdown: Action[],
//              bindings: {"<button>:<event>": Action[]}, revision}
//   Device    {id, name, integration_id, provider ref}
//
// These are plain cold-path structs (they allocate). The config/API layer
// fills them from JSON; the compiler validates them and produces the
// allocation-free runtime tables (engine/compiled.h). Nothing here is ever
// executed directly.
namespace hubbo {

struct ActionSpec {
    std::string integrationId;            // integration_id
    std::optional<std::string> deviceId;  // device_id; nullopt == JSON null
    std::string commandId;                // command_id
    std::vector<ParamInput> params;       // params (object, in any order)
};

struct BindingSpec {
    std::string trigger;  // "<button>:<event>"; validated, so kept as text
    std::vector<ActionSpec> actions;  // empty == unbound
};

struct ActivitySpec {
    std::string id;
    std::string name;
    std::vector<ActionSpec> startup;
    std::vector<ActionSpec> shutdown;
    std::vector<BindingSpec> bindings;
    uint32_t revision = 0;
};

struct DeviceSpec {
    std::string id;
    std::string name;
    std::string integrationId;  // integration_id
    std::string providerRef;    // internal target, e.g. HA entity/registry id; never API-executable
};

struct ConfigSpec {
    std::vector<DeviceSpec> devices;
    std::vector<ActivitySpec> activities;
    // Bindings that apply whatever activity is active (including none), when
    // the active activity does not bind the same trigger. This is where the
    // remote's activity buttons and Off live (system.activate / system.off).
    std::vector<BindingSpec> globalBindings;
    uint32_t revision = 0;
};

}  // namespace hubbo

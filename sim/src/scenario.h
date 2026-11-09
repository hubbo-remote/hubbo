#pragma once

// Scripted scenarios for hub-sim: plain text, one command per line,
// '#' starts a comment. Time only moves when the script says so, so every
// run is deterministic. The first failed expectation stops the scenario.
//
// Setup
//   reset                         fixture config, nothing active, faults cleared
//                                 (a fresh hub at t=0 unless the hub is pinned)
//   activate <activity_id>        as the API would (engine.activate)
//   off
//   ha latency <ms>               HA results arrive later (Pending); 0 = immediate
//   ha fail <command_id> [error]  next call of that command fails (default adapter_failed)
//   ha available | unavailable
//   ble connected | disconnected
//   reboot                        power-cycle: same storage, t=0, config and
//                                 the saved active activity restored (no startup)
// Remote (real radio payloads through RemoteInput)
//   press <button>                button down, held until release
//   release <button>
//   tap <button> [ms]             press, hold ms (default 100), release
//   hold <button> <ms>            same as tap with an explicit duration
//   advance <ms>                  time passes; keepalives sent while held
//   silence <ms>                  time passes with no packets (lost release)
// Expectations
//   expect <line>                 next unchecked command equals <line>;
//                                 " -> status" may be omitted
//   expect_none                   no unchecked commands left
//   expect_event <line>           some later event equals <line> (skips others)
//   expect_no_event <line>        no unchecked event equals <line>
//   expect_hid <report>           next unchecked HID report, e.g. "consumer 0x0042"
//   expect_active <id|none>
//   expect_result <error_code>    result of the last activate/off ("ok", ...)
//   drain                         mark everything so far as checked
//   print                         print unchecked commands/events

#include <cstddef>
#include <iosfwd>
#include <memory>
#include <string>

#include "hub.h"

namespace hubbo::sim {

class ScenarioRunner {
public:
    // `trace` (optional) receives a live log of everything that happens.
    // `storage` (optional) persists config across runs (hub-sim serve
    // --data); the hub boots from it. `pinned` keeps one hub for the
    // runner's lifetime (others hold references to it), so `reset` resets it
    // in place instead of replacing it.
    // `seed` is the config used on first boot and by `reset` (default:
    // fixtureConfig()).
    // `state` (optional) overrides where the active activity persists
    // (default: a record in the same storage, surviving `reboot`).
    // `credentials` (optional) are loaded by every boot (bootHub()), as on
    // the firmware.
    explicit ScenarioRunner(std::ostream *trace = nullptr, storage::Storage *storage = nullptr,
                            bool pinned = false, const ConfigSpec *seed = nullptr,
                            storage::StateStore *state = nullptr, auth::Credentials *credentials = nullptr,
                            bool realHomeAssistant = false);

    // Runs a whole script; returns true if every line succeeded.
    bool run(std::istream &script, const std::string &name);
    // Runs one line; false (and error()) on failure.
    bool runLine(const std::string &line);

    const std::string &error() const { return error_; }
    SimHub &hub() { return *hub_; }

private:
    void reset();
    void reboot();
    void newHub();
    bool fail(std::string message);

    std::ostream *trace_;
    storage::MemoryStorage ownStorage_;  // when none is given; survives reboot
    storage::Storage *storage_;
    storage::StateStore *state_;
    auth::Credentials *credentials_;
    bool pinned_;
    bool realHomeAssistant_;
    ConfigSpec seed_;
    std::unique_ptr<SimHub> hub_;
    size_t commandCursor_ = 0;
    size_t eventCursor_ = 0;
    size_t hidCursor_ = 0;
    ErrorCode lastResult_ = ErrorCode::Ok;
    std::string error_;
};

}  // namespace hubbo::sim

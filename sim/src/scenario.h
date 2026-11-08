#pragma once

// Scripted scenarios for hub-sim: plain text, one command per line,
// '#' starts a comment. Time only moves when the script says so, so every
// run is deterministic. The first failed expectation stops the scenario.
//
// Setup
//   reset                         fresh hub, fixture config loaded, t=0
//   activate <activity_id>        as the API would (engine.activate)
//   off
//   ha latency <ms>               HA results arrive later (Pending); 0 = immediate
//   ha fail <command_id> [error]  next call of that command fails (default adapter_failed)
//   ha available | unavailable
//   ble connected | disconnected
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
    explicit ScenarioRunner(std::ostream *trace = nullptr);

    // Runs a whole script; returns true if every line succeeded.
    bool run(std::istream &script, const std::string &name);
    // Runs one line; false (and error()) on failure.
    bool runLine(const std::string &line);

    const std::string &error() const { return error_; }
    SimHub &hub() { return *hub_; }

private:
    void reset();
    bool fail(std::string message);

    std::ostream *trace_;
    std::unique_ptr<SimHub> hub_;
    size_t commandCursor_ = 0;
    size_t eventCursor_ = 0;
    size_t hidCursor_ = 0;
    ErrorCode lastResult_ = ErrorCode::Ok;
    std::string error_;
};

}  // namespace hubbo::sim

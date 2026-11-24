#pragma once

// GET /debug/stats (dev builds): what the board budget checks
// (firmware/budget.json, README "Board budget") as one JSON object, which
// `bin/hubbo board-check` reads. Only the fields a budget rule or the check
// itself uses.

#include "engine_runtime.h"
#include "http_server.h"
#include "ir_transmitter.h"

namespace hubbo::fw {

// httpd. `reset` closes the window (diag::closeWindow()) and answers for all
// of it, starting the next; the tasks' CPU share is rebased with it. Only a
// closed window has the loop and latency fields. 503 if the loop didn't
// answer the close (itself a failure; the next reset returns that window).
HttpServer::DebugReply boardStats(EngineRuntime &runtime, const RmtIrTransmitter &ir, bool reset);

}  // namespace hubbo::fw

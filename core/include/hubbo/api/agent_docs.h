#pragma once

#include <cstddef>

#include "hubbo/http/route.h"

// The docs an LLM agent reads from the hub: the OpenAPI spec as JSON, the
// agent guide (docs/agent-api.md) and llms.txt (docs/llms.txt). Generated at build time from the repo
// copies by core/tools/embed_docs.py (core's CMake and the firmware's
// PlatformIO build both run it), so the hub serves what the repo says. In
// flash on the board, no heap.
namespace hubbo::api::docs {

struct Doc {
    const char *raw;   // the document itself
    size_t rawSize;
    const char *gzip;  // the same, gzipped (for Accept-Encoding: gzip)
    size_t gzipSize;
    const char *etag;  // quoted, from the raw bytes
};

extern const Doc kOpenApiJson;  // api/openapi.yaml as compact JSON
extern const Doc kAgentGuide;   // docs/agent-api.md, byte for byte

constexpr char kOpenApiPath[] = "/api/v1/openapi.json";
constexpr char kAgentGuidePath[] = "/api/v1/agent.md";  // where errors point (Router::pointErrorsTo)

// GET /llms.txt: docs/llms.txt, which points to the two above.
extern const char kLlmsTxt[];
void sendLlmsTxt(http::Response &out);  // hub-sim also answers its bare URL with it

}  // namespace hubbo::api::docs

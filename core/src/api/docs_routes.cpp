// What an LLM agent finds on the hub with no prior knowledge: GET /api/v1/
// (an index), and /api/v1/openapi.json, /api/v1/agent.md and /llms.txt
// (embedded at build time, see hubbo/api/agent_docs.h). All public.

#include "hubbo/api/agent_docs.h"
#include "routes.h"
#include "support.h"

namespace hubbo::api {

namespace {

// The document, gzipped when the client takes gzip; 304 on a current ETag.
void sendDoc(const http::Request &request, http::Response &out, const docs::Doc &doc, const char *contentType) {
    out.status = 200;
    out.contentType = contentType;
    out.headers.emplace_back("Vary", "Accept-Encoding");
    out.headers.emplace_back("Cache-Control", "no-cache");
    out.headers.emplace_back("ETag", doc.etag);
    const auto header = [&](std::string_view name) {
        return request.headers ? request.headers->header(name) : std::string_view();
    };
    if (header("If-None-Match") == doc.etag) {
        out.status = 304;
        out.body.clear();
        return;
    }
    if (header("Accept-Encoding").find("gzip") != std::string_view::npos) {
        out.headers.emplace_back("Content-Encoding", "gzip");
        out.body.assign(doc.gzip, doc.gzipSize);
    } else {
        out.body.assign(doc.raw, doc.rawSize);
    }
}

struct LlmsTxt : http::RouteHandler {
    void handle(const http::Request &, const http::PathParams &, http::Response &out) override {
        docs::sendLlmsTxt(out);
    }
};

}  // namespace

void docs::sendLlmsTxt(http::Response &out) {
    out.status = 200;
    out.contentType = "text/markdown; charset=utf-8";
    out.body = kLlmsTxt;
}

void addDocsRoutes(RouteSink &sink, ApiContext &ctx) {
    sink.add("/api/v1", auth::Operation::GetApiIndex, [&ctx](auto &, auto &, auto &out) {
        JsonDocument doc;
        doc["name"] = "Hubbo API";
        doc["version"] = ctx.info.version;
        doc["about"] =
            "Read docs (the agent guide) first: it covers everyday tasks. openapi is the full reference (large): use "
            "it to look up an exact schema or error.";
        doc["docs"] = docs::kAgentGuidePath;
        doc["openapi"] = docs::kOpenApiPath;
        doc["auth"] = "Authorization: Bearer <token>. An admin creates tokens in Settings → API tokens.";
        sendJson(out, 200, doc);
    });
    sink.add(docs::kOpenApiPath, auth::Operation::GetOpenApi, [](auto &request, auto &, auto &out) {
        sendDoc(request, out, docs::kOpenApiJson, "application/json");
    });
    sink.add(docs::kAgentGuidePath, auth::Operation::GetAgentGuide, [](auto &request, auto &, auto &out) {
        sendDoc(request, out, docs::kAgentGuide, "text/markdown; charset=utf-8");
    });
    // Outside the API and its policy table: the llms.txt convention's place.
    sink.handlers.push_back(std::make_unique<LlmsTxt>());
    sink.router.addHostRoute(http::Method::Get, "/llms.txt", *sink.handlers.back(), auth::Requirement::Public);
    // A 401, or an unknown /api path, points an agent to the guide.
    sink.router.pointErrorsTo("api", docs::kAgentGuidePath);
}

}  // namespace hubbo::api

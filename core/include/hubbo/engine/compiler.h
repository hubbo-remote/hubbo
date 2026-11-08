#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

#include "hubbo/domain/adapter.h"
#include "hubbo/domain/config.h"
#include "hubbo/domain/error.h"
#include "hubbo/engine/compiled.h"

// Validation and compilation of source config into runtime tables.
//
// This is the security and correctness gate: every action must name a
// registered integration, a registered device owned by that integration, a
// command the integration offers for that device, and parameters that match
// the command's typed schema. Anything else is rejected with a structured
// error and a JSON Pointer path into the source document, e.g.
//   {code: "invalid_enum_value", path: "/activities/0/bindings/up:down/0/params/key"}
// Runs on the config writer side; allocates freely; never on the hot path.
namespace hubbo {

struct ValidationIssue {
    ErrorCode code;
    std::string path;  // JSON Pointer (RFC 6901) into the validated document
};

class ValidationReport {
public:
    // Keeps the first kMaxValidationIssues issues; counts the rest.
    void add(ErrorCode code, std::string path);

    bool ok() const { return issues_.empty(); }
    const std::vector<ValidationIssue> &issues() const { return issues_; }
    size_t dropped() const { return dropped_; }

    // True if an issue has this code and path (handy in tests).
    bool has(ErrorCode code, const std::string &path) const;

private:
    std::vector<ValidationIssue> issues_;
    size_t dropped_ = 0;
};

// Validates and compiles a whole configuration. Returns nullptr (and fills
// `report`) on any error; a partial config is never produced. Paths are
// rooted at the config document: /devices/N/..., /activities/N/...,
// /global_bindings/<trigger>/N/...
std::unique_ptr<CompiledConfig> compileConfig(const ConfigSpec &config, const AdapterSet &adapters,
                                              ValidationReport *report);

// Validates one activity as it would be saved into `context` (replacing an
// activity with the same id, or added). Paths are rooted at the activity
// document: /id, /startup/N/..., /bindings/<trigger>/N/params/<name>.
// Use for PUT /activities/{id} and POST /activities/validate (dry run).
ValidationReport validateActivity(const ActivitySpec &activity, const ConfigSpec &context,
                                  const AdapterSet &adapters);

// JSON Pointer token escaping ("~" -> "~0", "/" -> "~1").
std::string jsonPointerToken(const std::string &token);

}  // namespace hubbo

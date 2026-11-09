#pragma once

#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hubbo/config/json.h"
#include "hubbo/engine/engine.h"
#include "hubbo/storage/record_store.h"

// ConfigService: the single owner of the hub's configuration.
// docs/design/persistence-and-contracts.md, sections 1-3.
//
// The hub runs exactly what's saved. A commit goes: build the candidate ->
// serialize -> parse the bytes back -> compile the parsed config -> save it
// as the next generation of the "config" record -> publish the parsed and
// compiled result. If any step fails, nothing changes: the saved record,
// the running config and the engine keep the last good version. A field
// that doesn't survive the round trip can't be live.
//
// States (state()):
//   Fresh               no intact config record yet; running the defaults
//                       (which are saved by the first commit or at load).
//   Ok                  running the newest intact record.
//   Blocked             an intact record exists but can't be used (newer
//                       schema, failed migration, doesn't compile). Nothing
//                       runs (no activities, no bindings), both slots are left
//                       untouched, and every edit is refused with
//                       ConfigBlocked. Only restore()/reset() may write.
//   StorageUnavailable  the medium isn't usable: every edit fails with
//                       StorageFailed. If the last load itself couldn't read,
//                       nothing runs; if storage went away later, the loaded
//                       config keeps running.
//
// state() follows the medium live: Ok/Fresh on a medium that isn't
// Available reads as StorageUnavailable, and back once it is. Blocked
// stays Blocked whatever the medium does. A load that failed on storage is
// retried by refresh(): at once on a health transition, then with
// exponential backoff (1 s doubling to 60 s) while a slot stays unreadable
// on an available medium. The retry obeys the record store's rules, so an
// unreadable slot is never mistaken for an empty store and nothing can be
// overwritten. Every edit runs the same steps in the same order: editable
// check, revision check, build, round trip, compile, save, publish.
//
// One generation counter (section 2): config().revision is the generation
// of the saved record; every commit bumps it; each object's revision is the
// generation of its last change; restore/reset stamp everything with the
// new generation. No zeros.
//
// Integrations (section 3): config().integrations is authoritative and
// always holds the built-ins ("system", "ble").
//
// Threading: one task (the HTTP/config task) calls everything here. The
// engine only sees published snapshots. Nothing here runs on a button press
// or activity switch.
namespace hubbo::config {

enum class ConfigState : uint8_t { Fresh, Ok, Blocked, StorageUnavailable };
const char *configStateName(ConfigState state);  // "fresh", "ok", "blocked", "storage_unavailable"

// Where something refers to a device or activity: the activity id ("" for
// the global bindings) and a JSON Pointer inside that document, e.g.
// {"act_watch_apple_tv", "/bindings/volume_up:down/0"}.
struct Referrer {
    std::string activityId;
    std::string path;
};

struct Outcome {
    // Ok, NotFound, RevisionConflict, ValidationFailed, DeviceInUse,
    // Referenced, LimitReached, StorageFailed, ConfigBlocked, Busy.
    ErrorCode code = ErrorCode::Ok;
    uint32_t currentRevision = 0;     // RevisionConflict: the object's current revision
    ValidationReport issues;          // ValidationFailed: paths rooted at the request body
    std::vector<Referrer> referrers;  // DeviceInUse / Referenced: who refers to it
    std::string id;                   // the created/changed object
    uint32_t revision = 0;            // its revision (= the new generation) after the change

    bool ok() const { return code == ErrorCode::Ok; }
};

struct LoadReport {
    ConfigState state = ConfigState::Fresh;
    // Blocked: why (Corrupt, UnsupportedSchema, MigrationFailed,
    // MalformedJson, ValidationFailed); StorageUnavailable: StorageFailed; else Ok (or
    // StorageFailed if a Fresh/migrated config couldn't be saved).
    ErrorCode reason = ErrorCode::Ok;
    uint64_t generation = 0;   // of the record loaded or blocked; 0 if none
    int migratedFrom = -1;     // schema version migrated from, else -1
    bool saved = false;        // load wrote a record (fresh defaults, migration)
    ValidationReport issues;   // Blocked by a compile failure: what failed (config-rooted paths)
};

class ConfigService {
public:
    static constexpr const char *kRecordName = "config";

    // `migrate` replaces the built-in migrations (tests only).
    ConfigService(storage::RecordStore &records, const AdapterSet &adapters, Engine::Store &store,
                  Migration migrate = nullptr);

    // Boot: load the newest intact "config" record (or start Fresh from
    // `defaults`), migrate, compile and publish. See the states above.
    LoadReport load(const ConfigSpec &defaults);

    ConfigState state() const;  // live; see above
    // Retries a load that failed on storage (see above). Cheap otherwise:
    // call it often (API requests, a 1 Hz tick) with a monotonic clock.
    ConfigState refresh(uint64_t nowMs);
    const LoadReport &lastLoad() const { return load_; }
    // While Blocked: the saved record's raw payload (and generation), for a
    // backup that exports it marked as blocked instead of the empty runtime
    // config. Reads the store (config task; up to kMaxConfigFileBytes).
    // Ok; NotFound (not Blocked, or Blocked by invalid defaults with no
    // record); Corrupt (no intact copy to export); StorageFailed.
    ErrorCode blockedRecord(std::string *payload, uint64_t *generation = nullptr);

    // Restore (backup) and factory reset: replace everything, stamped with
    // the next generation. Allowed in every state but StorageUnavailable;
    // leaves Blocked on success. `dryRun`: the same pipeline (built-ins,
    // inapplicable fields dropped, stamping, size limit, round trip,
    // compile) without the save; Outcome.revision is the generation it would
    // get. A dry run works while storage is unavailable.
    Outcome restore(ConfigSpec config, bool dryRun = false);
    Outcome reset(const ConfigSpec &defaults) { return restore(defaults); }

    const ConfigSpec &config() const { return config_; }
    // The running config's generation (while Blocked: the blocked record's).
    uint32_t generation() const;
    const ActivitySpec *findActivity(std::string_view id) const;
    const DeviceSpec *findDevice(std::string_view id) const;
    const IntegrationSpec *findIntegration(std::string_view id) const;

    // ---- activities (token or admin) ----
    // `activity.id` and `.revision` are ignored; the new id is in Outcome.id.
    Outcome createActivity(ActivitySpec activity);
    // Whole-activity replace. `activity.id`, if set, must match `id`.
    Outcome replaceActivity(std::string_view id, ActivitySpec activity, uint32_t baseRevision);
    // Refused with Referenced (+ referrers) while a system.activate targets it.
    Outcome deleteActivity(std::string_view id, uint32_t baseRevision);
    // Dry run (POST /activities/validate). Paths rooted at the activity. An
    // empty id validates a not-yet-created activity.
    ValidationReport validateActivity(const ActivitySpec &activity) const;

    Outcome replaceGlobalBindings(std::vector<BindingSpec> bindings, uint32_t baseRevision);

    // ---- devices (admin) ----
    Outcome createDevice(DeviceSpec device);
    Outcome updateDevice(std::string_view id, const DeviceUpdate &update);
    // Refused with DeviceInUse (+ referrers) while anything references it.
    Outcome deleteDevice(std::string_view id, uint32_t baseRevision);
    std::vector<Referrer> deviceReferrers(std::string_view id) const;

    // ---- integrations (admin; non-secret settings only) ----
    // `integration.id` and `.type` must match the existing one.
    Outcome replaceIntegration(std::string_view id, IntegrationSpec integration, uint32_t baseRevision);

    // ---- direct execution ----
    // POST /devices/{id}/commands/{command_id}/execute. NotFound;
    // IntegrationDisabled; ValidationFailed (UnknownCommand at /command_id,
    // HoldNotAllowed for hold commands, params at /params/<name>); Ok.
    Outcome compileCommand(std::string_view deviceId, std::string_view commandId,
                           const std::vector<ParamInput> &params, CompiledCommand *out) const;

private:
    // Restore: restore/reset (every object gets the new generation; otherwise
    // objects with revision 0 - the changed one - do). DeleteActivity: the
    // snapshot names the deleted activity, so the engine switches to the next
    // one if it was active (any other publish never starts an activity).
    enum class CommitKind : uint8_t { Edit, Restore, DeleteActivity };
    // A dry run's result: the round-tripped config and its compiled form.
    struct Built {
        ConfigSpec parsed;
        std::unique_ptr<CompiledConfig> compiled;
    };
    // The commit pipeline (see the top). `dryRun`: stop before saving (and
    // fill `built` if given).
    Outcome commit(ConfigSpec next, Outcome outcome, const std::string &issuePrefix,
                   CommitKind kind = CommitKind::Edit, bool dryRun = false, Built *built = nullptr);
    ErrorCode editable() const;  // Ok, ConfigBlocked or StorageFailed (live state)
    std::string uniqueId(const char *prefix, const std::string &name, const char *fallback) const;

    template <typename T>
    Outcome add(std::vector<T> ConfigSpec::*member, size_t limit, const char *idPrefix, const char *fallbackName,
                const char *section, T item);
    template <typename T>
    Outcome replace(std::vector<T> ConfigSpec::*member, size_t index, const char *section, T item);
    template <typename T>
    Outcome erase(std::vector<T> ConfigSpec::*member, size_t index, CommitKind kind = CommitKind::Edit);

    storage::RecordStore &records_;
    const AdapterSet &adapters_;
    Engine::Store &store_;
    Migration migrate_;
    ConfigState state_ = ConfigState::Fresh;
    LoadReport load_;
    bool runningNothing_ = false;  // the published snapshot is the empty config
    // refresh() backoff while a load keeps failing on an available medium.
    static constexpr uint64_t kFirstRetryMs = 1000;
    static constexpr uint64_t kMaxRetryMs = 60000;
    uint64_t retryDelayMs_ = 0;  // 0: retry at the next refresh
    uint64_t retryAtMs_ = 0;
    storage::Health retryHealth_ = storage::Health::Available;
    ConfigSpec config_;
    ConfigSpec defaults_;  // from load(), for a retried load
};

}  // namespace hubbo::config

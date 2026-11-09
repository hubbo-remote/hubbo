// Whole-system reboot round trips: for every way a config can come to exist
// and every mutation, save -> "reboot" (a fresh ConfigService, Engine and
// adapters over the same storage) -> load must give back exactly what was
// saved, cleanly, with the same active activity restored silently and the
// same compiled behaviour. Plus seeded random operation sequences with
// clean and mid-save (crash) reboots.
#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <fstream>
#include <random>
#include <sstream>

#include "fixtures.h"
#include "hub.h"
#include "hubbo/config/json.h"
#include "hubbo/engine/compiler.h"
#include "hubbo/storage/state_store.h"
#include "hubbo/storage/storage.h"

using namespace hubbo;
using namespace hubbo::sim;
using config::ConfigState;

namespace {

// ---------------------------------------------------------------------------
// Deep comparison with a readable first difference ("" = equal).

std::string diffLiteral(const ParamLiteral &a, const ParamLiteral &b) {
    using K = ParamLiteral::Kind;
    // JSON has one number type: an integral Number comes back as Int. Both
    // coerce identically, so compare numbers by value.
    const bool aNum = a.kind == K::Int || a.kind == K::Number;
    const bool bNum = b.kind == K::Int || b.kind == K::Number;
    if (aNum && bNum) {
        const double x = a.kind == K::Int ? static_cast<double>(a.integer) : a.number;
        const double y = b.kind == K::Int ? static_cast<double>(b.integer) : b.number;
        return x == y ? "" : "number " + std::to_string(x) + " vs " + std::to_string(y);
    }
    if (a.kind != b.kind) return "literal kind";
    if (a.kind == K::Bool && a.boolean != b.boolean) return "bool";
    if (a.kind == K::String && a.string != b.string) return "string '" + a.string + "' vs '" + b.string + "'";
    return "";
}

std::string diffActions(const std::vector<ActionSpec> &a, const std::vector<ActionSpec> &b, const std::string &at) {
    if (a.size() != b.size()) return at + ": " + std::to_string(a.size()) + " vs " + std::to_string(b.size());
    for (size_t i = 0; i < a.size(); i++) {
        const std::string p = at + "/" + std::to_string(i);
        if (a[i].integrationId != b[i].integrationId || a[i].deviceId != b[i].deviceId ||
            a[i].commandId != b[i].commandId) {
            return p + ": target";
        }
        if (a[i].params.size() != b[i].params.size()) return p + ": param count";
        for (size_t j = 0; j < a[i].params.size(); j++) {
            if (a[i].params[j].name != b[i].params[j].name) return p + ": param name";
            const std::string d = diffLiteral(a[i].params[j].value, b[i].params[j].value);
            if (!d.empty()) return p + "/params/" + a[i].params[j].name + ": " + d;
        }
    }
    return "";
}

std::string diffBindings(const std::vector<BindingSpec> &a, const std::vector<BindingSpec> &b, const std::string &at) {
    if (a.size() != b.size()) return at + ": binding count";
    for (size_t i = 0; i < a.size(); i++) {
        if (a[i].trigger != b[i].trigger) return at + ": trigger " + a[i].trigger + " vs " + b[i].trigger;
        const std::string d = diffActions(a[i].actions, b[i].actions, at + "/" + a[i].trigger);
        if (!d.empty()) return d;
    }
    return "";
}

std::string diffConfig(const ConfigSpec &a, const ConfigSpec &b) {
    if (a.revision != b.revision) return "revision";
    if (a.globalBindingsRevision != b.globalBindingsRevision) return "global bindings revision";
    if (a.integrations.size() != b.integrations.size()) return "integration count";
    for (size_t i = 0; i < a.integrations.size(); i++) {
        const auto &x = a.integrations[i];
        const auto &y = b.integrations[i];
        if (x.id != y.id || x.type != y.type || x.name != y.name || x.enabled != y.enabled || x.url != y.url ||
            x.verifyTls != y.verifyTls || x.caCertPem != y.caCertPem || x.deviceName != y.deviceName ||
            x.revision != y.revision) {
            return "integration " + x.id;
        }
    }
    if (a.devices.size() != b.devices.size()) return "device count";
    for (size_t i = 0; i < a.devices.size(); i++) {
        const auto &x = a.devices[i];
        const auto &y = b.devices[i];
        if (x.id != y.id || x.name != y.name || x.integrationId != y.integrationId || x.kind != y.kind ||
            x.provider.type != y.provider.type || x.provider.entityId != y.provider.entityId ||
            x.provider.uniqueId != y.provider.uniqueId || x.provider.peerName != y.provider.peerName ||
            x.revision != y.revision) {
            return "device " + x.id;
        }
    }
    std::string d = diffBindings(a.globalBindings, b.globalBindings, "/global_bindings");
    if (!d.empty()) return d;
    if (a.activities.size() != b.activities.size()) return "activity count";
    for (size_t i = 0; i < a.activities.size(); i++) {
        const auto &x = a.activities[i];
        const auto &y = b.activities[i];
        const std::string at = "/activities/" + x.id;
        if (x.id != y.id || x.name != y.name || x.revision != y.revision) return at;
        if (!(d = diffActions(x.startup, y.startup, at + "/startup")).empty()) return d;
        if (!(d = diffActions(x.shutdown, y.shutdown, at + "/shutdown")).empty()) return d;
        if (!(d = diffBindings(x.bindings, y.bindings, at + "/bindings")).empty()) return d;
    }
    return "";
}

// Compiled behaviour as text: every table entry and action, with commands
// by id. (Compiled actions point into their hub's adapter catalogs, so the
// "before" side has to be captured before its hub goes away.)
std::string compiledSignature(const CompiledConfig &c) {
    std::ostringstream out;
    auto span = [&](const ActionSpan &s) {
        out << "[";
        for (uint8_t i = 0; i < s.count; i++) {
            const CompiledAction &a = c.span(s)[i];
            out << static_cast<int>(a.op) << ":" << static_cast<int>(a.adapterIndex) << ":" << a.deviceIndex << ":"
                << (a.command != nullptr ? a.command->id : "-") << ":" << a.commandIndex << ":"
                << static_cast<int>(a.paramsPresent);
            for (const auto &p : a.params) out << "," << static_cast<int>(p.type) << "/" << p.integer << "/" << p.number;
            out << ";";
        }
        out << "]";
    };
    auto table = [&](const TriggerTable &t) {
        for (size_t b = 0; b < remote::kButtonCount; b++) {
            for (size_t k = 0; k < remote::kEventKindCount; k++) {
                if (t.spans[b][k].count != 0) {
                    out << b << "." << k;
                    span(t.spans[b][k]);
                }
            }
        }
    };
    for (const auto &d : c.devices) out << "dev " << d.id.c_str() << " " << d.providerRef.c_str() << "\n";
    for (const auto &a : c.activities) {
        out << "act " << a.id.c_str() << " startup";
        span(a.startup);
        out << " shutdown";
        span(a.shutdown);
        table(a.table);
        out << "\n";
    }
    out << "globals ";
    table(c.globals);
    return out.str();
}

std::string revisionProblem(const ConfigSpec &c) {
    if (c.revision < 1) return "config revision";
    if (c.globalBindingsRevision < 1) return "global bindings revision";
    for (const auto &a : c.activities) {
        if (a.revision < 1) return "activity " + a.id;
    }
    for (const auto &d : c.devices) {
        if (d.revision < 1) return "device " + d.id;
    }
    for (const auto &i : c.integrations) {
        if (i.revision < 1) return "integration " + i.id;
    }
    return "";
}

// ---------------------------------------------------------------------------
// A hub whose "flash" (config storage and state store) survives reboots.

constexpr uint32_t kSaveDelayMs = ActivePersister::kDefaultDelayMs + 200;

// The "flash": one medium holding the config, active_activity (and, on a
// real hub, secrets) records.
struct Box {
    storage::MemoryStorage disk;
    ConfigSpec defaults;
    std::unique_ptr<SimHub> hub;
    config::LoadReport report;

    explicit Box(ConfigSpec seed = fixtureConfig()) : defaults(std::move(seed)) { boot(); }

    void boot() {
        hub.reset();
        hub = std::make_unique<SimHub>(&disk);
        report = hub->boot(defaults);
    }

    // The newest saved config document ("" if none).
    std::string savedConfig() {
        storage::SlotRecordStore records(disk, config::kMaxConfigFileBytes);
        storage::Record r;
        return records.load(config::ConfigService::kRecordName, &r) == ErrorCode::Ok ? r.payload : "";
    }
    std::string slots() { return disk.files()["config.a"] + "|" + disk.files()["config.b"]; }

    // "" if the running config doesn't compile.
    std::string compiled() const {
        ValidationReport r;
        const auto c = compileConfig(hub->config(), hub->adapters(), &r);
        return c ? compiledSignature(*c) : "";
    }
};

// Checks the invariant across one clean reboot. Returns "" or what broke.
std::string rebootAndCheck(Box &box) {
    box.hub->advance(kSaveDelayMs);  // let the active activity be saved
    const ConfigSpec expected = box.hub->config();
    const std::string savedText = box.savedConfig();
    const std::string slots = box.slots();
    const std::string active = box.hub->engine().hasActiveActivity() ? box.hub->engine().activeActivityId() : "";
    const std::string before = box.compiled();
    if (before.empty()) return "the running config doesn't compile";
    std::string bad = revisionProblem(expected);
    if (!bad.empty()) return "revision < 1 before reboot: " + bad;

    box.boot();

    const config::LoadReport &r = box.report;
    if (r.state != ConfigState::Ok) return std::string("load state ") + config::configStateName(r.state);
    if (r.reason != ErrorCode::Ok) return std::string("load: ") + errorCodeName(r.reason);
    if (r.saved) return "a clean boot saved";
    if (r.generation != expected.revision) return "loaded generation " + std::to_string(r.generation);
    if (!(bad = diffConfig(expected, box.hub->config())).empty()) return "config differs: " + bad;
    if (config::serializeConfig(box.hub->config()) != savedText) return "save -> load -> save isn't byte-identical";
    if (box.slots() != slots) return "a clean boot wrote a slot";
    const std::string restored = box.hub->engine().hasActiveActivity() ? box.hub->engine().activeActivityId() : "";
    if (restored != active) {
        return "active '" + restored + "', expected '" + active + "' (boot: " +
               activeRestoreName(box.hub->lastBoot().active) + " " + errorCodeName(box.hub->lastBoot().reason) +
               " saved '" + box.hub->lastBoot().activityId + "')";
    }
    if (!box.hub->commands().empty()) return "boot ran actions: " + box.hub->commands().front().text;
    if (box.compiled() != before) return "compiled behaviour differs after the reboot";
    return "";
}

#define EXPECT_REBOOT_CLEAN(box)                    \
    do {                                            \
        const std::string problem = rebootAndCheck(box); \
        EXPECT_EQ(problem, "");                     \
    } while (0)

ActivitySpec simpleActivity(const std::string &name) {
    ActivitySpec a;
    a.name = name;
    a.startup = {haAction("dev_lg_tv", "media_player.turn_on"), waitMs(250)};
    a.shutdown = {haAction("dev_lg_tv", "media_player.turn_off")};
    a.bindings = {bind("up:down", {bleKey("key_down", "ARROW_UP")}), bind("up:up", {bleKey("key_up", "ARROW_UP")})};
    return a;
}

ConfigSpec seedFromBackup() {
    std::ifstream in(HUBBO_API_FIXTURES_DIR "/admin/backup.json");
    std::stringstream text;
    text << in.rdbuf();
    ConfigSpec seed;
    EXPECT_EQ(config::parseBackup(text.str(), &seed).code, ErrorCode::Ok);
    return seed;
}

// The largest config the limits allow: 16 activities, 32-action sequences,
// 64 devices, max-length ids/names, a max-length enum option, 3-param
// commands, and the compiled-action cap.
ConfigSpec maximalConfig() {
    ConfigSpec c = fixtureConfig();
    c.revision = 0;  // as a restore might carry
    c.globalBindings.clear();
    c.devices.push_back(haDevice("dev_limits", std::string(limits::kMaxNameLength, 'L'), "receiver",
                                 "media_player.limits_test"));
    while (c.devices.size() < limits::kMaxDevices) {
        std::string id = "dev_" + std::to_string(c.devices.size());
        id.resize(limits::kMaxIdLength, 'x');
        c.devices.push_back(haDevice(id, std::string(limits::kMaxNameLength, 'D'), "light", "light.family_room"));
    }
    c.activities.clear();
    const std::string longSource(limits::kMaxParamStringLength, 's');
    for (size_t i = 0; i < limits::kMaxActivities; i++) {
        ActivitySpec a;
        a.id = "act_" + std::to_string(i);
        a.id.resize(limits::kMaxIdLength, 'y');
        a.name = std::string(limits::kMaxNameLength, static_cast<char>('A' + i));
        a.revision = i % 2 ? 0 : 7;  // 0 is normalized to 1
        for (size_t k = 0; k < limits::kMaxSequenceLength; k++) {
            a.startup.push_back(k % 2 ? waitMs(static_cast<int>(limits::kMaxWaitMs))
                                      : haAction("dev_limits", "media_player.select_source",
                                                 {param("source", longSource.c_str())}));
            a.shutdown.push_back(haAction("dev_lights", "light.turn_on",
                                          {param("brightness", 255), param("color_temp_kelvin", 2700),
                                           param("transition", 0.5)}));
        }
        for (size_t b = 0; b < 60; b++) {
            const auto button = static_cast<remote::Button>(b % remote::kButtonCount);
            const auto kind = static_cast<remote::EventKind>(b / remote::kButtonCount);
            a.bindings.push_back(bind(formatTrigger({button, kind}), {bleKey("key_tap", "KEY_Z")}));
        }
        c.activities.push_back(std::move(a));
    }
    c.globalBindings = {bind("tv:tap", {activateAction(c.activities[0].id.c_str())}), bind("off:tap", {offAction()})};
    return c;
}

}  // namespace

// ---- every way a config can come to exist ----------------------------------

TEST(RebootRoundTrip, FreshEmptyDefaults) {
    Box box{ConfigSpec{}};  // the board's first boot
    EXPECT_EQ(box.report.state, ConfigState::Fresh);
    EXPECT_TRUE(box.report.saved);
    EXPECT_NE(box.hub->configService().findIntegration("ble"), nullptr);  // built-ins
    EXPECT_REBOOT_CLEAN(box);
    EXPECT_REBOOT_CLEAN(box);
}

TEST(RebootRoundTrip, CxxFixtures) {
    Box box;
    EXPECT_REBOOT_CLEAN(box);
}

TEST(RebootRoundTrip, SimSeedFromApiBackup) {
    Box box{seedFromBackup()};
    EXPECT_EQ(box.report.state, ConfigState::Fresh);
    EXPECT_REBOOT_CLEAN(box);
}

TEST(RebootRoundTrip, RestoreWithZeroAndMissingRevisions) {
    Box box;
    ConfigSpec restored = seedFromBackup();
    restored.revision = 0;
    restored.globalBindingsRevision = 0;
    for (auto &a : restored.activities) a.revision = 0;
    for (auto &d : restored.devices) d.revision = 0;
    for (auto &i : restored.integrations) i.revision = 0;
    ASSERT_TRUE(box.hub->configService().restore(restored).ok());
    EXPECT_REBOOT_CLEAN(box);

    // A restored backup archive (integrations carry no revision there).
    const std::string archive = config::serializeBackup(box.hub->config(), {});
    ConfigSpec parsed;
    ASSERT_EQ(config::parseBackup(archive, &parsed).code, ErrorCode::Ok);
    ASSERT_TRUE(box.hub->configService().restore(parsed).ok());
    EXPECT_REBOOT_CLEAN(box);
}

TEST(RebootRoundTrip, ConfigAtTheLimits) {
    Box box;
    const config::Outcome o = box.hub->configService().restore(maximalConfig());
    ASSERT_TRUE(o.ok()) << errorCodeName(o.code) << (o.issues.ok() ? "" : " " + o.issues.issues()[0].path);
    box.hub->activate(box.hub->config().activities[15].id);
    EXPECT_REBOOT_CLEAN(box);
}

TEST(RebootRoundTrip, EveryMutationType) {
    Box box;
    auto &service = box.hub->configService();
    // Re-fetch after each reboot: the service is a new object.
    auto svc = [&]() -> config::ConfigService & { return box.hub->configService(); };

    const config::Outcome created = service.createActivity(simpleActivity("Movie Night"));
    ASSERT_TRUE(created.ok());
    EXPECT_REBOOT_CLEAN(box);

    ActivitySpec edit = *svc().findActivity(created.id);
    edit.name = "Movie Night Deluxe";
    edit.bindings.push_back(bind("info:hold", {bleKey("key_tap", "KEY_I")}));
    ASSERT_TRUE(svc().replaceActivity(created.id, edit, edit.revision).ok());
    EXPECT_REBOOT_CLEAN(box);

    auto globals = svc().config().globalBindings;
    globals.push_back(bind("red:tap", {activateAction(created.id.c_str())}));
    ASSERT_TRUE(svc().replaceGlobalBindings(globals, svc().config().globalBindingsRevision).ok());
    EXPECT_REBOOT_CLEAN(box);

    const config::Outcome device = svc().createDevice(haDevice("", "Bedroom TV", "tv", "media_player.xbox"));
    ASSERT_TRUE(device.ok());
    config::DeviceUpdate update;
    update.name = "Bedroom Xbox";
    update.kind = "console";
    update.revision = svc().findDevice(device.id)->revision;
    ASSERT_TRUE(svc().updateDevice(device.id, update).ok());
    EXPECT_REBOOT_CLEAN(box);

    IntegrationSpec ha = *svc().findIntegration("ha_main");
    ha.url = "https://ha.example:8123";
    ha.caCertPem = "-----BEGIN CERTIFICATE-----\nAAAA\n-----END CERTIFICATE-----\n";
    ha.verifyTls = false;
    ASSERT_TRUE(svc().replaceIntegration("ha_main", ha, ha.revision).ok());
    EXPECT_REBOOT_CLEAN(box);

    ASSERT_TRUE(svc().deleteDevice(device.id, svc().findDevice(device.id)->revision).ok());
    globals.pop_back();
    ASSERT_TRUE(svc().replaceGlobalBindings(globals, svc().config().globalBindingsRevision).ok());
    ASSERT_TRUE(svc().deleteActivity(created.id, svc().findActivity(created.id)->revision).ok());
    EXPECT_REBOOT_CLEAN(box);
}

TEST(RebootRoundTrip, ActiveActivityAndOff) {
    Box box;
    box.hub->activate("act_play_xbox");
    EXPECT_REBOOT_CLEAN(box);
    EXPECT_STREQ(box.hub->engine().activeActivityId(), "act_play_xbox");
    box.hub->off();
    EXPECT_REBOOT_CLEAN(box);
    EXPECT_FALSE(box.hub->engine().hasActiveActivity());
}

TEST(RebootRoundTrip, DeletingTheActiveActivityFallsBackAndThatSurvives) {
    Box box;
    auto svc = [&]() -> config::ConfigService & { return box.hub->configService(); };
    box.hub->activate("act_play_xbox");
    box.hub->advance(kSaveDelayMs);
    auto globals = svc().config().globalBindings;
    globals.erase(std::remove_if(globals.begin(), globals.end(),
                                 [](const BindingSpec &b) { return b.trigger == "music:tap"; }),
                  globals.end());
    ASSERT_TRUE(svc().replaceGlobalBindings(globals, svc().config().globalBindingsRevision).ok());
    ASSERT_TRUE(svc().deleteActivity("act_play_xbox", 1).ok());
    box.hub->advance(1);  // the engine notices the swap and falls back
    EXPECT_STREQ(box.hub->engine().activeActivityId(), "act_watch_apple_tv");
    EXPECT_REBOOT_CLEAN(box);
    EXPECT_STREQ(box.hub->engine().activeActivityId(), "act_watch_apple_tv");
}

// A config saved by an older firmware (document version 1, with the old
// DIGIT_n key names) is migrated once, saved as a new generation, and then
// boots clean.
TEST(RebootRoundTrip, SchemaMigrationIsSavedOnceThenBootsClean) {
    storage::MemoryStorage disk;
    ConfigSpec old = fixtureConfig();
    old.activities[0].bindings.push_back(bind("number_1:tap", {bleKey("key_tap", "KEY_1")}));
    std::string v1 = config::serializeConfig(old);
    v1.replace(v1.find("\"schema_version\":2"), 18, "\"schema_version\":1");
    v1.replace(v1.find("\"KEY_1\""), 7, "\"DIGIT_1\"");
    {
        storage::SlotRecordStore records(disk, config::kMaxConfigFileBytes);
        ASSERT_EQ(records.save(config::ConfigService::kRecordName, 7, v1), ErrorCode::Ok);
    }
    FakeAdapters fakes;
    for (int boot = 0; boot < 2; boot++) {
        Engine::Store store;
        storage::SlotRecordStore records(disk, config::kMaxConfigFileBytes);
        config::ConfigService service(records, fakes.set, store);
        const config::LoadReport r = service.load(ConfigSpec{});
        ASSERT_EQ(r.state, ConfigState::Ok);
        const ActivitySpec &a = *service.findActivity("act_watch_apple_tv");
        EXPECT_EQ(a.bindings.back().actions[0].params[0].value.string, "KEY_1");
        if (boot == 0) {
            EXPECT_EQ(r.migratedFrom, 1);
            EXPECT_TRUE(r.saved);
            EXPECT_EQ(r.generation, 8u);  // saved once, as a new generation
        } else {
            EXPECT_EQ(r.migratedFrom, -1);
            EXPECT_FALSE(r.saved);
            EXPECT_EQ(r.generation, 8u);
        }
    }
}

// ---- random sequences -------------------------------------------------------

namespace {

class RandomRun {
public:
    explicit RandomRun(uint32_t seed) : rng_(seed), box_(seed % 3 == 0 ? seedFromBackup() : fixtureConfig()) {}

    // Returns "" or the first broken invariant.
    std::string run(int operations) {
        for (int i = 0; i < operations; i++) {
            const std::string problem = step(i);
            if (!problem.empty()) return "op " + std::to_string(i) + " (" + last_ + "): " + problem;
        }
        last_ = "final reboot";
        return rebootAndCheck(box_);
    }

private:
    config::ConfigService &svc() { return box_.hub->configService(); }
    size_t pick(size_t n) { return n == 0 ? 0 : std::uniform_int_distribution<size_t>(0, n - 1)(rng_); }
    bool chance(int percent) { return static_cast<int>(pick(100)) < percent; }

    template <typename T>
    const T *any(const std::vector<T> &items) {
        return items.empty() ? nullptr : &items[pick(items.size())];
    }

    ActionSpec randomAction() {
        switch (pick(6)) {
            case 0: return waitMs(static_cast<int64_t>(pick(3000)));
            case 1: return bleKey("key_tap", chance(50) ? "SELECT" : "ARROW_LEFT");
            case 2: return haAction("dev_lg_tv", "media_player.select_source", {param("source", "HDMI 2")});
            case 3: return haAction("dev_xbox", "media_player.turn_on");
            case 4: {
                const DeviceSpec *d = any(svc().config().devices);
                if (d != nullptr && d->provider.type == kHomeAssistantType &&
                    d->provider.entityId.rfind("light.", 0) == 0) {
                    return action("ha_main", d->id, "light.turn_on", {param("brightness", static_cast<int>(pick(256)))});
                }
                return haAction("dev_lg_tv", "media_player.turn_off");
            }
            default: return haAction("dev_lg_tv", "media_player.volume_up");
        }
    }

    ActivitySpec randomActivity() {
        ActivitySpec a;
        a.name = "Random " + std::to_string(pick(1000));
        for (size_t i = pick(5); i > 0; i--) a.startup.push_back(randomAction());
        for (size_t i = pick(3); i > 0; i--) a.shutdown.push_back(randomAction());
        for (size_t i = pick(6); i > 0; i--) {
            const auto button = static_cast<remote::Button>(pick(remote::kButtonCount));
            const auto kind = static_cast<remote::EventKind>(pick(remote::kEventKindCount));
            const std::string trigger = formatTrigger({button, kind});
            bool dup = false;
            for (const auto &b : a.bindings) dup = dup || b.trigger == trigger;
            if (!dup) a.bindings.push_back(bind(trigger, {randomAction()}));
        }
        if (chance(10)) a.startup.push_back(haAction("dev_ghost", "media_player.turn_on"));  // invalid
        return a;
    }

    // One random operation; invalid ones must change nothing.
    std::string step(int index) {
        const std::string before = box_.slots();
        const uint32_t revision = svc().config().revision;
        config::Outcome o;
        bool mutation = true;
        switch (pick(12)) {
            case 0:
            case 1:
                last_ = "create activity";
                o = svc().createActivity(randomActivity());
                break;
            case 2: {
                last_ = "replace activity";
                const ActivitySpec *a = any(svc().config().activities);
                if (a == nullptr) return "";
                ActivitySpec edit = randomActivity();
                const uint32_t base = chance(15) ? a->revision + 1 : a->revision;  // some stale
                o = svc().replaceActivity(a->id, edit, base);
                break;
            }
            case 3: {
                last_ = "delete activity";
                const ActivitySpec *a = any(svc().config().activities);
                if (a == nullptr) return "";
                o = svc().deleteActivity(a->id, a->revision);
                break;
            }
            case 4: {
                last_ = "replace global bindings";
                std::vector<BindingSpec> globals = {bind("off:tap", {offAction()})};
                if (const ActivitySpec *a = any(svc().config().activities)) {
                    globals.push_back(bind("tv:tap", {activateAction(a->id.c_str())}));
                }
                if (chance(10)) globals.push_back(bind("bogus", {offAction()}));  // invalid
                o = svc().replaceGlobalBindings(globals, svc().config().globalBindingsRevision);
                break;
            }
            case 5: {
                last_ = "create device";
                o = svc().createDevice(haDevice("", "Lamp " + std::to_string(pick(100)), "light", "light.family_room"));
                break;
            }
            case 6: {
                last_ = "update device";
                const DeviceSpec *d = any(svc().config().devices);
                if (d == nullptr) return "";
                config::DeviceUpdate u;
                u.name = d->name + "!";
                u.revision = d->revision;
                o = svc().updateDevice(d->id, u);
                break;
            }
            case 7: {
                last_ = "delete device";
                const DeviceSpec *d = any(svc().config().devices);
                if (d == nullptr) return "";
                o = svc().deleteDevice(d->id, d->revision);
                break;
            }
            case 8: {
                last_ = "integration settings";
                const IntegrationSpec *in = svc().findIntegration("ha_main");
                if (in == nullptr) return "";
                IntegrationSpec edit = *in;
                edit.url = "http://ha-" + std::to_string(pick(100)) + ".local:8123";
                edit.caCertPem = chance(50) ? std::optional<std::string>("PEM") : std::nullopt;
                o = svc().replaceIntegration("ha_main", edit, edit.revision);
                break;
            }
            case 9: {
                mutation = false;
                last_ = "activate/off";
                const ActivitySpec *a = any(svc().config().activities);
                if (a != nullptr && chance(70)) {
                    box_.hub->activate(a->id);
                } else {
                    box_.hub->off();
                }
                box_.hub->advance(static_cast<uint32_t>(pick(kSaveDelayMs * 2)));
                break;
            }
            case 10:
                mutation = false;
                last_ = "clean reboot";
                return rebootAndCheck(box_);
            default:
                mutation = false;
                last_ = "crash mid-save";
                return crashDuringSave();
        }
        (void)index;
        if (mutation) {
            const std::string bad = revisionProblem(svc().config());
            if (!bad.empty()) return "revision < 1: " + bad;
            if (!o.ok() && (box_.slots() != before || svc().config().revision != revision)) {
                return std::string("a refused edit (") + errorCodeName(o.code) + ") changed something";
            }
        }
        return "";
    }

    // Power fails somewhere inside a save. After reboot the hub holds the new
    // config if the save was acknowledged and the old one otherwise (never
    // anything else), and the next boot is clean.
    std::string crashDuringSave() {
        const ConfigSpec old = svc().config();
        ActivitySpec added = simpleActivity("Crash " + std::to_string(pick(1000)));
        box_.disk.crashAfter(static_cast<int>(pick(3)), pick(2) ? pick(400) : 0);
        const config::Outcome o = svc().createActivity(added);
        box_.disk.reboot();
        box_.boot();
        if (box_.report.state != ConfigState::Ok) return "crash left no usable config";
        const ConfigSpec &now = box_.hub->config();
        if (o.ok()) {
            if (now.activities.size() != old.activities.size() + 1) return "an acknowledged save was lost";
        } else if (!diffConfig(old, now).empty()) {
            return "a failed save changed the config: " + diffConfig(old, now);
        }
        return rebootAndCheck(box_);
    }

    std::mt19937 rng_;
    Box box_;
    std::string last_;
};

}  // namespace

// HUBBO_ROUNDTRIP_ITERATIONS=N runs more seeds (long runs); default 300.
TEST(RebootRoundTrip, RandomSequences) {
    uint32_t iterations = 300;
    if (const char *env = std::getenv("HUBBO_ROUNDTRIP_ITERATIONS")) iterations = static_cast<uint32_t>(std::atoi(env));
    for (uint32_t seed = 1; seed <= iterations; seed++) {
        RandomRun run(seed);
        const std::string problem = run.run(12);
        ASSERT_EQ(problem, "") << "seed " << seed;
    }
}

// Fields that don't apply to a type aren't stored, so they must not linger
// in the running config either (or a reboot would silently change it).
TEST(RebootRoundTrip, FieldsThatDontApplyToTheTypeAreNotKept) {
    Box box;
    auto svc = [&]() -> config::ConfigService & { return box.hub->configService(); };
    IntegrationSpec ble = *svc().findIntegration("ble");
    ble.url = "http://not-for-ble";
    ble.verifyTls = false;
    ble.caCertPem = "PEM";
    ASSERT_TRUE(svc().replaceIntegration("ble", ble, ble.revision).ok());
    DeviceSpec tv = haDevice("", "Den TV", "tv", "media_player.family_room_lg_tv");
    tv.provider.peerName = "not-for-ha";
    ASSERT_TRUE(svc().createDevice(tv).ok());
    EXPECT_REBOOT_CLEAN(box);
}

// ---- unusable and unacknowledged state ------------------------------------

// A config record this firmware can't use (a newer schema after a downgrade,
// or one that doesn't compile) is never overwritten: the hub boots Blocked,
// runs nothing, refuses edits, and only an explicit restore/reset writes.
TEST(RebootRoundTrip, UnusableConfigIsNeverOverwritten) {
    for (const char *fixture : {"config.newer-schema.json", "config.does-not-compile.json"}) {
        SCOPED_TRACE(fixture);
        Box box;
        ASSERT_TRUE(box.hub->configService().createActivity(simpleActivity("Precious")).ok());
        box.hub->activate("act_precious");
        box.hub->advance(kSaveDelayMs);
        std::ifstream in(std::string(HUBBO_SIM_FIXTURES_DIR) + "/" + fixture);
        std::stringstream text;
        text << in.rdbuf();
        {
            storage::SlotRecordStore records(box.disk, config::kMaxConfigFileBytes);
            ASSERT_EQ(records.save(config::ConfigService::kRecordName, 100, text.str()), ErrorCode::Ok);
        }
        const std::string slots = box.slots();
        for (int boot = 0; boot < 2; boot++) {
            box.boot();
            EXPECT_EQ(box.report.state, ConfigState::Blocked);
            EXPECT_EQ(box.report.generation, 100u);
            EXPECT_FALSE(box.hub->engine().hasActiveActivity());
            EXPECT_TRUE(box.hub->commands().empty());
            EXPECT_EQ(box.hub->configService().createActivity(simpleActivity("Nope")).code,
                      ErrorCode::ConfigBlocked);
            EXPECT_EQ(box.slots(), slots);  // both slots untouched
        }
        // Only an explicit reset writes; it leaves Blocked above the blocked generation.
        ASSERT_TRUE(box.hub->configService().reset(fixtureConfig()).ok());
        EXPECT_EQ(box.hub->configService().state(), ConfigState::Ok);
        EXPECT_EQ(box.hub->configService().generation(), 101u);
        EXPECT_REBOOT_CLEAN(box);
    }
}

// A save that failed (power lost mid-write) was never acknowledged, so the
// next boot has the old config.
TEST(RebootRoundTrip, UnacknowledgedSaveNeverLoads) {
    for (size_t torn : {size_t{0}, size_t{30}, size_t{4000}}) {
        Box box;
        const ConfigSpec before = box.hub->config();
        box.disk.crashAfter(0, torn);
        EXPECT_EQ(box.hub->configService().createActivity(simpleActivity("Unacknowledged")).code,
                  ErrorCode::StorageFailed);
        box.disk.reboot();
        box.boot();
        EXPECT_EQ(diffConfig(before, box.hub->config()), "") << "torn " << torn;
    }
}

// Restore stamps every object with the new generation, so a client holding
// a revision from before the restore always gets a conflict.
TEST(RebootRoundTrip, RestoreStampsEverythingWithTheNewGeneration) {
    Box box;
    auto svc = [&]() -> config::ConfigService & { return box.hub->configService(); };
    const uint32_t staleRevision = svc().findActivity("act_play_xbox")->revision;
    ConfigSpec backup = fixtureConfig();
    for (auto &a : backup.activities) a.revision = staleRevision;  // as archived
    ASSERT_TRUE(svc().restore(backup).ok());
    const uint32_t generation = svc().generation();
    for (const auto &a : svc().config().activities) EXPECT_EQ(a.revision, generation);
    for (const auto &d : svc().config().devices) EXPECT_EQ(d.revision, generation);
    for (const auto &i : svc().config().integrations) EXPECT_EQ(i.revision, generation);
    EXPECT_EQ(svc().config().globalBindingsRevision, generation);
    ActivitySpec edit = *svc().findActivity("act_play_xbox");
    EXPECT_EQ(svc().replaceActivity("act_play_xbox", edit, staleRevision).code, ErrorCode::RevisionConflict);
    EXPECT_REBOOT_CLEAN(box);
}

// The shutdown flush writes the debounced active activity at once.
TEST(RebootRoundTrip, FlushOnShutdownKeepsTheActiveActivity) {
    Box box;
    box.hub->activate("act_play_xbox");
    box.hub->advance(10);  // well inside the save delay
    box.hub->flush();
    box.boot();
    EXPECT_STREQ(box.hub->engine().activeActivityId(), "act_play_xbox");
}

// Backups may carry revision 0, or none at all (api/openapi.yaml
// BackupRevision): parseBackup accepts both and restore stamps everything
// with the new generation. Each restore then survives a reboot.
TEST(RebootRoundTrip, RestoreBackupsWithZeroAndMissingRevisions) {
    for (bool missing : {false, true}) {
        SCOPED_TRACE(missing ? "missing" : "zero");
        Box box;
        JsonDocument doc;
        ASSERT_EQ(deserializeJson(doc, config::serializeBackup(seedFromBackup(), {})), DeserializationError::Ok);
        auto clear = [&](JsonObject o) {
            if (missing) {
                o.remove("revision");
            } else {
                o["revision"] = 0;
            }
        };
        for (JsonObject a : doc["activities"].as<JsonArray>()) clear(a);
        for (JsonObject d : doc["devices"].as<JsonArray>()) clear(d);
        clear(doc["global_bindings"].as<JsonObject>());
        std::string archive;
        serializeJson(doc, archive);

        ConfigSpec parsed;
        const config::ParseResult result = config::parseBackup(archive, &parsed);
        ASSERT_EQ(result.code, ErrorCode::Ok) << (result.report.ok() ? "" : result.report.issues()[0].path);
        ASSERT_TRUE(box.hub->configService().restore(parsed).ok());
        const uint32_t generation = box.hub->configService().generation();
        for (const auto &a : box.hub->config().activities) EXPECT_EQ(a.revision, generation);
        for (const auto &d : box.hub->config().devices) EXPECT_EQ(d.revision, generation);
        EXPECT_EQ(box.hub->config().globalBindingsRevision, generation);
        EXPECT_REBOOT_CLEAN(box);
    }
}

// Stored config documents still require revisions >= 1: only backups relax it.
TEST(RebootRoundTrip, StoredConfigStillRequiresRevisions) {
    std::string text = config::serializeConfig(fixtureConfig());
    const std::string revision = "\"revision\":1}";
    text.replace(text.rfind(revision), revision.size(), "\"revision\":0}");  // the last activity's
    ConfigSpec c;
    const config::ParseResult result = config::parseConfig(text, &c);
    EXPECT_EQ(result.code, ErrorCode::ValidationFailed);
}

// "Can't read" is never "doesn't exist": if the slot holding the newest
// config can't be read at boot, the hub must not boot the older slot (the
// next save would overwrite the newest one). It boots StorageUnavailable,
// writes nothing, and once the medium reads again the latest edit is there.
TEST(RebootRoundTrip, AnUnreadableNewestSlotLosesNothing) {
    Box box;
    ASSERT_TRUE(box.hub->configService().createActivity(simpleActivity("Latest Edit")).ok());
    const uint32_t generation = box.hub->configService().generation();
    // Which slot holds the newest record?
    storage::Record a;
    const bool aIsNewest = storage::decodeSlot(box.disk.files()["config.a"], &a) && a.generation == generation;
    const std::string newestSlot = aIsNewest ? "config.a" : "config.b";
    const std::string slots = box.slots();

    box.disk.failReads(newestSlot);
    box.boot();
    EXPECT_EQ(box.report.state, ConfigState::StorageUnavailable);
    EXPECT_EQ(box.hub->configService().createActivity(simpleActivity("Nope")).code, ErrorCode::StorageFailed);
    box.hub->advance(kSaveDelayMs);
    EXPECT_EQ(box.slots(), slots);  // nothing written

    box.disk.failReads(newestSlot, false);
    box.boot();
    EXPECT_EQ(box.report.state, ConfigState::Ok);
    EXPECT_EQ(box.report.generation, generation);
    EXPECT_NE(box.hub->configService().findActivity("act_latest_edit"), nullptr);
    EXPECT_REBOOT_CLEAN(box);
}

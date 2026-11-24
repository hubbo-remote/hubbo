// IR (docs/design/ir.md): Pronto codes, IR profiles and devices, the IR
// adapter's sending rules (repeats, holds, toggles, key delay, one code at a
// time) against hub-sim's fake emitter, and an IR device on the remote.
#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <vector>

#include "fakes.h"
#include "fixtures.h"
#include "hub.h"
#include "hubbo/config/json.h"
#include "hubbo/config/profile.h"
#include "hubbo/config/service.h"
#include "hubbo/domain/pronto.h"
#include "hubbo/integrations/ir.h"

using namespace hubbo;
using namespace hubbo::config;
using namespace hubbo::integrations;

namespace {

constexpr const char *kNecPower =
    "0000 006D 0002 0001 0156 00AB 0015 05F2 0156 0056";  // a short, valid code (2 once pairs, 1 repeat pair)

ValidationReport parse(const std::string &json, ProfileDocument *out = nullptr) {
    ProfileDocument doc;
    ValidationReport report;
    parseProfile(json, out != nullptr ? out : &doc, &report);
    return report;
}

// A minimal IR profile; `extra` is spliced in before the closing brace.
std::string irJson(const std::string &commands, const std::string &extra = "") {
    return R"({"schema_version":1,"id":"user:test","revision":1,"source":"user","manufacturer":"Acme",)"
           R"("model":"TV","kind":"tv","platform":"ir","commands":)" +
           commands + extra + "}";
}

std::string command(const std::string &pronto) { return R"({"Power":{"pronto":")" + pronto + R"("}})"; }

std::string firstIssue(const ValidationReport &r) {
    return r.ok() ? "ok" : r.issues()[0].path + " " + errorCodeName(r.issues()[0].code);
}

}  // namespace

// ---- Pronto ---------------------------------------------------------------

TEST(Pronto, ParsesAndFormatsTheSpecsText) {
    std::vector<uint16_t> words;
    ASSERT_TRUE(pronto::parse(kNecPower, &words));
    ASSERT_EQ(words.size(), 10u);
    EXPECT_EQ(pronto::frequency(words.data()), 0x6D);
    EXPECT_EQ(pronto::oncePairs(words.data()), 2u);
    EXPECT_EQ(pronto::repeatPairs(words.data()), 1u);
    EXPECT_EQ(pronto::format(words.data(), words.size()), kNecPower);
    // 0x6D: 26.3 µs periods, a 38 kHz carrier.
    EXPECT_EQ(pronto::carrierHz(0x6D), 38029u);
    EXPECT_EQ(pronto::durationUs(0x0156, 0x6D), 8993u);  // NEC's 9 ms leader
    // A low frequency word (the catalog has 9 kHz codes) is fine.
    EXPECT_EQ(pronto::carrierHz(0x01CC), 9011u);
}

TEST(Pronto, RefusesWhatTheSpecDoesnt) {
    const char *bad[] = {
        "",
        "0000 006D 0001 0000",                      // no pairs after the header
        "0100 006D 0001 0000 0010 0010",            // not raw (learned with a carrier word only)
        "0000 0000 0001 0000 0010 0010",            // no frequency
        "0000 006D 0000 0000 0010 0010",            // no pairs counted
        "0000 006D 0002 0000 0010 0010",            // the header says more
        "0000 006D 0001 0000 0010 0010 0010 0010",  // the header says less
        "0000 006D 0001 0000 0010 0000",            // a zero duration
        "0000 006d 0001 0000 0010 0010",            // lowercase
        "0000 006D 0001 0000 0010  0010",           // two spaces
        "0000 006D 0001 0000 0010 0010 ",           // trailing space
        "0000 006D 0001 0000 0010 001",             // a short word
        "0000 006D 0001 0000 0010 00G0",            // not hex
    };
    for (const char *text : bad) {
        std::vector<uint16_t> words = {7};
        EXPECT_FALSE(pronto::parse(text, &words)) << text;
        EXPECT_EQ(words.size(), 1u) << text;  // nothing appended
    }
    std::string big = "0000 006D 0201 0000";
    for (size_t i = 0; i < 2 * 0x201; i++) big += " 0010";
    std::vector<uint16_t> words;
    EXPECT_FALSE(pronto::parse(big, &words));  // over kMaxPairs in one section
}

// ---- IR profiles ----------------------------------------------------------

TEST(IrProfile, TheTestProfilesParse) {
    for (const char *file : {"nec-tv.json", "rc5-tv.json", "sony-receiver.json", "harmony-apple-tv.json"}) {
        ProfileDocument doc;
        const ValidationReport r = parse(sim::irProfileText(file), &doc);
        ASSERT_TRUE(r.ok()) << file << ": " << firstIssue(r);
        EXPECT_EQ(doc.platform, kIrType);
        ASSERT_TRUE(doc.profile.irWords);
        for (const ProfileCommand &c : doc.profile.commands) EXPECT_FALSE(c.pronto.empty()) << c.name;
    }
    ProfileDocument sony;
    parse(sim::irProfileText("sony-receiver.json"), &sony);
    EXPECT_EQ(*sony.profile.timing->irRepeats, 3u);
    EXPECT_EQ(*sony.profile.findCommand("Dimmer")->holdMs, 1500u);
    ProfileDocument rc5;
    parse(sim::irProfileText("rc5-tv.json"), &rc5);
    EXPECT_FALSE(rc5.profile.findCommand("VolumeUp")->prontoToggle.empty());
}

TEST(IrProfile, RefusesWhatTheSpecDoesnt) {
    struct Case {
        std::string json;
        ErrorCode code;
        const char *path;
    } cases[] = {
        {irJson(command("0000 006D 0001 0000 0010 0000")), ErrorCode::InvalidPronto, "/commands/Power/pronto"},
        {irJson(command("0000 006D 0002 0000 0010 0010")), ErrorCode::InvalidPronto, "/commands/Power/pronto"},
        {irJson(command("0000 0000 0001 0000 0010 0010")), ErrorCode::InvalidPronto, "/commands/Power/pronto"},
        {irJson(R"({"Power":{"pronto":7}})"), ErrorCode::WrongType, "/commands/Power/pronto"},
        {irJson(R"({"Power":{}})"), ErrorCode::Required, "/commands/Power/pronto"},
        {irJson(R"({"Power":{"pronto":")" + std::string(kNecPower) + R"(","pronto_toggle":"0000"}})"),
         ErrorCode::InvalidPronto, "/commands/Power/pronto_toggle"},
        {irJson(R"({"Power":{"page":"consumer","usage":"0x0040"}})"), ErrorCode::UnknownField,
         "/commands/Power/page"},
        {irJson(command(kNecPower), R"(,"timing":{"ir_repeats":21})"), ErrorCode::ParamOutOfRange,
         "/timing/ir_repeats"},
    };
    for (const Case &c : cases) {
        const ValidationReport r = parse(c.json);
        EXPECT_TRUE(r.has(c.code, c.path)) << c.json << "\n  first: " << firstIssue(r);
    }
    EXPECT_TRUE(parse(irJson(command(kNecPower), R"(,"timing":{"ir_repeats":0})")).ok());
    // The spec's 512 commands (the config's size bounds IR, not a count).
    std::string many = "{";
    for (int i = 0; i < 513; i++) {
        many += (i > 0 ? "," : "") + std::string("\"C") + std::to_string(i) + R"(":{"pronto":")" + kNecPower + "\"}";
    }
    many += "}";
    EXPECT_TRUE(parse(irJson(many)).has(ErrorCode::TooLong, "/commands")) << firstIssue(parse(irJson(many)));
    // The spec's 256 KiB, minified and without meta, from text or a parsed document.
    std::string big = "[";  // 256 KiB of strings (one JSON string can't be that long)
    for (int i = 0; i < 256; i++) big += (i > 0 ? ",\"" : "\"") + std::string(1024, 'x') + "\"";
    big += "]";
    EXPECT_TRUE(parse(irJson(command(kNecPower), R"(,"meta":{"notes":)" + big + "}")).ok());
    const std::string tooBig = irJson(command(kNecPower), R"(,"x":)" + big);
    EXPECT_TRUE(parse(tooBig).has(ErrorCode::TooLong, "")) << firstIssue(parse(tooBig));
    JsonDocument doc;
    ASSERT_FALSE(deserializeJson(doc, tooBig));
    ProfileDocument out;
    ValidationReport r;
    EXPECT_EQ(parseProfile(doc.as<JsonVariantConst>(), &out, &r), ErrorCode::ValidationFailed);
    ASSERT_EQ(r.issues().size(), 1u);
    EXPECT_TRUE(r.has(ErrorCode::TooLong, ""));
}

TEST(IrProfile, TheHashIsTheSpecs) {
    // As the spec's tools compute it (sorted keys, compact, ensure_ascii off).
    const struct {
        const char *file;
        const char *sha256;
    } known[] = {
        {"harmony-apple-tv.json", "2c67d534f97319e04f41246cf375361f9af6162cdd40729e1d2cb8d33d26c176"},
        {"nec-tv.json", "8f33885e9786d2c986493a8ba455284bfac5e4c4b9c4a7b9d1bd80c789737468"},
        {"rc5-tv.json", "3e32f121417a40b3e8018a259cb57346dbf5f020a887b95be60ef973cbd9bf01"},
        {"sony-receiver.json", "50362108b903f54cbded7d606b26694213e6f72678b60477315ca059e9ffbe88"},
    };
    for (const auto &k : known) {
        std::string sha;
        ASSERT_TRUE(profileSha256(sim::irProfileText(k.file), &sha)) << k.file;
        EXPECT_EQ(sha, k.sha256) << k.file;
    }
    // Escapes and non-ASCII as Python writes them; key order doesn't matter.
    // python: json.dumps({"a":"é\"\\\n\u0001","b":[1,-2,True,None]}, sort_keys=True,
    //                    separators=(",",":"), ensure_ascii=False)
    std::string a, b;
    ASSERT_TRUE(profileSha256(R"({"b":[1,-2,true,null],"a":"é\"\\\n\u0001"})", &a));
    ASSERT_TRUE(profileSha256(R"({"a":"é\"\\\n\u0001","b":[1,-2,true,null]})", &b));
    EXPECT_EQ(a, b);
    EXPECT_EQ(a, "d168d3b6bf7824a466d0df37910ba2a22d4a370329cd54bcf55710e37340e622");
    std::string unchanged = "x";
    EXPECT_FALSE(profileSha256(R"({"a":1.5})", &unchanged));
    EXPECT_FALSE(profileSha256("{", &unchanged));
    EXPECT_EQ(unchanged, "x");
}

TEST(IrDevice, StoresAndReadsBackItsCodes) {
    DeviceSpec d = sim::irDevice("dev_tv", "TV", "rc5-tv.json");
    EXPECT_EQ(d.integrationId, kIrIntegrationId);
    EXPECT_EQ(d.provider.type, kIrType);
    EXPECT_EQ(providerRef(d), "dev_tv");
    JsonDocument doc;
    encodeDevice(d, doc.to<JsonObject>(), DeviceDetail::Stored);
    EXPECT_EQ(doc["profile"]["id"], "user:test-rc5-tv");
    EXPECT_EQ(doc["profile"]["sha256"].as<std::string>().size(), 64u);
    // The codes come back exactly as the profile wrote them.
    JsonDocument source;
    deserializeJson(source, sim::irProfileText("rc5-tv.json"));
    EXPECT_EQ(doc["commands"], source["commands"]);
    EXPECT_EQ(doc["timing"], source["timing"]);
    EXPECT_TRUE(doc["commands"]["Mute"]["page"].isNull());

    DeviceSpec back;
    ValidationReport r;
    ASSERT_TRUE(decodeDevice(doc.as<JsonVariantConst>(), DeviceShape::Stored, &back, &r)) << firstIssue(r);
    JsonDocument again;
    encodeDevice(back, again.to<JsonObject>(), DeviceDetail::Stored);
    EXPECT_EQ(again, doc);
}

TEST(IrDevice, AddedThroughTheConfigService) {
    storage::MemoryStorage mem;
    storage::SlotRecordStore records(mem, kMaxConfigFileBytes);
    sim::FakeAdapters fakes;
    Engine::Store store;
    ConfigService service(records, fakes.set, store);
    ASSERT_EQ(service.load(sim::fixtureConfig()).state, ConfigState::Fresh);
    EXPECT_NE(service.findIntegration(kIrIntegrationId), nullptr);  // a built-in

    for (const char *file : {"nec-tv.json", "sony-receiver.json", "harmony-apple-tv.json"}) {
        DeviceSpec d = sim::irDevice("", "", file);
        d.id.clear();
        d.revision = 0;
        const Outcome added = service.createDevice(std::move(d));
        ASSERT_TRUE(added.ok()) << file << ": " << (added.issues.ok() ? errorCodeName(added.code)
                                                                       : added.issues.issues()[0].path);
    }
    // An IR device needs its profile, like a Bluetooth one.
    DeviceSpec bare = sim::irDevice("", "Bare", "nec-tv.json");
    bare.id.clear();
    bare.profile.reset();
    EXPECT_TRUE(service.createDevice(std::move(bare)).issues.has(ErrorCode::Required, "/profile"));
}

// ---- the adapter ----------------------------------------------------------

namespace {

class IrAdapterTest : public ::testing::Test {
protected:
    IrAdapterTest() {
        tx.clock = [this] { return now; };
        devices.push_back(sim::irDevice("dev_nec", "NEC TV", "nec-tv.json"));
        devices.push_back(sim::irDevice("dev_rc5", "RC5 TV", "rc5-tv.json"));
        devices.push_back(sim::irDevice("dev_sony", "Receiver", "sony-receiver.json"));
    }

    const DeviceSpec &device(const char *id) {
        for (const DeviceSpec &d : devices) {
            if (d.id == id) return d;
        }
        ADD_FAILURE() << id;
        return devices[0];
    }

    ExecResult exec(const char *deviceId, IrCommand command, const char *name,
                    std::optional<int32_t> holdMs = std::nullopt, bool direct = false) {
        const CommandCatalog catalog = adapter.commandsFor(device(deviceId));
        const auto index = static_cast<uint16_t>(command);
        const CommandSpec &spec = catalog.items[index];
        const ParamSpec &names = spec.params[0];
        int option = -1;
        for (uint16_t i = 0; i < names.optionCount; i++) {
            if (std::string(names.options[i]) == name) option = i;
        }
        EXPECT_GE(option, 0) << name;
        std::vector<ParamValue> params = {ParamValue::ofOption(static_cast<uint16_t>(option))};
        if (spec.paramCount > 1) params.push_back(holdMs ? ParamValue::ofInt(*holdMs) : spec.params[1].defaultValue);
        const Ticket ticket = ++lastTicket;
        return adapter.execute({&spec, index, params.data(), static_cast<uint8_t>(holdMs ? 0x3 : 0x1), deviceId,
                                deviceId, ticket, now, direct});
    }
    ExecResult tap(const char *deviceId, const char *name) { return exec(deviceId, IrCommand::Press, name); }

    void advance(uint32_t ms) {
        for (uint32_t i = 0; i < ms; i++) {
            now++;
            adapter.poll(now, sink);
        }
    }

    std::vector<sim::IrSend> sends() { return tx.sends(); }

    struct Sink : CompletionSink {
        std::vector<std::pair<Ticket, ExecResult>> done;
        void complete(Ticket t, ExecResult r, uint32_t) override { done.push_back({t, r}); }
    } sink;
    uint32_t now = 10000;
    Ticket lastTicket = 0;
    std::vector<DeviceSpec> devices;
    sim::FakeIrTransmitter tx;
    IrAdapter adapter{"ir", tx};
};

}  // namespace

TEST_F(IrAdapterTest, TheCatalogIsTheRemoteGroup) {
    const CommandCatalog catalog = adapter.commandsFor(device("dev_nec"));
    ASSERT_EQ(catalog.count, 3u);
    EXPECT_STREQ(catalog.items[0].id, "press");
    EXPECT_STREQ(catalog.items[1].id, "press_down");
    EXPECT_STREQ(catalog.items[1].releasedBy, "press_up");
    EXPECT_STREQ(catalog.items[2].id, "press_up");
    for (size_t i = 0; i < catalog.count; i++) EXPECT_STREQ(catalog.items[i].group, "remote");
    EXPECT_EQ(catalog.items[0].params[0].optionCount, device("dev_nec").profile->commands.size());
    // One catalog per distinct profile: a second device from it shares it.
    const CommandCatalog again = adapter.commandsFor(sim::irDevice("dev_other", "Other", "nec-tv.json"));
    EXPECT_EQ(again.items, catalog.items);
    EXPECT_NE(adapter.commandsFor(device("dev_rc5")).items, catalog.items);
    // Not an IR device: nothing.
    EXPECT_EQ(adapter.commandsFor(sim::bleDevice("dev_apple_tv", "Apple TV", "streamer")).count, 0u);
}

TEST_F(IrAdapterTest, ATapSendsItsFramesThenCompletes) {
    EXPECT_EQ(tap("dev_nec", "VolumeUp").status, ExecStatus::Pending);
    ASSERT_EQ(sends().size(), 1u);  // at once: the emitter was idle
    EXPECT_EQ(sends()[0].deviceId, "dev_nec");
    EXPECT_EQ(sends()[0].command, "VolumeUp");
    EXPECT_EQ(sends()[0].carrierHz, 38029u);
    EXPECT_EQ(sends()[0].minFrames, 1u);  // no ir_repeats: one frame
    EXPECT_FALSE(sends()[0].hold);
    EXPECT_TRUE(sink.done.empty());
    advance(200);
    ASSERT_EQ(sink.done.size(), 1u);
    EXPECT_EQ(sink.done[0].first, lastTicket);
    EXPECT_EQ(sink.done[0].second.status, ExecStatus::Accepted);
    EXPECT_EQ(sends()[0].frames, 1u);
    EXPECT_TRUE(sends()[0].endedAtMs.has_value());

    // ir_repeats 3: three frames per tap.
    tap("dev_sony", "VolumeUp");
    advance(500);
    EXPECT_EQ(sends().back().minFrames, 3u);
    EXPECT_EQ(sends().back().frames, 3u);
}

TEST_F(IrAdapterTest, ADirectTapAnswersAtOnce) {
    EXPECT_EQ(exec("dev_nec", IrCommand::Press, "Mute", std::nullopt, true).status, ExecStatus::Accepted);
    advance(200);
    EXPECT_TRUE(sink.done.empty());
    EXPECT_EQ(sends().size(), 1u);
}

TEST_F(IrAdapterTest, NotReadyFails) {
    tx.setReady(false);
    EXPECT_EQ(tap("dev_nec", "Mute").error, ErrorCode::AdapterUnavailable);
    EXPECT_EQ(exec("dev_nec", IrCommand::PressDown, "VolumeUp").error, ErrorCode::AdapterUnavailable);
    EXPECT_TRUE(sends().empty());
    EXPECT_STREQ(irIntegrationState(IntegrationSpec{}, false), "not_ready");
    EXPECT_STREQ(irIntegrationState(IntegrationSpec{}, true), "ready");
}

TEST_F(IrAdapterTest, AHoldRepeatsUntilTheRelease) {
    EXPECT_EQ(exec("dev_nec", IrCommand::PressDown, "VolumeUp").status, ExecStatus::Accepted);
    ASSERT_EQ(sends().size(), 1u);
    EXPECT_TRUE(sends()[0].hold);
    advance(1000);
    EXPECT_TRUE(tx.busy());
    const uint32_t framesSoFar = sends()[0].frames;
    EXPECT_GT(framesSoFar, 5u);  // a NEC repeat frame is about 110 ms
    EXPECT_EQ(exec("dev_nec", IrCommand::PressUp, "VolumeUp").status, ExecStatus::Accepted);
    EXPECT_TRUE(sends()[0].stoppedAtMs.has_value());
    advance(200);
    EXPECT_FALSE(tx.busy());
    EXPECT_TRUE(sends()[0].endedAtMs.has_value());
    EXPECT_GE(sends()[0].frames, framesSoFar);  // the frame in flight finished
    EXPECT_TRUE(sink.done.empty());               // a hold reports nothing
    EXPECT_FALSE(adapter.sending());
}

TEST_F(IrAdapterTest, AHoldSendsAtLeastWhatATapDoes) {
    exec("dev_sony", IrCommand::PressDown, "VolumeUp");
    exec("dev_sony", IrCommand::PressUp, "VolumeUp");  // at once
    advance(500);
    EXPECT_EQ(sends()[0].frames, 3u);  // ir_repeats
}

TEST_F(IrAdapterTest, ACommandsHoldMsHoldsThatLong) {
    EXPECT_EQ(tap("dev_sony", "Dimmer").status, ExecStatus::Pending);  // hold_ms 1500
    EXPECT_TRUE(sends()[0].hold);
    advance(1400);
    EXPECT_TRUE(sink.done.empty());
    advance(300);
    EXPECT_EQ(*sends()[0].stoppedAtMs, 10000u + 1500u);
    ASSERT_EQ(sink.done.size(), 1u);
    EXPECT_EQ(sink.done[0].second.status, ExecStatus::Accepted);
    // A step's hold_ms overrides it.
    exec("dev_nec", IrCommand::Press, "VolumeUp", 300);
    advance(600);
    EXPECT_EQ(*sends()[1].stoppedAtMs - sends()[1].atMs, 300u);
}

TEST_F(IrAdapterTest, ToggleCodesAlternatePerDevice) {
    const auto lastToggle = [&] { return adapter.toggle("dev_rc5"); };
    EXPECT_FALSE(lastToggle());  // starts at 0
    tap("dev_rc5", "VolumeUp");
    EXPECT_TRUE(lastToggle());   // flipped by the press: the toggled code went
    advance(300);
    tap("dev_rc5", "Mute");      // any command with a toggle code flips it
    EXPECT_FALSE(lastToggle());
    advance(300);
    // Holding sends one code for the whole hold: one press, one flip.
    exec("dev_rc5", IrCommand::PressDown, "VolumeUp");
    advance(500);
    exec("dev_rc5", IrCommand::PressUp, "VolumeUp");
    EXPECT_TRUE(lastToggle());
    advance(300);
    // A command without one leaves the bit alone; another device has its own.
    tap("dev_nec", "Mute");
    EXPECT_TRUE(lastToggle());
    EXPECT_FALSE(adapter.toggle("dev_nec"));
}

TEST_F(IrAdapterTest, TheToggledCodeIsSent) {
    // What the emitter got: the toggled and plain codes differ in their
    // second pair (where RC5's toggle bit sits).
    struct Capture : IrTransmitter {
        std::vector<uint32_t> firstMark;
        bool ready() const override { return true; }
        bool busy() const override { return false; }
        bool send(const IrSignal &s, uint16_t, bool) override {
            firstMark.push_back(s.first[2]);  // the second mark: where the toggle bit sits
            return true;
        }
        void stop() override {}
        bool takeFailed() override { return false; }
    } capture;
    IrAdapter ir{"ir", capture};
    const CommandCatalog catalog = ir.commandsFor(device("dev_rc5"));
    const ParamValue p[] = {ParamValue::ofOption(1), ParamValue::ofInt(0)};  // VolumeUp
    for (Ticket t = 1; t <= 3; t++) {
        ir.execute({&catalog.items[0], 0, p, 0x1, "dev_rc5", "dev_rc5", t, now, false});
        for (int i = 0; i < 2; i++) {  // sent, then past the key delay
            now += 1000;
            ir.poll(now, sink);
        }
    }
    ASSERT_EQ(capture.firstMark.size(), 3u);
    EXPECT_NE(capture.firstMark[0], capture.firstMark[1]);
    EXPECT_EQ(capture.firstMark[0], capture.firstMark[2]);
    // pronto_toggle's second pair is 0x20 0x20 (the toggled bit), pronto's 0x40 0x20.
    EXPECT_EQ(capture.firstMark[0], pronto::durationUs(0x20, 0x73));
    EXPECT_EQ(capture.firstMark[1], pronto::durationUs(0x40, 0x73));
}

TEST_F(IrAdapterTest, KeyDelaySpacesCommandsToOneDevice) {
    tap("dev_sony", "VolumeUp");  // key_delay_ms 200
    tap("dev_sony", "VolumeUp");
    EXPECT_EQ(adapter.queued(), 1u);
    while (sends().size() < 2) advance(1);
    ASSERT_TRUE(sends()[0].endedAtMs.has_value());
    EXPECT_GE(sends()[1].atMs - *sends()[0].endedAtMs, 200u);
    EXPECT_LE(sends()[1].atMs - *sends()[0].endedAtMs, 202u);
    // Another device doesn't wait for it.
    advance(1000);
    tap("dev_sony", "Mute");
    tap("dev_nec", "Mute");
    while (sends().size() < 4) advance(1);
    EXPECT_LE(sends()[3].atMs - *sends()[2].endedAtMs, 2u);
}

TEST_F(IrAdapterTest, OneCodeAtATime) {
    tap("dev_nec", "VolumeUp");
    tap("dev_rc5", "Mute");
    tap("dev_sony", "Mute");
    EXPECT_EQ(adapter.queued(), 2u);
    EXPECT_EQ(sends().size(), 1u);
    advance(2000);
    ASSERT_EQ(sends().size(), 3u);
    for (size_t i = 1; i < 3; i++) EXPECT_GE(sends()[i].atMs, *sends()[i - 1].endedAtMs);
    EXPECT_EQ(sink.done.size(), 3u);
    // The queue is bounded.
    for (size_t i = 0; i < IrAdapter::kQueueSize + 1; i++) tap("dev_nec", "Mute");
    EXPECT_EQ(tap("dev_nec", "Mute").error, ErrorCode::QueueFull);
}

TEST_F(IrAdapterTest, AnotherCommandEndsAHold) {
    exec("dev_nec", IrCommand::PressDown, "VolumeUp");
    advance(500);
    tap("dev_rc5", "Mute");
    EXPECT_TRUE(sends()[0].stoppedAtMs.has_value());
    advance(400);
    ASSERT_EQ(sends().size(), 2u);
    EXPECT_EQ(sends()[1].deviceId, "dev_rc5");
    EXPECT_GE(sends()[1].atMs, *sends()[0].endedAtMs);
    // Its late release changes nothing.
    exec("dev_nec", IrCommand::PressUp, "VolumeUp");
    advance(400);
    EXPECT_EQ(sends().size(), 2u);
}

TEST_F(IrAdapterTest, AHoldReleasedBeforeItsTurnIsATap) {
    tap("dev_nec", "Mute");
    exec("dev_rc5", IrCommand::PressDown, "VolumeUp");
    exec("dev_rc5", IrCommand::PressUp, "VolumeUp");
    advance(1000);
    ASSERT_EQ(sends().size(), 2u);
    EXPECT_FALSE(sends()[1].hold);
    EXPECT_EQ(sends()[1].frames, 1u);
}

TEST_F(IrAdapterTest, ReleaseAllEndsABindingsHoldButNotATimedOne) {
    exec("dev_nec", IrCommand::PressDown, "VolumeUp");
    advance(300);
    adapter.releaseAll(now);
    EXPECT_TRUE(sends()[0].stoppedAtMs.has_value());
    advance(300);
    tap("dev_sony", "Dimmer");  // a timed hold
    advance(100);
    adapter.releaseAll(now);
    EXPECT_FALSE(sends()[1].stoppedAtMs.has_value());
    advance(2000);
    EXPECT_EQ(sink.done.size(), 1u);
}

TEST_F(IrAdapterTest, CancelDropsOrEndsTheWork) {
    tap("dev_sony", "Dimmer");  // ticket 1: a timed hold, running
    const Ticket timed = lastTicket;
    tap("dev_nec", "Mute");     // ticket 2: queued
    const Ticket queued = lastTicket;
    adapter.cancel(queued, now);
    EXPECT_EQ(adapter.queued(), 0u);
    advance(100);
    adapter.cancel(timed, now);
    EXPECT_TRUE(sends()[0].stoppedAtMs.has_value());
    advance(1000);
    EXPECT_TRUE(sink.done.empty());  // cancelled work never completes
    EXPECT_EQ(sends().size(), 1u);
}

TEST_F(IrAdapterTest, ASendThatFailsOnTheWayFailsItsTicket) {
    tx.failNextSend();
    EXPECT_EQ(tap("dev_rc5", "VolumeUp").status, ExecStatus::Pending);
    EXPECT_TRUE(adapter.toggle("dev_rc5"));  // flipped when it went
    advance(300);
    ASSERT_EQ(sink.done.size(), 1u);
    EXPECT_EQ(sink.done[0].second.error, ErrorCode::AdapterFailed);
    EXPECT_FALSE(adapter.toggle("dev_rc5"));  // the device didn't see it: the next press sends the same code
    tap("dev_rc5", "VolumeUp");
    advance(300);
    EXPECT_EQ(sink.done[1].second.status, ExecStatus::Accepted);
    EXPECT_TRUE(adapter.toggle("dev_rc5"));
}

TEST_F(IrAdapterTest, ARefusedSendLeavesTheToggle) {
    struct Refusing : IrTransmitter {
        bool ready() const override { return true; }
        bool busy() const override { return false; }
        bool send(const IrSignal &, uint16_t, bool) override { return false; }
        void stop() override {}
        bool takeFailed() override { return false; }
    } refusing;
    IrAdapter ir{"ir", refusing};
    const CommandCatalog catalog = ir.commandsFor(device("dev_rc5"));
    const ParamValue p[] = {ParamValue::ofOption(1), ParamValue::ofInt(0)};  // VolumeUp
    ir.execute({&catalog.items[0], 0, p, 0x1, "dev_rc5", "dev_rc5", 7, now, false});
    EXPECT_FALSE(ir.toggle("dev_rc5"));
    ir.poll(now + 1, sink);
    ASSERT_EQ(sink.done.size(), 1u);
    EXPECT_EQ(sink.done[0].second.error, ErrorCode::AdapterUnavailable);
}

TEST(FakeIrTransmitter, AStopWhileTheNextFrameIsQueuedSendsIt) {
    // As the board: a 10 ms first frame, then 20 ms repeat frames, each
    // queued 5 ms before the one in flight ends.
    const uint32_t first[] = {1000, 9000}, repeat[] = {2000, 18000};
    IrSignal signal;
    signal.carrierHz = 38000;
    signal.first = first;
    signal.firstCount = 2;
    signal.repeat = repeat;
    signal.repeatCount = 2;
    const auto framesStoppedAt = [&](uint32_t stopMs) {
        uint32_t now = 0;
        sim::FakeIrTransmitter tx;
        tx.clock = [&] { return now; };
        EXPECT_TRUE(tx.send(signal, 1, true));
        now = stopMs;
        tx.stop();
        now = 1000;
        EXPECT_FALSE(tx.busy());
        return tx.sends()[0].frames;
    };
    EXPECT_EQ(framesStoppedAt(4), 1u);   // in the first frame
    EXPECT_EQ(framesStoppedAt(5), 2u);   // its last 5 ms: the second is queued
    EXPECT_EQ(framesStoppedAt(24), 2u);  // frame 2 runs 10..30
    EXPECT_EQ(framesStoppedAt(26), 3u);
}

TEST_F(IrAdapterTest, ReleaseAllKeepsAReleasedHoldAsATap) {
    tap("dev_nec", "Mute");
    exec("dev_rc5", IrCommand::PressDown, "VolumeUp");
    exec("dev_rc5", IrCommand::PressUp, "VolumeUp");  // a quick tap, waiting its turn
    exec("dev_sony", IrCommand::PressDown, "VolumeUp");  // still held: dropped
    adapter.releaseAll(now);
    EXPECT_EQ(adapter.queued(), 1u);
    advance(1000);
    ASSERT_EQ(sends().size(), 2u);
    EXPECT_EQ(sends()[1].deviceId, "dev_rc5");
    EXPECT_FALSE(sends()[1].hold);
}

TEST_F(IrAdapterTest, ResetForgetsEverything) {
    tap("dev_rc5", "VolumeUp");
    tap("dev_nec", "Mute");
    ASSERT_TRUE(adapter.toggle("dev_rc5"));
    adapter.reset();
    EXPECT_EQ(adapter.queued(), 0u);
    EXPECT_FALSE(adapter.sending());
    EXPECT_FALSE(adapter.toggle("dev_rc5"));
    advance(1000);
    EXPECT_TRUE(sink.done.empty());
}

TEST_F(IrAdapterTest, ACatalogLivesWhileSomethingUsesIt) {
    std::weak_ptr<const void> weak;
    {
        const CommandCatalog catalog = adapter.commandsFor(device("dev_nec"));
        ASSERT_TRUE(catalog.owner);
        weak = catalog.owner;
        // Shared while held.
        EXPECT_EQ(adapter.commandsFor(device("dev_nec")).items, catalog.items);
        // A job holds it past the catalog.
        exec("dev_nec", IrCommand::PressDown, "VolumeUp");
    }
    EXPECT_FALSE(weak.expired());
    exec("dev_nec", IrCommand::PressUp, "VolumeUp");
    advance(300);
    EXPECT_TRUE(weak.expired());  // nothing uses that profile: freed
}

TEST(IrSignal, SpecFramesFromProntoWords) {
    std::vector<uint16_t> words;
    // Once only: every frame is the once section.
    ASSERT_TRUE(pronto::parse("0000 006D 0002 0000 0156 00AB 0015 05F2", &words));
    std::vector<uint32_t> durations = {7};  // appended after what's there
    const IrCodeLayout code = appendIrCode(words.data(), &durations);
    EXPECT_EQ(code.at, 1u);
    EXPECT_EQ(code.once, 4u);
    EXPECT_EQ(code.repeat, 0u);
    EXPECT_EQ(code.carrierHz, pronto::carrierHz(0x6D));
    ASSERT_EQ(durations.size(), 5u);
    EXPECT_EQ(durations[1], pronto::durationUs(0x156, 0x6D));
    IrSignal s = irSignal(code, durations, "dev", "Power");
    EXPECT_EQ(s.first, durations.data() + 1);
    EXPECT_EQ(s.firstCount, 4u);
    EXPECT_EQ(s.repeat, s.first);
    EXPECT_EQ(s.repeatCount, 4u);
    EXPECT_STREQ(s.command, "Power");
    // Repeat only: the first frame is the repeat section.
    words.clear();
    ASSERT_TRUE(pronto::parse("0000 006D 0000 0001 0156 0056", &words));
    const IrCodeLayout repeatOnly = appendIrCode(words.data(), &durations);
    s = irSignal(repeatOnly, durations);
    EXPECT_EQ(s.first, durations.data() + 5);
    EXPECT_EQ(s.firstCount, 2u);
    EXPECT_EQ(s.repeat, s.first);
    EXPECT_STREQ(s.deviceId, "");
}

// ---- on the hub -----------------------------------------------------------

TEST(IrOnTheHub, TheRemoteHoldsAndTapsAnIrDevice) {
    sim::SimHub hub;
    ConfigSpec c = sim::fixtureConfig();
    c.devices.push_back(sim::irDevice("dev_receiver", "Receiver", "sony-receiver.json"));
    c.devices.push_back(sim::irDevice("dev_tv", "TV", "nec-tv.json"));
    const auto ir = [](const char *device, const char *cmd, const char *name) {
        return sim::action("ir", device, cmd, {sim::param("command", name)});
    };
    c.globalBindings.push_back(sim::bind("volume_up:down", {ir("dev_receiver", "press_down", "VolumeUp")}));
    c.globalBindings.push_back(sim::bind("volume_up:up", {ir("dev_receiver", "press_up", "VolumeUp")}));
    c.globalBindings.push_back(sim::bind("mute:tap", {ir("dev_tv", "press", "Mute")}));
    const ValidationReport loaded = hub.load(c);
    ASSERT_TRUE(loaded.ok()) << firstIssue(loaded);

    hub.press(remote::Button::VolumeUp);
    hub.advance(1000);
    hub.release(remote::Button::VolumeUp);
    hub.advance(500);
    hub.tap(remote::Button::Mute);
    hub.advance(500);
    const std::vector<sim::IrSend> sends = hub.irTransmitter().sends();
    ASSERT_EQ(sends.size(), 2u);
    EXPECT_EQ(sends[0].deviceId, "dev_receiver");
    EXPECT_TRUE(sends[0].hold);
    EXPECT_GT(sends[0].frames, 10u);  // about 45 ms frames for a second
    ASSERT_TRUE(sends[0].stoppedAtMs.has_value());
    EXPECT_EQ(sends[1].deviceId, "dev_tv");
    EXPECT_EQ(sends[1].command, "Mute");
    EXPECT_FALSE(sends[1].hold);
}

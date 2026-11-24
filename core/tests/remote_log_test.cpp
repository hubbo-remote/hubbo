#include "hubbo/remote/log.h"

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <vector>

#include "hubbo/remote/input.h"
#include "hubbo/remote/packet.h"

namespace hubbo::remote {
namespace {

struct Read {
    std::vector<LogEntry> entries;
    std::vector<uint32_t> ages;
    RemoteLogInfo info;
};

Read readAll(const RemoteLog &log, uint32_t nowMs) {
    Read r;
    r.info = log.read(nowMs, [&](const LogEntry &e, uint32_t age) {
        r.entries.push_back(e);
        r.ages.push_back(age);
    });
    return r;
}

LogEntry anomalyAt(uint32_t atMs, uint32_t stallMs = 0) {
    LogEntry e;
    e.atMs = atMs;
    e.type = LogType::Anomaly;
    e.anomaly = Anomaly::RfStall;
    e.stallMs = stallMs;
    return e;
}

struct RemoteLogTest : ::testing::Test {
    MutexLogLock lock;
    std::unique_ptr<RemoteLog> log = std::make_unique<RemoteLog>(lock);  // ~900 KB: never on the stack
};

TEST_F(RemoteLogTest, ReadsOldestFirstWithAges) {
    for (uint32_t t : {100u, 200u, 300u}) log->append(anomalyAt(t, t));
    const Read r = readAll(*log, 1000);
    ASSERT_EQ(r.entries.size(), 3u);
    EXPECT_EQ(r.entries[0].stallMs, 100u);
    EXPECT_EQ(r.entries[2].stallMs, 300u);
    EXPECT_EQ(r.ages, (std::vector<uint32_t>{900, 800, 700}));
    EXPECT_EQ(r.info.spanMs, 1000u);  // since boot
    EXPECT_EQ(r.info.dropped, 0u);
}

TEST_F(RemoteLogTest, KeepsFiveMinutes) {
    log->append(anomalyAt(1000, 1));
    log->append(anomalyAt(2000, 2));
    Read r = readAll(*log, 1000 + kRemoteLogWindowMs);
    ASSERT_EQ(r.entries.size(), 2u);  // exactly 5 minutes old: still in
    EXPECT_EQ(r.ages[0], kRemoteLogWindowMs);
    r = readAll(*log, 1001 + kRemoteLogWindowMs);
    ASSERT_EQ(r.entries.size(), 1u);
    EXPECT_EQ(r.entries[0].stallMs, 2u);
    EXPECT_EQ(r.info.spanMs, kRemoteLogWindowMs);
    EXPECT_EQ(r.info.dropped, 0u);  // aging out isn't dropping
    // Stamped just after the reader's now (another task's clock): age 0.
    log->append(anomalyAt(5000, 3));
    r = readAll(*log, 4990);
    EXPECT_EQ(r.ages.back(), 0u);
}

TEST_F(RemoteLogTest, TheCountBoundDropsTheOldestAndShortensTheSpan) {
    const uint32_t extra = 10;
    for (uint32_t i = 0; i < kRemoteLogCapacity + extra; ++i) log->append(anomalyAt(1000 + i, i));
    const uint32_t now = 1000 + kRemoteLogCapacity + extra;
    const Read r = readAll(*log, now);
    ASSERT_EQ(r.entries.size(), kRemoteLogCapacity);
    EXPECT_EQ(r.entries.front().stallMs, extra);  // the first 10 went
    EXPECT_EQ(r.entries.back().stallMs, kRemoteLogCapacity + extra - 1);
    for (size_t i = 1; i < r.entries.size(); ++i) ASSERT_LT(r.entries[i - 1].stallMs, r.entries[i].stallMs);
    EXPECT_EQ(r.info.dropped, extra);
    EXPECT_EQ(r.info.spanMs, now - (1000 + extra - 1));  // complete since the newest dropped one
}

TEST_F(RemoteLogTest, PushingOutAnAgedEntryIsNoDrop) {
    for (uint32_t i = 0; i < kRemoteLogCapacity; ++i) log->append(anomalyAt(i));
    const uint32_t later = kRemoteLogCapacity + kRemoteLogWindowMs;
    for (uint32_t i = 0; i < 5; ++i) log->append(anomalyAt(later + i, 7));
    const Read r = readAll(*log, later + 5);
    EXPECT_EQ(r.info.dropped, 0u);
    EXPECT_EQ(r.info.spanMs, kRemoteLogWindowMs);
    ASSERT_FALSE(r.entries.empty());
    EXPECT_EQ(r.entries.back().stallMs, 7u);
}

TEST_F(RemoteLogTest, AWrappedMostlyStaleRingReadsOnlyTheRecent) {
    for (uint32_t i = 0; i < kRemoteLogCapacity + 100; ++i) log->append(anomalyAt(i, 0));
    const uint32_t later = 2 * kRemoteLogWindowMs;
    for (uint32_t i = 1; i <= 3; ++i) log->append(anomalyAt(later + i, i));
    const Read r = readAll(*log, later + 10);
    ASSERT_EQ(r.entries.size(), 3u);
    EXPECT_EQ(r.entries[0].stallMs, 1u);
    EXPECT_EQ(r.entries[2].stallMs, 3u);
    EXPECT_EQ(r.ages, (std::vector<uint32_t>{9, 8, 7}));
    EXPECT_EQ(r.info.spanMs, kRemoteLogWindowMs);  // what was dropped is long past the window
}

TEST_F(RemoteLogTest, ClearIsARestart) {
    for (uint32_t i = 0; i < kRemoteLogCapacity + 1; ++i) log->append(anomalyAt(i));
    log->clear(500000);
    const Read r = readAll(*log, 500100);
    EXPECT_TRUE(r.entries.empty());
    EXPECT_EQ(r.info.dropped, 0u);
    EXPECT_EQ(r.info.spanMs, 100u);
}

TEST_F(RemoteLogTest, Names) {
    EXPECT_STREQ(logTypeName(LogType::Anomaly), "anomaly");
    EXPECT_STREQ(packetKindName(PacketKind::Repeat), "repeat");
    EXPECT_STREQ(anomalyName(Anomaly::RfStall), "rf_stall");
    EXPECT_STREQ(bindingKindName(BindingKind::Activity), "activity");
    Signal s = Signal::Unknown;
    ASSERT_TRUE(signalFromName("weak", &s));
    EXPECT_EQ(s, Signal::Weak);
    EXPECT_STREQ(signalName(Signal::Strong), "strong");
    EXPECT_FALSE(signalFromName("loud", &s));
}

// RemoteInput's hooks: each payload's packet (before its events), the
// events, and the anomalies.
struct NullSink : EventSink {
    void onRemoteEvent(const RemoteEvent &) override {}
};

struct InputHooks : RemoteLogTest {
    NullSink sink;
    RemoteInput input{sink};
    void SetUp() override { input.setLog(log.get()); }
    void send(uint8_t group, ButtonSet held, uint32_t nowMs, Signal signal = Signal::Strong) {
        uint8_t packet[kButtonPacketLength];
        encodePacket(group, held, packet);
        input.onPayload(packet, sizeof packet, nowMs, signal);
    }
};

std::string describe(const LogEntry &e) {
    const std::string button = e.button == Button::Count ? "-" : buttonName(e.button);
    switch (e.type) {
        case LogType::Packet:
            return std::string("packet ") + packetKindName(e.packet) + " " + button + " " + signalName(e.signal);
        case LogType::Event:
            return std::string("event ") + eventKindName(e.event) + " " + button + (e.timedOut ? " timed_out" : "");
        case LogType::Anomaly:
            return std::string("anomaly ") + anomalyName(e.anomaly);
        case LogType::Action:
            return "action";
    }
    return "?";
}

std::vector<std::string> describeAll(const RemoteLog &log, uint32_t nowMs) {
    std::vector<std::string> out;
    log.read(nowMs, [&](const LogEntry &e, uint32_t) { out.push_back(describe(e)); });
    return out;
}

TEST_F(InputHooks, LogsPacketsThenTheirEvents) {
    const uint8_t keepalive[] = {0x00, 0x4F, 0x00, 0x00, 0xB1};
    const uint8_t group = buttonInfo(Button::Up).group;
    send(group, buttonBit(Button::Up), 0);
    input.onPayload(keepalive, sizeof keepalive, 40, Signal::Weak);
    send(group, buttonBit(Button::Up), 80);  // the same held set again
    send(group, 0, 120);
    send(group, 0, 160);  // an all-clear that releases nothing
    EXPECT_EQ(describeAll(*log, 200), (std::vector<std::string>{
                                          "packet down up strong",
                                          "event down up",
                                          "packet hold up weak",
                                          "packet repeat up strong",
                                          "packet up up strong",
                                          "event up up",
                                          "event tap up",
                                          "packet up - strong",
                                      }));
}

TEST_F(InputHooks, LogsATimedOutRelease) {
    send(buttonInfo(Button::Select).group, buttonBit(Button::Select), 0);
    input.poll(1000);  // no packet for 500 ms: released, before its hold was due
    EXPECT_EQ(describeAll(*log, 1000), (std::vector<std::string>{
                                           "packet down select strong",
                                           "event down select",
                                           "event up select timed_out",
                                           "event tap select timed_out",
                                       }));
}

TEST_F(InputHooks, LogsBadChecksumsAndUnknownCodes) {
    uint8_t packet[kButtonPacketLength];
    const uint8_t group = buttonInfo(Button::Up).group;
    encodePacket(group, buttonBit(Button::Up), packet);
    packet[9] ^= 0xFF;
    input.onPayload(packet, sizeof packet, 0, Signal::Unknown);
    const uint8_t unknown[] = {0x1D, 0xC1, 0, 0x77, 0, 0, 0, 0, 0, 0xAB};  // keypad code 0x77: no button
    input.onPayload(unknown, sizeof unknown, 10, Signal::Unknown);
    EXPECT_EQ(describeAll(*log, 20), (std::vector<std::string>{
                                         "anomaly bad_checksum",
                                         "packet up - unknown",
                                         "anomaly unknown_code",
                                     }));
}

TEST(RemoteLogHooks, ANullLogIsNoLog) {
    NullSink sink;
    RemoteInput input(sink);  // no setLog
    uint8_t packet[kButtonPacketLength];
    encodePacket(buttonInfo(Button::Up).group, buttonBit(Button::Up), packet);
    input.onPayload(packet, sizeof packet, 0);
    logAnomaly(nullptr, 0, Anomaly::RfStall, 9);
    EXPECT_EQ(input.held(), buttonBit(Button::Up));
}

}  // namespace
}  // namespace hubbo::remote

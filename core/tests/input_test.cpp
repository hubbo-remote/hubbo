#include "hubbo/remote/input.h"

#include <gtest/gtest.h>

#include <array>
#include <initializer_list>
#include <string>
#include <vector>

using namespace hubbo::remote;

namespace {

struct Recorder : EventSink {
    std::vector<RemoteEvent> events;
    void onRemoteEvent(const RemoteEvent &event) override { events.push_back(event); }

    // "down:up@0" style strings make sequences easy to read in failures.
    std::vector<std::string> log() const {
        std::vector<std::string> out;
        for (const auto &e : events) {
            out.push_back(std::string(eventKindName(e.kind)) + ":" + buttonName(e.button) + "@" +
                          std::to_string(e.atMs) + (e.timedOut ? "!" : ""));
        }
        return out;
    }
};

// A valid button payload holding `buttons` (all from `group`).
std::array<uint8_t, kButtonPacketLength> payload(uint8_t group, std::initializer_list<Button> buttons) {
    ButtonSet held = 0;
    for (Button b : buttons) held |= buttonBit(b);
    std::array<uint8_t, kButtonPacketLength> p;
    EXPECT_TRUE(encodePacket(group, held, p.data()));
    return p;
}

// Any non-button payload: stands in for the remote's keepalives (their
// exact bytes don't matter, only that a packet arrived).
const uint8_t kKeepalive[5] = {0x00, 0x4F, 0x00, 0x00, 0xB1};

class InputTest : public ::testing::Test {
protected:
    Recorder rec;
    RemoteInput input{rec};

    void keypad(uint32_t t, std::initializer_list<Button> b) {
        auto p = payload(kGroupKeypad, b);
        input.onPayload(p.data(), p.size(), t);
    }
    void media(uint32_t t, std::initializer_list<Button> b) {
        auto p = payload(kGroupMediaHome, b);
        input.onPayload(p.data(), p.size(), t);
    }
    void keepalive(uint32_t t) { input.onPayload(kKeepalive, sizeof(kKeepalive), t); }

    using Log = std::vector<std::string>;
};

}  // namespace

TEST_F(InputTest, ShortPressIsDownUpTap) {
    keypad(1000, {Button::Up});
    keypad(1120, {});
    input.poll(5000);
    EXPECT_EQ(rec.log(), (Log{"down:up@1000", "up:up@1120", "tap:up@1120"}));
}

TEST_F(InputTest, RepeatedPacketsForTheSameHoldAreNotNewPresses) {
    keypad(0, {Button::Down});
    keypad(40, {Button::Down});
    keypad(80, {Button::Down});
    keypad(100, {});
    EXPECT_EQ(rec.log(), (Log{"down:down@0", "up:down@100", "tap:down@100"}));
}

TEST_F(InputTest, LongPressFiresHoldOnceAndNoTap) {
    media(0, {Button::Info});
    for (uint32_t t = 40; t <= 1200; t += 40) {
        keepalive(t);
        input.poll(t);
    }
    media(1210, {});
    EXPECT_EQ(rec.log(), (Log{"down:info@0", "hold:info@800", "up:info@1210"}));
}

TEST_F(InputTest, HoldTimestampIsExactEvenWhenPolledLate) {
    media(0, {Button::Info});
    keepalive(400);
    keepalive(850);  // first chance to notice: hold is reported at 800
    EXPECT_EQ(rec.log(), (Log{"down:info@0", "hold:info@800"}));
}

TEST_F(InputTest, LostReleaseTimesOutAsTap) {
    keypad(0, {Button::Right});
    keepalive(40);
    input.poll(539);
    EXPECT_EQ(rec.events.size(), 1u);
    input.poll(540);
    EXPECT_EQ(rec.log(), (Log{"down:right@0", "up:right@540!", "tap:right@540!"}));
    EXPECT_EQ(input.held(), 0u);
}

TEST_F(InputTest, LostReleaseDuringHold) {
    media(0, {Button::VolumeUp});
    for (uint32_t t = 40; t <= 1000; t += 40) keepalive(t);
    input.poll(3000);  // very late poll: release is judged as of 1000 + 500
    EXPECT_EQ(rec.log(),
              (Log{"down:volume_up@0", "hold:volume_up@800", "up:volume_up@1500!"}));
}

TEST_F(InputTest, LatePollDoesNotInventAHoldAfterSilence) {
    keypad(0, {Button::Select});
    input.poll(5000);  // silent since 0: released at 500, before hold at 800
    EXPECT_EQ(rec.log(), (Log{"down:select@0", "up:select@500!", "tap:select@500!"}));
}

TEST_F(InputTest, ChordAcrossGroupsKeepsEachGroupIndependent) {
    keypad(0, {Button::Up});
    media(10, {Button::VolumeUp});
    media(50, {});  // releases only the media group
    EXPECT_EQ(input.held(), buttonBit(Button::Up));
    keypad(60, {});
    EXPECT_EQ(rec.log(), (Log{"down:up@0", "down:volume_up@10", "up:volume_up@50",
                              "tap:volume_up@50", "up:up@60", "tap:up@60"}));
}

TEST_F(InputTest, ChangeWithinGroupReleasesBeforePressing) {
    keypad(0, {Button::Up});
    keypad(30, {Button::Down});
    EXPECT_EQ(rec.log(), (Log{"down:up@0", "up:up@30", "tap:up@30", "down:down@30"}));
}

TEST_F(InputTest, BadChecksumIsIgnoredButCounted) {
    auto p = payload(kGroupKeypad, {Button::Up});
    p[9] ^= 1;
    input.onPayload(p.data(), p.size(), 0);
    EXPECT_TRUE(rec.events.empty());
    EXPECT_EQ(input.badChecksumCount(), 1u);
}

TEST_F(InputTest, ReleaseAll) {
    keypad(0, {Button::Up, Button::Left});
    input.releaseAll(10);
    EXPECT_EQ(rec.log(), (Log{"down:up@0", "down:left@0", "up:up@10", "tap:up@10",
                              "up:left@10", "tap:left@10"}));
    EXPECT_EQ(input.held(), 0u);
}

TEST_F(InputTest, SurvivesMillisWraparound) {
    const uint32_t start = 0xFFFFFF00u;  // wraps 256 ms in
    media(start, {Button::Info});
    for (uint32_t i = 1; i <= 25; i++) keepalive(start + i * 40);
    EXPECT_EQ(rec.log(), (Log{"down:info@4294967040", "hold:info@544"}));
}

TEST(EventKind, NameRoundTrip) {
    for (size_t i = 0; i < kEventKindCount; i++) {
        EventKind kind;
        ASSERT_TRUE(eventKindFromName(eventKindName(static_cast<EventKind>(i)), &kind));
        EXPECT_EQ(static_cast<size_t>(kind), i);
    }
}

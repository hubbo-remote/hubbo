#include "hubbo/remote/packet.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>

using namespace hubbo::remote;

namespace {

using Payload = std::array<uint8_t, 10>;

DecodeResult decode(const Payload &p, DecodedPacket *out) {
    return decodePacket(p.data(), p.size(), out);
}

}  // namespace

// Real captures from Harmonizer/docs/harmony_companion_codes.
TEST(Packet, CapturedPressesDecode) {
    struct Case {
        Payload bytes;
        uint8_t group;
        Button button;
    };
    const Case cases[] = {
        {{0x1D, 0xC3, 0xEC, 0x01, 0, 0, 0, 0, 0, 0x33}, kGroupMediaHome, Button::Off},
        {{0x1D, 0xC3, 0x24, 0x02, 0, 0, 0, 0, 0, 0xFA}, kGroupMediaHome, Button::Back},
        {{0x1D, 0xC3, 0xF2, 0x0F, 0, 0, 0, 0, 0, 0x1F}, kGroupMediaHome, Button::Light1},
        {{0x1D, 0xC3, 0xE9, 0x00, 0, 0, 0, 0, 0, 0x37}, kGroupMediaHome, Button::VolumeUp},
        {{0x1D, 0xC1, 0x00, 0x65, 0, 0, 0, 0, 0, 0xBD}, kGroupKeypad, Button::Menu},
        {{0x1D, 0xC1, 0x00, 0x58, 0, 0, 0, 0, 0, 0xCA}, kGroupKeypad, Button::Select},
        {{0x1D, 0xC1, 0x00, 0x52, 0, 0, 0, 0, 0, 0xD0}, kGroupKeypad, Button::Up},
    };
    for (const auto &c : cases) {
        DecodedPacket out;
        ASSERT_EQ(decode(c.bytes, &out), DecodeResult::Buttons) << buttonName(c.button);
        EXPECT_EQ(out.group, c.group);
        EXPECT_EQ(out.held, buttonBit(c.button)) << buttonName(c.button);
        EXPECT_EQ(out.unknownCodes, 0);
    }
}

TEST(Packet, CapturedReleasesAreEmptySets) {
    DecodedPacket out;
    ASSERT_EQ(decode({0x1D, 0xC3, 0, 0, 0, 0, 0, 0, 0, 0x20}, &out), DecodeResult::Buttons);
    EXPECT_EQ(out.group, kGroupMediaHome);
    EXPECT_EQ(out.held, 0u);

    ASSERT_EQ(decode({0x1D, 0xC1, 0, 0, 0, 0, 0, 0, 0, 0x22}, &out), DecodeResult::Buttons);
    EXPECT_EQ(out.group, kGroupKeypad);
    EXPECT_EQ(out.held, 0u);
}

TEST(Packet, MultipleButtonsInOneGroup) {
    // up + right on the keypad: 0x1D + 0xC1 + 0x52 + 0x4F = 0x17F -> crc 0x81
    DecodedPacket out;
    ASSERT_EQ(decode({0x1D, 0xC1, 0, 0x52, 0x4F, 0, 0, 0, 0, 0x81}, &out), DecodeResult::Buttons);
    EXPECT_EQ(out.held, buttonBit(Button::Up) | buttonBit(Button::Right));
}

TEST(Packet, EncodeMatchesCapturedBytes) {
    uint8_t out[kButtonPacketLength];
    ASSERT_TRUE(encodePacket(kGroupMediaHome, buttonBit(Button::Off), out));
    const Payload off{0x1D, 0xC3, 0xEC, 0x01, 0, 0, 0, 0, 0, 0x33};
    EXPECT_TRUE(std::equal(off.begin(), off.end(), out));
    ASSERT_TRUE(encodePacket(kGroupKeypad, 0, out));
    const Payload keypadRelease{0x1D, 0xC1, 0, 0, 0, 0, 0, 0, 0, 0x22};
    EXPECT_TRUE(std::equal(keypadRelease.begin(), keypadRelease.end(), out));
}

TEST(Packet, EveryButtonRoundTrips) {
    for (size_t i = 0; i < kButtonCount; i++) {
        const auto button = static_cast<Button>(i);
        const uint8_t group = buttonInfo(button).group;
        uint8_t out[kButtonPacketLength];
        ASSERT_TRUE(encodePacket(group, buttonBit(button), out));
        DecodedPacket decoded;
        ASSERT_EQ(decodePacket(out, sizeof(out), &decoded), DecodeResult::Buttons);
        EXPECT_EQ(decoded.group, group);
        EXPECT_EQ(decoded.held, buttonBit(button)) << buttonName(button);
    }
}

TEST(Packet, EncodeRefusesWhatTheRemoteCantSend) {
    uint8_t out[kButtonPacketLength];
    // Wrong group: dropped.
    EXPECT_FALSE(encodePacket(kGroupKeypad, buttonBit(Button::Up) | buttonBit(Button::Off), out));
    DecodedPacket decoded;
    ASSERT_EQ(decodePacket(out, sizeof(out), &decoded), DecodeResult::Buttons);
    EXPECT_EQ(decoded.held, buttonBit(Button::Up));
    // Media/home has two slots.
    EXPECT_FALSE(encodePacket(kGroupMediaHome,
                              buttonBit(Button::Red) | buttonBit(Button::Green) | buttonBit(Button::Blue),
                              out));
}

TEST(Packet, BadChecksum) {
    DecodedPacket out;
    EXPECT_EQ(decode({0x1D, 0xC3, 0xEC, 0x01, 0, 0, 0, 0, 0, 0x34}, &out),
              DecodeResult::BadChecksum);
}

TEST(Packet, NonButtonPayloads) {
    DecodedPacket out;
    const uint8_t keepalive[5] = {0x00, 0x4F, 0x00, 0x00, 0xB1};
    EXPECT_EQ(decodePacket(keepalive, sizeof(keepalive), &out), DecodeResult::NotButtons);
    EXPECT_EQ(decode({0x1D, 0xC2, 0, 0, 0, 0, 0, 0, 0, 0x21}, &out), DecodeResult::NotButtons);
    EXPECT_EQ(decodePacket(nullptr, 10, &out), DecodeResult::NotButtons);
}

TEST(Packet, UnknownCodesAreCountedNotFatal) {
    // keypad code 0x77 is not a known button: 0x1D + 0xC1 + 0x77 = 0x155 -> crc 0xAB
    DecodedPacket out;
    ASSERT_EQ(decode({0x1D, 0xC1, 0, 0x77, 0, 0, 0, 0, 0, 0xAB}, &out), DecodeResult::Buttons);
    EXPECT_EQ(out.held, 0u);
    EXPECT_EQ(out.unknownCodes, 1);
}

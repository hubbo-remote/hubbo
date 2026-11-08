#include "hubbo/remote/packet.h"

namespace hubbo::remote {

namespace {

void addCode(uint8_t group, uint16_t code, DecodedPacket *out) {
    if (code == 0) {
        return;  // empty slot
    }
    Button button;
    if (buttonFromCode(group, code, &button)) {
        out->held |= buttonBit(button);
    } else if (out->unknownCodes < 0xFF) {
        out->unknownCodes++;
    }
}

}  // namespace

uint8_t packetChecksum(const uint8_t *bytes) {
    uint8_t sum = 0;
    for (size_t i = 0; i < 9; i++) {
        sum = static_cast<uint8_t>(sum + bytes[i]);
    }
    return static_cast<uint8_t>(0u - sum);
}

bool encodePacket(uint8_t group, ButtonSet held, uint8_t out[kButtonPacketLength]) {
    for (size_t i = 0; i < kButtonPacketLength; i++) out[i] = 0;
    out[0] = 0x1D;  // first byte of every captured payload; decode ignores it
    out[1] = group;
    bool complete = (held & ~groupMask(group)) == 0;
    held &= groupMask(group);
    size_t slot = 0;
    const size_t slots = group == kGroupKeypad ? 6 : 2;
    for (size_t i = 0; i < kButtonCount && held != 0; i++) {
        const auto button = static_cast<Button>(i);
        if ((held & buttonBit(button)) == 0) continue;
        held &= ~buttonBit(button);
        if (slot == slots) {
            complete = false;
            break;
        }
        const uint16_t code = buttonInfo(button).code;
        if (group == kGroupKeypad) {
            out[3 + slot] = static_cast<uint8_t>(code);
        } else {
            out[2 + 2 * slot] = static_cast<uint8_t>(code & 0xFF);
            out[3 + 2 * slot] = static_cast<uint8_t>(code >> 8);
        }
        slot++;
    }
    out[9] = packetChecksum(out);
    return complete;
}

DecodeResult decodePacket(const uint8_t *bytes, size_t length, DecodedPacket *out) {
    if (bytes == nullptr || out == nullptr || length < kButtonPacketLength) {
        return DecodeResult::NotButtons;
    }
    const uint8_t group = bytes[1];
    if (group != kGroupKeypad && group != kGroupMediaHome) {
        return DecodeResult::NotButtons;
    }

    if (packetChecksum(bytes) != bytes[9]) {
        return DecodeResult::BadChecksum;
    }

    *out = DecodedPacket{};
    out->group = group;
    if (group == kGroupKeypad) {
        for (size_t i = 3; i <= 8; i++) {
            addCode(group, bytes[i], out);
        }
    } else {
        addCode(group, static_cast<uint16_t>(bytes[2] | (bytes[3] << 8)), out);
        addCode(group, static_cast<uint16_t>(bytes[4] | (bytes[5] << 8)), out);
    }
    return DecodeResult::Buttons;
}

}  // namespace hubbo::remote

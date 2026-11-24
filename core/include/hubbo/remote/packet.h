#pragma once

#include <cstddef>
#include <cstdint>

#include "hubbo/remote/button.h"

// Decodes one nRF24 payload from a Harmony Companion remote.
//
// Button payloads are 10 bytes, and each one carries the complete set of
// buttons currently held *in its group*, not a press/release event:
//
//   byte:  0    1    2      3      4      5      6    7    8    9
//   C1:    ID   C1   00     B1     B2     B3     B4   B5   B6   CRC
//   C3:    ID   C3   B1_LO  B1_HI  B2_LO  B2_HI  00   00   00   CRC
//
// CRC = -(sum of bytes 0..8) mod 256. A payload with every slot zero is the
// group's "released" packet. While a button is held the remote also sends
// short non-button packets roughly every 40 ms; those only prove the remote
// is still talking (see RemoteInput).
namespace hubbo::remote {

enum class DecodeResult : uint8_t {
    Buttons,     // valid button payload; `out` filled
    NotButtons,  // not a button payload (keepalive etc.); still a live packet
    BadChecksum, // looked like a button payload but failed the checksum
};

struct DecodedPacket {
    uint8_t group = 0;     // kGroupKeypad or kGroupMediaHome
    ButtonSet held = 0;    // every button held in that group
    uint8_t unknownCodes = 0; // nonzero slots that matched no known button
};

constexpr size_t kButtonPacketLength = 10;

// The nRF24's received power detector for a packet (the board reads it per
// packet); Unknown where there's no radio to ask.
enum class Signal : uint8_t { Unknown, Strong, Weak };

DecodeResult decodePacket(const uint8_t *bytes, size_t length, DecodedPacket *out);

// Builds the button payload the remote would send for `held` in `group`
// (used by the simulator and tests). Returns false, and drops the extras, if
// `held` has buttons from another group or more than the group's slots
// (keypad 6, media/home 2).
bool encodePacket(uint8_t group, ButtonSet held, uint8_t out[kButtonPacketLength]);

// -(sum of bytes 0..8) mod 256: byte 9 of a button payload.
uint8_t packetChecksum(const uint8_t *bytes);

}  // namespace hubbo::remote

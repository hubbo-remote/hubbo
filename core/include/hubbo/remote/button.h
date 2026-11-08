#pragma once

#include <cstddef>
#include <cstdint>

// Harmony Companion buttons. The enum value is a dense index, used directly
// as a lookup-table index on the hot path. The string name is the stable
// public identifier (API, config files) and matches Harmonizer's names.
//
// Adding a button: append before Count. Never reorder; never rename a name.
namespace hubbo::remote {

enum class Button : uint8_t {
    // Keypad group (0xC1)
    Up,
    Down,
    Left,
    Right,
    Select,
    Menu,
    Number1,
    Number2,
    Number3,
    Number4,
    Number5,
    Number6,
    Number7,
    Number8,
    Number9,
    Number0,
    NumberDotDash,
    NumberEnter,

    // MediaHome group (0xC3)
    Off,
    Music,
    Tv,
    Movie,
    Light1,
    Light2,
    Plus,
    Minus,
    Switch1,
    Switch2,
    Red,
    Green,
    Yellow,
    Blue,
    Dvr,
    Guide,
    Info,
    Back,
    Exit,
    VolumeUp,
    VolumeDown,
    Mute,
    ChannelUp,
    ChannelDown,
    SkipBackward,
    SkipForward,
    Play,
    Pause,
    Record,
    Stop,

    Count
};

constexpr size_t kButtonCount = static_cast<size_t>(Button::Count);
static_assert(kButtonCount <= 64, "ButtonSet is a 64-bit mask");

constexpr uint8_t kGroupKeypad = 0xC1;
constexpr uint8_t kGroupMediaHome = 0xC3;

struct ButtonInfo {
    Button button;
    uint8_t group;
    uint16_t code;
    const char *name;
};

// Every button, in enum order.
const ButtonInfo &buttonInfo(Button button);
const char *buttonName(Button button);

// Lookups for config/API (by name) and radio decode (by group + code).
// Return false when there is no such button.
bool buttonFromName(const char *name, Button *out);
bool buttonFromCode(uint8_t group, uint16_t code, Button *out);

// A set of buttons as a bitmask indexed by Button.
using ButtonSet = uint64_t;

constexpr ButtonSet buttonBit(Button button) {
    return ButtonSet{1} << static_cast<uint8_t>(button);
}

// Every button in a radio group (kGroupKeypad / kGroupMediaHome); 0 for any
// other group.
ButtonSet groupMask(uint8_t group);

}  // namespace hubbo::remote

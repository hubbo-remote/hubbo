#include "hubbo/remote/button.h"

#include <cstring>

namespace hubbo::remote {
namespace {

// Codes from Harmonizer (src/smart_hub_buttons.cpp) and its captured packet
// log (docs/harmony_companion_codes). Order must match the Button enum
// (checked at compile time below).
constexpr ButtonInfo kButtons[] = {
    {Button::Up, kGroupKeypad, 0x0052, "up"},
    {Button::Down, kGroupKeypad, 0x0051, "down"},
    {Button::Left, kGroupKeypad, 0x0050, "left"},
    {Button::Right, kGroupKeypad, 0x004F, "right"},
    {Button::Select, kGroupKeypad, 0x0058, "select"},
    {Button::Menu, kGroupKeypad, 0x0065, "menu"},
    {Button::Number1, kGroupKeypad, 0x001E, "number_1"},
    {Button::Number2, kGroupKeypad, 0x001F, "number_2"},
    {Button::Number3, kGroupKeypad, 0x0020, "number_3"},
    {Button::Number4, kGroupKeypad, 0x0021, "number_4"},
    {Button::Number5, kGroupKeypad, 0x0022, "number_5"},
    {Button::Number6, kGroupKeypad, 0x0023, "number_6"},
    {Button::Number7, kGroupKeypad, 0x0024, "number_7"},
    {Button::Number8, kGroupKeypad, 0x0025, "number_8"},
    {Button::Number9, kGroupKeypad, 0x0026, "number_9"},
    {Button::Number0, kGroupKeypad, 0x0027, "number_0"},
    {Button::NumberDotDash, kGroupKeypad, 0x0056, "number_dot_dash"},
    {Button::NumberEnter, kGroupKeypad, 0x0028, "number_enter"},

    {Button::Off, kGroupMediaHome, 0x01EC, "off"},
    {Button::Music, kGroupMediaHome, 0x01E8, "music"},
    {Button::Tv, kGroupMediaHome, 0x01ED, "tv"},
    {Button::Movie, kGroupMediaHome, 0x01E9, "movie"},
    {Button::Light1, kGroupMediaHome, 0x0FF2, "light_1"},
    {Button::Light2, kGroupMediaHome, 0x0FF3, "light_2"},
    {Button::Plus, kGroupMediaHome, 0x0FF0, "plus"},
    {Button::Minus, kGroupMediaHome, 0x0FF1, "minus"},
    {Button::Switch1, kGroupMediaHome, 0x0FF4, "switch_1"},
    {Button::Switch2, kGroupMediaHome, 0x0FF5, "switch_2"},
    {Button::Red, kGroupMediaHome, 0x01F7, "red"},
    {Button::Green, kGroupMediaHome, 0x01F6, "green"},
    {Button::Yellow, kGroupMediaHome, 0x01F5, "yellow"},
    {Button::Blue, kGroupMediaHome, 0x01F4, "blue"},
    {Button::Dvr, kGroupMediaHome, 0x009A, "dvr"},
    {Button::Guide, kGroupMediaHome, 0x008D, "guide"},
    {Button::Info, kGroupMediaHome, 0x01FF, "info"},
    {Button::Back, kGroupMediaHome, 0x0224, "back"},
    {Button::Exit, kGroupMediaHome, 0x0094, "exit"},
    {Button::VolumeUp, kGroupMediaHome, 0x00E9, "volume_up"},
    {Button::VolumeDown, kGroupMediaHome, 0x00EA, "volume_down"},
    {Button::Mute, kGroupMediaHome, 0x00E2, "mute"},
    {Button::ChannelUp, kGroupMediaHome, 0x009C, "channel_up"},
    {Button::ChannelDown, kGroupMediaHome, 0x009D, "channel_down"},
    {Button::SkipBackward, kGroupMediaHome, 0x00B4, "skip_backward"},
    {Button::SkipForward, kGroupMediaHome, 0x00B3, "skip_forward"},
    {Button::Play, kGroupMediaHome, 0x00B0, "play"},
    {Button::Pause, kGroupMediaHome, 0x00B1, "pause"},
    {Button::Record, kGroupMediaHome, 0x00B2, "record"},
    {Button::Stop, kGroupMediaHome, 0x00B7, "stop"},
};

static_assert(sizeof(kButtons) / sizeof(kButtons[0]) == kButtonCount,
              "kButtons must list every Button");

constexpr bool inEnumOrder() {
    for (size_t i = 0; i < kButtonCount; i++) {
        if (static_cast<size_t>(kButtons[i].button) != i) return false;
    }
    return true;
}
static_assert(inEnumOrder(), "kButtons must be in Button enum order");

constexpr ButtonSet maskOf(uint8_t group) {
    ButtonSet mask = 0;
    for (const auto &info : kButtons) {
        if (info.group == group) mask |= buttonBit(info.button);
    }
    return mask;
}
constexpr ButtonSet kKeypadMask = maskOf(kGroupKeypad);
constexpr ButtonSet kMediaHomeMask = maskOf(kGroupMediaHome);

}  // namespace

const ButtonInfo &buttonInfo(Button button) {
    return kButtons[static_cast<size_t>(button)];
}

ButtonSet groupMask(uint8_t group) {
    switch (group) {
        case kGroupKeypad: return kKeypadMask;
        case kGroupMediaHome: return kMediaHomeMask;
        default: return 0;
    }
}

const char *buttonName(Button button) {
    return buttonInfo(button).name;
}

bool buttonFromName(const char *name, Button *out) {
    if (name == nullptr) {
        return false;
    }
    for (const auto &info : kButtons) {
        if (std::strcmp(info.name, name) == 0) {
            *out = info.button;
            return true;
        }
    }
    return false;
}

bool buttonFromCode(uint8_t group, uint16_t code, Button *out) {
    // 48 entries: a linear scan is a few hundred ns and needs no extra RAM.
    for (const auto &info : kButtons) {
        if (info.group == group && info.code == code) {
            *out = info.button;
            return true;
        }
    }
    return false;
}

}  // namespace hubbo::remote

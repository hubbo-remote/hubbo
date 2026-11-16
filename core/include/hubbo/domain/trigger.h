#pragma once

#include <cstddef>
#include <string>
#include <string_view>

#include "hubbo/remote/button.h"
#include "hubbo/remote/input.h"

// A trigger is a button plus an event kind. Its public string form is
// "<button>:<event>" using buttonName()/eventKindName(), e.g. "up:down",
// "info:hold", "select:tap". Every binding is keyed by one.
namespace hubbo {

struct Trigger {
    remote::Button button = remote::Button::Up;
    remote::EventKind kind = remote::EventKind::Down;

    bool operator==(const Trigger &other) const {
        return button == other.button && kind == other.kind;
    }
};

constexpr size_t kTriggerCount = remote::kButtonCount * remote::kEventKindCount;

// Dense index in [0, kTriggerCount).
constexpr size_t triggerIndex(Trigger t) {
    return static_cast<size_t>(t.button) * remote::kEventKindCount + static_cast<size_t>(t.kind);
}

// Strict: exactly one ':', known names, no whitespace. Returns false otherwise.
bool parseTrigger(std::string_view text, Trigger *out);
std::string formatTrigger(Trigger trigger);
// A trigger a macro can be on: a press (:down, :tap or :hold), never :up
// (docs/design/macros.md). False for an unparseable one too.
bool isMacroTrigger(std::string_view text);

}  // namespace hubbo

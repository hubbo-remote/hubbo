#include "hubbo/domain/trigger.h"

namespace hubbo {

bool isMacroTrigger(std::string_view text) {
    Trigger trigger;
    return parseTrigger(text, &trigger) && trigger.kind != remote::EventKind::Up;
}

bool parseTrigger(std::string_view text, Trigger *out) {
    const size_t colon = text.find(':');
    if (colon == std::string_view::npos || text.find(':', colon + 1) != std::string_view::npos) {
        return false;
    }
    // Longest names are well under this; anything longer is invalid anyway.
    char button[32];
    char event[16];
    const std::string_view b = text.substr(0, colon);
    const std::string_view e = text.substr(colon + 1);
    if (b.empty() || e.empty() || b.size() >= sizeof(button) || e.size() >= sizeof(event)) {
        return false;
    }
    b.copy(button, b.size());
    button[b.size()] = '\0';
    e.copy(event, e.size());
    event[e.size()] = '\0';

    Trigger t;
    if (!remote::buttonFromName(button, &t.button) || !remote::eventKindFromName(event, &t.kind)) {
        return false;
    }
    *out = t;
    return true;
}

std::string formatTrigger(Trigger trigger) {
    std::string s = remote::buttonName(trigger.button);
    s += ':';
    s += remote::eventKindName(trigger.kind);
    return s;
}

}  // namespace hubbo

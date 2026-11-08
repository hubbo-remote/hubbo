#include "hubbo/engine/compiled.h"

namespace hubbo {

int CompiledConfig::findActivity(std::string_view id) const {
    for (size_t i = 0; i < activities.size(); i++) {
        if (activities[i].id == id) {
            return static_cast<int>(i);
        }
    }
    return -1;
}

ActionSpan CompiledConfig::lookup(int activity, remote::Button button, remote::EventKind kind,
                                  bool *fromGlobal) const {
    if (static_cast<size_t>(button) >= remote::kButtonCount ||
        static_cast<size_t>(kind) >= remote::kEventKindCount) {
        return {};
    }
    if (activity >= 0 && static_cast<size_t>(activity) < activities.size()) {
        const ActionSpan &s = activities[static_cast<size_t>(activity)].table.at(button, kind);
        if (s.count != 0) {
            if (fromGlobal != nullptr) *fromGlobal = false;
            return s;
        }
    }
    if (fromGlobal != nullptr) *fromGlobal = true;
    return globals.at(button, kind);
}

}  // namespace hubbo

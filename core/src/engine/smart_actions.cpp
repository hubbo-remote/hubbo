#include "hubbo/engine/smart_actions.h"

#include <optional>
#include <string>
#include <string_view>
#include <tuple>

#include "hubbo/ha/capabilities.h"

namespace hubbo {

SmartSlotActions smartSlotActions(const DeviceSpec &device, const CommandCatalog &catalog) {
    SmartSlotActions slot;
    if (device.provider.type != kHomeAssistantType) return slot;
    const std::string_view domain = ha::entityDomainName(device.provider.entityId);
    // The action for `service` (with an int `step`), appended to `list` if offered.
    auto add = [&](std::vector<ActionSpec> &list, const char *service, std::optional<int> step = std::nullopt) {
        const std::string id = std::string(domain) + "." + service;
        if (catalog.find(id) < 0) return false;
        ActionSpec &action = list.emplace_back();
        action.integrationId = device.integrationId;
        action.deviceId = device.id;
        action.commandId = id;
        if (step) action.params.push_back({"step", ParamLiteral::ofInt(*step)});
        return true;
    };
    const std::string &kind = device.kind;
    const bool cover = kind == "cover";
    if (kind == "light" || kind == "fan" || kind == "switch" || cover) {
        add(slot.button.tap, cover ? "open_cover" : "turn_on");
        add(slot.button.hold, cover ? "close_cover" : "turn_off");
    }
    // +/-: the same press each way, `sign` flipping the step.
    for (auto [press, sign, move] :
         {std::tuple{&slot.plus, 1, "open_cover"}, std::tuple{&slot.minus, -1, "close_cover"}}) {
        if (kind == "light") {
            add(press->tap, "brightness_step", 26 * sign);  // 10% of 255
        } else if (kind == "fan") {
            add(press->tap, sign > 0 ? "increase_speed" : "decrease_speed");
        } else if (cover && add(press->tap, "position_step", 10 * sign)) {
            add(press->hold, move);
            add(press->release, "stop_cover");
        } else if (cover) {
            add(press->tap, move);  // no position: open and close
        }
    }
    return slot;
}

}  // namespace hubbo

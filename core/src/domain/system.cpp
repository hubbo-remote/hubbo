#include "hubbo/domain/system.h"

#include <iterator>

#include "hubbo/domain/limits.h"

namespace hubbo {

namespace {

constexpr ParamSpec kWaitParams[] = {
    ParamSpec::integer("duration_ms", 0, static_cast<int32_t>(limits::kMaxWaitMs)),
};
constexpr ParamSpec kActivateParams[] = {
    ParamSpec::activityRef("activity_id"),
};

constexpr CommandSpec kCommands[] = {
    {"wait", kWaitParams, 1},
    {"activate", kActivateParams, 1},
    {"off", nullptr, 0},
};

}  // namespace

CommandCatalog systemCatalog() {
    return {kCommands, static_cast<uint16_t>(std::size(kCommands))};
}

}  // namespace hubbo

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "hubbo/domain/error.h"
#include "hubbo/domain/limits.h"

// Typed, bounded action parameters.
//
// Every command an integration offers is described by a CommandSpec listing
// its parameters (type, required, range or enum options). Config arrives as
// loosely typed ParamInputs (from JSON); resolveParams() checks them against
// the spec and produces compact ParamValues in spec order, which is all the
// hot path ever sees. Strings never reach an adapter: an enum is resolved to
// its option index at compile time.
namespace hubbo {

// ActivityRef: a string naming an activity in the same config; compiled to
// that activity's index. Only system.activate uses it.
enum class ParamType : uint8_t { Bool, Int, Number, Enum, ActivityRef };

// "bool", "int", "number", "enum", "activity_ref"
const char *paramTypeName(ParamType type);

// A validated value. Bool/Int/Enum use `integer` (Enum: option index);
// Number uses `number`.
struct ParamValue {
    ParamType type = ParamType::Int;
    int32_t integer = 0;
    float number = 0.0f;

    static constexpr ParamValue ofBool(bool v) { return {ParamType::Bool, v ? 1 : 0, 0.0f}; }
    static constexpr ParamValue ofInt(int32_t v) { return {ParamType::Int, v, 0.0f}; }
    static constexpr ParamValue ofNumber(float v) { return {ParamType::Number, 0, v}; }
    static constexpr ParamValue ofOption(uint16_t index) {
        return {ParamType::Enum, static_cast<int32_t>(index), 0.0f};
    }

    bool asBool() const { return integer != 0; }
    uint16_t asOption() const { return static_cast<uint16_t>(integer); }
};

struct ParamSpec {
    const char *name;
    ParamType type;
    bool required;
    int32_t minInt;
    int32_t maxInt;
    float minNumber;
    float maxNumber;
    const char *const *options;  // Enum only; static storage
    uint16_t optionCount;
    ParamValue defaultValue;     // used when optional and omitted
    // Display metadata for the API/UI (GET /devices/{id}/capabilities).
    const char *label = nullptr;  // e.g. "Volume"
    const char *unit = nullptr;   // e.g. "%", "ms"
    float step = 0.0f;            // UI increment; 0 = none

    constexpr ParamSpec described(const char *newLabel, const char *newUnit = nullptr,
                                  float newStep = 0.0f) const {
        ParamSpec s = *this;
        s.label = newLabel;
        s.unit = newUnit;
        s.step = newStep;
        return s;
    }

    static constexpr ParamSpec integer(const char *name, int32_t lo, int32_t hi, bool required = true,
                                       int32_t fallback = 0) {
        return {name, ParamType::Int, required, lo, hi, 0.0f, 0.0f, nullptr, 0,
                ParamValue::ofInt(fallback)};
    }
    static constexpr ParamSpec number(const char *name, float lo, float hi, bool required = true,
                                      float fallback = 0.0f) {
        return {name, ParamType::Number, required, 0, 0, lo, hi, nullptr, 0,
                ParamValue::ofNumber(fallback)};
    }
    static constexpr ParamSpec boolean(const char *name, bool required = true, bool fallback = false) {
        return {name, ParamType::Bool, required, 0, 0, 0.0f, 0.0f, nullptr, 0,
                ParamValue::ofBool(fallback)};
    }
    static constexpr ParamSpec activityRef(const char *name) {
        return {name, ParamType::ActivityRef, true, 0, 0, 0.0f, 0.0f, nullptr, 0, ParamValue{}};
    }
    static constexpr ParamSpec enumeration(const char *name, const char *const *options,
                                           uint16_t count, bool required = true,
                                           uint16_t fallback = 0) {
        return {name, ParamType::Enum, required, 0, 0, 0.0f, 0.0f, options, count,
                ParamValue::ofOption(fallback)};
    }
};

// What executing a command leaves behind.
//   Momentary: nothing (a tap, a volume step, turning something on).
//   Hold: a state that must be released (key_down holds a key until key_up).
//   Wait: nothing, but it parks its sequence until something happens
//     (ble wait_connected). Startup and shutdown only: a binding or direct
//     execution gets SequenceOnly.
// Direct execution accepts only Momentary; a binding may use a Hold only in a
// "<button>:down" binding whose matching ":up" binding releases it.
enum class CommandEffect : uint8_t { Momentary, Hold, Wait };
const char *commandEffectName(CommandEffect effect);  // "momentary", "hold", "wait"

// One command an integration offers for a device.
struct CommandSpec {
    const char *id;  // e.g. "key_down", "media_player.volume_set"
    const ParamSpec *params;
    uint8_t paramCount;
    CommandEffect effect = CommandEffect::Momentary;
    // Hold only: the command id that releases it, taking the same params
    // (key_down -> "key_up").
    const char *releasedBy = nullptr;
    // For the UI's picker: "remote", "keys" or "custom" (ble_hid); nullptr: none.
    const char *group = nullptr;
    // Adapter-owned data for this command (e.g. a device's key table); the
    // adapter keeps it alive with the catalog. Never read by the core.
    const void *data = nullptr;
    // The longest this command may stay Pending (a timed press, a wait for a
    // connection); 0: limits::kDefaultActionTimeoutMs. The adapter finishes
    // it sooner; this is the executor's backstop.
    uint32_t timeoutMs = 0;
};

// The commands offered for one device. Storage belongs to the adapter and
// must stay valid (and stable in order) while compiled configs reference it.
struct CommandCatalog {
    const CommandSpec *items = nullptr;
    uint16_t count = 0;

    // Index of the command with this id, or -1.
    int find(std::string_view id) const;
};

// A loosely typed value as parsed from config/API JSON.
struct ParamLiteral {
    enum class Kind : uint8_t { Null, Bool, Int, Number, String };

    Kind kind = Kind::Null;
    bool boolean = false;
    int64_t integer = 0;
    double number = 0.0;
    std::string string;

    static ParamLiteral null() { return {}; }
    static ParamLiteral ofBool(bool v);
    static ParamLiteral ofInt(int64_t v);
    static ParamLiteral ofNumber(double v);
    static ParamLiteral ofString(std::string v);
};

// Resolves references that depend on the rest of the config.
class RefResolver {
public:
    virtual ~RefResolver() = default;
    virtual int activityIndex(std::string_view id) const = 0;  // -1 if none
};

struct ParamInput {
    std::string name;
    ParamLiteral value;
};

// Checks one literal against its spec. Returns Ok and fills `out`, or
// InvalidParamType / ParamOutOfRange / InvalidEnumValue.
// Int accepts integral numbers (3.0); Number accepts integers; NaN and
// infinities are out of range. ActivityRef needs `refs` and fails with
// UnknownActivity when the id does not resolve.
ErrorCode coerceParam(const ParamSpec &spec, const ParamLiteral &literal, ParamValue *out,
                      const RefResolver *refs = nullptr);

// Resolves a whole parameter list against `command` into `out` (spec order,
// command.paramCount entries). On failure returns the error and sets
// `*badParam` to the offending parameter name. `present` (optional) gets bit
// i set when param i was supplied rather than defaulted, so adapters can
// leave omitted optional fields out instead of using placeholder defaults.
ErrorCode resolveParams(const CommandSpec &command, const std::vector<ParamInput> &inputs,
                        ParamValue out[limits::kMaxParams], std::string *badParam,
                        const RefResolver *refs = nullptr, uint8_t *present = nullptr);

}  // namespace hubbo

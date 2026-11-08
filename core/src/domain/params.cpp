#include "hubbo/domain/params.h"

#include <cmath>

namespace hubbo {

const char *paramTypeName(ParamType type) {
    switch (type) {
        case ParamType::Bool: return "bool";
        case ParamType::Int: return "int";
        case ParamType::Number: return "number";
        case ParamType::Enum: return "enum";
        case ParamType::ActivityRef: return "activity_ref";
    }
    return "int";
}

int CommandCatalog::find(std::string_view id) const {
    for (uint16_t i = 0; i < count; i++) {
        if (id == items[i].id) {
            return i;
        }
    }
    return -1;
}

ParamLiteral ParamLiteral::ofBool(bool v) {
    ParamLiteral p;
    p.kind = Kind::Bool;
    p.boolean = v;
    return p;
}

ParamLiteral ParamLiteral::ofInt(int64_t v) {
    ParamLiteral p;
    p.kind = Kind::Int;
    p.integer = v;
    return p;
}

ParamLiteral ParamLiteral::ofNumber(double v) {
    ParamLiteral p;
    p.kind = Kind::Number;
    p.number = v;
    return p;
}

ParamLiteral ParamLiteral::ofString(std::string v) {
    ParamLiteral p;
    p.kind = Kind::String;
    p.string = std::move(v);
    return p;
}

ErrorCode coerceParam(const ParamSpec &spec, const ParamLiteral &literal, ParamValue *out,
                      const RefResolver *refs) {
    using Kind = ParamLiteral::Kind;
    switch (spec.type) {
        case ParamType::Bool:
            if (literal.kind != Kind::Bool) {
                return ErrorCode::InvalidParamType;
            }
            *out = ParamValue::ofBool(literal.boolean);
            return ErrorCode::Ok;

        case ParamType::Int: {
            int64_t value;
            if (literal.kind == Kind::Int) {
                value = literal.integer;
            } else if (literal.kind == Kind::Number && std::isfinite(literal.number) &&
                       std::trunc(literal.number) == literal.number) {
                if (literal.number < spec.minInt || literal.number > spec.maxInt) {
                    return ErrorCode::ParamOutOfRange;
                }
                value = static_cast<int64_t>(literal.number);
            } else {
                return ErrorCode::InvalidParamType;
            }
            if (value < spec.minInt || value > spec.maxInt) {
                return ErrorCode::ParamOutOfRange;
            }
            *out = ParamValue::ofInt(static_cast<int32_t>(value));
            return ErrorCode::Ok;
        }

        case ParamType::Number: {
            double value;
            if (literal.kind == Kind::Number) {
                value = literal.number;
            } else if (literal.kind == Kind::Int) {
                value = static_cast<double>(literal.integer);
            } else {
                return ErrorCode::InvalidParamType;
            }
            if (!std::isfinite(value) || value < spec.minNumber || value > spec.maxNumber) {
                return ErrorCode::ParamOutOfRange;
            }
            *out = ParamValue::ofNumber(static_cast<float>(value));
            return ErrorCode::Ok;
        }

        case ParamType::Enum:
            if (literal.kind != Kind::String) {
                return ErrorCode::InvalidParamType;
            }
            for (uint16_t i = 0; i < spec.optionCount; i++) {
                if (literal.string == spec.options[i]) {
                    *out = ParamValue::ofOption(i);
                    return ErrorCode::Ok;
                }
            }
            return ErrorCode::InvalidEnumValue;

        case ParamType::ActivityRef: {
            if (literal.kind != Kind::String) {
                return ErrorCode::InvalidParamType;
            }
            if (refs == nullptr) {
                return ErrorCode::Internal;
            }
            const int index = refs->activityIndex(literal.string);
            if (index < 0) {
                return ErrorCode::UnknownActivity;
            }
            ParamValue value;
            value.type = ParamType::ActivityRef;
            value.integer = index;
            *out = value;
            return ErrorCode::Ok;
        }
    }
    return ErrorCode::Internal;
}

ErrorCode resolveParams(const CommandSpec &command, const std::vector<ParamInput> &inputs,
                        ParamValue out[limits::kMaxParams], std::string *badParam,
                        const RefResolver *refs) {
    auto fail = [&](ErrorCode code, const std::string &name) {
        if (badParam != nullptr) {
            *badParam = name;
        }
        return code;
    };

    if (command.paramCount > limits::kMaxParams) {
        return fail(ErrorCode::Internal, "");  // adapter catalog bug
    }
    if (inputs.size() > limits::kMaxParams) {
        return fail(ErrorCode::TooManyParams, "");
    }

    bool seen[limits::kMaxParams] = {};
    for (const auto &input : inputs) {
        int slot = -1;
        for (uint8_t i = 0; i < command.paramCount; i++) {
            if (input.name == command.params[i].name) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            return fail(ErrorCode::UnknownParam, input.name);
        }
        if (seen[slot]) {
            return fail(ErrorCode::DuplicateParam, input.name);
        }
        seen[slot] = true;
        const ErrorCode code = coerceParam(command.params[slot], input.value, &out[slot], refs);
        if (code != ErrorCode::Ok) {
            return fail(code, input.name);
        }
    }

    for (uint8_t i = 0; i < command.paramCount; i++) {
        if (seen[i]) {
            continue;
        }
        if (command.params[i].required) {
            return fail(ErrorCode::MissingParam, command.params[i].name);
        }
        out[i] = command.params[i].defaultValue;
    }
    return ErrorCode::Ok;
}

}  // namespace hubbo

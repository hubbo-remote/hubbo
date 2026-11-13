#pragma once

// Strict JSON readers shared by config/json.cpp and config/profile.cpp.
// Internal to the config module.

#include <charconv>
#include <cstring>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "hubbo/config/json.h"

namespace hubbo::config::detail {

template <size_t N>
inline bool oneOf(const std::string &value, const char *const (&options)[N]) {
    for (const char *o : options) {
        if (value == o) return true;
    }
    return false;
}

inline std::string child(std::string_view path, std::string_view key) {
    return std::string(path) + "/" + jsonPointerToken(std::string(key));
}

// "<path>/<index>". std::to_chars, not std::to_string: that inlines a 240 B
// frame into its callers, and edits run on the HTTP task's stack.
inline std::string child(std::string_view path, size_t index) {
    char digits[20];
    const char *end = std::to_chars(digits, digits + sizeof(digits), index).ptr;
    std::string out;
    out.reserve(path.size() + 1 + static_cast<size_t>(end - digits));
    out.append(path).append(1, '/').append(digits, static_cast<size_t>(end - digits));
    return out;
}

inline void addIssue(ValidationReport *report, ErrorCode code, const std::string &path) {
    if (report != nullptr) report->add(code, path);
}

// Strict reader for one JSON object: every key must be asked for, or it is
// reported as unknown in finish(). Error paths are only built on failure.
class Fields {
public:
    Fields(JsonVariantConst json, ValidationReport *report, std::string path)
        : report_(report), path_(std::move(path)) {
        if (!json.is<JsonObjectConst>()) {
            fail(ErrorCode::WrongType, path_);
            return;
        }
        object_ = json.as<JsonObjectConst>();
    }

    bool isObject() const { return !object_.isNull(); }
    std::string at(const char *key) const { return child(path_, key); }

    // The raw value; Required reported if missing and required.
    JsonVariantConst get(const char *key, bool required) {
        if (knownCount_ < kMaxFields) known_[knownCount_++] = key;
        if (object_.isNull()) return {};
        JsonVariantConst v = object_[key];
        if (v.isUnbound() && required) failAt(ErrorCode::Required, key);
        return v;
    }

    bool string(const char *key, std::string *out, bool required, size_t maxLength = kMaxStringLength) {
        JsonVariantConst v = get(key, required);
        return !v.isUnbound() && readString(v, key, out, maxLength);
    }

    // A string that may be JSON null (-> nullopt). True if present and valid.
    bool nullableString(const char *key, std::optional<std::string> *out, bool required, size_t maxLength) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (v.isNull()) {
            out->reset();
            return true;
        }
        std::string s;
        if (!readString(v, key, &s, maxLength)) return false;
        *out = std::move(s);
        return true;
    }

    bool boolean(const char *key, bool *out, bool required) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        if (!v.is<bool>()) return failAt(ErrorCode::WrongType, key);
        *out = v.as<bool>();
        return true;
    }

    // Revision: integer 1..UINT32_MAX (0..UINT32_MAX as found in backups,
    // where restore re-stamps it anyway).
    bool revision(const char *key, uint32_t *out, bool required, bool allowZero = false) {
        JsonVariantConst v = get(key, required);
        if (v.isUnbound()) return false;
        const int64_t lowest = allowZero ? 0 : 1;
        if (!v.is<int64_t>() || v.as<int64_t>() < lowest ||
            v.as<int64_t>() > std::numeric_limits<uint32_t>::max()) {
            return failAt(ErrorCode::WrongType, key);
        }
        *out = static_cast<uint32_t>(v.as<int64_t>());
        return true;
    }

    void schemaVersion(bool required, int64_t expected = kSchemaVersion) {
        JsonVariantConst v = get("schema_version", required);
        if (v.isUnbound()) return;
        if (!v.is<int64_t>()) {
            failAt(ErrorCode::WrongType, "schema_version");
        } else if (v.as<int64_t>() != expected) {
            failAt(ErrorCode::UnsupportedSchema, "schema_version");
        }
    }

    // A nested value handed to `decode(json, path) -> bool`; a failure there
    // (which reports its own issues) fails this object too.
    template <typename Decode>
    void nested(const char *key, bool required, Decode decode) {
        JsonVariantConst v = get(key, required);
        if (!v.isUnbound() && !decode(v, at(key))) ok_ = false;
    }

    // Reports unknown keys; returns whether everything was fine.
    bool finish() {
        if (object_.isNull()) return ok_;
        for (JsonPairConst kv : object_) {
            bool known = false;
            for (size_t i = 0; i < knownCount_ && !known; i++) known = std::strcmp(known_[i], kv.key().c_str()) == 0;
            if (!known) fail(ErrorCode::UnknownField, child(path_, kv.key().c_str()));
        }
        return ok_;
    }

    void fail(ErrorCode code, const std::string &path) {
        ok_ = false;
        addIssue(report_, code, path);
    }

    bool failAt(ErrorCode code, const char *key) {
        fail(code, at(key));
        return false;
    }

private:
    static constexpr size_t kMaxFields = 24;  // more than any schema object has

    bool readString(JsonVariantConst v, const char *key, std::string *out, size_t maxLength) {
        if (!v.is<const char *>()) return failAt(ErrorCode::WrongType, key);
        const char *s = v.as<const char *>();
        const size_t n = std::strlen(s);
        if (n > maxLength) return failAt(ErrorCode::TooLong, key);
        out->assign(s, n);
        return true;
    }

    ValidationReport *report_;
    std::string path_;
    JsonObjectConst object_;
    const char *known_[kMaxFields] = {};
    size_t knownCount_ = 0;
    bool ok_ = true;
};

// A JSON array of at most `maxItems`, each decoded by
// `decode(json, T*, path) -> bool`. Replaces *out.
template <typename T, typename Decode>
bool decodeList(JsonVariantConst json, size_t maxItems, std::vector<T> *out, ValidationReport *report,
                const std::string &path, Decode decode) {
    if (!json.is<JsonArrayConst>()) {
        addIssue(report, ErrorCode::WrongType, path);
        return false;
    }
    JsonArrayConst array = json.as<JsonArrayConst>();
    if (array.size() > maxItems) {
        addIssue(report, ErrorCode::TooLong, path);
        return false;
    }
    bool ok = true;
    out->clear();
    out->reserve(array.size());
    size_t i = 0;
    for (JsonVariantConst item : array) {
        // Decoded in place: a T on this frame (a DeviceSpec is over 600 B)
        // would stack under each decode on the HTTP task.
        ok = decode(item, &out->emplace_back(), child(path, i++)) && ok;
    }
    return ok;
}

// A JSON object of at most `maxEntries`, or nullopt (issue reported).
inline std::optional<JsonObjectConst> objectWithin(JsonVariantConst json, size_t maxEntries, ValidationReport *report,
                                            const std::string &path) {
    if (!json.is<JsonObjectConst>()) {
        addIssue(report, ErrorCode::WrongType, path);
        return std::nullopt;
    }
    JsonObjectConst object = json.as<JsonObjectConst>();
    if (object.size() > maxEntries) {
        addIssue(report, ErrorCode::TooLong, path);
        return std::nullopt;
    }
    return object;
}

}  // namespace hubbo::config::detail

#pragma once

#include <cstddef>
#include <string_view>

// A small JSON structure scanner for messages too big to parse in one go
// (HA discovery results): find a member's value and walk an array or an
// object element by element without building a document. It only finds value
// boundaries (strings, escapes, nesting); each element is then parsed on its
// own (ArduinoJson). Malformed input is reported, never trusted.
namespace hubbo::integrations::json {

constexpr size_t npos = std::string_view::npos;

struct Span {
    size_t begin = npos;  // offset of the value's first byte
    size_t end = npos;    // one past its last byte
    bool found() const { return begin != npos; }
    std::string_view in(std::string_view text) const {
        return found() ? text.substr(begin, end - begin) : std::string_view();
    }
};

size_t skipSpace(std::string_view text, size_t pos);
// End of the value starting at `pos` (after whitespace), or npos if malformed.
size_t skipValue(std::string_view text, size_t pos);
// The value of top-level member `key` of the object `text` (keys compared
// raw: fine for ASCII keys without escapes). Not found / malformed: !found().
Span member(std::string_view text, std::string_view key);
// The same, for the object at `within` of `text`; offsets are into `text`.
Span member(std::string_view text, Span within, std::string_view key);
// Where member `key`'s value begins in the object starting at `object` (an
// offset into `text`), or npos. Skips only the members before it, never the
// value itself: the way into a big value without scanning it twice.
size_t valueStart(std::string_view text, size_t object, std::string_view key);

// Walks the elements of the array, or the members of the object, starting
// at `begin` of `text`, up to its closing bracket. One pass: the value's end
// isn't needed up front.
class Cursor {
public:
    Cursor() = default;
    Cursor(std::string_view text, size_t begin);
    // Next element (its text; for an object the member's value, and its key
    // without quotes in `key` if given), false at the end or on malformed
    // input (ok()).
    bool next(std::string_view text, std::string_view *element, std::string_view *key = nullptr);
    bool ok() const { return ok_; }

private:
    size_t pos_ = 0;
    bool object_ = false;
    bool ok_ = false;
    bool done_ = true;
    bool first_ = true;
};

}  // namespace hubbo::integrations::json

#pragma once

#include <cstddef>
#include <string_view>

// A small JSON structure scanner for messages too big to parse in one go
// (HA discovery results): find a top-level member's value and walk an array
// element by element without building a document. It only finds value
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

// Walks the elements of the array at `span` of `text`.
class ArrayCursor {
public:
    ArrayCursor() = default;
    ArrayCursor(std::string_view text, Span array);
    // Next element (its text), false at the end or on malformed input (ok()).
    bool next(std::string_view text, std::string_view *element);
    bool ok() const { return ok_; }

private:
    size_t pos_ = 0;
    size_t end_ = 0;
    bool ok_ = false;
    bool done_ = true;
    bool first_ = true;
};

}  // namespace hubbo::integrations::json

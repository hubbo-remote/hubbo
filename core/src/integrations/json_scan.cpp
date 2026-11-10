#include "hubbo/integrations/json_scan.h"

namespace hubbo::integrations::json {

namespace {

bool isSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

// End of the string whose opening quote is at `pos`, or npos.
size_t skipString(std::string_view text, size_t pos) {
    for (size_t i = pos + 1; i < text.size(); i++) {
        if (text[i] == '\\') {
            i++;
        } else if (text[i] == '"') {
            return i + 1;
        }
    }
    return npos;
}

}  // namespace

size_t skipSpace(std::string_view text, size_t pos) {
    while (pos < text.size() && isSpace(text[pos])) pos++;
    return pos;
}

size_t skipValue(std::string_view text, size_t pos) {
    pos = skipSpace(text, pos);
    if (pos >= text.size()) return npos;
    const char c = text[pos];
    if (c == '"') return skipString(text, pos);
    if (c == '{' || c == '[') {
        // Nesting depth only; strings are skipped whole so brackets inside
        // them don't count.
        size_t depth = 0;
        for (size_t i = pos; i < text.size(); i++) {
            const char d = text[i];
            if (d == '"') {
                const size_t after = skipString(text, i);
                if (after == npos) return npos;
                i = after - 1;
            } else if (d == '{' || d == '[') {
                depth++;
            } else if (d == '}' || d == ']') {
                if (depth == 0) return npos;
                if (--depth == 0) return i + 1;
            }
        }
        return npos;
    }
    // Number, true, false, null: up to the next delimiter.
    size_t i = pos;
    while (i < text.size() && !isSpace(text[i]) && text[i] != ',' && text[i] != '}' && text[i] != ']') i++;
    return i == pos ? npos : i;
}

size_t valueStart(std::string_view text, size_t object, std::string_view key) {
    size_t pos = skipSpace(text, object);
    if (pos >= text.size() || text[pos] != '{') return npos;
    pos++;
    for (;;) {
        pos = skipSpace(text, pos);
        if (pos >= text.size() || text[pos] != '"') return npos;  // also '}': not found
        const size_t keyEnd = skipString(text, pos);
        if (keyEnd == npos) return npos;
        const std::string_view name = text.substr(pos + 1, keyEnd - pos - 2);
        pos = skipSpace(text, keyEnd);
        if (pos >= text.size() || text[pos] != ':') return npos;
        const size_t valueBegin = skipSpace(text, pos + 1);
        if (name == key) return valueBegin < text.size() ? valueBegin : npos;
        const size_t valueEnd = skipValue(text, valueBegin);
        if (valueEnd == npos) return npos;
        pos = skipSpace(text, valueEnd);
        if (pos < text.size() && text[pos] == ',') pos++;
    }
}

Span member(std::string_view text, std::string_view key) { return member(text, Span{0, text.size()}, key); }

Span member(std::string_view text, Span within, std::string_view key) {
    const size_t begin = within.found() ? valueStart(text, within.begin, key) : npos;
    const size_t end = begin == npos ? npos : skipValue(text, begin);
    return end == npos ? Span{} : Span{begin, end};
}

Cursor::Cursor(std::string_view text, size_t begin) {
    if (begin >= text.size() || (text[begin] != '[' && text[begin] != '{')) return;
    object_ = text[begin] == '{';
    pos_ = begin + 1;
    ok_ = true;
    done_ = false;
}

bool Cursor::next(std::string_view text, std::string_view *element, std::string_view *key) {
    if (done_) return false;
    const auto fail = [this] {
        ok_ = false;
        done_ = true;
        return false;
    };
    size_t pos = skipSpace(text, pos_);
    if (pos >= text.size()) return fail();  // unterminated
    if (text[pos] == (object_ ? '}' : ']')) {
        done_ = true;
        return false;
    }
    if (!first_) {
        if (text[pos] != ',') return fail();
        pos = skipSpace(text, pos + 1);
    }
    if (object_) {
        // "key": value
        const size_t keyEnd = pos < text.size() && text[pos] == '"' ? skipString(text, pos) : npos;
        const size_t colon = keyEnd == npos ? npos : skipSpace(text, keyEnd);
        if (colon == npos || colon >= text.size() || text[colon] != ':') return fail();
        if (key != nullptr) *key = text.substr(pos + 1, keyEnd - pos - 2);
        pos = skipSpace(text, colon + 1);
    }
    const size_t valueEnd = skipValue(text, pos);
    if (valueEnd == npos) return fail();
    first_ = false;
    pos_ = valueEnd;
    *element = text.substr(pos, valueEnd - pos);
    return true;
}

}  // namespace hubbo::integrations::json

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

Span member(std::string_view text, std::string_view key) {
    size_t pos = skipSpace(text, 0);
    if (pos >= text.size() || text[pos] != '{') return {};
    pos++;
    for (;;) {
        pos = skipSpace(text, pos);
        if (pos >= text.size()) return {};
        if (text[pos] == '}') return {};
        if (text[pos] != '"') return {};
        const size_t keyEnd = skipString(text, pos);
        if (keyEnd == npos) return {};
        const std::string_view name = text.substr(pos + 1, keyEnd - pos - 2);
        pos = skipSpace(text, keyEnd);
        if (pos >= text.size() || text[pos] != ':') return {};
        const size_t valueBegin = skipSpace(text, pos + 1);
        const size_t valueEnd = skipValue(text, valueBegin);
        if (valueEnd == npos) return {};
        if (name == key) return {valueBegin, valueEnd};
        pos = skipSpace(text, valueEnd);
        if (pos < text.size() && text[pos] == ',') pos++;
    }
}

ArrayCursor::ArrayCursor(std::string_view text, Span array) {
    if (!array.found() || array.end > text.size() || array.end - array.begin < 2 || text[array.begin] != '[') {
        ok_ = false;
        done_ = true;
        return;
    }
    pos_ = array.begin + 1;
    end_ = array.end - 1;  // the closing ']'
    ok_ = true;
    done_ = false;
}

bool ArrayCursor::next(std::string_view text, std::string_view *element) {
    if (done_) return false;
    size_t pos = skipSpace(text, pos_);
    if (!first_) {
        if (pos < end_ && text[pos] == ',') {
            pos = skipSpace(text, pos + 1);
        } else if (pos != end_) {
            ok_ = false;
            done_ = true;
            return false;
        }
    }
    if (pos >= end_) {
        done_ = true;
        return false;
    }
    const size_t valueEnd = skipValue(text, pos);
    if (valueEnd == npos || valueEnd > end_) {
        ok_ = false;
        done_ = true;
        return false;
    }
    first_ = false;
    pos_ = valueEnd;
    *element = text.substr(pos, valueEnd - pos);
    return true;
}

}  // namespace hubbo::integrations::json

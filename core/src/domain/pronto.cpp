#include "hubbo/domain/pronto.h"

#include <algorithm>
#include <cmath>

namespace hubbo::pronto {

namespace {

int hexDigit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

}  // namespace

bool parse(std::string_view text, std::vector<uint16_t> *words) {
    // "XXXX" then " XXXX"...: 5 characters a word, less the first space.
    if ((text.size() + 1) % 5 != 0) return false;
    const size_t count = (text.size() + 1) / 5;
    if (count < kHeaderWords + 2) return false;
    const size_t start = words->size();  // no reserve: callers append code after code
    for (size_t i = 0; i < count; i++) {
        const size_t at = i * 5;
        if (i > 0 && text[at - 1] != ' ') break;
        uint32_t w = 0;
        bool ok = true;
        for (size_t d = 0; d < 4 && ok; d++) {
            const int v = hexDigit(text[at + d]);
            ok = v >= 0;
            w = w * 16 + static_cast<uint32_t>(v);
        }
        if (!ok) break;
        words->push_back(static_cast<uint16_t>(w));
    }
    const uint16_t *code = words->data() + start;
    const bool valid = words->size() - start == count && code[0] == 0 && frequency(code) != 0 &&
                       oncePairs(code) <= kMaxPairs && repeatPairs(code) <= kMaxPairs &&
                       oncePairs(code) + repeatPairs(code) > 0 && wordCount(code) == count &&
                       std::find(code + kHeaderWords, code + count, 0) == code + count;  // no zero durations
    if (!valid) words->resize(start);
    return valid;
}

std::string format(const uint16_t *words, size_t count) {
    static constexpr char kHex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(count * 5);
    for (size_t i = 0; i < count; i++) {
        if (i > 0) out.push_back(' ');
        for (int shift = 12; shift >= 0; shift -= 4) out.push_back(kHex[(words[i] >> shift) & 0xF]);
    }
    return out;
}

uint32_t carrierHz(uint16_t frequency) {
    return static_cast<uint32_t>(std::lround(1000000.0 / (frequency * kPeriodUnitUs)));
}

uint32_t durationUs(uint16_t count, uint16_t frequency) {
    return static_cast<uint32_t>(std::lround(count * (frequency * kPeriodUnitUs)));
}

}  // namespace hubbo::pronto

#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

// Raw Pronto IR codes (the profile spec's IR section): hex words "0000", the
// frequency word F, the once section's pair count, the repeat section's,
// then the pairs (once first). Each duration counts carrier periods of
// F x 0.241246 µs; the carrier is 1,000,000 / (F x 0.241246) Hz.
namespace hubbo::pronto {

constexpr size_t kHeaderWords = 4;
// Pairs in one section: room over the catalog (its largest seen is about
// 220), and what the transmitter's buffer holds.
constexpr size_t kMaxPairs = 512;
constexpr double kPeriodUnitUs = 0.241246;

// Appends the code's words (header included) to `words` if `text` is a raw
// Pronto code as the spec writes it: uppercase hex words of four digits,
// single spaces, "0000" first, a nonzero frequency, a length that matches
// the header, at least one pair, at most kMaxPairs per section and no zero
// durations. False (nothing appended) otherwise.
bool parse(std::string_view text, std::vector<uint16_t> *words);

// The spec's text for `count` words, as parse() reads it.
std::string format(const uint16_t *words, size_t count);

// A parsed code's header fields (words as parse() appended them).
inline uint16_t frequency(const uint16_t *code) { return code[1]; }
inline size_t oncePairs(const uint16_t *code) { return code[2]; }
inline size_t repeatPairs(const uint16_t *code) { return code[3]; }
inline size_t wordCount(const uint16_t *code) { return kHeaderWords + 2 * (oncePairs(code) + repeatPairs(code)); }

uint32_t carrierHz(uint16_t frequency);
uint32_t durationUs(uint16_t count, uint16_t frequency);

}  // namespace hubbo::pronto

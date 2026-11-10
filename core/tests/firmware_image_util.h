#pragma once

// Fake firmware images in the real app .bin layout, for validateFirmwareImage()
// and the firmware routes.

#include <string>

#include "hubbo/api/firmware_update_port.h"

namespace hubbo::test {

struct ImageSpec {
    uint8_t magic = 0xE9;
    uint16_t chip = api::kEsp32S3ChipId;
    uint16_t minRev = 0;
    uint32_t descMagic = 0xABCD5432;
    std::string marker = "HUBBO-IMAGE/1 board=simulator layout=1 version=v1.2.3-4-gabc";
    bool hashAppended = true;
    bool badHash = false;
    size_t padTo = 512;
};

inline void put16(std::string &b, size_t at, uint16_t v) {
    b[at] = static_cast<char>(v & 0xFF);
    b[at + 1] = static_cast<char>(v >> 8);
}

// The layout a real app .bin has: image header, segment header, app desc,
// then (here) the marker string, padding and the appended SHA-256.
inline std::string image(const ImageSpec &spec = {}) {
    std::string b(32, '\0');
    b[0] = static_cast<char>(spec.magic);
    put16(b, 12, spec.chip);
    put16(b, 15, spec.minRev);
    b[23] = spec.hashAppended ? 1 : 0;
    std::string desc(256, '\0');
    put16(desc, 0, static_cast<uint16_t>(spec.descMagic & 0xFFFF));
    put16(desc, 2, static_cast<uint16_t>(spec.descMagic >> 16));
    b += desc;
    b += "HUBBO-IMAGE/1 ";  // a lone prefix literal (as the validator's own) comes first
    b += '\0';
    b += spec.marker;
    b += '\0';
    if (b.size() < spec.padTo) b.resize(spec.padTo, '\xFF');
    uint8_t digest[auth::kSha256Size];
    auth::SoftwareHash().sha256(reinterpret_cast<const uint8_t *>(b.data()), b.size(), digest);
    if (spec.badHash) digest[0] ^= 1;
    b.append(reinterpret_cast<const char *>(digest), sizeof(digest));
    return b;
}

}  // namespace hubbo::test

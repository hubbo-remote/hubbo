#pragma once

// The web admin, embedded in the firmware image (in flash, no heap). The
// table is generated at build time from web/dist by scripts/web_assets.py:
// each file gzipped, with its content type.

#include <cstddef>
#include <cstdint>
#include <string_view>

namespace hubbo::fw::web {

struct Asset {
    const char *path;         // "/index.html", "/a/index-KTMPjhFK.js"
    const char *contentType;  // "text/javascript; charset=utf-8"
    const uint8_t *gzip;      // gzipped body, in flash
    size_t size;
    bool immutable;           // a content-hashed file under /a/: cache forever
};

// The exact path, or null.
const Asset *find(std::string_view path);

extern const char kEtag[];  // quoted, over the whole build: the version of the mutable files
extern const char kCsp[];   // Content-Security-Policy for the pages

}  // namespace hubbo::fw::web

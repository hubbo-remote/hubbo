#pragma once

// The Hubbo image marker (scripts/image_marker.py generates the definition):
//   "HUBBO-IMAGE/1 board=<board::kName> layout=<board::kLayout> version=<git describe>"
// A NUL-terminated string in the image's rodata. A firmware update refuses
// an image without it (api::validateFirmwareImage), or with another board or
// layout.

namespace hubbo::fw::image {

extern const char kMarker[];
const char *version();  // the marker's version=, e.g. "v0.5-3-gabc1234-dirty"

}  // namespace hubbo::fw::image

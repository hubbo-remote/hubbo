# ArduinoJson (vendored)

- Version: 7.4.2, single-header release `ArduinoJson-v7.4.2.h`, unmodified.
- Source: https://github.com/bblanchon/ArduinoJson/releases/download/v7.4.2/ArduinoJson-v7.4.2.h
- SHA-256: a05ac98f4481d2398c103ca5ffcce8e2fcd7fa2fa1d8d9f38d5757454f448d97
- License: MIT (LICENSE.txt).

Used by the portable core (config/json.*) on the host and the ESP32, so both
parse JSON identically. Firmware code should include this copy
(`#include <ArduinoJson.h>`) rather than adding ArduinoJson to lib_deps.
To upgrade: replace ArduinoJson.h with a new single-header release and update
the version and hash above.

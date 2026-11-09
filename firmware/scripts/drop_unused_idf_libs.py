# Drops precompiled ESP-IDF libraries Hubbo never uses from the link.
#
# arduino-esp32 links every library in its `ld_libs` list. esp_matter's
# archive comes before libstdc++ and contains emitted copies of common
# std::string members (e.g. find(char)), so an ordinary call in our code got
# resolved from a Matter object, which dragged in all of Matter and, through
# it, the Wi-Fi stack: about 24 KB of static RAM and 230 KB of flash for code
# that never runs. Without the archive those symbols resolve from libstdc++.
Import("env")  # noqa: F821 (SCons)

UNUSED = {"espressif__esp_matter"}

env.Replace(LIBS=[lib for lib in env.get("LIBS", []) if str(lib) not in UNUSED])  # noqa: F821

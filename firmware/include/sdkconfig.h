#pragma once

// Shadows the prebuilt framework's sdkconfig.h (include/ comes first on
// every path, libraries' too: scripts/sdkconfig_shadow.py) to raise NimBLE's
// limits for several Bluetooth hosts (docs/design/multi-bluetooth.md). A -D
// can't do it: sdkconfig.h would redefine the connection limit.
//
// Each is the hosts (4) plus one: a pairing window needs a free connection
// while all hosts are linked, and a re-pair's new bond is stored before the
// old one is deleted. CCCDs: a few per bonded host. The controller (prebuilt)
// takes its activity count at runtime from NimBLEDevice::init.
#include_next <sdkconfig.h>

#undef CONFIG_BT_NIMBLE_MAX_CONNECTIONS
#define CONFIG_BT_NIMBLE_MAX_CONNECTIONS 5
#undef CONFIG_BT_NIMBLE_MAX_BONDS
#define CONFIG_BT_NIMBLE_MAX_BONDS 5
#undef CONFIG_BT_NIMBLE_MAX_CCCDS
#define CONFIG_BT_NIMBLE_MAX_CCCDS 32

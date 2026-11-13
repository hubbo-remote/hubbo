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

// The GAP Device Name holds the hub's whole name: 32 code points, up to 128
// bytes (core's kHubNameMaxBytes; hub_name.h). nimconfig.h sets
// CONFIG_BT_NIMBLE_GAP_DEVICE_NAME_MAX_LEN to 31 unconditionally, so the
// NimBLE value it feeds is set here instead (both its definitions are
// #ifndef-guarded).
#define MYNEWT_VAL_BLE_SVC_GAP_DEVICE_NAME_MAX_LENGTH 128

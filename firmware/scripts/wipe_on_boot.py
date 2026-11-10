# Gate wipes: HUBBO_WIPE_ON_BOOT=1 in the build environment compiles in a
# full wipe at boot (all of NVS and the config partition), so the board comes
# up factory-fresh. Off unless the variable is exactly "1"; never set it for
# a normal build (bin/hubbo ota --wipe builds it once, then the normal image).
import os

Import("env")  # noqa: F821 (SCons)

if os.environ.get("HUBBO_WIPE_ON_BOOT", "") == "1":
    print("*** HUBBO_WIPE_ON_BOOT: this image ERASES all settings on every boot ***")
    env.Append(CPPDEFINES=["HUBBO_WIPE_ON_BOOT"])  # noqa: F821

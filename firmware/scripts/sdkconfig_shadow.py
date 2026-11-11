# include/sdkconfig.h shadows the prebuilt framework's sdkconfig.h (it raises
# NimBLE's limits). PlatformIO puts include/ on the path of our
# sources only, so NimBLE-Arduino would build with the framework's limit and
# disagree with us (NimBLEServer sizes an array by it). Put include/ first
# for every library too.
Import("env")  # noqa: F821 (SCons)

env.Prepend(CPPPATH=["$PROJECT_INCLUDE_DIR"])  # noqa: F821

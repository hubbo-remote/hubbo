# Portable core: plain C++17, no Arduino, ESP-IDF or FreeRTOS headers.
# The same sources build into the firmware via core/library.json.
file(GLOB_RECURSE HUBBO_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)

add_library(hubbo_core STATIC ${HUBBO_CORE_SOURCES})
target_include_directories(hubbo_core PUBLIC include)
target_compile_options(hubbo_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Host-only twin for hub-sim: the same sources with HUBBO_SIMULATOR_NO_AUTH,
# so auth::authorize() accepts the simulator principal. Only sim/ links it;
# the firmware build (library.json) never defines the flag, and auth/policy.h
# refuses to compile it for ESP32.
add_library(hubbo_core_sim STATIC ${HUBBO_CORE_SOURCES})
target_include_directories(hubbo_core_sim PUBLIC include)
target_compile_definitions(hubbo_core_sim PUBLIC HUBBO_SIMULATOR_NO_AUTH=1)
target_compile_options(hubbo_core_sim PRIVATE -Wall -Wextra -Wpedantic -Werror)

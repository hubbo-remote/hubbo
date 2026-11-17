# hub-sim: the portable core on a developer machine with simulated adapters,
# deterministic time and scripted scenarios. Host-only: nothing here is part
# of core/library.json, and src/host_guard.h fails any ESP32 compile.
#
# The support libraries don't link a core themselves; each executable picks
# one: hubbo_core (unit tests) or hubbo_core_sim (no-auth simulator).

add_library(hubbo_sim_support STATIC
    src/fake_ha.cpp
    src/fake_mqtt.cpp
    src/fakes.cpp
    src/fixtures.cpp
    src/hub.cpp
    src/release_feed.cpp
    src/scenario.cpp
    src/showcase.cpp
)
target_include_directories(hubbo_sim_support PUBLIC src ${PROJECT_SOURCE_DIR}/core/include
                                                    ${PROJECT_SOURCE_DIR}/core/third_party/ArduinoJson)
# The shared API dataset (api/fixtures), seeded by `hub-sim serve`.
target_compile_definitions(hubbo_sim_support PUBLIC HUBBO_API_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/api/fixtures"
                                                    HUBBO_SIM_FIXTURES_DIR="${PROJECT_SOURCE_DIR}/sim/fixtures")
target_compile_options(hubbo_sim_support PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_library(hubbo_sim_http STATIC
    src/http_server.cpp
    src/runtime.cpp
    src/sim_api.cpp
)
target_link_libraries(hubbo_sim_http PUBLIC hubbo_sim_support hubbo_core_sim)
target_compile_options(hubbo_sim_http PRIVATE -Wall -Wextra -Wpedantic -Werror)

add_executable(hub-sim src/main.cpp)
target_link_libraries(hub-sim PRIVATE hubbo_sim_http)
target_compile_options(hub-sim PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Every scenario script runs as its own ctest.
file(GLOB HUBBO_SCENARIOS CONFIGURE_DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/scenarios/*.hubbo)
foreach(script ${HUBBO_SCENARIOS})
    get_filename_component(name ${script} NAME_WE)
    add_test(NAME scenario.${name} COMMAND hub-sim run ${script})
endforeach()

add_subdirectory(tests)

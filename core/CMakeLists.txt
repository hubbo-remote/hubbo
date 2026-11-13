# Portable core: plain C++17, no Arduino, ESP-IDF or FreeRTOS headers.
# The same sources build into the firmware via core/library.json.
file(GLOB_RECURSE HUBBO_CORE_SOURCES CONFIGURE_DEPENDS src/*.cpp)

# The built-in device profiles (profiles/, synced from hubbo-devices),
# embedded by the same script the firmware's build runs.
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(HUBBO_PROFILES_DIR ${PROJECT_SOURCE_DIR}/profiles)
file(GLOB HUBBO_PROFILE_FILES CONFIGURE_DEPENDS ${HUBBO_PROFILES_DIR}/*.json)
set(HUBBO_BUILTIN_PROFILES ${CMAKE_CURRENT_BINARY_DIR}/builtin_profiles.cpp)
add_custom_command(
    OUTPUT ${HUBBO_BUILTIN_PROFILES}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_profiles.py
            ${HUBBO_PROFILES_DIR} ${HUBBO_BUILTIN_PROFILES}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_profiles.py ${HUBBO_PROFILE_FILES}
    COMMENT "Embedding the built-in profiles")
list(APPEND HUBBO_CORE_SOURCES ${HUBBO_BUILTIN_PROFILES})

# The agent docs (api/openapi.yaml as JSON, docs/agent-api.md, docs/llms.txt), embedded by
# the same script the firmware's build runs.
set(HUBBO_OPENAPI_YAML ${PROJECT_SOURCE_DIR}/api/openapi.yaml)
set(HUBBO_AGENT_GUIDE ${PROJECT_SOURCE_DIR}/docs/agent-api.md)
set(HUBBO_LLMS_TXT ${PROJECT_SOURCE_DIR}/docs/llms.txt)
set(HUBBO_AGENT_DOCS ${CMAKE_CURRENT_BINARY_DIR}/agent_docs.cpp)
add_custom_command(
    OUTPUT ${HUBBO_AGENT_DOCS}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_docs.py
            ${HUBBO_OPENAPI_YAML} ${HUBBO_AGENT_GUIDE} ${HUBBO_LLMS_TXT} ${HUBBO_AGENT_DOCS}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_docs.py ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_profiles.py
            ${HUBBO_OPENAPI_YAML} ${HUBBO_AGENT_GUIDE} ${HUBBO_LLMS_TXT}
    COMMENT "Embedding the agent docs")
list(APPEND HUBBO_CORE_SOURCES ${HUBBO_AGENT_DOCS})

add_library(hubbo_core STATIC ${HUBBO_CORE_SOURCES})
target_include_directories(hubbo_core PUBLIC include third_party/ArduinoJson)
target_compile_options(hubbo_core PRIVATE -Wall -Wextra -Wpedantic -Werror)

# Host-only twin for hub-sim: the same sources with HUBBO_SIMULATOR_NO_AUTH,
# so auth::authorize() accepts the simulator principal. Only sim/ links it;
# the firmware build (library.json) never defines the flag, and auth/policy.h
# refuses to compile it for ESP32.
add_library(hubbo_core_sim STATIC ${HUBBO_CORE_SOURCES})
target_include_directories(hubbo_core_sim PUBLIC include third_party/ArduinoJson)
target_compile_definitions(hubbo_core_sim PUBLIC HUBBO_SIMULATOR_NO_AUTH=1)
target_compile_options(hubbo_core_sim PRIVATE -Wall -Wextra -Wpedantic -Werror)

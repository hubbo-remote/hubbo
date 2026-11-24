cmake_minimum_required(VERSION 3.22)
project(hubbo CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

enable_testing()

# The remote log (core/include/hubbo/remote/log.h) is in every host build:
# hub-sim and the tests. The firmware defines it for dev builds only.
add_compile_definitions(HUBBO_REMOTE_LOG=1)

add_subdirectory(core)
add_subdirectory(core/tests)
add_subdirectory(sim)

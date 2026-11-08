find_package(GTest REQUIRED)

file(GLOB HUBBO_CORE_TESTS CONFIGURE_DEPENDS *_test.cpp)

add_executable(hubbo_core_tests ${HUBBO_CORE_TESTS})
# hubbo_sim_support provides the shared fixtures (Watch Apple TV, Play Xbox)
# and recording fake adapters.
target_link_libraries(hubbo_core_tests PRIVATE hubbo_sim_support hubbo_core GTest::gtest_main)
target_compile_options(hubbo_core_tests PRIVATE -Wall -Wextra -Werror)

include(GoogleTest)
gtest_discover_tests(hubbo_core_tests)

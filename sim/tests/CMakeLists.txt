find_package(GTest REQUIRED)

file(GLOB HUBBO_SIM_TESTS CONFIGURE_DEPENDS *_test.cpp)

add_executable(hubbo_sim_tests ${HUBBO_SIM_TESTS})
target_link_libraries(hubbo_sim_tests PRIVATE hubbo_sim_http GTest::gtest_main)
target_compile_options(hubbo_sim_tests PRIVATE -Wall -Wextra -Werror)

include(GoogleTest)
gtest_discover_tests(hubbo_sim_tests)

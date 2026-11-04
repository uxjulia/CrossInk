add_executable(HalFrontlightTest HalFrontlightTest.cpp ${REPO_ROOT}/lib/hal/HalFrontlight.cpp)
target_include_directories(HalFrontlightTest PRIVATE stubs ${REPO_ROOT}/lib/hal)
target_link_libraries(HalFrontlightTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HalFrontlightTest)

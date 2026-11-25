add_executable(SwipeDeleteTest SwipeDeleteTest.cpp)
target_include_directories(SwipeDeleteTest PRIVATE
  stubs ${REPO_ROOT}/src
  ${REPO_ROOT}/freeink-sdk/libs/ui/FreeInkUI/include
  ${REPO_ROOT}/freeink-sdk/libs/assets/Icons/include)
target_link_libraries(SwipeDeleteTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(SwipeDeleteTest)

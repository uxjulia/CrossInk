add_executable(ProgressSlotsTest ProgressSlotsTest.cpp)
target_include_directories(ProgressSlotsTest PRIVATE stubs ../book_reading_stats_atomic/stubs
  ${REPO_ROOT}/src ${REPO_ROOT}/src/activities/reader)
target_link_libraries(ProgressSlotsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ProgressSlotsTest)

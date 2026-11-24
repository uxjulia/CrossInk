add_executable(LookupHistoryTest
  LookupHistoryTest.cpp
  ${REPO_ROOT}/src/util/LookupHistory.cpp
)
target_include_directories(LookupHistoryTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/src/util
)
target_link_libraries(LookupHistoryTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(LookupHistoryTest)

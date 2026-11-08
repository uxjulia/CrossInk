add_executable(BookReadingStatsAtomicTest
  BookReadingStatsAtomicTest.cpp
  ReadingStatsLinkStubs.cpp
  ${REPO_ROOT}/src/activities/reader/BookReadingStats.cpp
  ${REPO_ROOT}/src/activities/reader/StatsCounterId.cpp
)

target_include_directories(BookReadingStatsAtomicTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/activities/reader
)

target_link_libraries(BookReadingStatsAtomicTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BookReadingStatsAtomicTest)
target_compile_definitions(BookReadingStatsAtomicTest PRIVATE SIMULATOR)

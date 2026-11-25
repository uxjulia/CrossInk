add_executable(ReadingSessionCheckpointTest
 ReadingSessionCheckpointTest.cpp
 ${REPO_ROOT}/src/activities/reader/ReadingSessionStats.cpp
 ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
)
target_include_directories(ReadingSessionCheckpointTest PRIVATE
 ${REPO_ROOT}/test/daily_reading_stats/stubs ${REPO_ROOT}/test/sync_files/stubs ${REPO_ROOT}/lib/Memory
 ${REPO_ROOT}/src/activities/reader)
target_link_libraries(ReadingSessionCheckpointTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ReadingSessionCheckpointTest)

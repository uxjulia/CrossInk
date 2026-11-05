add_executable(DailyReadingStatsTest
 DailyReadingStatsTest.cpp
 ${REPO_ROOT}/src/activities/reader/DailyReadingStats.cpp
 ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
 ${REPO_ROOT}/src/activities/reader/StatsUploadPayload.cpp
)
target_include_directories(DailyReadingStatsTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO_ROOT}/src/activities/reader ${REPO_ROOT}/src)
target_link_libraries(DailyReadingStatsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(DailyReadingStatsTest)

target_compile_definitions(DailyReadingStatsTest PRIVATE DAILY_READING_STATS_TEST)

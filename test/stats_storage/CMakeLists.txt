add_executable(StatsStorageTest
 StatsStorageTest.cpp
 ${REPO_ROOT}/src/activities/reader/GlobalReadingStats.cpp
 ${REPO_ROOT}/src/util/StatsStorage.cpp
 ${REPO_ROOT}/src/activities/reader/BookReadingStats.cpp
 ${REPO_ROOT}/src/activities/reader/DailyReadingStats.cpp
 ${REPO_ROOT}/src/activities/reader/ReadingSessionJournal.cpp
 ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
 ${REPO_ROOT}/src/sync/SyncFiles.cpp
 ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)
target_include_directories(StatsStorageTest PRIVATE
 ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO_ROOT}/test/reading_session_journal/stubs
 ${REPO_ROOT}/test/book_reading_stats_atomic/stubs
 ${REPO_ROOT}/test/daily_reading_stats/stubs ${REPO_ROOT}/test/sync_files/stubs
 ${REPO_ROOT}/lib/uzlib/src ${REPO_ROOT}/src ${REPO_ROOT}/src/activities/reader)
target_compile_definitions(StatsStorageTest PRIVATE SIMULATOR STATS_STORAGE_TEST DAILY_READING_STATS_TEST)
target_link_libraries(StatsStorageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(StatsStorageTest)

add_executable(ReadingSessionJournalTest
 ReadingSessionJournalTest.cpp
 ${REPO_ROOT}/src/activities/reader/ReadingSessionJournal.cpp
 ${REPO_ROOT}/src/activities/reader/ReadingStatsUtils.cpp
 ${REPO_ROOT}/src/sync/SyncFiles.cpp
 ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)
target_include_directories(ReadingSessionJournalTest PRIVATE
 ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO_ROOT}/test/daily_reading_stats/stubs
 ${REPO_ROOT}/test/sync_files/stubs ${REPO_ROOT}/src ${REPO_ROOT}/src/activities/reader
 ${REPO_ROOT}/lib/uzlib/src)
target_compile_definitions(ReadingSessionJournalTest PRIVATE SIMULATOR)
target_link_libraries(ReadingSessionJournalTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ReadingSessionJournalTest)

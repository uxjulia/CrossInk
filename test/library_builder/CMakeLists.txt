add_executable(LibraryBuilderTest
  LibraryBuilderTest.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryBuilder.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryIndexFile.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryMetadataCache.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibrarySort.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryFormat.cpp
  ${REPO_ROOT}/lib/LibraryIndex/LibraryText.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(LibraryBuilderTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/LibraryIndex
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
)
target_link_libraries(LibraryBuilderTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(LibraryBuilderTest)

add_executable(BookCacheUtilsTest
  BookCacheUtilsTest.cpp
  ${REPO_ROOT}/src/util/BookCacheUtils.cpp
  ${REPO_ROOT}/src/activities/reader/BookStatsTracking.cpp
  ${REPO_ROOT}/src/sync/BookSyncFile.cpp
  ${REPO_ROOT}/src/sync/SyncFiles.cpp
  ${REPO_ROOT}/src/sync/SyncIds.cpp
  ${REPO_ROOT}/src/sync/SyncPending.cpp
  ${REPO_ROOT}/src/sync/SyncState.cpp
  ${REPO_ROOT}/src/sync/SyncParked.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)
target_include_directories(BookCacheUtilsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
)
target_include_directories(BookCacheUtilsTest PRIVATE
  ${REPO_ROOT}/test/sync_ids/stubs
  ${REPO_ROOT}/test/sync_parked/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/KOReaderSync
  ${REPO_ROOT}/lib/uzlib/src
)
target_compile_definitions(BookCacheUtilsTest PRIVATE SIMULATOR)
target_link_libraries(BookCacheUtilsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(BookCacheUtilsTest)

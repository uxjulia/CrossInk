add_executable(SyncPendingTest
 SyncPendingTest.cpp
 ${REPO_ROOT}/src/sync/SyncFiles.cpp
 ${REPO_ROOT}/src/sync/SyncPending.cpp
 ${REPO_ROOT}/src/sync/SyncState.cpp
 ${REPO_ROOT}/src/sync/SyncParked.cpp
 ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)
target_include_directories(SyncPendingTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs
                           ${CMAKE_CURRENT_SOURCE_DIR}/../sync_parked/stubs ${REPO_ROOT}/src/sync
                           ${REPO_ROOT}/lib/uzlib/src)
target_compile_definitions(SyncPendingTest PRIVATE SIMULATOR)
target_link_libraries(SyncPendingTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(SyncPendingTest)

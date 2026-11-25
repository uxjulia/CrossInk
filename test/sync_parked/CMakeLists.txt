add_executable(SyncParkedTest
 SyncParkedTest.cpp
 ${REPO_ROOT}/src/sync/SyncFiles.cpp
 ${REPO_ROOT}/src/sync/SyncParked.cpp
 ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)
target_include_directories(SyncParkedTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs
                           ${CMAKE_CURRENT_SOURCE_DIR}/../sync_ids/stubs ${REPO_ROOT}/src/sync
                           ${REPO_ROOT}/lib/uzlib/src)
target_compile_definitions(SyncParkedTest PRIVATE SIMULATOR)
target_link_libraries(SyncParkedTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(SyncParkedTest)

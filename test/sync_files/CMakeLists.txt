add_executable(SyncFilesTest
 SyncFilesTest.cpp
 ${REPO_ROOT}/src/sync/SyncFiles.cpp
 ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)
target_include_directories(SyncFilesTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO_ROOT}/src/sync
                           ${REPO_ROOT}/lib/uzlib/src)
target_link_libraries(SyncFilesTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(SyncFilesTest)

add_executable(SyncIdsTest
 SyncIdsTest.cpp
 ${REPO_ROOT}/src/sync/SyncFiles.cpp
 ${REPO_ROOT}/src/sync/SyncIds.cpp
 ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)
target_include_directories(SyncIdsTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO_ROOT}/src/sync
                           ${REPO_ROOT}/lib/uzlib/src)
target_compile_definitions(SyncIdsTest PRIVATE SIMULATOR)
target_link_libraries(SyncIdsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(SyncIdsTest)

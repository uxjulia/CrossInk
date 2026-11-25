add_executable(LogRetentionTest LogRetentionTest.cpp ${REPO_ROOT}/lib/Logging/Logging.cpp)
target_include_directories(LogRetentionTest PRIVATE stubs ${REPO_ROOT}/lib/Logging)
target_link_libraries(LogRetentionTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(LogRetentionTest)

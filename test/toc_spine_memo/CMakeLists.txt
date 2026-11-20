add_executable(TocSpineMemoTest TocSpineMemoTest.cpp)
target_include_directories(TocSpineMemoTest PRIVATE ${REPO_ROOT}/lib/Epub/Epub)
target_link_libraries(TocSpineMemoTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TocSpineMemoTest)

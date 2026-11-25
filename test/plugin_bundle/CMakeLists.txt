add_executable(PluginBundleTest PluginBundleTest.cpp ${REPO_ROOT}/src/util/PluginBundle.cpp)
target_include_directories(PluginBundleTest PRIVATE stubs ${REPO_ROOT}/src ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/FsHelpers)
target_link_libraries(PluginBundleTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PluginBundleTest)

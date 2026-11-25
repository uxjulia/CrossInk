add_executable(PluginEventOutboxTest
  PluginEventOutboxTest.cpp
  ${REPO_ROOT}/src/util/PluginEventOutbox.cpp
)
target_include_directories(PluginEventOutboxTest PRIVATE stubs ${REPO_ROOT}/src ${REPO_ROOT}/lib/FsHelpers)
target_link_libraries(PluginEventOutboxTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PluginEventOutboxTest)

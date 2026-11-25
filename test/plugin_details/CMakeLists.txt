add_executable(PluginDetailsTest PluginDetailsTest.cpp
  ${REPO_ROOT}/src/util/HtmlToPlainText.cpp
  ${REPO_ROOT}/lib/Epub/Epub/htmlEntities.cpp)
target_include_directories(PluginDetailsTest PRIVATE ${REPO_ROOT}/src ${REPO_ROOT}/lib/Epub)
target_link_libraries(PluginDetailsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PluginDetailsTest)

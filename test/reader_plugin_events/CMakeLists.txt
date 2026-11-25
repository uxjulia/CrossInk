add_executable(ReaderPluginEventsTest
  ReaderPluginEventsTest.cpp
  ${REPO_ROOT}/src/activities/reader/PluginReaderEvents.cpp
  ${REPO_ROOT}/src/activities/reader/ReaderSession.cpp
)
target_include_directories(ReaderPluginEventsTest PRIVATE stubs ${REPO_ROOT}/src)
target_link_libraries(ReaderPluginEventsTest PRIVATE crosspoint_test_common)
# This assert-based harness must also run its checks in Release builds.
target_compile_options(ReaderPluginEventsTest PRIVATE -UNDEBUG)
add_test(NAME ReaderPluginEventsTest COMMAND ReaderPluginEventsTest)

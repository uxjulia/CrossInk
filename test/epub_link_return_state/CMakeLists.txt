add_executable(EpubLinkReturnStateTest
  EpubLinkReturnStateTest.cpp
  ${REPO_ROOT}/src/activities/reader/EpubLinkReturnState.cpp
)
target_include_directories(EpubLinkReturnStateTest PRIVATE
  ${REPO_ROOT}/lib/FsHelpers
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/test/reader_progress_save_debouncer/stubs
)
target_link_libraries(EpubLinkReturnStateTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(EpubLinkReturnStateTest)

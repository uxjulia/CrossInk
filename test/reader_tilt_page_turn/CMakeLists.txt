add_executable(ReaderTiltPageTurnTest ReaderTiltPageTurnTest.cpp)
target_include_directories(ReaderTiltPageTurnTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/reader
)
target_link_libraries(ReaderTiltPageTurnTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(ReaderTiltPageTurnTest)

add_executable(TouchActionButtonsTest
  TouchActionButtonsTest.cpp
  ${REPO_ROOT}/src/components/TouchActionButtons.cpp
)
target_include_directories(TouchActionButtonsTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/components
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/src
)
target_link_libraries(TouchActionButtonsTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(TouchActionButtonsTest)

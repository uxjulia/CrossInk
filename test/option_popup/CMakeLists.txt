add_executable(OptionPopupTest
  OptionPopupTest.cpp
)

target_include_directories(OptionPopupTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
)

target_compile_definitions(OptionPopupTest PRIVATE
  CROSSINK_APP_CAP_TOUCH=1
  SIMULATOR=1
)

target_link_libraries(OptionPopupTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(OptionPopupTest)

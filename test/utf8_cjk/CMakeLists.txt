add_executable(Utf8CjkTest
  Utf8CjkTest.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(Utf8CjkTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(Utf8CjkTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(Utf8CjkTest)

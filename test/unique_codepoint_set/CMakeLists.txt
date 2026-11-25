add_executable(UniqueCodepointSetTest
  UniqueCodepointSetTest.cpp
)

target_include_directories(UniqueCodepointSetTest PRIVATE
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(UniqueCodepointSetTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(UniqueCodepointSetTest)

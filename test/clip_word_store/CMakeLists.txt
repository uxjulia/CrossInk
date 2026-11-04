add_executable(ClipWordStoreTest
  ClipWordStoreTest.cpp
  ${REPO_ROOT}/src/clippings/ClipTextBuilder.cpp
)

target_link_libraries(ClipWordStoreTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

target_include_directories(ClipWordStoreTest PRIVATE
  ${REPO_ROOT}/test/minibidi_arabic/stubs  # no-op Logging.h for host tests
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/clippings
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Memory
)

gtest_discover_tests(ClipWordStoreTest)

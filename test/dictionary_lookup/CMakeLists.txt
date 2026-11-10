add_executable(DictionaryLookupTest
  DictionaryLookupTest.cpp
  ${REPO_ROOT}/src/util/Dictionary.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(DictionaryLookupTest PRIVATE
  stubs
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/Memory
)
target_link_libraries(DictionaryLookupTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(DictionaryLookupTest)

add_executable(DictionaryLookupTest
  DictionaryLookupTest.cpp
  DictZipTest.cpp
  ${REPO_ROOT}/src/util/DictZip.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
  ${REPO_ROOT}/src/util/Dictionary.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)
target_include_directories(DictionaryLookupTest PRIVATE
  stubs
  ${REPO_ROOT}/src/util
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${REPO_ROOT}/lib/Memory
)
find_package(ZLIB REQUIRED)
target_link_libraries(DictionaryLookupTest PRIVATE ZLIB::ZLIB crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(DictionaryLookupTest)

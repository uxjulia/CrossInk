add_executable(SectionPersistenceTest
  SectionPersistenceTest.cpp
  EpubProgressPersistenceTest.cpp
  SectionPersistenceLinkStubs.cpp
  ${REPO_ROOT}/lib/Epub/Epub/Section.cpp
  ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp
)

target_include_directories(SectionPersistenceTest PRIVATE
  ${REPO_ROOT}/freeink-sdk/libs/book/ContentProtection/include
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/InflateReader
  ${REPO_ROOT}/lib/uzlib/src
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/Epub
  ${REPO_ROOT}/lib/Epub/Epub
  ${REPO_ROOT}/lib/Epub/Epub/parsers
  ${REPO_ROOT}/lib/Epub/Epub/css
  ${REPO_ROOT}/lib/EpdFont
  ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/ZipFile
  ${REPO_ROOT}/lib/MemoryBudget
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Utf8
  ${REPO_ROOT}/lib/XmlParserUtils
)

target_link_libraries(SectionPersistenceTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  expat
)

gtest_discover_tests(SectionPersistenceTest)

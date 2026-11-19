add_executable(TouchReaderPreviewModelTest
  TouchReaderPreviewModelTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/RuledLineLayout.cpp
  ${REPO_ROOT}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
)

target_include_directories(TouchReaderPreviewModelTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/include
  ${REPO_ROOT}/lib/Epub
)

target_link_libraries(TouchReaderPreviewModelTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(TouchReaderPreviewModelTest)

target_compile_definitions(TouchReaderPreviewModelTest PRIVATE
  SIMULATOR CROSSINK_APP_CAP_TOUCH=0 CROSSINK_APP_CAP_USB_DRIVE=0
)

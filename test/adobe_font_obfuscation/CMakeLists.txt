find_package(EXPAT REQUIRED)
set(ADOBE_SDK ${REPO_ROOT}/freeink-sdk/libs/font/FreeInkFont)
file(GLOB ADOBE_FONT_SOURCES ${ADOBE_SDK}/src/*.c ${ADOBE_SDK}/src/*.cpp ${ADOBE_SDK}/src/freetype/*.c)
add_executable(AdobeFontObfuscationTest AdobeFontObfuscationTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/parsers/AdobeFontObfuscation.cpp
  ${REPO_ROOT}/lib/FsHelpers/FsHelpers.cpp
  ${REPO_ROOT}/lib/FsHelpers/NaturalSort.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp ${ADOBE_FONT_SOURCES})
target_include_directories(AdobeFontObfuscationTest PRIVATE
  ../fs_helpers/stubs ${REPO_ROOT}/lib/FsHelpers ../content_opf_parser/stubs
  ${REPO_ROOT}/lib/Epub/Epub/parsers ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/ScalableFont
  ${ADOBE_SDK}/include ${ADOBE_SDK}/third_party/freetype/include ${ADOBE_SDK}/third_party/stb)
target_compile_definitions(AdobeFontObfuscationTest PRIVATE FREEINK_FONT_ENABLE_CFF=1
  FREEINK_FONT_ENABLE_AUTOHINT=1 FREEINK_FONT_ENABLE_NATIVE_HINTING=1 FREEINK_FONT_ENABLE_MONOCHROME=1)
target_compile_options(AdobeFontObfuscationTest PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(AdobeFontObfuscationTest PRIVATE -fsanitize=address,undefined)
target_link_libraries(AdobeFontObfuscationTest PRIVATE EXPAT::EXPAT GTest::gtest_main crosspoint_test_common)
gtest_discover_tests(AdobeFontObfuscationTest)

cmake_minimum_required(VERSION 3.16)
project(UiFontErrorTests C CXX)
enable_testing()
set(SDK ${CMAKE_CURRENT_SOURCE_DIR}/../../freeink-sdk/libs/font/FreeInkFont)
file(GLOB FONT_SOURCES ${SDK}/src/*.c ${SDK}/src/*.cpp ${SDK}/src/freetype/*.c)
add_executable(ui_font_errors UiFontErrorTest.cpp ../../src/FilenameFontSystem.cpp
  ../../lib/ScalableFont/HalScalableFont.cpp ${FONT_SOURCES})
target_compile_features(ui_font_errors PRIVATE cxx_std_20)
target_compile_options(ui_font_errors PRIVATE -UNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer)
target_link_options(ui_font_errors PRIVATE -fsanitize=address,undefined)
target_compile_definitions(ui_font_errors PRIVATE SIMULATOR=1 CROSSINK_SCALABLE_FONTS=1
  CROSSINK_APP_CAP_TOUCH=1 CROSSINK_APP_CAP_USB_DRIVE=1
  FREEINK_FONT_ENABLE_CFF=1 FREEINK_FONT_ENABLE_AUTOHINT=1
  FREEINK_FONT_ENABLE_NATIVE_HINTING=1 FREEINK_FONT_ENABLE_MONOCHROME=1)
target_include_directories(ui_font_errors PRIVATE stubs ../otf_fonts/stubs ../scalable_fonts/stubs
  ../../src ../../include ../../lib/ScalableFont ../../lib/EpdFont ../../lib/Epub ../../lib/Memory
  ${SDK}/include ${SDK}/third_party/freetype/include ${SDK}/third_party/stb)
add_test(NAME ui_font_errors COMMAND ui_font_errors ${CMAKE_CURRENT_SOURCE_DIR}/../..)

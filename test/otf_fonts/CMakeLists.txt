# Integration uses the real adapter and pinned SDK, with only storage/RTOS host shims.
set(OTF_SDK ${CMAKE_CURRENT_SOURCE_DIR}/../../freeink-sdk/libs/font/FreeInkFont)
file(GLOB OTF_SOURCES ${OTF_SDK}/src/*.c ${OTF_SDK}/src/*.cpp ${OTF_SDK}/src/freetype/*.c)
foreach(mode IN ITEMS scalable c3)
  add_executable(otf_${mode} OtfFontTest.cpp ../../src/FontFileValidation.cpp
    ../../lib/EpdFont/SdCardFontRegistry.cpp)
  target_compile_features(otf_${mode} PRIVATE cxx_std_20)
  target_compile_options(otf_${mode} PRIVATE -UNDEBUG -fsanitize=address,undefined -fno-omit-frame-pointer)
  target_link_options(otf_${mode} PRIVATE -fsanitize=address,undefined)
  target_include_directories(otf_${mode} PRIVATE stubs ../../lib/ScalableFont
    ../scalable_fonts/stubs ../../lib/EpdFont ../../lib/Memory ../../src
    ${OTF_SDK}/include ${OTF_SDK}/third_party/freetype/include ${OTF_SDK}/third_party/stb)
  target_compile_definitions(otf_${mode} PRIVATE SIMULATOR=1)
  if(mode STREQUAL scalable)
    target_sources(otf_${mode} PRIVATE ../../lib/ScalableFont/HalScalableFont.cpp ${OTF_SOURCES})
    target_compile_definitions(otf_${mode} PRIVATE CROSSINK_SCALABLE_FONTS=1
      FREEINK_FONT_ENABLE_AUTOHINT=1 FREEINK_FONT_ENABLE_NATIVE_HINTING=1 FREEINK_FONT_ENABLE_MONOCHROME=1 FREEINK_FONT_ENABLE_CFF=1)
  else()
    target_compile_definitions(otf_${mode} PRIVATE CROSSINK_SCALABLE_FONTS=0)
  endif()
  add_test(NAME otf_${mode} COMMAND otf_${mode} ${CMAKE_CURRENT_SOURCE_DIR}/../..
    ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/FontAwesome.otf)
endforeach()

find_program(OTF_NODE node)
if(OTF_NODE)
  add_test(NAME otf_web_filter COMMAND ${OTF_NODE}
    ${CMAKE_CURRENT_SOURCE_DIR}/WebFontFilterTest.js ${CMAKE_CURRENT_SOURCE_DIR}/../../web/pages/fonts.js)
endif()

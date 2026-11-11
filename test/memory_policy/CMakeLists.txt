add_executable(MemoryPoolPolicyTest MemoryPoolPolicyTest.cpp)
target_include_directories(MemoryPoolPolicyTest PRIVATE stubs ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget)
target_compile_definitions(MemoryPoolPolicyTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1)
target_link_libraries(MemoryPoolPolicyTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(MemoryPoolPolicyTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")

add_executable(JpegDecoderPolicyTest JpegDecoderPolicyTest.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/JpegToFramebufferConverter.cpp
  ${REPO_ROOT}/lib/Epub/Epub/converters/ImageToFramebufferDecoder.cpp)
target_include_directories(JpegDecoderPolicyTest PRIVATE jpeg_decoder_stubs stubs
  ${REPO_ROOT}/lib/Epub/Epub/converters ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/MemoryBudget ${REPO_ROOT}/lib/GfxRenderer)
target_compile_definitions(JpegDecoderPolicyTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1)
target_link_libraries(JpegDecoderPolicyTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(JpegDecoderPolicyTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")

find_package(ZLIB REQUIRED)
add_executable(InflateStreamStorageTest InflateStreamStorageTest.cpp
  ${REPO_ROOT}/lib/miniz/src/InflateStream.cpp ${REPO_ROOT}/lib/miniz/src/miniz_impl.c
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp)
target_include_directories(InflateStreamStorageTest PRIVATE stubs ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget ${REPO_ROOT}/lib/miniz/src)
target_compile_definitions(InflateStreamStorageTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1)
target_link_libraries(InflateStreamStorageTest PRIVATE crosspoint_test_common GTest::gtest_main ZLIB::ZLIB)
gtest_discover_tests(InflateStreamStorageTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")

add_executable(SdFontBitmapStorageTest SdFontBitmapStorageTest.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp ${REPO_ROOT}/lib/Utf8/Utf8.cpp)
target_include_directories(SdFontBitmapStorageTest PRIVATE stubs ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/Utf8)
target_compile_definitions(SdFontBitmapStorageTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1)
target_link_libraries(SdFontBitmapStorageTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(SdFontBitmapStorageTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")

add_executable(CssArenaBackingTest CssArenaBackingTest.cpp ${REPO_ROOT}/lib/Epub/Epub/css/CssParser.cpp)
target_include_directories(CssArenaBackingTest PRIVATE stubs ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget ${REPO_ROOT}/lib/Epub/Epub/css)
target_compile_definitions(CssArenaBackingTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1)
target_link_libraries(CssArenaBackingTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(CssArenaBackingTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")

function(add_layout_test name root)
  add_executable(${name} TextLayoutBackingTest.cpp
    ${root}/lib/Epub/Epub/ParsedText.cpp ${root}/lib/Epub/Epub/blocks/TextBlock.cpp
    ${root}/lib/Epub/Epub/hyphenation/HyphenationCommon.cpp
    ${root}/lib/MiniBidi/BidiUtils.cpp ${root}/lib/MiniBidi/minibidi.c ${root}/lib/Utf8/Utf8.cpp)
  target_include_directories(${name} PRIVATE stubs ${root}/lib/Memory ${root}/lib/MemoryBudget
    ${root}/lib/Epub ${root}/lib/EpdFont ${root}/lib/Utf8 ${root}/lib/MiniBidi ${root}/lib/Serialization)
  target_compile_definitions(${name} PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1)
  target_link_libraries(${name} PRIVATE crosspoint_test_common GTest::gtest_main)
  gtest_discover_tests(${name} TEST_PREFIX "${name}/" PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")
endfunction()
add_layout_test(TextLayoutBackingTest ${REPO_ROOT})
if(PSRAM_REFERENCE_ROOT)
  add_layout_test(TextLayoutReferenceTest ${PSRAM_REFERENCE_ROOT})
  target_compile_definitions(TextLayoutReferenceTest PRIVATE PSRAM_REFERENCE=1)
endif()

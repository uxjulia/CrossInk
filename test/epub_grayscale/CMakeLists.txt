add_executable(EpubGrayscaleTest EpubGrayscaleTest.cpp
  ${REPO_ROOT}/src/activities/reader/EpubGrayscale.cpp)
target_include_directories(EpubGrayscaleTest PRIVATE stubs ../memory_policy/stubs
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget
  ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/src/activities/reader)
target_compile_definitions(EpubGrayscaleTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1)
target_link_libraries(EpubGrayscaleTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(EpubGrayscaleTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")

# Real text/glyph rasterization with a memory-only display and storage backend.
add_executable(EpubTextGrayscaleTest EpubTextGrayscaleTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/GfxRenderer.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFont.cpp
  ${REPO_ROOT}/lib/EpdFont/EpdFontFamily.cpp
  ${REPO_ROOT}/lib/InflateReader/InflateReader.cpp
  ${REPO_ROOT}/lib/EpdFont/FontDecompressor.cpp
  ${REPO_ROOT}/lib/EpdFont/SdCardFont.cpp
  ${REPO_ROOT}/lib/Epub/Epub/blocks/TextBlock.cpp
  ${REPO_ROOT}/lib/Memory/BuildScratch.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/MiniBidi/BidiUtils.cpp
  ${REPO_ROOT}/lib/MiniBidi/minibidi.c
  ${REPO_ROOT}/lib/uzlib/src/tinflate.c
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c)
target_include_directories(EpubTextGrayscaleTest PRIVATE raster_stubs
  ${REPO_ROOT}/include ${REPO_ROOT}/src/activities/reader
  ${REPO_ROOT}/freeink-sdk/libs/display/FreeInkDisplay/include
  ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/ScreenCalibration ../memory_policy/stubs
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/EpdFont ${REPO_ROOT}/lib/Memory
  ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/MiniBidi
  ${REPO_ROOT}/lib/miniz/src ${REPO_ROOT}/lib/InflateReader ${REPO_ROOT}/lib/uzlib/src)
target_compile_definitions(EpubTextGrayscaleTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1
  CROSSINK_APP_CAP_TOUCH=0 CROSSINK_APP_CAP_USB_DRIVE=0)
target_link_libraries(EpubTextGrayscaleTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(EpubTextGrayscaleTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")

add_executable(RetainedPxcCacheTest RetainedPxcCacheTest.cpp
  ${REPO_ROOT}/src/activities/reader/EpubGrayscale.cpp)
target_include_directories(RetainedPxcCacheTest PRIVATE stubs ../memory_policy/stubs
  ${REPO_ROOT}/lib/Epub ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/MemoryBudget
  ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/src/activities/reader)
target_compile_definitions(RetainedPxcCacheTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1)
target_link_libraries(RetainedPxcCacheTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(RetainedPxcCacheTest PROPERTIES ENVIRONMENT "UBSAN_OPTIONS=halt_on_error=1")

add_executable(DeferredGrayscaleTest DeferredGrayscaleTest.cpp)
target_include_directories(DeferredGrayscaleTest PRIVATE stubs ../memory_policy/stubs
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/src/activities/reader)
target_compile_definitions(DeferredGrayscaleTest PRIVATE ARDUINO_ARCH_ESP32=1 CROSSINK_MEMORY_TEST=1)
target_link_libraries(DeferredGrayscaleTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(DeferredGrayscaleTest)

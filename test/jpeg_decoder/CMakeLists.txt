find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(JPEGDEC_SOURCE_DIR "" CACHE PATH "Existing pinned JPEGDEC source for host tests")
if(NOT JPEGDEC_SOURCE_DIR)
  FetchContent_Declare(jpegdec
    GIT_REPOSITORY https://github.com/bitbank2/JPEGDEC.git
    GIT_TAG 86282979224c8a32fd51e091ed5a35b0c699a52b
    SOURCE_SUBDIR crossink-host-tests) # Skip the dependency's ESP-IDF CMake entry point.
  if(CMAKE_VERSION VERSION_LESS 3.18)
    # Older MakeAvailable ignores SOURCE_SUBDIR and enters the ESP-IDF project.
    FetchContent_GetProperties(jpegdec)
    if(NOT jpegdec_POPULATED)
      FetchContent_Populate(jpegdec)
    endif()
  else()
    FetchContent_MakeAvailable(jpegdec)
  endif()
  set(JPEGDEC_SOURCE_DIR "${jpegdec_SOURCE_DIR}")
endif()
execute_process(
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/patch_jpegdec.py ${JPEGDEC_SOURCE_DIR}
  RESULT_VARIABLE jpeg_patch_result)
if(NOT jpeg_patch_result EQUAL 0)
  message(FATAL_ERROR "JPEGDEC host dependency patch failed")
endif()
add_library(jpegdec_host STATIC ${JPEGDEC_SOURCE_DIR}/src/JPEGDEC.cpp)
target_include_directories(jpegdec_host PUBLIC ${JPEGDEC_SOURCE_DIR}/src)
target_compile_definitions(jpegdec_host PUBLIC __LINUX__)

add_executable(JpegDecoderTest JpegDecoderTest.cpp)
target_compile_definitions(JpegDecoderTest PRIVATE JPEG_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(JpegDecoderTest PRIVATE crosspoint_test_common GTest::gtest_main jpegdec_host)
gtest_discover_tests(JpegDecoderTest)

cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(sleep_image_deck_tests LANGUAGES CXX)
  set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  enable_testing()
endif()
add_executable(SleepImageDeckTest SleepImageDeckTest.cpp
  ${REPO_ROOT}/src/activities/boot_sleep/ImageFolderIndex.cpp
  ${REPO_ROOT}/src/activities/boot_sleep/SleepImageDeck.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp)
target_include_directories(SleepImageDeckTest PRIVATE stubs ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/src/activities/boot_sleep ${REPO_ROOT}/lib/Serialization)
target_compile_features(SleepImageDeckTest PRIVATE cxx_std_20)
target_compile_options(SleepImageDeckTest PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME SleepImageDeckTest COMMAND SleepImageDeckTest)

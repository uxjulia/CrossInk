cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(asset_cache_storage_tests LANGUAGES CXX)
  set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  enable_testing()
endif()
foreach(target AssetCacheStorageTest AssetCacheNoPsramTest)
  add_executable(${target} AssetCacheStorageTest.cpp
    ${REPO_ROOT}/lib/AssetCacheStorage/AssetCacheStorage.cpp
    ${REPO_ROOT}/lib/HalFlashPartition/HalFlashPartition.cpp)
  target_include_directories(${target} PRIVATE stubs ${REPO_ROOT}/test/language_cache/stubs
    ${REPO_ROOT}/lib/AssetCacheStorage ${REPO_ROOT}/lib/HalFlashPartition)
  target_compile_definitions(${target} PRIVATE SIMULATOR)
  target_compile_features(${target} PRIVATE cxx_std_20)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror -UNDEBUG)
  add_test(NAME ${target} COMMAND ${target})
endforeach()
target_compile_definitions(AssetCacheStorageTest PRIVATE CROSSINK_ASSET_CACHE_TEST_PSRAM)

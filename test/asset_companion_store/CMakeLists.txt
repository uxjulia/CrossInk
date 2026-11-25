cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(asset_companion_store_tests LANGUAGES CXX)
  set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  enable_testing()
endif()
find_package(OpenSSL REQUIRED)
add_executable(AssetCompanionStoreTest AssetCompanionStoreTest.cpp
  ${REPO_ROOT}/src/network/AssetCompanionStore.cpp
  ${REPO_ROOT}/lib/AssetCompanionIndex/AssetCompanionIndex.cpp)
target_include_directories(AssetCompanionStoreTest PRIVATE stubs ${REPO_ROOT}/src/network ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/AssetCatalog ${REPO_ROOT}/lib/AssetCompanionIndex ${REPO_ROOT}/lib/Memory)
target_compile_features(AssetCompanionStoreTest PRIVATE cxx_std_20)
target_compile_options(AssetCompanionStoreTest PRIVATE -Wall -Wextra -Werror -Wno-deprecated-declarations -UNDEBUG)
target_link_libraries(AssetCompanionStoreTest PRIVATE OpenSSL::Crypto)
add_test(NAME AssetCompanionStoreTest COMMAND AssetCompanionStoreTest)

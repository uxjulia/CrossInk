cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(asset_download_tests LANGUAGES CXX)
  set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  enable_testing()
endif()
find_package(OpenSSL REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(ASSET_I18N "${CMAKE_CURRENT_BINARY_DIR}/i18n")
file(MAKE_DIRECTORY ${ASSET_I18N})
add_custom_command(OUTPUT ${ASSET_I18N}/I18nKeys.h ${ASSET_I18N}/I18nStrings.h ${ASSET_I18N}/I18nStrings.cpp
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/gen_i18n.py
    ${REPO_ROOT}/lib/I18n/translations ${ASSET_I18N}
  DEPENDS ${REPO_ROOT}/scripts/gen_i18n.py ${REPO_ROOT}/lib/I18n/translations/english.yaml
    ${REPO_ROOT}/lib/I18n/languages.json
  WORKING_DIRECTORY ${REPO_ROOT} VERBATIM)
add_custom_target(AssetI18nSchema DEPENDS ${ASSET_I18N}/I18nKeys.h ${ASSET_I18N}/I18nStrings.h
  ${ASSET_I18N}/I18nStrings.cpp)
add_executable(AssetDownloadTest AssetDownloadTest.cpp
  ${REPO_ROOT}/src/network/AssetDownloadService.cpp
  ${REPO_ROOT}/lib/AssetCatalog/AssetCatalog.cpp
  ${REPO_ROOT}/lib/LanguageCache/LanguageCache.cpp
  ${REPO_ROOT}/lib/I18n/I18n.cpp ${ASSET_I18N}/I18nStrings.cpp
  ${REPO_ROOT}/lib/HalFlashPartition/HalFlashPartition.cpp
  ${REPO_ROOT}/lib/AssetCacheStorage/AssetCacheStorage.cpp)
add_dependencies(AssetDownloadTest AssetI18nSchema)
target_compile_definitions(AssetDownloadTest PRIVATE SIMULATOR)
target_include_directories(AssetDownloadTest PRIVATE stubs ${ASSET_I18N}
  ${REPO_ROOT}/lib/I18n ${REPO_ROOT}/lib/HalFlashPartition ${REPO_ROOT}/lib/AssetCacheStorage ${REPO_ROOT}/src/network ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/lib/AssetCatalog ${REPO_ROOT}/lib/LanguageCache ${REPO_ROOT}/lib/Memory)
target_compile_features(AssetDownloadTest PRIVATE cxx_std_20)
target_compile_options(AssetDownloadTest PRIVATE -Wall -Wextra -Werror -Wno-deprecated-declarations -UNDEBUG)
target_link_libraries(AssetDownloadTest PRIVATE OpenSSL::Crypto)
add_test(NAME AssetDownloadTest COMMAND AssetDownloadTest)

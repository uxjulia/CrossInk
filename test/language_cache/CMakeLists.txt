cmake_minimum_required(VERSION 3.16)
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
  project(language_cache_tests LANGUAGES CXX)
  set(REPO_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../..")
  enable_testing()
endif()
add_executable(LanguageCacheTest LanguageCacheTest.cpp ${REPO_ROOT}/lib/LanguageCache/LanguageCache.cpp)
target_include_directories(LanguageCacheTest PRIVATE ${REPO_ROOT}/lib/LanguageCache ${REPO_ROOT}/lib/Memory)
target_compile_features(LanguageCacheTest PRIVATE cxx_std_20)
target_compile_options(LanguageCacheTest PRIVATE -Wall -Wextra -Werror)
add_test(NAME LanguageCacheTest COMMAND LanguageCacheTest)

add_executable(FlashReadOnlyTest FlashReadOnlyTest.cpp
  ${REPO_ROOT}/lib/HalFlashPartition/HalFlashPartition.cpp)
target_include_directories(FlashReadOnlyTest PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/HalFlashPartition)
target_compile_definitions(FlashReadOnlyTest PRIVATE SIMULATOR)
target_compile_features(FlashReadOnlyTest PRIVATE cxx_std_20)
target_compile_options(FlashReadOnlyTest PRIVATE -Wall -Wextra -Werror -UNDEBUG)
add_test(NAME FlashReadOnlyTest COMMAND FlashReadOnlyTest)

find_package(Python3 COMPONENTS Interpreter REQUIRED)
set(LANGUAGE_GENERATED "${CMAKE_CURRENT_BINARY_DIR}/generated")
file(MAKE_DIRECTORY "${LANGUAGE_GENERATED}")
file(GLOB_RECURSE LANGUAGE_USAGE CONFIGURE_DEPENDS ${REPO_ROOT}/src/*.cpp ${REPO_ROOT}/src/*.h
  ${REPO_ROOT}/lib/*.cpp ${REPO_ROOT}/lib/*.h)
list(FILTER LANGUAGE_USAGE EXCLUDE REGEX "I18n(Keys|Strings)\\.(cpp|h)$")
add_custom_command(OUTPUT ${LANGUAGE_GENERATED}/I18nKeys.h ${LANGUAGE_GENERATED}/I18nStrings.h
  ${LANGUAGE_GENERATED}/I18nStrings.cpp
  COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/scripts/gen_i18n.py
  ${REPO_ROOT}/lib/I18n/translations ${LANGUAGE_GENERATED} --strip-unused
  --src-dirs ${REPO_ROOT}/src ${REPO_ROOT}/lib
  DEPENDS ${REPO_ROOT}/scripts/gen_i18n.py ${REPO_ROOT}/lib/I18n/translations/english.yaml
    ${REPO_ROOT}/lib/I18n/languages.json ${LANGUAGE_USAGE}
  WORKING_DIRECTORY ${REPO_ROOT} VERBATIM)
add_custom_target(LanguageTestSchema DEPENDS ${LANGUAGE_GENERATED}/I18nKeys.h
  ${LANGUAGE_GENERATED}/I18nStrings.h ${LANGUAGE_GENERATED}/I18nStrings.cpp)
add_executable(I18nIntegrationTool I18nIntegrationTool.cpp
  ${REPO_ROOT}/lib/I18n/I18n.cpp ${REPO_ROOT}/lib/AssetCacheStorage/AssetCacheStorage.cpp ${LANGUAGE_GENERATED}/I18nStrings.cpp
  ${REPO_ROOT}/lib/LanguageCache/LanguageCache.cpp
  ${REPO_ROOT}/lib/HalFlashPartition/HalFlashPartition.cpp)
add_dependencies(I18nIntegrationTool LanguageTestSchema)
target_include_directories(I18nIntegrationTool PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${LANGUAGE_GENERATED} ${REPO_ROOT}/lib/I18n ${REPO_ROOT}/lib/LanguageCache
  ${REPO_ROOT}/lib/HalFlashPartition ${REPO_ROOT}/lib/AssetCacheStorage ${REPO_ROOT}/lib/Memory)
target_compile_definitions(I18nIntegrationTool PRIVATE SIMULATOR CROSSINK_ASSET_CACHE_TEST_PSRAM CROSSINK_LANGUAGE_BENCHMARK CROSSINK_LANGUAGE_BACKEND_PSRAM)
target_compile_features(I18nIntegrationTool PRIVATE cxx_std_20)
target_compile_options(I18nIntegrationTool PRIVATE -Wall -Wextra -Werror)
add_test(NAME I18nIntegrationTest COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/integration_test.py
  $<TARGET_FILE:I18nIntegrationTool> ${REPO_ROOT})

add_executable(LanguageSettingsAtomicTest AtomicFileTest.cpp ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp)
target_include_directories(LanguageSettingsAtomicTest PRIVATE ${REPO_ROOT}/lib/Serialization)
target_compile_features(LanguageSettingsAtomicTest PRIVATE cxx_std_20)
target_compile_options(LanguageSettingsAtomicTest PRIVATE -Wall -Wextra -Werror)
add_test(NAME LanguageSettingsAtomicTest COMMAND LanguageSettingsAtomicTest)
add_test(NAME LanguageGeneratorTest COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/generator_test.py ${REPO_ROOT})
add_test(NAME LanguageBuiltinTest COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/builtin_test.py ${REPO_ROOT})
add_test(NAME I18nTranslationTest COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/test/scripts/test_i18n.py)

add_executable(LanguageAllocationTest LanguageAllocationTest.cpp ${REPO_ROOT}/lib/LanguageCache/LanguageCache.cpp)
add_dependencies(LanguageAllocationTest LanguageTestSchema)
target_include_directories(LanguageAllocationTest PRIVATE ${REPO_ROOT}/test/memory_policy/stubs
  ${LANGUAGE_GENERATED} ${REPO_ROOT}/lib/I18n ${REPO_ROOT}/lib/AssetCacheStorage ${REPO_ROOT}/lib/HalFlashPartition
  ${REPO_ROOT}/lib/LanguageCache ${REPO_ROOT}/lib/Memory)
target_compile_definitions(LanguageAllocationTest PRIVATE ARDUINO_ARCH_ESP32 CROSSINK_MEMORY_TEST)
target_compile_features(LanguageAllocationTest PRIVATE cxx_std_20)
target_compile_options(LanguageAllocationTest PRIVATE -Wall -Wextra -Werror -fno-exceptions -UNDEBUG)
add_test(NAME LanguageAllocationTest COMMAND LanguageAllocationTest)

add_test(NAME LanguageTemplateTest COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/language_template_test.py ${REPO_ROOT})

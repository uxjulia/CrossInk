add_executable(ClippingStoreTest ClippingStoreTest.cpp
  ${REPO_ROOT}/src/ClippingStore.cpp
  ${REPO_ROOT}/src/sync/SyncFiles.cpp
  ${REPO_ROOT}/src/clippings/ClippingIdentity.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c)
target_include_directories(ClippingStoreTest PRIVATE stubs ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/uzlib/src)
target_compile_options(ClippingStoreTest PRIVATE -UNDEBUG -fno-exceptions)
add_test(NAME ClippingStoreDataSafety COMMAND ClippingStoreTest)

target_compile_definitions(ClippingStoreTest PRIVATE SIMULATOR)
if(NOT APPLE)
  find_package(OpenSSL REQUIRED)
  target_link_libraries(ClippingStoreTest PRIVATE OpenSSL::Crypto)
endif()

add_executable(ClippingSyncTest ClippingSyncTest.cpp ArrayHeap.cpp
  ${REPO_ROOT}/src/ClippingStore.cpp
  ${REPO_ROOT}/src/sync/SyncFiles.cpp
  ${REPO_ROOT}/src/network/ClippingsUpload.cpp
  ${REPO_ROOT}/src/clippings/ClippingIdentity.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c)
target_include_directories(ClippingSyncTest PRIVATE stubs ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Memory ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/Utf8 ${REPO_ROOT}/lib/uzlib/src)
target_compile_definitions(ClippingSyncTest PRIVATE SIMULATOR)
target_link_libraries(ClippingSyncTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
if(NOT APPLE)
  find_package(OpenSSL REQUIRED)
  target_link_libraries(ClippingSyncTest PRIVATE OpenSSL::Crypto)
endif()
gtest_discover_tests(ClippingSyncTest)

# Compile the real non-simulator download path with chunked transport and SD mocks.
add_executable(ClippingDownloadTransportTest ClippingDownloadTransportTest.cpp
  ${REPO_ROOT}/src/network/StatsUploadClient.cpp)
target_include_directories(ClippingDownloadTransportTest PRIVATE transport_stubs stubs
  ${REPO_ROOT}/src/network ${REPO_ROOT}/lib/Memory)
target_link_libraries(ClippingDownloadTransportTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(ClippingDownloadTransportTest)

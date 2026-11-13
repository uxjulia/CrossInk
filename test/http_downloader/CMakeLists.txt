find_package(EXPAT REQUIRED)
add_executable(HttpDownloaderTest
  HttpDownloaderTest.cpp
  ${REPO_ROOT}/lib/OpdsParser/OpdsParser.cpp
  ${REPO_ROOT}/src/network/HttpDownloader.cpp
  ${REPO_ROOT}/src/util/UrlUtils.cpp
)
target_include_directories(HttpDownloaderTest PRIVATE
  stubs
  ${REPO_ROOT}/test/opds_parser/stubs
  ${REPO_ROOT}/lib/OpdsParser
  ${REPO_ROOT}/lib/XmlParserUtils
  ${REPO_ROOT}/src
  ${REPO_ROOT}/src/network ${REPO_ROOT}/lib/FsHelpers
  ${REPO_ROOT}/src/util
)
target_compile_definitions(HttpDownloaderTest PRIVATE SIMULATOR FREEINK_NET_WOLFSSL=1)
target_link_libraries(HttpDownloaderTest PRIVATE crosspoint_test_common EXPAT::EXPAT GTest::gtest_main)
gtest_discover_tests(HttpDownloaderTest)

# Compile the actual device-only free-space preflight on the host as well.
# The existing simulator target intentionally excludes this code path.
add_executable(HttpDownloaderDevicePathTest ${REPO_ROOT}/lib/OpdsParser/OpdsParser.cpp HttpDownloaderTest.cpp ${REPO_ROOT}/src/network/HttpDownloader.cpp ${REPO_ROOT}/src/util/UrlUtils.cpp)
target_include_directories(HttpDownloaderDevicePathTest PRIVATE stubs ${REPO_ROOT}/test/opds_parser/stubs ${REPO_ROOT}/lib/OpdsParser ${REPO_ROOT}/lib/XmlParserUtils ${REPO_ROOT}/src ${REPO_ROOT}/src/network ${REPO_ROOT}/lib/FsHelpers ${REPO_ROOT}/src/util)
target_compile_definitions(HttpDownloaderDevicePathTest PRIVATE FREEINK_NET_WOLFSSL=1)
target_link_libraries(HttpDownloaderDevicePathTest PRIVATE EXPAT::EXPAT crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(HttpDownloaderDevicePathTest TEST_SUFFIX .DevicePath)

foreach(target HttpDownloaderTest HttpDownloaderDevicePathTest)
  target_sources(${target} PRIVATE ${REPO_ROOT}/src/network/OpdsCatalogClient.cpp
    ${REPO_ROOT}/lib/OpdsParser/OpdsAuthChallenge.cpp
    ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/src/StreamingJsonParser.cpp)
  target_include_directories(${target} PRIVATE ${REPO_ROOT}/freeink-sdk/libs/network/JsonSax/include)
endforeach()
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME HttpSdkSessionRegression COMMAND ${Python3_EXECUTABLE} ${REPO_ROOT}/test/scripts/test_http_sdk_session.py)

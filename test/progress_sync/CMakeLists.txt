# Exercise the real device transport branch, not the simulator HTTP adapter.
foreach(target IN ITEMS ProgressSyncTest ProgressSyncSpRsaTest)
  add_executable(${target} ProgressSyncTest.cpp
    ${REPO_ROOT}/lib/KOReaderSync/KOReaderSyncClient.cpp ${REPO_ROOT}/lib/SyncLog/SyncLog.cpp)
  target_include_directories(${target} PRIVATE stubs
    ${REPO_ROOT}/lib/KOReaderSync ${REPO_ROOT}/lib/SyncLog
    ${REPO_ROOT}/test/clipping_sync/stubs ${REPO_ROOT}/lib/HttpHeader
    ${REPO_ROOT}/lib/Serialization ${REPO_ROOT}/lib/Memory)
  target_link_libraries(${target} PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
  if(target STREQUAL "ProgressSyncSpRsaTest")
    target_compile_definitions(${target} PRIVATE WOLFSSL_HAVE_SP_RSA)
    gtest_discover_tests(${target} TEST_PREFIX "SpRsa.")
  else()
    gtest_discover_tests(${target})
  endif()
endforeach()

add_executable(PreparedProgressTest PreparedProgressTest.cpp
  ${REPO_ROOT}/src/sync/PreparedProgress.cpp ${REPO_ROOT}/src/sync/SyncFiles.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c)
target_include_directories(PreparedProgressTest PRIVATE ${REPO_ROOT}/test/clipping_sync/stubs
  ${REPO_ROOT}/src ${REPO_ROOT}/lib/KOReaderSync ${REPO_ROOT}/lib/uzlib/src)
target_link_libraries(PreparedProgressTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PreparedProgressTest)

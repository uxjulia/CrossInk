add_executable(OpdsServerStoreTest
  OpdsServerStoreTest.cpp
  ${REPO_ROOT}/src/OpdsServerStore.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
)
target_include_directories(OpdsServerStoreTest PRIVATE
  stubs
  ${REPO_ROOT}/include
  ${REPO_ROOT}/test/persistable_store_atomic/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/Epub
)
target_link_libraries(OpdsServerStoreTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(OpdsServerStoreTest)

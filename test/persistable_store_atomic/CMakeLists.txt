add_executable(PersistableStoreAtomicTest
  PersistableStoreAtomicTest.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
)

target_include_directories(PersistableStoreAtomicTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Serialization
)

target_link_libraries(PersistableStoreAtomicTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
  ArduinoJson
)

gtest_discover_tests(PersistableStoreAtomicTest)

add_executable(CrossPointStateTest
  CrossPointStateTest.cpp
  ${REPO_ROOT}/src/CrossPointState.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
  ${REPO_ROOT}/lib/Serialization/AtomicFile.cpp
  ${REPO_ROOT}/lib/uzlib/src/uzlib_checksums.c
)
target_include_directories(CrossPointStateTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/lib/uzlib/src
)
target_link_libraries(CrossPointStateTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(CrossPointStateTest)

add_executable(PersistableStoreAtomicTest
  PersistableStoreAtomicTest.cpp
  ${REPO_ROOT}/lib/Serialization/PersistableStore.cpp
)

target_include_directories(PersistableStoreAtomicTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/Serialization
  ${REPO_ROOT}/.pio/libdeps/simulator/ArduinoJson/src
)

target_link_libraries(PersistableStoreAtomicTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(PersistableStoreAtomicTest)

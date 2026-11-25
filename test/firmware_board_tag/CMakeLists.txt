add_executable(FirmwareBoardTagTest
  FirmwareBoardTagTest.cpp
  ${REPO_ROOT}/src/network/FirmwareBoardTag.cpp
)

target_include_directories(FirmwareBoardTagTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/network
)

target_compile_definitions(FirmwareBoardTagTest PRIVATE
  FREEINK_DEVICE_STICKY=1
)

target_link_libraries(FirmwareBoardTagTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FirmwareBoardTagTest)

add_executable(PiccoFirmwareBoardTagTest
  FirmwareBoardTagTest.cpp
  ${REPO_ROOT}/src/network/FirmwareBoardTag.cpp
)
target_include_directories(PiccoFirmwareBoardTagTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/network
)
target_compile_definitions(PiccoFirmwareBoardTagTest PRIVATE FREEINK_DEVICE_PICCO=1)
target_link_libraries(PiccoFirmwareBoardTagTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PiccoFirmwareBoardTagTest TEST_PREFIX "Picco.")

add_executable(EegoA4FirmwareBoardTagTest
  FirmwareBoardTagTest.cpp
  ${REPO_ROOT}/src/network/FirmwareBoardTag.cpp
)
target_include_directories(EegoA4FirmwareBoardTagTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/src/network
)
target_compile_definitions(EegoA4FirmwareBoardTagTest PRIVATE FREEINK_DEVICE_EEGO_A4=1)
target_link_libraries(EegoA4FirmwareBoardTagTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(EegoA4FirmwareBoardTagTest TEST_PREFIX "EegoA4.")

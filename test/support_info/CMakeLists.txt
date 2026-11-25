add_executable(SupportInfoTest SupportInfoTest.cpp ${REPO_ROOT}/lib/SupportInfo/SupportInfo.cpp)
target_include_directories(SupportInfoTest PRIVATE ${REPO_ROOT}/lib/SupportInfo ${REPO_ROOT}/lib/DeviceInfo)
target_link_libraries(SupportInfoTest PRIVATE crosspoint_test_common GTest::gtest_main ArduinoJson)
gtest_discover_tests(SupportInfoTest)

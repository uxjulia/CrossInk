add_executable(PngToneMapperTest PngToneMapperTest.cpp
  ${REPO_ROOT}/freeink-sdk/libs/assets/ImageToneMapper/src/ImageToneMapper.cpp)
target_include_directories(PngToneMapperTest PRIVATE
  ${REPO_ROOT}/freeink-sdk/libs/assets/ImageToneMapper/include)
target_link_libraries(PngToneMapperTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(PngToneMapperTest)

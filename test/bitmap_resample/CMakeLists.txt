add_executable(BitmapResampleTest
  BitmapResampleTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/Bitmap.cpp
  ${REPO_ROOT}/lib/GfxRenderer/BitmapHelpers.cpp
)

target_include_directories(BitmapResampleTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib
  ${REPO_ROOT}/lib/GfxRenderer
)

target_link_libraries(BitmapResampleTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(BitmapResampleTest)

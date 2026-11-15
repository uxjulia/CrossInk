add_executable(FontCacheManagerTest
  FontCacheManagerTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp
)

target_include_directories(FontCacheManagerTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs
  ${REPO_ROOT}/lib/GfxRenderer
  ${REPO_ROOT}/lib/Utf8
)

target_link_libraries(FontCacheManagerTest PRIVATE
  crosspoint_test_common
  GTest::gtest_main
)

gtest_discover_tests(FontCacheManagerTest)

add_executable(IdleScalablePrewarmTest
  IdleScalablePrewarmTest.cpp
  ${REPO_ROOT}/lib/GfxRenderer/FontCacheManager.cpp
  ${REPO_ROOT}/lib/Utf8/Utf8.cpp)
target_compile_definitions(IdleScalablePrewarmTest PRIVATE CROSSINK_SCALABLE_FONTS=1)
target_include_directories(IdleScalablePrewarmTest PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/stubs ${REPO_ROOT}/lib/GfxRenderer ${REPO_ROOT}/lib/Utf8)
target_link_libraries(IdleScalablePrewarmTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(IdleScalablePrewarmTest)

add_executable(AssetCompanionIndexTest
  AssetCompanionIndexTest.cpp
  ${REPO_ROOT}/lib/AssetCompanionIndex/AssetCompanionIndex.cpp
)
target_include_directories(AssetCompanionIndexTest PRIVATE
  ${REPO_ROOT}/lib/AssetCompanionIndex
  ${REPO_ROOT}/lib/AssetCatalog
)
target_link_libraries(AssetCompanionIndexTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(AssetCompanionIndexTest)

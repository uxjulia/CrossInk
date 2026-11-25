add_executable(AssetCatalogTest
  AssetCatalogTest.cpp
  ${REPO_ROOT}/lib/AssetCatalog/AssetCatalog.cpp
)
target_include_directories(AssetCatalogTest PRIVATE ${REPO_ROOT}/lib/AssetCatalog)
target_compile_definitions(AssetCatalogTest PRIVATE ASSET_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")
target_link_libraries(AssetCatalogTest PRIVATE crosspoint_test_common GTest::gtest_main)
gtest_discover_tests(AssetCatalogTest)

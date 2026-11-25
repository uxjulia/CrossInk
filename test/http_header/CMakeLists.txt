add_executable(HttpHeaderTest HttpHeaderTest.cpp ${REPO_ROOT}/lib/HttpHeader/HttpHeader.cpp)
target_include_directories(HttpHeaderTest PRIVATE ${REPO_ROOT}/lib/HttpHeader)
target_link_libraries(HttpHeaderTest PRIVATE GTest::gtest_main)
gtest_discover_tests(HttpHeaderTest)

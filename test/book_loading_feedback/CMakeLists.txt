cmake_minimum_required(VERSION 3.16)
project(BookLoadingFeedback LANGUAGES CXX)
enable_testing()
add_executable(book_loading_feedback BookLoadingFeedbackTest.cpp
  ../../src/activities/reader/BookLoadingFeedback.cpp)
target_include_directories(book_loading_feedback PRIVATE stubs ../../src/activities/reader)
target_compile_features(book_loading_feedback PRIVATE cxx_std_17)
target_compile_options(book_loading_feedback PRIVATE -UNDEBUG)
add_test(NAME book_loading_feedback COMMAND book_loading_feedback)

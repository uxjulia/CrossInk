add_test(NAME EegoFrontlightVariantRegression
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_frontlight.py
    --cxx ${CMAKE_CXX_COMPILER})

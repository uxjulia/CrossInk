add_test(NAME EegoUidRegression
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_uid.py
    --cxx ${CMAKE_CXX_COMPILER})

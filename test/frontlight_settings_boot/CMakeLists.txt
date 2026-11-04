FetchContent_GetProperties(arduinojson)
add_test(NAME FrontlightSettingsBootRegression
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/test_boot.py
    --cxx ${CMAKE_CXX_COMPILER} --arduinojson ${arduinojson_SOURCE_DIR}/src)

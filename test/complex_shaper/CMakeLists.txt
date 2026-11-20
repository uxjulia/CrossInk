foreach(target IN ITEMS ComplexShaperTest ComplexShaperPsramTest)
  add_executable(${target}
    ComplexShaperTest.cpp
    ${REPO_ROOT}/lib/EpdFont/ComplexShaper.cpp
    ${REPO_ROOT}/lib/Utf8/Utf8.cpp
    ${OT_SHAPER_SOURCES}
  )

  target_include_directories(${target} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/stubs
    ${REPO_ROOT}/lib/EpdFont
    ${OT_SHAPER_INCLUDE_DIR}
    ${REPO_ROOT}/lib/Utf8
  )

  target_compile_definitions(${target} PRIVATE
    SHAPING_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
  )

  target_link_libraries(${target} PRIVATE
    crosspoint_test_common
    GTest::gtest_main
  )

  if(target STREQUAL ComplexShaperPsramTest)
    target_compile_definitions(${target} PRIVATE CROSSINK_SCALABLE_FONTS=1)
    gtest_discover_tests(${target} TEST_PREFIX "S3.")
  else()
    gtest_discover_tests(${target})
  endif()
endforeach()

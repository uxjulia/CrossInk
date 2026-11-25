foreach(device X4 X4CLASSIC X3 X4PRO STICKY)
  set(target ButtonNavigation${device}Test)
  add_executable(${target}
    ButtonNavigationTest.cpp
    ${REPO_ROOT}/src/util/ButtonNavigator.cpp
    ${REPO_ROOT}/freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp
  )
  target_include_directories(${target} PRIVATE
    stubs ${REPO_ROOT}/test/input_wake/stubs ${REPO_ROOT}/src
    ${REPO_ROOT}/freeink-sdk/libs/hardware/InputManager/include
    ${REPO_ROOT}/freeink-sdk/libs/hardware/BoardConfig/include
  )
  target_compile_definitions(${target} PRIVATE FREEINK_CAP_TOUCH=0 FREEINK_DEVICE_${device}=1)
  target_compile_options(${target} PRIVATE -UNDEBUG -Wno-unused-parameter)
  target_link_libraries(${target} PRIVATE crosspoint_test_common)
  foreach(scenario next previous explicit-buttons press-only hold release-navigation release-hold custom-repeat timer-wrap repeat-at-zero release-at-zero slow-callback rapid-taps)
    add_test(NAME ${target}.${scenario} COMMAND ${target} ${scenario})
  endforeach()
endforeach()

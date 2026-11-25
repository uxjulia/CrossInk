foreach(device X4 X4CLASSIC X3 X4PRO STICKY)
  set(target ButtonCapture${device}Test)
  add_executable(${target}
    ButtonCaptureTest.cpp
    ${REPO_ROOT}/src/util/ButtonNavigator.cpp
    ${REPO_ROOT}/lib/hal/InputWake.cpp
    ${REPO_ROOT}/freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp
  )
  target_include_directories(${target} PRIVATE
    stubs ${REPO_ROOT}/test/input_wake/stubs ${REPO_ROOT}/src ${REPO_ROOT}/lib/hal
    ${REPO_ROOT}/freeink-sdk/libs/hardware/InputManager/include
    ${REPO_ROOT}/freeink-sdk/libs/hardware/BoardConfig/include
  )
  target_compile_definitions(${target} PRIVATE ARDUINO_ARCH_ESP32=1 FREEINK_CAP_TOUCH=0 FREEINK_DEVICE_${device}=1)
  target_compile_options(${target} PRIVATE -UNDEBUG -Wno-unused-parameter)
  target_link_libraries(${target} PRIVATE crosspoint_test_common)
  foreach(scenario regular bounce quick-taps burst clock-wrap power-hold long-side startup-held task-failure stop overflow noise chord chord-gap slow-render unsupported-style)
    add_test(NAME ${target}.${scenario} COMMAND ${target} ${scenario})
  endforeach()
  foreach(phase RANGE 0 9)
    add_test(NAME ${target}.quick-taps-${phase} COMMAND ${target} quick-taps-${phase})
  endforeach()
endforeach()

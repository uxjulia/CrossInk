set(INPUT_WAKE_SOURCES
  InputWakeTest.cpp
  ${REPO_ROOT}/lib/hal/InputWake.cpp
  ${REPO_ROOT}/freeink-sdk/libs/hardware/InputManager/src/InputManager.cpp
)
function(add_input_wake_test target device)
  add_executable(${target} ${INPUT_WAKE_SOURCES})
  target_include_directories(${target} PRIVATE
    stubs ${REPO_ROOT}/lib/hal
    ${REPO_ROOT}/freeink-sdk/libs/hardware/InputManager/include
    ${REPO_ROOT}/freeink-sdk/libs/hardware/BoardConfig/include
  )
  target_compile_definitions(${target} PRIVATE FREEINK_CAP_TOUCH=0 FREEINK_DEVICE_${device}=1 ${ARGN})
  target_compile_options(${target} PRIVATE -UNDEBUG -Wno-unused-parameter)
  target_link_libraries(${target} PRIVATE crosspoint_test_common)
endfunction()
foreach(device X4PRO X4CLASSIC STICKY X4 X3)
  set(target InputWake${device}Test)
  add_input_wake_test(${target} ${device} ARDUINO_ARCH_ESP32=1)
  foreach(scenario normal shared-service signal-failure service-failure partial-attach-failure partial-type-failure touch-attach-failure other-touch-controller toggle timeout-wrap)
    add_test(NAME ${target}.${scenario} COMMAND ${target} ${scenario})
  endforeach()
endforeach()
add_input_wake_test(InputWakeNativeTest X4PRO ESP_PLATFORM=1)
add_test(NAME InputWakeNativeTest COMMAND InputWakeNativeTest native-fallback)
add_input_wake_test(InputWakeEsp32Test X4PRO ARDUINO_ARCH_ESP32=1 CONFIG_IDF_TARGET_ESP32=1)
add_test(NAME InputWakeEsp32Test COMMAND InputWakeEsp32Test original-esp32)

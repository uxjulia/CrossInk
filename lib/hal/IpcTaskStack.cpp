#include <BoardConfig.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#include <cstring>

extern "C" BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t taskCode, const char* name, uint32_t stackDepth,
                                                     void* parameters, UBaseType_t priority, TaskHandle_t* createdTask,
                                                     BaseType_t coreId);

extern "C" BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t taskCode, const char* name, uint32_t stackDepth,
                                                     void* parameters, UBaseType_t priority, TaskHandle_t* createdTask,
                                                     BaseType_t coreId) {
#if FREEINK_MCU_S3
  // Older prebuilt Arduino libraries give IPC tasks only 1 KiB. GPIO interrupt
  // setup runs on these tasks and can overflow during a silent sync restart.
  // Match Espressif's 2 KiB fix without rebuilding X4 Pro's USB components:
  // https://github.com/espressif/esp32-arduino-lib-builder/pull/386
  // Both stacks stay in internal RAM; this costs at most 2 KiB more on S3.
  constexpr uint32_t MIN_IPC_STACK_BYTES = 2048;
  if (stackDepth < MIN_IPC_STACK_BYTES && name && (std::strcmp(name, "ipc0") == 0 || std::strcmp(name, "ipc1") == 0)) {
    stackDepth = MIN_IPC_STACK_BYTES;
  }
#endif
  return __real_xTaskCreatePinnedToCore(taskCode, name, stackDepth, parameters, priority, createdTask, coreId);
}

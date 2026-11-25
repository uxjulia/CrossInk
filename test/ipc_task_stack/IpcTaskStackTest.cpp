#include <freertos/task.h>
#include <gtest/gtest.h>

extern "C" BaseType_t __wrap_xTaskCreatePinnedToCore(TaskFunction_t, const char*, uint32_t, void*, UBaseType_t,
                                                     TaskHandle_t*, BaseType_t);

namespace {
void task(void*) {}
uint32_t capturedStack = 0;
TaskHandle_t handle = nullptr;
int context;
const char* expectedName;
BaseType_t expectedCore;
}  // namespace

extern "C" BaseType_t __real_xTaskCreatePinnedToCore(TaskFunction_t taskCode, const char* name, uint32_t stackDepth,
                                                     void* parameters, UBaseType_t priority, TaskHandle_t* createdTask,
                                                     BaseType_t coreId) {
  EXPECT_EQ(taskCode, task);
  EXPECT_EQ(name, expectedName);
  EXPECT_EQ(parameters, &context);
  EXPECT_EQ(priority, 24u);
  EXPECT_EQ(createdTask, &handle);
  EXPECT_EQ(coreId, expectedCore);
  capturedStack = stackDepth;
  *createdTask = &context;
  return -7;  // Propagate failures as well as successful task handles.
}

TEST(IpcTaskStack, PreservesTaskArgumentsAndResult) {
  struct Case {
    const char* name;
    uint32_t requested;
    uint32_t s3Expected;
  };
  const Case cases[] = {{"ipc0", 1024, 2048},  {"ipc1", 1024, 2048},     {"ipc0", 1280, 2048},
                        {"ipc1", 1280, 2048},  {"ipc0", 2048, 2048},     {"ipc1", 2048, 2048},
                        {"ipc0", 4096, 4096},  {"ipc1", 4096, 4096},     {"ipc", 1024, 1024},
                        {"ipc10", 1024, 1024}, {"loopTask", 1024, 1024}, {"ActivityManagerRender", 4096, 4096},
                        {"", 1024, 1024},      {nullptr, 1024, 1024}};
  for (const auto& testCase : cases) {
    for (const BaseType_t core : {0, 1, -1}) {
      expectedName = testCase.name;
      expectedCore = core;
      handle = nullptr;
      EXPECT_EQ(__wrap_xTaskCreatePinnedToCore(task, testCase.name, testCase.requested, &context, 24, &handle, core),
                -7);
      EXPECT_EQ(handle, &context);
#if FREEINK_MCU_S3
      const uint32_t expectedStack = testCase.s3Expected;
#else
      const uint32_t expectedStack = testCase.requested;
#endif
      EXPECT_EQ(capturedStack, expectedStack)
          << (testCase.name ? testCase.name : "(null)") << " depth=" << testCase.requested;
    }
  }
}

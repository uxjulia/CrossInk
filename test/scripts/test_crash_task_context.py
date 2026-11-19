"""Exercise production RISC-V panic capture and saved-report output on the host."""

from pathlib import Path
import subprocess
import tempfile


ROOT = Path(__file__).resolve().parents[2]
SOURCE = (ROOT / "lib/hal/HalSystem.cpp").read_text()


def function(signature):
    start = SOURCE.index(signature)
    return SOURCE[start:SOURCE.index("\n}", start) + 2]


start = SOURCE.index("struct RiscvPanicRegisters {")
registers = SOURCE[start:SOURCE.index("\n};", start) + 3]

harness = r'''
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include "HalSystem.h"
#define CONFIG_IDF_TARGET_ARCH_RISCV 1
#define CONFIG_IDF_TARGET_ARCH_XTENSA 0
#define IRAM_ATTR
#define portNUM_PROCESSORS 1
#define CROSSINK_FIRMWARE_DEVICE_TYPE "x3-x4"
constexpr uint32_t PANIC_CAPTURE_MAGIC = 0x50414e49;
constexpr size_t MAX_PANIC_STACK_DEPTH = 32, MAX_PANIC_BACKTRACE_DEPTH = 32;
char panicMessage[256] = {};
HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH] = {};
uint32_t panicBacktrace[MAX_PANIC_BACKTRACE_DEPTH] = {};
size_t panicBacktraceDepth = 0;
uint32_t panicCaptureMarker = 0;
struct esp_cpu_frame_t {
    uint32_t mepc, ra, sp, s0, mcause, mtval, mstatus;
};
struct Task { char name[64]; } task;
using TaskHandle_t = Task*;
TaskHandle_t currentTask = &task;
const char* currentName = task.name;
size_t readableNameBytes = sizeof(task.name);
int taskLookups = 0, nameLookups = 0, boundLookups = 0;
uint32_t currentMin = 0x3fcb9420, currentMax = 0x3fcbb420;
bool esp_ptr_in_dram(const void* pointer) {
    const auto address = reinterpret_cast<uintptr_t>(pointer);
    const auto nameStart = reinterpret_cast<uintptr_t>(task.name);
    return pointer == &task || (address >= nameStart && address - nameStart < readableNameBytes);
}
TaskHandle_t xTaskGetCurrentTaskHandleForCore(int core) {
    assert(core == 0);
    ++taskLookups;
    return currentTask;
}
const char* pcTaskGetName(TaskHandle_t handle) {
    assert(handle == &task);
    ++nameLookups;
    return currentName;
}
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
void esp_hw_stack_guard_get_bounds(int core, uint32_t* low, uint32_t* high) {
    assert(core == 0);
    ++boundLookups;
    *low = currentMin;
    *high = currentMax;
}
#endif
namespace AppVersion { const char* version() { return "test"; } }
void esp_app_get_elf_sha256(char* buffer, size_t size) {
    assert(size >= 10);
    std::strcpy(buffer, "fbc1a4038");
}
std::string getLastLogs() { return "retained logs\n"; }
void clearLastLogs() {}
void pauseLogRetention(bool) {}
''' + registers + r'''
RiscvPanicRegisters panicRiscvRegisters = {};
static_assert(sizeof(RiscvPanicRegisters) == 56, "Keep retained C3 diagnostics small");
''' + function("void IRAM_ATTR captureRiscvPanicRegisters(") + r'''
namespace HalSystem {
''' + function("void clearPanic()") + function("std::string getPanicInfo(bool full)") + r'''
}
int main() {
    const esp_cpu_frame_t frame{0x4236f916, 0x4236ab12, 0x3fcb9150, 8, 27, 0x46112e23, 0x1881};
    std::strcpy(task.name, "loopTask");
    captureRiscvPanicRegisters(&frame, 0);
    assert(panicRiscvRegisters.captured == PANIC_CAPTURE_MAGIC);
    assert(panicRiscvRegisters.mepc == frame.mepc && panicRiscvRegisters.ra == frame.ra);
    assert(panicRiscvRegisters.sp == frame.sp && panicRiscvRegisters.fp == frame.s0);
    assert(panicRiscvRegisters.mcause == frame.mcause && panicRiscvRegisters.mtval == frame.mtval);
    assert(panicRiscvRegisters.mstatus == frame.mstatus);
    // The saved report must use the pre-reboot snapshot, not the new boot's task/bounds.
    std::strcpy(task.name, "newBoot");
    currentMin = currentMax = 0;
    std::string report = HalSystem::getPanicInfo(true);
    assert(report.find("Detected in task: loopTask") != std::string::npos);
    assert(report.find("newBoot") == std::string::npos);
    assert(report.find("MEPC (faulting instruction): 0x4236F916") != std::string::npos);
    assert(report.find("SP (stack pointer): 0x3FCB9150") != std::string::npos);
    assert(report.find("MCAUSE: 0x0000001B") != std::string::npos);
    assert(report.find("retained logs") != std::string::npos);
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    assert(boundLookups == 1);
    // Preserve bounds even when the faulting SP is below the lower limit.
    assert(report.find("Stack bounds (hardware guard): 0x3FCB9420 - 0x3FCBB420") != std::string::npos);
#else
    assert(boundLookups == 0);
    assert(report.find("Stack bounds (hardware guard): (unavailable)") != std::string::npos);
#endif
    // No task exists during very early startup; no stale name survives.
    currentTask = nullptr;
    captureRiscvPanicRegisters(&frame, 0);
    assert(HalSystem::getPanicInfo(true).find("Detected in task: (unavailable)") != std::string::npos);
    currentTask = reinterpret_cast<TaskHandle_t>(1);
    const int namesBefore = nameLookups;
    captureRiscvPanicRegisters(&frame, 0);
    assert(nameLookups == namesBefore);
    currentTask = &task;
    for (const char* name : {static_cast<const char*>(nullptr), reinterpret_cast<const char*>(1)}) {
        currentName = name;
        captureRiscvPanicRegisters(&frame, 0);
        assert(panicRiscvRegisters.taskName[0] == '\0');
    }
    currentName = task.name;
    // A non-terminated task name stays bounded and explicitly terminated.
    std::memset(task.name, 'X', sizeof(task.name));
    captureRiscvPanicRegisters(&frame, 0);
    assert(std::strlen(panicRiscvRegisters.taskName) == sizeof(panicRiscvRegisters.taskName) - 1);
    readableNameBytes = 4;
    captureRiscvPanicRegisters(&frame, 0);
    assert(std::strcmp(panicRiscvRegisters.taskName, "XXXX") == 0);
    // Invalid core numbers must not reach task APIs or hardware registers.
    const int tasksBefore = taskLookups, boundsBefore = boundLookups;
    for (int core : {-1, portNUM_PROCESSORS}) {
        captureRiscvPanicRegisters(&frame, core);
        assert(panicRiscvRegisters.taskName[0] == '\0');
        assert(panicRiscvRegisters.stackMin == 0 && panicRiscvRegisters.stackMax == 0);
    }
    assert(taskLookups == tasksBefore && boundLookups == boundsBefore);
    readableNameBytes = sizeof(task.name);
    for (const auto bounds : {std::pair{0u, 0u}, std::pair{0x2000u, 0x1000u}}) {
        currentMin = bounds.first;
        currentMax = bounds.second;
        captureRiscvPanicRegisters(&frame, 0);
        assert(HalSystem::getPanicInfo(true).find("Stack bounds (hardware guard): (unavailable)") != std::string::npos);
    }
    HalSystem::clearPanic();
    assert(panicRiscvRegisters.captured == 0);
    report = HalSystem::getPanicInfo(true);
    assert(report.find("Detected in task:") == std::string::npos);
    assert(report.find("Stack bounds") == std::string::npos);
}
'''

with tempfile.TemporaryDirectory(prefix="crash-task-context-") as directory:
    path = Path(directory)
    (path / "test.cpp").write_text(harness)
    for guard in (0, 1):
        subprocess.run([
            "c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
            "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
            f"-DCONFIG_ESP_SYSTEM_HW_STACK_GUARD={guard}",
            "-I", str(ROOT / "lib/hal"), str(path / "test.cpp"), "-o", str(path / "test"),
        ], check=True)
        subprocess.run([str(path / "test")], check=True)
print("Crash task capture/report regression passed with stack guard enabled and disabled (ASan/UBSan)")

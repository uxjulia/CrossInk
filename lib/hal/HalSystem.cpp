#include "HalSystem.h"

#include <string>

#include "AppVersion.h"
#include "Arduino.h"
#include "HalStorage.h"
#include "Logging.h"
#include "esp_app_desc.h"
#include "esp_debug_helpers.h"
#include "esp_private/esp_cpu_internal.h"
#include "esp_private/esp_system_attr.h"
#include "esp_private/panic_internal.h"

#if CONFIG_IDF_TARGET_ARCH_RISCV || CONFIG_IDF_TARGET_ARCH_XTENSA
#include "esp_memory_utils.h"
#include "freertos/task.h"
#endif
#if CONFIG_IDF_TARGET_ARCH_RISCV && CONFIG_ESP_SYSTEM_HW_STACK_GUARD
#include "esp_private/hw_stack_guard.h"
#endif

#if defined(BOARD_HAS_PSRAM) && CONFIG_SPIRAM_BOOT_INIT && !defined(SIMULATOR)
#include "esp32-hal-psram.h"
#include "esp_heap_caps.h"
#include "esp_psram.h"
#endif

#if CONFIG_IDF_TARGET_ARCH_XTENSA
#include "esp_cpu_utils.h"
#include "xtensa/corebits.h"
#endif

#define MAX_PANIC_STACK_DEPTH 32
#define MAX_PANIC_BACKTRACE_DEPTH 32
#define PANIC_CAPTURE_MAGIC 0x50414E49u

RTC_NOINIT_ATTR char panicMessage[256];
RTC_NOINIT_ATTR HalSystem::StackFrame panicStack[MAX_PANIC_STACK_DEPTH];
#if CONFIG_IDF_TARGET_ARCH_RISCV
RTC_NOINIT_ATTR uint32_t panicBacktrace[MAX_PANIC_BACKTRACE_DEPTH];
RTC_NOINIT_ATTR volatile size_t panicBacktraceDepth;
#endif

#if CONFIG_IDF_TARGET_ARCH_RISCV
// Preserve the exception-frame values that identify a C3 panic. Unlike the raw
// stack words below, MEPC is the actual faulting instruction and MTVAL is the
// fault address/value reported by the CPU.
struct RiscvPanicRegisters {
  uint32_t mepc;
  uint32_t ra;
  uint32_t sp;
  uint32_t fp;
  uint32_t mcause;
  uint32_t mtval;
  uint32_t mstatus;
  uint32_t stackMin;
  uint32_t stackMax;
  char taskName[16];
  uint32_t captured;
};
RTC_NOINIT_ATTR RiscvPanicRegisters panicRiscvRegisters;
#endif
#if CONFIG_IDF_TARGET_ARCH_XTENSA
constexpr size_t MAX_PANIC_CORES = 2;
constexpr size_t PANIC_TASK_NAME_BYTES = 16;
// Keep each S3 core's register dump and backtrace separate. ESP-IDF prints the
// primary panic core first, then any other core that has an exception frame.
struct XtensaPanicRegisters {
  uint32_t pc;
  uint32_t a0;
  uint32_t a1;
  uint32_t ps;
  uint32_t exccause;
  uint32_t excvaddr;
  uint32_t captured;
  char taskName[PANIC_TASK_NAME_BYTES];
};
RTC_NOINIT_ATTR XtensaPanicRegisters panicXtensaRegisters[MAX_PANIC_CORES];
RTC_NOINIT_ATTR uint32_t panicXtensaBacktrace[MAX_PANIC_CORES][MAX_PANIC_BACKTRACE_DEPTH];
RTC_NOINIT_ATTR size_t panicXtensaBacktraceDepth[MAX_PANIC_CORES];
RTC_NOINIT_ATTR int32_t panicPrimaryCore;
RTC_NOINIT_ATTR uint32_t panicCoreCaptureCount;
#endif
// RTC_NOINIT is uninitialized on cold boot, so only this exact marker proves a
// panic diagnostic was captured before the reset.
RTC_NOINIT_ATTR volatile uint32_t panicCaptureMarker;

extern "C" {

void __real_panic_abort(const char* message);
void __real_panic_print_backtrace(const void* frame, int core);

static DRAM_ATTR const char PANIC_REASON_UNKNOWN[] = "(unknown panic reason)";

#if CONFIG_IDF_TARGET_ARCH_XTENSA
void IRAM_ATTR resetXtensaPanicCapture() {
  for (size_t core = 0; core < MAX_PANIC_CORES; ++core) {
    panicXtensaRegisters[core].captured = 0;
    panicXtensaBacktraceDepth[core] = 0;
  }
  panicPrimaryCore = -1;
}

void IRAM_ATTR captureXtensaPanicBacktrace(const void* frame, const int core) {
  if (core < 0 || static_cast<size_t>(core) >= MAX_PANIC_CORES) return;
  // RTC_NOINIT may contain arbitrary bytes if this is the first boot's panic.
  if (panicCaptureMarker != PANIC_CAPTURE_MAGIC) {
    panicCoreCaptureCount = 0;
    panicMessage[0] = '\0';
  }
  if (panicCoreCaptureCount == 0) {
    resetXtensaPanicCapture();
    panicPrimaryCore = core;
  }
  ++panicCoreCaptureCount;

  const auto* exceptionFrame = static_cast<const XtExcFrame*>(frame);
  auto& registers = panicXtensaRegisters[core];
  registers.pc = exceptionFrame->pc;
  registers.a0 = exceptionFrame->a0;
  registers.a1 = exceptionFrame->a1;
  registers.ps = exceptionFrame->ps;
  registers.exccause = exceptionFrame->exccause;
  registers.excvaddr = exceptionFrame->excvaddr;
  registers.taskName[0] = '\0';
  const TaskHandle_t task = xTaskGetCurrentTaskHandleForCore(core);
  if (task && esp_ptr_in_dram(task)) {
    const char* name = pcTaskGetName(task);
    if (name && esp_ptr_byte_accessible(name)) {
      size_t i = 0;
      for (; i < sizeof(registers.taskName) - 1 && name[i]; ++i) registers.taskName[i] = name[i];
      registers.taskName[i] = '\0';
    }
  }
  registers.captured = PANIC_CAPTURE_MAGIC;

  esp_backtrace_frame_t backtraceFrame = {
      .pc = static_cast<uint32_t>(exceptionFrame->pc),
      .sp = static_cast<uint32_t>(exceptionFrame->a1),
      .next_pc = static_cast<uint32_t>(exceptionFrame->a0),
      .exc_frame = exceptionFrame,
  };

  size_t depth = 0;
  uint32_t pc = esp_cpu_process_stack_pc(backtraceFrame.pc);
  panicXtensaBacktrace[core][depth++] = pc;

  bool corrupted =
      !esp_stack_ptr_is_sane(backtraceFrame.sp) ||
      (!esp_ptr_executable(reinterpret_cast<const void*>(pc)) && exceptionFrame->exccause != EXCCAUSE_INSTR_PROHIBITED);
  while (depth < MAX_PANIC_BACKTRACE_DEPTH && backtraceFrame.next_pc != 0 && !corrupted) {
    if (!esp_backtrace_get_next_frame(&backtraceFrame)) {
      break;
    }

    pc = esp_cpu_process_stack_pc(backtraceFrame.pc);
    if (esp_ptr_executable(reinterpret_cast<const void*>(pc))) {
      panicXtensaBacktrace[core][depth++] = pc;
    }
  }

  panicXtensaBacktraceDepth[core] = depth;
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;
}
#endif

#if CONFIG_IDF_TARGET_ARCH_RISCV
void IRAM_ATTR captureRiscvPanicRegisters(const void* frame, const int core) {
  const auto* exceptionFrame = static_cast<const esp_cpu_frame_t*>(frame);
  panicRiscvRegisters.mepc = exceptionFrame->mepc;
  panicRiscvRegisters.ra = exceptionFrame->ra;
  panicRiscvRegisters.sp = exceptionFrame->sp;
  panicRiscvRegisters.fp = exceptionFrame->s0;
  panicRiscvRegisters.mcause = exceptionFrame->mcause;
  panicRiscvRegisters.mtval = exceptionFrame->mtval;
  panicRiscvRegisters.mstatus = exceptionFrame->mstatus;
  panicRiscvRegisters.taskName[0] = '\0';
  panicRiscvRegisters.stackMin = 0;
  panicRiscvRegisters.stackMax = 0;
  if (core >= 0 && core < portNUM_PROCESSORS) {
    // Use the same panic-handler APIs as ESP-IDF's serial diagnostics. Do not
    // scan stack watermarks, allocate, or take scheduler locks during a panic.
#if CONFIG_ESP_SYSTEM_HW_STACK_GUARD
    esp_hw_stack_guard_get_bounds(core, &panicRiscvRegisters.stackMin, &panicRiscvRegisters.stackMax);
#endif
    const TaskHandle_t task = xTaskGetCurrentTaskHandleForCore(core);
    if (task && esp_ptr_in_dram(task)) {
      const char* name = pcTaskGetName(task);
      size_t i = 0;
      for (; name && i < sizeof(panicRiscvRegisters.taskName) - 1 && esp_ptr_in_dram(name + i) && name[i]; ++i) {
        panicRiscvRegisters.taskName[i] = name[i];
      }
      panicRiscvRegisters.taskName[i] = '\0';
    }
  }
  panicRiscvRegisters.captured = PANIC_CAPTURE_MAGIC;
}
#endif

void IRAM_ATTR __wrap_panic_abort(const char* message) {
#if CONFIG_IDF_TARGET_ARCH_XTENSA
  if (panicCaptureMarker != PANIC_CAPTURE_MAGIC) {
    panicCoreCaptureCount = 0;
    resetXtensaPanicCapture();
  }
#endif
  if (!message) message = PANIC_REASON_UNKNOWN;
  // IRAM-safe bounded copy (strncpy is not IRAM-safe in panic context)
  int i = 0;
  for (; i < (int)sizeof(panicMessage) - 1 && message[i]; i++) {
    panicMessage[i] = message[i];
  }
  panicMessage[i] = '\0';
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_abort(message);
}

void IRAM_ATTR __wrap_panic_print_backtrace(const void* frame, int core) {
  if (!frame) {
    __real_panic_print_backtrace(frame, core);
    return;
  }

#if CONFIG_IDF_TARGET_ARCH_XTENSA
  captureXtensaPanicBacktrace(frame, core);
  __real_panic_print_backtrace(frame, core);
  return;
#elif !__riscv
  __real_panic_print_backtrace(frame, core);
  return;
#else
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
  panicBacktraceDepth = 0;

  captureRiscvPanicRegisters(frame, core);

  // Copied from components/esp_system/port/arch/riscv/panic_arch.c
  uint32_t sp = (uint32_t)((RvExcFrame*)frame)->sp;
  const int per_line = 8;
  int depth = 0;
  for (int x = 0; x < 1024; x += per_line * sizeof(uint32_t)) {
    uint32_t* spp = (uint32_t*)(sp + x);
    // panic_print_hex(sp + x);
    // panic_print_str(": ");
    panicStack[depth].sp = sp + x;
    for (int y = 0; y < per_line; y++) {
      // panic_print_str("0x");
      // panic_print_hex(spp[y]);
      // panic_print_str(y == per_line - 1 ? "\r\n" : " ");
      panicStack[depth].spp[y] = spp[y];
    }

    depth++;
    if (depth >= MAX_PANIC_STACK_DEPTH) {
      break;
    }
  }
  panicCaptureMarker = PANIC_CAPTURE_MAGIC;

  __real_panic_print_backtrace(frame, core);
#endif
}
}

namespace HalSystem {

void begin() {
#if defined(BOARD_HAS_PSRAM) && CONFIG_SPIRAM_BOOT_INIT && !defined(SIMULATOR)
  // SD firmware updates replace the app but retain the installed bootloader.
  // Older bootloaders do not initialize PSRAM as this Arduino SDK expects.
  if (heap_caps_get_total_size(MALLOC_CAP_SPIRAM) == 0) {
    const esp_err_t initResult = esp_psram_is_initialized() ? ESP_OK : esp_psram_init();
    if (initResult != ESP_OK) {
      LOG_ERR("HAL", "PSRAM initialization failed: %d", int(initResult));
    } else if (!psramInit() || !psramAddToHeap()) {
      LOG_ERR("HAL", "Could not add PSRAM to heap");
    } else {
      LOG_INF("HAL", "Initialized PSRAM for app-only firmware update");
    }
  }
#endif
  // On a panic reboot, preserve diagnostics until checkPanic() has tried to write them to the SD card.
  // Ordinary boots clear any stale retained diagnostics.
  if (!isRebootFromPanic()) {
    clearPanic();
  } else {
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    // The saved per-core data remains available for the report below. A second
    // panic during this boot must start a fresh capture, even if SD writing fails.
    panicCoreCaptureCount = 0;
#endif
    // Panic reboot: preserve logs and panic info, but clamp logHead in case the
    // panic occurred before begin() ever ran (e.g. in a static constructor).
    // If logHead was out of range, logMessages is also garbage — clear it so
    // getLastLogs() does not dump corrupt data into the crash report.
    if (sanitizeLogHead()) {
      clearLastLogs();
    }
  }
}

void checkPanic() {
  if (isRebootFromPanic()) {
    auto panicInfo = getPanicInfo(true);
    // The report now owns the old text; capture logs from this boot again even
    // if the SD card cannot save the report.
    clearLastLogs();
    pauseLogRetention(false);
    auto file = Storage.open("/crash_report.txt", O_WRITE | O_CREAT | O_TRUNC);
    if (file) {
      const size_t written = file.write(panicInfo.c_str(), panicInfo.size());
      file.close();
      if (written == panicInfo.size()) {
        // Keep the crash data for CrashActivity, but mark it consumed so a
        // later watchdog reset cannot be mistaken for this panic.
        panicCaptureMarker = 0;
        LOG_INF("SYS", "Dumped panic info to SD card");
      } else {
        LOG_ERR("SYS", "Failed to write complete crash report (%zu of %zu bytes)", written, panicInfo.size());
      }
    } else {
      LOG_ERR("SYS", "Failed to open crash_report.txt for writing");
    }
  }
}

void clearPanic() {
  panicCaptureMarker = 0;
  panicMessage[0] = '\0';
  for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
    panicStack[i].sp = 0;
  }
#if CONFIG_IDF_TARGET_ARCH_RISCV
  panicBacktraceDepth = 0;
  panicRiscvRegisters.captured = 0;
#endif
#if CONFIG_IDF_TARGET_ARCH_XTENSA
  resetXtensaPanicCapture();
  panicCoreCaptureCount = 0;
#endif
  clearLastLogs();
  pauseLogRetention(false);
}

std::string getPanicInfo(bool full) {
  if (!full) {
    return panicMessage;
  } else {
    std::string info;

    info += std::string("CrossInk version: ") + AppVersion::version();
    info += "\nCrossInk device type: " CROSSINK_FIRMWARE_DEVICE_TYPE;
    char elfSha[65] = {};
    esp_app_get_elf_sha256(elfSha, sizeof(elfSha));
    info += "\nFirmware ELF SHA256: " + std::string(elfSha);
    info += "\n\nPanic reason: ";
    info += panicMessage[0] ? panicMessage : "(not captured; see exception registers)";
    info += "\n\nLast logs:\n" + getLastLogs();
    auto toHex = [](uint32_t value) {
      char buffer[9];
      snprintf(buffer, sizeof(buffer), "%08X", value);
      return std::string(buffer);
    };
#if CONFIG_IDF_TARGET_ARCH_RISCV
    if (panicRiscvRegisters.captured == PANIC_CAPTURE_MAGIC) {
      info += "\n\nDetected in task: ";
      info += panicRiscvRegisters.taskName[0] ? panicRiscvRegisters.taskName : "(unavailable)";
      info += "\nStack bounds (hardware guard): ";
      if (panicRiscvRegisters.stackMin < panicRiscvRegisters.stackMax) {
        info += "0x" + toHex(panicRiscvRegisters.stackMin) + " - 0x" + toHex(panicRiscvRegisters.stackMax);
      } else {
        info += "(unavailable)";
      }
      info += "\n\nRISC-V exception registers:\n";
      info += "MEPC (faulting instruction): 0x" + toHex(panicRiscvRegisters.mepc);
      info += "\nRA (caller): 0x" + toHex(panicRiscvRegisters.ra);
      info += "\nSP (stack pointer): 0x" + toHex(panicRiscvRegisters.sp);
      info += "\nS0/FP (frame pointer): 0x" + toHex(panicRiscvRegisters.fp);
      info += "\nMCAUSE: 0x" + toHex(panicRiscvRegisters.mcause);
      info += "\nMTVAL (fault address/value): 0x" + toHex(panicRiscvRegisters.mtval);
      info += "\nMSTATUS: 0x" + toHex(panicRiscvRegisters.mstatus);
    }
#endif
#if CONFIG_IDF_TARGET_ARCH_XTENSA
    if (panicPrimaryCore >= 0 && static_cast<size_t>(panicPrimaryCore) < MAX_PANIC_CORES) {
      info += "\nPrimary panic core: " + std::to_string(panicPrimaryCore);
    }
    for (size_t core = 0; core < MAX_PANIC_CORES; ++core) {
      const auto& registers = panicXtensaRegisters[core];
      if (registers.captured != PANIC_CAPTURE_MAGIC) continue;
      info += "\n\nCore " + std::to_string(core);
      if (static_cast<int32_t>(core) == panicPrimaryCore) info += " (primary)";
      info += " task: ";
      info += registers.taskName[0] ? registers.taskName : "(unavailable)";
      info += "\nPC (instruction): 0x" + toHex(registers.pc);
      info += "\nA0 (return address): 0x" + toHex(registers.a0);
      info += "\nA1 (stack pointer): 0x" + toHex(registers.a1);
      info += "\nPS (processor state): 0x" + toHex(registers.ps);
      info += "\nEXCCAUSE: 0x" + toHex(registers.exccause);
      info += "\nEXCVADDR (fault address): 0x" + toHex(registers.excvaddr);
      const size_t depth =
          panicXtensaBacktraceDepth[core] <= MAX_PANIC_BACKTRACE_DEPTH ? panicXtensaBacktraceDepth[core] : 0;
      if (depth > 0) {
        info += "\nBacktrace:\n";
        for (size_t i = 0; i < depth; ++i) info += "0x" + toHex(panicXtensaBacktrace[core][i]) + "\n";
      }
    }
#endif
    if (panicStack[0].sp != 0) {
      info += "\n\nStack memory:\n";
      for (size_t i = 0; i < MAX_PANIC_STACK_DEPTH; i++) {
        if (panicStack[i].sp == 0) {
          break;
        }
        info += "0x" + toHex(panicStack[i].sp) + ": ";
        for (size_t j = 0; j < 8; j++) {
          info += "0x" + toHex(panicStack[i].spp[j]) + " ";
        }
        info += "\n";
      }
    }

#if CONFIG_IDF_TARGET_ARCH_RISCV
    const size_t backtraceDepth = panicBacktraceDepth <= MAX_PANIC_BACKTRACE_DEPTH ? panicBacktraceDepth : 0;
    if (backtraceDepth > 0) {
      info += "\nStack trace:\n";
      for (size_t i = 0; i < backtraceDepth; i++) {
        info += "0x" + toHex(panicBacktrace[i]) + "\n";
      }
    }
#endif

    return info;
  }
}

bool isRebootFromPanic() {
  const auto resetReason = esp_reset_reason();
  if (resetReason == ESP_RST_PANIC || resetReason == ESP_RST_CPU_LOCKUP) {
    return true;
  }

  const bool watchdogReset =
      resetReason == ESP_RST_INT_WDT || resetReason == ESP_RST_TASK_WDT || resetReason == ESP_RST_WDT;
  return watchdogReset && panicCaptureMarker == PANIC_CAPTURE_MAGIC;
}

}  // namespace HalSystem

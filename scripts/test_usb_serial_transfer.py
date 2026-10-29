#!/usr/bin/env python3
"""Compile the production USB I/O functions against a stalled/short-write serial port."""
from pathlib import Path
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
SOURCE = (ROOT / "src/network/UsbSerialFileTransfer.cpp").read_text()
OUTPUT = SOURCE[SOURCE.index("bool writeRaw("):SOURCE.index("uint32_t readLe32(")]
INPUT = SOURCE[SOURCE.index("bool readExact("):SOURCE.index("bool readPath(")]
SIZE_OUTPUT = SOURCE[SOURCE.index("bool writeLe32("):SOURCE.index("bool readExact(")]
DOWNLOAD = SOURCE[SOURCE.index("void handleRead("):SOURCE.index("void handleCommand(")]

HARNESS = r'''
#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <deque>
#include <vector>
static unsigned long now;
static unsigned kicks;
static unsigned errors;
static bool releaseChecksum;
static unsigned long checksumAt;
static unsigned long millis() { return now; }
static void vTaskDelay(int) { ++now; }
#define LOG_ERR(...) (++errors)
constexpr uint8_t ACK = 6;
constexpr uint32_t WRITE_TIMEOUT_MS = 1000;
struct Port {
  std::deque<size_t> writes;
  std::deque<uint8_t> incoming;
  std::vector<uint8_t> outgoing;
  bool blocked = false;
  unsigned busy = 0;
  size_t write(const uint8_t* p, size_t n) {
    size_t accepted = blocked ? 0 : n;
    if (!writes.empty()) { accepted = std::min(n, writes.front()); writes.pop_front(); }
    outgoing.insert(outgoing.end(), p, p + accepted);
    return accepted;
  }
  int available() { return incoming.size(); }
  size_t read(uint8_t* p, size_t n) {
    n = std::min(n, incoming.size());
    for (size_t i = 0; i < n; ++i) { p[i] = incoming.front(); incoming.pop_front(); }
    return n;
  }
  template<class... A> void printf(const char*, A...) { ++busy; }
} logSerial;
static void serviceSerialTx() {
  ++kicks;
  if (releaseChecksum && now >= checksumAt && !logSerial.outgoing.empty()) {
    logSerial.incoming = {1, 2, 3, 4}; releaseChecksum = false;
  }
}
static void reset() {
  logSerial = Port{}; now = kicks = errors = 0;
  releaseChecksum = false; checksumAt = 0;
}
'''

TESTS = r'''
int main() {
  reset();
  logSerial.writes = {0, 2, 0, 1};
  assert(writeLine("READY\n"));
  assert(logSerial.outgoing == std::vector<uint8_t>({'R','E','A','D','Y','\n'}));
  assert(kicks >= 4 && errors == 0);
  assert(writeRaw(nullptr, 0));

  reset(); logSerial.blocked = true;
  assert(!writeAck());
  assert(now == WRITE_TIMEOUT_MS && errors == 1 && logSerial.outgoing.empty());

  // A queued ACK must progress even when no subsequent log line is written.
  reset(); assert(writeAck()); releaseChecksum = true;
  uint8_t crc[4]{}; size_t received = 0;
  assert(readExact(crc, 4, 45000, &received, "checksum"));
  assert(received == 4 && crc[0] == 1 && crc[3] == 4 && logSerial.busy == 0);

  // A paused host can send the CRC after the old ten-second deadline.
  reset(); assert(writeAck()); releaseChecksum = true; checksumAt = 12000;
  assert(readExact(crc, 4, 45000, &received, "checksum"));
  assert(now == 12000 && logSerial.busy == 2 && received == 4);

  reset(); logSerial.incoming = {1, 2};
  assert(!readExact(crc, 4, 45000, &received, "checksum"));
  assert(now == 45000 && received == 2 && logSerial.busy > 0);

  // Timer rollover must not turn a bounded wait into an infinite one.
  reset(); now = ~0ul - 10; logSerial.blocked = true;
  assert(!writeAck()); assert(errors == 1);

  // A disconnected download must stop at the first failed output, not wait
  // another second for each remaining chunk of a potentially large file.
  for (auto accepted : {std::deque<size_t>{}, std::deque<size_t>{6}, std::deque<size_t>{6,4}}) {
    reset(); closes = reads = 0; logSerial.blocked = true; logSerial.writes = accepted;
    handleRead();
    assert(now == WRITE_TIMEOUT_MS && closes == 1 && reads <= 1);
  }
  puts("USB protocol I/O regression checks passed");
}
'''

FILE_STUBS = r'''
constexpr size_t PATH_BUFFER_SIZE = 256;
uint8_t transferBuffer[256];
unsigned closes, reads;
struct HalFile {
  unsigned left = 4096;
  uint64_t fileSize64() { return left; }
  int available() { return left; }
  int read(uint8_t* data, size_t size) {
    ++reads; const size_t n = std::min(size, size_t(left));
    memset(data, 0, n); left -= n; return n;
  }
  void close() { ++closes; }
};
struct StorageStub {
  bool openFileForRead(const char*, const char*, HalFile&) { return true; }
} Storage;
bool readNormalizedPath(char* path, size_t) { strcpy(path, "/test.bin"); return true; }
bool ensureFileTransferAllowed() { return true; }
bool isProtectedPath(const char*) { return false; }
bool isDirectory(const char*, bool& result) { result = false; return true; }
uint32_t esp_rom_crc32_le(uint32_t crc, const uint8_t*, uint32_t) { return crc; }
void yield() {}
'''

with tempfile.TemporaryDirectory(prefix="crossink-usb-test-") as tmp:
    path = Path(tmp)
    cpp = path / "test.cpp"
    cpp.write_text(HARNESS + OUTPUT + SIZE_OUTPUT + INPUT + FILE_STUBS + DOWNLOAD + TESTS)
    binary = path / "test"
    subprocess.run(["c++", "-std=c++17", "-Wall", "-Wextra", "-Werror",
                    "-fsanitize=address,undefined", str(cpp), "-o", str(binary)], check=True)
    subprocess.run([str(binary)], check=True)

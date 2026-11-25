#include <HalFlashPartition.h>
#include <unistd.h>

#include <cassert>
#include <cstdlib>
#include <cstring>

int main() {
  char path[] = "/tmp/crossink-readonly-flash-XXXXXX";
  const int fd = mkstemp(path);
  assert(fd >= 0);
  close(fd);
  assert(unlink(path) == 0);
  assert(setenv("CROSSINK_LANGUAGE_FLASH", path, 1) == 0);
  {
    HalFlashPartition flash;
    assert(flash.begin(false));
    assert(flash.size() == 0x360000);
    assert(access(path, F_OK) != 0);
    uint8_t bytes[32] = {};
    assert(flash.read(0, bytes, sizeof(bytes)));
    for (const auto byte : bytes) assert(byte == 0xff);
    assert(!flash.read(flash.size() - 1, bytes, sizeof(bytes)));
    const uint8_t value = 0x5a;
    assert(!flash.write(0, &value, 1));
    assert(access(path, F_OK) != 0);
    // The same instance may provision only for a later explicit install.
    assert(flash.begin(true));
    assert(access(path, F_OK) == 0);
    assert(flash.write(0, &value, 1));
  }
  {
    HalFlashPartition flash;
    assert(flash.begin(false));
    uint8_t value = 0;
    assert(flash.read(0, &value, 1));
    assert(value == 0x5a);
  }
  assert(unlink(path) == 0);
}

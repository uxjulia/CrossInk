#include "DeviceSecret.h"

#include <Logging.h>
#include <Memory.h>
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/random.h>
#ifdef SIMULATOR
#include <WolfsslCrypto.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cerrno>
#include <cstdlib>
#else
#include <esp_random.h>
#include <nvs.h>
#endif

#include <cstring>
#include <mutex>

bool deviceSecret(uint8_t (&out)[32]) {
  static uint8_t secret[32];
  static bool loaded = false;
  static std::mutex lock;  // web-server task and reader can both ask first
  std::lock_guard<std::mutex> guard(lock);

  // A failure is not cached: the next call retries.
  if (!loaded) {
#ifdef SIMULATOR
    // Native device state lives outside the simulated SD root. Never regenerate
    // an unreadable existing secret: doing so would orphan wrapped book keys.
    const char* configured = std::getenv("CROSSINK_DEVICE_SECRET_PATH");
    const char* path = configured ? configured : ".crossink-device-secret";
    int fd = ::open(path, O_RDONLY | O_NOFOLLOW);
    if (fd >= 0) {
      struct stat st{};
      loaded = fstat(fd, &st) == 0 && S_ISREG(st.st_mode) && st.st_size == sizeof(secret) &&
               ::read(fd, secret, sizeof(secret)) == sizeof(secret);
      ::close(fd);
    } else if (errno == ENOENT) {
      fd = ::open(path, O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW, 0600);
      if (fd >= 0) {
        loaded = secureRandomBytes(secret, sizeof(secret)) && ::write(fd, secret, sizeof(secret)) == sizeof(secret) &&
                 fsync(fd) == 0;
        ::close(fd);
        if (!loaded) unlink(path);
      }
    }
#else
    nvs_handle_t handle;
    if (nvs_open("devid", NVS_READWRITE, &handle) == ESP_OK) {
      size_t stored = sizeof(secret);
      const esp_err_t err = nvs_get_blob(handle, "secret", secret, &stored);
      if (err == ESP_OK) {
        loaded = stored == sizeof(secret);
      } else if (err == ESP_ERR_NVS_NOT_FOUND) {
        // Only a missing secret is created. Any other read error leaves the
        // stored one alone: replacing it would orphan every wrapped book key.
        esp_fill_random(secret, sizeof(secret));
        loaded = nvs_set_blob(handle, "secret", secret, sizeof(secret)) == ESP_OK && nvs_commit(handle) == ESP_OK;
      }
      nvs_close(handle);
    }
#endif
    if (!loaded) {
      LOG_ERR("DSEC", "Device secret unavailable");
      return false;
    }
  }
  memcpy(out, secret, sizeof(out));
  return true;
}

// RNG state can exceed the device task-stack budget.
bool secureRandomBytes(uint8_t* out, size_t length) {
  auto rng = makeUniqueNoThrow<WC_RNG>();
  if (!rng || wc_InitRng(rng.get()) != 0) {
    LOG_ERR("DSEC", "RNG initialization failed");
    return false;
  }
  const bool ok = wc_RNG_GenerateBlock(rng.get(), out, length) == 0;
  wc_FreeRng(rng.get());
  if (!ok) LOG_ERR("DSEC", "RNG failed");
  return ok;
}

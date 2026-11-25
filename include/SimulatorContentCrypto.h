#pragma once
// The simulator defines Arduino/PlatformIO for firmware compatibility, but
// crypto links the native wolfSSL library and must use its exact configuration.
#pragma push_macro("ARDUINO")
#pragma push_macro("PLATFORMIO")
#undef ARDUINO
#undef PLATFORMIO
#include <wolfssl/options.h>
#include <wolfssl/wolfcrypt/settings.h>
#pragma pop_macro("PLATFORMIO")
#pragma pop_macro("ARDUINO")

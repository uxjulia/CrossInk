#pragma once

// PlatformIO normally supplies these through build_flags/extra_scripts. Keep
// fallbacks here so editor indexers and simulator-like tools still parse files.
#ifndef CROSSPOINT_VERSION
#define CROSSPOINT_VERSION "dev"
#endif

#ifndef CROSSINK_VERSION
#define CROSSINK_VERSION "dev"
#endif

#ifndef CROSSINK_FIRMWARE_VARIANT
#ifdef CROSSPOINT_FIRMWARE_VARIANT
#define CROSSINK_FIRMWARE_VARIANT CROSSPOINT_FIRMWARE_VARIANT
#else
#define CROSSINK_FIRMWARE_VARIANT "unknown"
#endif
#endif

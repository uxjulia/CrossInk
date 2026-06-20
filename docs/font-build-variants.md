---
title: Font Build Variants
nav_order: 12
---

# Font Build Variants

CrossInk ships multiple firmware build variants because the ESP32-C3 has limited flash and RAM. The variants trade available point sizes against emoji and miscellaneous-symbol support.

## Variants

### `teensy`

Only the smallest point sizes.

- Emoji and miscellaneous-symbol support
- 4 font sizes:
  - 8 pt
  - 9 pt
  - 10 pt
  - 12 pt

### `tiny`

No 18 pt or 20 pt font size. This is the preferred general-purpose build.

- Emoji and miscellaneous-symbol support
- 4 font sizes:
  - 10 pt
  - 12 pt
  - 14 pt
  - 16 pt

### `xlarge`

8 pt, 10 pt, and 12 pt font sizes are removed to reduce build size while still supporting emoji and symbols.

- Emoji and miscellaneous-symbol support
- 3 font sizes:
  - 16 pt
  - 18 pt
  - 20 pt

## Flashing A Variant

Download the matching `firmware-*.bin` from the [releases page](https://github.com/uxjulia/CrossInk/releases), or build and upload locally with PlatformIO:

```sh
pio run -e tiny --target upload
```

Replace `tiny` with `teensy` or `xlarge` as needed.

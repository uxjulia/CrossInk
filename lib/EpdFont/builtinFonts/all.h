#pragma once

// Reading fonts have generated variants with identical variable names:
//   default: emoji/symbol fallback + PHM CJK fallback
//   OMIT_PHM: emoji/symbol fallback, no PHM CJK
//   OMIT_EMOJI_FONTS: primary fonts only, no emoji and no PHM CJK
//
// Generate the variants with lib/EpdFont/scripts/convert-builtin-fonts.sh.
//
// Per-size guards:
//   OMIT_TEENSY_FONT - excludes 8px (Teensy) reading fonts; used by env:xlarge
//   OMIT_TINY_FONT   - excludes 10px (Tiny) reading fonts; used by env:xlarge
//   OMIT_SMALL_FONT  - excludes 12px (Small) reading fonts
//   OMIT_XLARGE_FONT - excludes 18px (Extra Large) reading fonts; used by env:tiny
//   OMIT_HUGE_FONT   - excludes 20px (Huge) reading fonts; used by all firmware envs except env:xlarge
#ifdef OMIT_EMOJI_FONTS
#define BUILTIN_READING_FONT_HEADER(name) <builtinFonts/noemoji/name.h>
#elif defined(OMIT_PHM)
#define BUILTIN_READING_FONT_HEADER(name) <builtinFonts/nophm/name.h>
#else
#define BUILTIN_READING_FONT_HEADER(name) <builtinFonts/name.h>
#endif

#ifndef OMIT_TEENSY_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_8_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_8_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_8_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_8_regular)
#endif
#ifndef OMIT_TINY_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_10_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_10_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_10_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_10_regular)
#endif
#ifndef OMIT_SMALL_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_12_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_12_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_12_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_12_regular)
#endif
#include BUILTIN_READING_FONT_HEADER(bitter_14_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_14_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_14_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_14_regular)
#include BUILTIN_READING_FONT_HEADER(bitter_16_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_16_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_16_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_16_regular)
#ifndef OMIT_XLARGE_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_18_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_18_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_18_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_18_regular)
#endif
#ifndef OMIT_HUGE_FONT
#include BUILTIN_READING_FONT_HEADER(bitter_20_bold)
#include BUILTIN_READING_FONT_HEADER(bitter_20_bolditalic)
#include BUILTIN_READING_FONT_HEADER(bitter_20_italic)
#include BUILTIN_READING_FONT_HEADER(bitter_20_regular)
#endif

#ifndef OMIT_TEENSY_FONT
#include BUILTIN_READING_FONT_HEADER(charein_8_bold)
#include BUILTIN_READING_FONT_HEADER(charein_8_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_8_italic)
#include BUILTIN_READING_FONT_HEADER(charein_8_regular)
#endif
#ifndef OMIT_TINY_FONT
#include BUILTIN_READING_FONT_HEADER(charein_10_bold)
#include BUILTIN_READING_FONT_HEADER(charein_10_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_10_italic)
#include BUILTIN_READING_FONT_HEADER(charein_10_regular)
#endif
#ifndef OMIT_SMALL_FONT
#include BUILTIN_READING_FONT_HEADER(charein_12_bold)
#include BUILTIN_READING_FONT_HEADER(charein_12_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_12_italic)
#include BUILTIN_READING_FONT_HEADER(charein_12_regular)
#endif
#include BUILTIN_READING_FONT_HEADER(charein_14_bold)
#include BUILTIN_READING_FONT_HEADER(charein_14_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_14_italic)
#include BUILTIN_READING_FONT_HEADER(charein_14_regular)
#include BUILTIN_READING_FONT_HEADER(charein_16_bold)
#include BUILTIN_READING_FONT_HEADER(charein_16_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_16_italic)
#include BUILTIN_READING_FONT_HEADER(charein_16_regular)
#ifndef OMIT_XLARGE_FONT
#include BUILTIN_READING_FONT_HEADER(charein_18_bold)
#include BUILTIN_READING_FONT_HEADER(charein_18_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_18_italic)
#include BUILTIN_READING_FONT_HEADER(charein_18_regular)
#endif
#ifndef OMIT_HUGE_FONT
#include BUILTIN_READING_FONT_HEADER(charein_20_bold)
#include BUILTIN_READING_FONT_HEADER(charein_20_bolditalic)
#include BUILTIN_READING_FONT_HEADER(charein_20_italic)
#include BUILTIN_READING_FONT_HEADER(charein_20_regular)
#endif

#ifndef OMIT_TEENSY_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_8_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_8_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_8_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_8_regular)
#endif
#ifndef OMIT_TINY_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_10_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_10_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_10_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_10_regular)
#endif
#ifndef OMIT_SMALL_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_12_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_12_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_12_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_12_regular)
#endif
#include BUILTIN_READING_FONT_HEADER(lexenddeca_14_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_14_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_14_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_14_regular)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_16_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_16_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_16_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_16_regular)
#ifndef OMIT_XLARGE_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_18_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_18_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_18_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_18_regular)
#endif
#ifndef OMIT_HUGE_FONT
#include BUILTIN_READING_FONT_HEADER(lexenddeca_20_bold)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_20_bolditalic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_20_italic)
#include BUILTIN_READING_FONT_HEADER(lexenddeca_20_regular)
#endif

#undef BUILTIN_READING_FONT_HEADER

// UI fonts - no emoji or PHM variants.
#include <builtinFonts/inter_10_bold.h>
#include <builtinFonts/inter_10_regular.h>
#include <builtinFonts/inter_12_bold.h>
#include <builtinFonts/inter_12_regular.h>
#include <builtinFonts/inter_8_regular.h>

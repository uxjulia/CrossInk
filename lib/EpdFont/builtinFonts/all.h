#pragma once

// Reading fonts — two variants controlled by OMIT_EMOJI_FONTS.
// The no-emoji variants live in noemoji/ with the same filenames and variable names.
// Generate both sets with lib/EpdFont/scripts/convert-builtin-fonts.sh.
//
// Per-size guards:
//   OMIT_TINY_FONT   — excludes 10px (Tiny) reading fonts; used by env:xlarge
//   OMIT_SMALL_FONT  — excludes 12px (Small) reading fonts
//   OMIT_XLARGE_FONT — excludes 18px (Extra Large) reading fonts; used by env:tiny
#ifndef OMIT_EMOJI_FONTS
#ifndef OMIT_TINY_FONT
#include <builtinFonts/bitter_10_bold.h>
#include <builtinFonts/bitter_10_bolditalic.h>
#include <builtinFonts/bitter_10_italic.h>
#include <builtinFonts/bitter_10_regular.h>
#endif
#ifndef OMIT_SMALL_FONT
#include <builtinFonts/bitter_12_bold.h>
#include <builtinFonts/bitter_12_bolditalic.h>
#include <builtinFonts/bitter_12_italic.h>
#include <builtinFonts/bitter_12_regular.h>
#endif
#include <builtinFonts/bitter_14_bold.h>
#include <builtinFonts/bitter_14_bolditalic.h>
#include <builtinFonts/bitter_14_italic.h>
#include <builtinFonts/bitter_14_regular.h>
#include <builtinFonts/bitter_16_bold.h>
#include <builtinFonts/bitter_16_bolditalic.h>
#include <builtinFonts/bitter_16_italic.h>
#include <builtinFonts/bitter_16_regular.h>
#ifndef OMIT_XLARGE_FONT
#include <builtinFonts/bitter_18_bold.h>
#include <builtinFonts/bitter_18_bolditalic.h>
#include <builtinFonts/bitter_18_italic.h>
#include <builtinFonts/bitter_18_regular.h>
#endif
#ifndef OMIT_TINY_FONT
#include <builtinFonts/charein_10_bold.h>
#include <builtinFonts/charein_10_bolditalic.h>
#include <builtinFonts/charein_10_italic.h>
#include <builtinFonts/charein_10_regular.h>
#endif
#ifndef OMIT_SMALL_FONT
#include <builtinFonts/charein_12_bold.h>
#include <builtinFonts/charein_12_bolditalic.h>
#include <builtinFonts/charein_12_italic.h>
#include <builtinFonts/charein_12_regular.h>
#endif
#include <builtinFonts/charein_14_bold.h>
#include <builtinFonts/charein_14_bolditalic.h>
#include <builtinFonts/charein_14_italic.h>
#include <builtinFonts/charein_14_regular.h>
#include <builtinFonts/charein_16_bold.h>
#include <builtinFonts/charein_16_bolditalic.h>
#include <builtinFonts/charein_16_italic.h>
#include <builtinFonts/charein_16_regular.h>
#ifndef OMIT_XLARGE_FONT
#include <builtinFonts/charein_18_bold.h>
#include <builtinFonts/charein_18_bolditalic.h>
#include <builtinFonts/charein_18_italic.h>
#include <builtinFonts/charein_18_regular.h>
#endif
#ifndef OMIT_TINY_FONT
#include <builtinFonts/lexenddeca_10_bold.h>
#include <builtinFonts/lexenddeca_10_bolditalic.h>
#include <builtinFonts/lexenddeca_10_italic.h>
#include <builtinFonts/lexenddeca_10_regular.h>
#endif
#ifndef OMIT_SMALL_FONT
#include <builtinFonts/lexenddeca_12_bold.h>
#include <builtinFonts/lexenddeca_12_bolditalic.h>
#include <builtinFonts/lexenddeca_12_italic.h>
#include <builtinFonts/lexenddeca_12_regular.h>
#endif
#include <builtinFonts/lexenddeca_14_bold.h>
#include <builtinFonts/lexenddeca_14_bolditalic.h>
#include <builtinFonts/lexenddeca_14_italic.h>
#include <builtinFonts/lexenddeca_14_regular.h>
#include <builtinFonts/lexenddeca_16_bold.h>
#include <builtinFonts/lexenddeca_16_bolditalic.h>
#include <builtinFonts/lexenddeca_16_italic.h>
#include <builtinFonts/lexenddeca_16_regular.h>
#ifndef OMIT_XLARGE_FONT
#include <builtinFonts/lexenddeca_18_bold.h>
#include <builtinFonts/lexenddeca_18_bolditalic.h>
#include <builtinFonts/lexenddeca_18_italic.h>
#include <builtinFonts/lexenddeca_18_regular.h>
#endif

#else
#ifndef OMIT_TINY_FONT
#include <builtinFonts/noemoji/bitter_10_bold.h>
#include <builtinFonts/noemoji/bitter_10_bolditalic.h>
#include <builtinFonts/noemoji/bitter_10_italic.h>
#include <builtinFonts/noemoji/bitter_10_regular.h>
#endif
#ifndef OMIT_SMALL_FONT
#include <builtinFonts/noemoji/bitter_12_bold.h>
#include <builtinFonts/noemoji/bitter_12_bolditalic.h>
#include <builtinFonts/noemoji/bitter_12_italic.h>
#include <builtinFonts/noemoji/bitter_12_regular.h>
#endif
#include <builtinFonts/noemoji/bitter_14_bold.h>
#include <builtinFonts/noemoji/bitter_14_bolditalic.h>
#include <builtinFonts/noemoji/bitter_14_italic.h>
#include <builtinFonts/noemoji/bitter_14_regular.h>
#include <builtinFonts/noemoji/bitter_16_bold.h>
#include <builtinFonts/noemoji/bitter_16_bolditalic.h>
#include <builtinFonts/noemoji/bitter_16_italic.h>
#include <builtinFonts/noemoji/bitter_16_regular.h>
#ifndef OMIT_XLARGE_FONT
#include <builtinFonts/noemoji/bitter_18_bold.h>
#include <builtinFonts/noemoji/bitter_18_bolditalic.h>
#include <builtinFonts/noemoji/bitter_18_italic.h>
#include <builtinFonts/noemoji/bitter_18_regular.h>
#endif
#ifndef OMIT_TINY_FONT
#include <builtinFonts/noemoji/charein_10_bold.h>
#include <builtinFonts/noemoji/charein_10_bolditalic.h>
#include <builtinFonts/noemoji/charein_10_italic.h>
#include <builtinFonts/noemoji/charein_10_regular.h>
#endif
#ifndef OMIT_SMALL_FONT
#include <builtinFonts/noemoji/charein_12_bold.h>
#include <builtinFonts/noemoji/charein_12_bolditalic.h>
#include <builtinFonts/noemoji/charein_12_italic.h>
#include <builtinFonts/noemoji/charein_12_regular.h>
#endif
#include <builtinFonts/noemoji/charein_14_bold.h>
#include <builtinFonts/noemoji/charein_14_bolditalic.h>
#include <builtinFonts/noemoji/charein_14_italic.h>
#include <builtinFonts/noemoji/charein_14_regular.h>
#include <builtinFonts/noemoji/charein_16_bold.h>
#include <builtinFonts/noemoji/charein_16_bolditalic.h>
#include <builtinFonts/noemoji/charein_16_italic.h>
#include <builtinFonts/noemoji/charein_16_regular.h>
#ifndef OMIT_XLARGE_FONT
#include <builtinFonts/noemoji/charein_18_bold.h>
#include <builtinFonts/noemoji/charein_18_bolditalic.h>
#include <builtinFonts/noemoji/charein_18_italic.h>
#include <builtinFonts/noemoji/charein_18_regular.h>
#endif
#ifndef OMIT_TINY_FONT
#include <builtinFonts/noemoji/lexenddeca_10_bold.h>
#include <builtinFonts/noemoji/lexenddeca_10_bolditalic.h>
#include <builtinFonts/noemoji/lexenddeca_10_italic.h>
#include <builtinFonts/noemoji/lexenddeca_10_regular.h>
#endif
#ifndef OMIT_SMALL_FONT
#include <builtinFonts/noemoji/lexenddeca_12_bold.h>
#include <builtinFonts/noemoji/lexenddeca_12_bolditalic.h>
#include <builtinFonts/noemoji/lexenddeca_12_italic.h>
#include <builtinFonts/noemoji/lexenddeca_12_regular.h>
#endif
#include <builtinFonts/noemoji/lexenddeca_14_bold.h>
#include <builtinFonts/noemoji/lexenddeca_14_bolditalic.h>
#include <builtinFonts/noemoji/lexenddeca_14_italic.h>
#include <builtinFonts/noemoji/lexenddeca_14_regular.h>
#include <builtinFonts/noemoji/lexenddeca_16_bold.h>
#include <builtinFonts/noemoji/lexenddeca_16_bolditalic.h>
#include <builtinFonts/noemoji/lexenddeca_16_italic.h>
#include <builtinFonts/noemoji/lexenddeca_16_regular.h>
#ifndef OMIT_XLARGE_FONT
#include <builtinFonts/noemoji/lexenddeca_18_bold.h>
#include <builtinFonts/noemoji/lexenddeca_18_bolditalic.h>
#include <builtinFonts/noemoji/lexenddeca_18_italic.h>
#include <builtinFonts/noemoji/lexenddeca_18_regular.h>
#endif
#endif

// UI fonts — no emoji variant in either build
#include <builtinFonts/dmsans_10_bold.h>
#include <builtinFonts/dmsans_10_regular.h>
#include <builtinFonts/dmsans_12_bold.h>
#include <builtinFonts/dmsans_12_regular.h>
#include <builtinFonts/inter_8_regular.h>

#!/bin/bash

set -e

cd "$(dirname "$0")"

EMOJI_FONT="../builtinFonts/source/NotoEmoji/NotoEmoji-Regular.ttf"
SYMBOLS_FONT="../builtinFonts/source/NotoSymbols/NotoSansSymbols-Regular.ttf"

# Additional Unicode intervals to include beyond the default Latin/Cyrillic/math set.
# 0x2669-0x266F: Music notes and accidentals (♩♪♫♬♭♮♯)
# 0x1F600-0x1F64F: Emoticons (😀😂🙂 etc.)
# 0x1F44B-0x1F44F: Hand gesture emojis (👋👌👍👎👏)
# 0x2764: Heart symbol (❤️)
# 0x03BB: Greek lambda (λ)
# 0x0410-0x0414, 0x0418, 0x041B, 0x041D-0x0423, 0x0425, 0x0427,
# 0x042B-0x042C, 0x042E-0x0432, 0x0434-0x0435, 0x0437, 0x043A,
# 0x043D-0x043E, 0x0440, 0x0442, 0x0446, 0x044C, 0x044E: Cyrillic subset
# 0x2113: Script small l (ℓ)
EMOJI_INTERVALS=(
  --additional-intervals 0x2669,0x266F
  --additional-intervals 0x1F600,0x1F64F
  --additional-intervals 0x1F44B,0x1F44F
  --additional-intervals 0x2764,0x2764
  --additional-intervals 0x03BB,0x03BB
  --additional-intervals 0x0410,0x0414
  --additional-intervals 0x0418,0x0418
  --additional-intervals 0x041B,0x041B
  --additional-intervals 0x041D,0x0423
  --additional-intervals 0x0425,0x0425
  --additional-intervals 0x0427,0x0427
  --additional-intervals 0x042B,0x042C
  --additional-intervals 0x042E,0x0432
  --additional-intervals 0x0434,0x0435
  --additional-intervals 0x0437,0x0437
  --additional-intervals 0x043A,0x043A
  --additional-intervals 0x043D,0x043E
  --additional-intervals 0x0440,0x0440
  --additional-intervals 0x0442,0x0442
  --additional-intervals 0x0446,0x0446
  --additional-intervals 0x044C,0x044C
  --additional-intervals 0x044E,0x044E
  --additional-intervals 0x2113,0x2113
)

CHAREINK_FALLBACK_INTERVALS=(
  --font-include-intervals 1:0x03BB,0x03BB
  --font-include-intervals 1:0x0410,0x0414
  --font-include-intervals 1:0x0418,0x0418
  --font-include-intervals 1:0x041B,0x041B
  --font-include-intervals 1:0x041D,0x0423
  --font-include-intervals 1:0x0425,0x0425
  --font-include-intervals 1:0x0427,0x0427
  --font-include-intervals 1:0x042B,0x042C
  --font-include-intervals 1:0x042E,0x0432
  --font-include-intervals 1:0x0434,0x0435
  --font-include-intervals 1:0x0437,0x0437
  --font-include-intervals 1:0x043A,0x043A
  --font-include-intervals 1:0x043D,0x043E
  --font-include-intervals 1:0x0440,0x0440
  --font-include-intervals 1:0x0442,0x0442
  --font-include-intervals 1:0x0446,0x0446
  --font-include-intervals 1:0x044C,0x044C
  --font-include-intervals 1:0x044E,0x044E
  --font-include-intervals 1:0x2113,0x2113
)

EMOJI_FALLBACK_INTERVALS=(
  --font-include-intervals 2:0x1F600,0x1F64F
  --font-include-intervals 2:0x1F44B,0x1F44F
  --font-include-intervals 2:0x2764,0x2764
)

SYMBOL_FALLBACK_INTERVALS=(
  --font-include-intervals 3:0x2669,0x266F
)

CHAREINK_EMOJI_FALLBACK_INTERVALS=(
  --font-include-intervals 1:0x1F600,0x1F64F
  --font-include-intervals 1:0x1F44B,0x1F44F
  --font-include-intervals 1:0x2764,0x2764
)

CHAREINK_SYMBOL_FALLBACK_INTERVALS=(
  --font-include-intervals 2:0x2669,0x266F
)

READING_FONT_SIZES=(10 12 14 16 18)
READING_FONT_STYLES=("Regular" "Bold" "Italic" "BoldItalic")

# =============================================================================
# NO-EMOJI VARIANTS — for OMIT_EMOJI_FONTS builds
# Omits the emoji/symbols font stack and emoji code point intervals.
# Output: builtinFonts/noemoji/<name>.h (same variable names as emoji variants)
# =============================================================================

mkdir -p ../builtinFonts/noemoji

echo "Generating no-emoji font variants..."

# LEXEND DECA (no-emoji)

for size in ${READING_FONT_SIZES[@]}; do
  for style in ${READING_FONT_STYLES[@]}; do
    font_name="lexenddeca_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/LexendDeca/LexendDeca-${style}.ttf"
    output_path="../builtinFonts/noemoji/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

# BITTER (no-emoji)

for size in ${READING_FONT_SIZES[@]}; do
  for style in ${READING_FONT_STYLES[@]}; do
    font_name="bitter_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bitter/Bitter-${style}.ttf"
    output_path="../builtinFonts/noemoji/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

# CHARE INK (no-emoji)

for size in ${READING_FONT_SIZES[@]}; do
  for style in ${READING_FONT_STYLES[@]}; do
    font_name="charein_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/ChareInk7/ChareInk7-${style}.ttf"
    output_path="../builtinFonts/noemoji/${font_name}.h"
    python fontconvert.py $font_name $size $font_path --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

echo ""
echo "No-emoji variants complete."
echo ""

# =============================================================================
# EMOJI VARIANTS — default builds
# Merges the emoji/symbols font stack into each reading font.
# =============================================================================

# LEXEND DECA

for size in ${READING_FONT_SIZES[@]}; do
  for style in ${READING_FONT_STYLES[@]}; do
    font_name="lexenddeca_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/LexendDeca/LexendDeca-${style}.ttf"
    fallback_path="../builtinFonts/source/ChareInk7/ChareInk7-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $fallback_path $EMOJI_FONT $SYMBOLS_FONT "${EMOJI_INTERVALS[@]}" "${CHAREINK_FALLBACK_INTERVALS[@]}" "${EMOJI_FALLBACK_INTERVALS[@]}" "${SYMBOL_FALLBACK_INTERVALS[@]}" --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

# BITTER

for size in ${READING_FONT_SIZES[@]}; do
  for style in ${READING_FONT_STYLES[@]}; do
    font_name="bitter_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bitter/Bitter-${style}.ttf"
    fallback_path="../builtinFonts/source/ChareInk7/ChareInk7-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $fallback_path $EMOJI_FONT $SYMBOLS_FONT "${EMOJI_INTERVALS[@]}" "${CHAREINK_FALLBACK_INTERVALS[@]}" "${EMOJI_FALLBACK_INTERVALS[@]}" "${SYMBOL_FALLBACK_INTERVALS[@]}" --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

# CHARE INK

for size in ${READING_FONT_SIZES[@]}; do
  for style in ${READING_FONT_STYLES[@]}; do
    font_name="charein_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/ChareInk7/ChareInk7-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $EMOJI_FONT $SYMBOLS_FONT "${EMOJI_INTERVALS[@]}" "${CHAREINK_EMOJI_FALLBACK_INTERVALS[@]}" "${CHAREINK_SYMBOL_FALLBACK_INTERVALS[@]}" --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

# UI Font - Inter

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="inter_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Inter/Inter-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path > $output_path
    echo "Generated $output_path"
  done
done

# Small UI Font - Inter

python fontconvert.py inter_8_regular 8 ../builtinFonts/source/Inter/Inter-Regular.ttf > ../builtinFonts/inter_8_regular.h

echo ""
echo "Running compression verification..."
python verify_compression.py ../builtinFonts/

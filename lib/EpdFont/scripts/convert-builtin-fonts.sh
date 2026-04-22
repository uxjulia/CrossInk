#!/bin/bash

set -e

cd "$(dirname "$0")"

EMOJI_FONT="../builtinFonts/source/NotoEmoji/NotoEmoji-Regular.ttf"
SYMBOLS_FONT="../builtinFonts/source/NotoSymbols/NotoSansSymbols-Regular.ttf"
SYMBOLS2_FONT="../builtinFonts/source/NotoSymbols2/NotoSansSymbols2-Regular.ttf"

# Additional Unicode intervals to include beyond the default Latin/Cyrillic/math set.
# 0x2699-0x266F: Music notes (♩♪♫♬♭♮♯)
# 0x1F600-0x1F64F: Emoticons (😀😂🙂 etc.)
# 0x1F44B-0x1F44F: Waving hand + other hand emojis (👋🤚🖐✋)
# 0x2764: Heart symbol (❤️)
# 0x0370-0x03FF: Greek and Coptic (for math symbols like π, Σ, etc.)
EMOJI_INTERVALS=(
  --additional-intervals 0x266F,0x2699
  --additional-intervals 0x1F600,0x1F64F
  --additional-intervals 0x1F44B,0x1F44F
  --additional-intervals 0x2764,0x2764
  --additional-intervals 0x0370,0x03FF
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
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $EMOJI_FONT $SYMBOLS_FONT $SYMBOLS2_FONT "${EMOJI_INTERVALS[@]}" --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

# BITTER

for size in ${READING_FONT_SIZES[@]}; do
  for style in ${READING_FONT_STYLES[@]}; do
    font_name="bitter_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bitter/Bitter-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $EMOJI_FONT $SYMBOLS_FONT $SYMBOLS2_FONT "${EMOJI_INTERVALS[@]}" --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

# CHARE INK

for size in ${READING_FONT_SIZES[@]}; do
  for style in ${READING_FONT_STYLES[@]}; do
    font_name="charein_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/ChareInk7/ChareInk7-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $EMOJI_FONT $SYMBOLS_FONT $SYMBOLS2_FONT "${EMOJI_INTERVALS[@]}" --2bit --compress --pnum > $output_path
    echo "Generated $output_path"
  done
done

# UI Font - DM Sans

UI_FONT_SIZES=(10 12)
UI_FONT_STYLES=("Regular" "Bold")

for size in ${UI_FONT_SIZES[@]}; do
  for style in ${UI_FONT_STYLES[@]}; do
    font_name="dmsans_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/DMSans/DMSans-${style}.ttf"
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
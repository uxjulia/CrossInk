#!/bin/bash

set -e

cd "$(dirname "$0")"

EMOJI_FONT="../builtinFonts/source/NotoEmoji/NotoEmoji-Regular.ttf"
SYMBOLS_FONT="../builtinFonts/source/NotoSymbols/NotoSansSymbols-Regular.ttf"
SYMBOLS2_FONT="../builtinFonts/source/NotoSymbols/NotoSansSymbols2-Regular.ttf"

# Additional Unicode intervals to include beyond the default Latin/Cyrillic/math set.
# 0x2600-0x26FF: Miscellaneous Symbols (♩♪♫♬♭♮♯ stars, hearts, etc.)
# 0x1F600-0x1F64F: Emoticons (😀😂🙂 etc.)
EMOJI_INTERVALS=(
  --additional-intervals 0x2600,0x26FF
  --additional-intervals 0x1F600,0x1F64F
)

READING_FONT_SIZES=(10 12 14 16)
READING_FONT_STYLES=("Regular" "Bold" "Italic" "BoldItalic")

# LEXEND DECA

for size in ${READING_FONT_SIZES[@]}; do
  for style in ${READING_FONT_STYLES[@]}; do
    font_name="lexenddeca_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/LexendDeca/LexendDeca-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $EMOJI_FONT $SYMBOLS_FONT $SYMBOLS2_FONT "${EMOJI_INTERVALS[@]}" --2bit --compress > $output_path
    echo "Generated $output_path"
  done
done

# BITTER

for size in ${READING_FONT_SIZES[@]}; do
  for style in ${READING_FONT_STYLES[@]}; do
    font_name="bitter_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/Bitter/Bitter-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $EMOJI_FONT $SYMBOLS_FONT $SYMBOLS2_FONT "${EMOJI_INTERVALS[@]}" --2bit --compress > $output_path
    echo "Generated $output_path"
  done
done

# CHARE INK

for size in ${READING_FONT_SIZES[@]}; do
  for style in ${READING_FONT_STYLES[@]}; do
    font_name="charein_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    font_path="../builtinFonts/source/ChareInk7/ChareInk7-${style}.ttf"
    output_path="../builtinFonts/${font_name}.h"
    python fontconvert.py $font_name $size $font_path $EMOJI_FONT $SYMBOLS_FONT $SYMBOLS2_FONT "${EMOJI_INTERVALS[@]}" --2bit --compress > $output_path
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

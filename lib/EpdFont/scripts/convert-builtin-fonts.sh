#!/bin/bash

set -e

cd "$(dirname "$0")"

EMOJI_FONT="../builtinFonts/source/NotoEmoji/NotoEmoji-Regular.ttf"
SYMBOLS_FONT="../builtinFonts/source/NotoSymbols/NotoSansSymbols-Regular.ttf"
PHM_FONT="../builtinFonts/source/NotoSansCJKsc/NotoSansCJKsc-Regular.otf"

# Additional Unicode intervals to include beyond the default Latin/Cyrillic/math set.
# 0x2669-0x266F: Music notes and accidentals (♩♪♫♬♭♮♯)
# 0x1F600-0x1F637, 0x1F641-0x1F644, 0x1F64F: Emoticons subset.
#   Omits U+1F638-1F640, U+1F645-1F647, and U+1F648-1F64E.
# 0x1F44B-0x1F44F: Hand gesture emojis (👋👌👍👎👏)
# 0x2764: Heart symbol (❤️)
# 0x03BB: Greek lambda (λ)
# 0x0410-0x0414, 0x0418, 0x041B, 0x041D-0x0423, 0x0425, 0x0427,
# 0x042B-0x042C, 0x042E-0x0432, 0x0434-0x0435, 0x0437, 0x043A,
# 0x043D-0x043E, 0x0440, 0x0442, 0x0446, 0x044C, 0x044E: Cyrillic subset
# 0x2113: Script small l (ℓ)
COMMON_FALLBACK_INTERVALS=(
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

EMOJI_ONLY_INTERVALS=(
  --additional-intervals 0x2669,0x266F
  --additional-intervals 0x1F600,0x1F637
  --additional-intervals 0x1F641,0x1F644
  --additional-intervals 0x1F64F,0x1F64F
  --additional-intervals 0x1F44B,0x1F44F
  --additional-intervals 0x2764,0x2764
)

EMOJI_INTERVALS=(
  "${COMMON_FALLBACK_INTERVALS[@]}"
  "${EMOJI_ONLY_INTERVALS[@]}"
)

PHM_INTERVALS=(
  --additional-intervals 0x4F1A,0x4F1A
  --additional-intervals 0x53BB,0x53BB
  --additional-intervals 0x5458,0x5458
  --additional-intervals 0x59DA,0x59DA
  --additional-intervals 0x5B98,0x5B98
  --additional-intervals 0x5BA4,0x5BA4
  --additional-intervals 0x5E26,0x5E26
  --additional-intervals 0x6211,0x6211
  --additional-intervals 0x62C9,0x62C9
  --additional-intervals 0x653E,0x653E
  --additional-intervals 0x6746,0x677F
  --additional-intervals 0x7532,0x7532
  --additional-intervals 0x7684,0x7684
  --additional-intervals 0x8BAE,0x8BAE
  --additional-intervals 0x8BF7,0x8BF7
  --additional-intervals 0x91CA,0x91CA
)

CHAREINK_FALLBACK_RANGES=(
  0x03BB,0x03BB
  0x0410,0x0414
  0x0418,0x0418
  0x041B,0x041B
  0x041D,0x0423
  0x0425,0x0425
  0x0427,0x0427
  0x042B,0x042C
  0x042E,0x0432
  0x0434,0x0435
  0x0437,0x0437
  0x043A,0x043A
  0x043D,0x043E
  0x0440,0x0440
  0x0442,0x0442
  0x0446,0x0446
  0x044C,0x044C
  0x044E,0x044E
  0x2113,0x2113
)

EMOJI_FALLBACK_RANGES=(
  0x1F600,0x1F637
  0x1F641,0x1F644
  0x1F64F,0x1F64F
  0x1F44B,0x1F44F
  0x2764,0x2764
)

SYMBOL_FALLBACK_RANGES=(
  0x2669,0x266F
)

PHM_FALLBACK_RANGES=(
  0x4F1A,0x4F1A
  0x53BB,0x53BB
  0x5458,0x5458
  0x59DA,0x59DA
  0x5B98,0x5B98
  0x5BA4,0x5BA4
  0x5E26,0x5E26
  0x6211,0x6211
  0x62C9,0x62C9
  0x653E,0x653E
  0x6746,0x677F
  0x7532,0x7532
  0x7684,0x7684
  0x8BAE,0x8BAE
  0x8BF7,0x8BF7
  0x91CA,0x91CA
)

READING_FONT_SIZES=(8 10 12 14 16 18 20)
READING_FONT_STYLES=("Regular" "Bold" "Italic" "BoldItalic")
READING_FONT_RENDER_ARGS=(--2bit --compress --pnum --darken-aa)

font_include_args() {
  local face_index="$1"
  shift
  for range in "$@"; do
    printf '%s\n' --font-include-intervals "${face_index}:${range}"
  done
}

generate_family() {
  local family_name="$1"
  local source_dir="$2"
  local source_prefix="$3"
  local output_dir="$4"
  local include_emoji="$5"
  local include_phm="$6"
  local use_chareink_common_fallback="$7"

  for size in ${READING_FONT_SIZES[@]}; do
    for style in ${READING_FONT_STYLES[@]}; do
      local style_lower
      style_lower="$(echo $style | tr '[:upper:]' '[:lower:]')"
      local font_name="${family_name}_${size}_${style_lower}"
      local font_path="../builtinFonts/source/${source_dir}/${source_prefix}-${style}.ttf"
      local output_path="${output_dir}/${font_name}.h"
      local font_stack=("$font_path")
      local interval_args=()
      local include_args=()

      if [[ "$include_emoji" == "yes" ]]; then
        interval_args+=("${EMOJI_INTERVALS[@]}")
        if [[ "$use_chareink_common_fallback" == "yes" ]]; then
          font_stack+=("../builtinFonts/source/ChareInk7/ChareInk7-${style}.ttf")
          include_args+=($(font_include_args $(( ${#font_stack[@]} - 1 )) "${CHAREINK_FALLBACK_RANGES[@]}"))
        fi
        font_stack+=("$EMOJI_FONT")
        include_args+=($(font_include_args $(( ${#font_stack[@]} - 1 )) "${EMOJI_FALLBACK_RANGES[@]}"))
        font_stack+=("$SYMBOLS_FONT")
        include_args+=($(font_include_args $(( ${#font_stack[@]} - 1 )) "${SYMBOL_FALLBACK_RANGES[@]}"))
      fi

      if [[ "$include_phm" == "yes" && "$style" == "Regular" ]]; then
        interval_args+=("${PHM_INTERVALS[@]}")
        font_stack+=("$PHM_FONT")
        include_args+=($(font_include_args $(( ${#font_stack[@]} - 1 )) "${PHM_FALLBACK_RANGES[@]}"))
      fi

      python fontconvert.py $font_name $size "${font_stack[@]}" "${interval_args[@]}" "${include_args[@]}" "${READING_FONT_RENDER_ARGS[@]}" > $output_path
      echo "Generated $output_path"
    done
  done
}

generate_reading_variant() {
  local output_dir="$1"
  local include_emoji="$2"
  local include_phm="$3"
  local label="$4"

  mkdir -p "$output_dir"
  echo "Generating ${label} font variants..."
  generate_family lexenddeca LexendDeca LexendDeca "$output_dir" "$include_emoji" "$include_phm" yes
  generate_family bitter Bitter Bitter "$output_dir" "$include_emoji" "$include_phm" yes
  generate_family charein ChareInk7 ChareInk7 "$output_dir" "$include_emoji" "$include_phm" no
  echo ""
  echo "${label} variants complete."
  echo ""
}

# Reading font variants:
#   builtinFonts/             default: emoji/symbol fallback + PHM CJK fallback
#   builtinFonts/noemoji/     OMIT_EMOJI_FONTS: primary fonts only, no emoji and no PHM CJK
#   builtinFonts/nophm/       OMIT_PHM only: emoji/symbol fallback, no PHM CJK
generate_reading_variant ../builtinFonts yes yes "default"
generate_reading_variant ../builtinFonts/noemoji no no "no-emoji"
generate_reading_variant ../builtinFonts/nophm yes no "no-PHM"

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

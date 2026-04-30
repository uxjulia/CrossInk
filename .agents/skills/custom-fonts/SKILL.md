# Generating or replacing a font workflow

1. Source font files live in `lib/EpdFont/builtinFonts/source/<FontName>/<FontName>-<Weight>.ttf`
2. Generate the headers by updating the script content at `lib/EpdFont/scripts/convert-builtin-fonts.sh` with the new font information. The pattern is as follows, where `myfont` and `MyFont` are replaced with the `<FontName>`

```
for size in 10 12 14 16; do
  for style in Regular Italic Bold BoldItalic; do
    name="myfont_${size}_$(echo $style | tr '[:upper:]' '[:lower:]')"
    $PYTHON fontconvert.py $name $size \
      ../builtinFonts/source/MyFont/MyFont-${style}.ttf \
      --2bit --compress \
      > ../builtinFonts/${name}.h
    echo "Generated ${name}.h"
  done
done
```

3. Update Font IDs by running the following script: `lib/EpdFont/scripts/build-font-ids.sh > src/fontIds.h`
4. Add includes for the new font to `lib/EpdFont/builtinFonts/all.h`
5. Add font objects to `src/main.cpp` for each font size, for example:

```
EpdFont myfont`10`RegularFont(&myfont_`10`_regular);
EpdFont myfont`10`BoldFont(&myfont_`10`_bold);
EpdFont myfont`10`ItalicFont(&myfont_`10`_italic);
EpdFont myfont`10`BoldItalicFont(&myfont_`10`_bolditalic);
EpdFontFamily myfont`10`FontFamily(&myfont`10`RegularFont, &myfont`10`BoldFont,
                                   &myfont`10`ItalicFont, &myfont`10`BoldItalicFont);
```

6. Register all font sizes further down in `src/main.cpp` (~line 216) with `renderer.insertFont(MYFONT_10_FONT_ID, myfont10FontFamily);`
7. Add the new FONT_FAMILY enum in `src/CrossPointSettings.h` (~line 95)
8. Add font ID mapping and line settings in `src/CrossPointSettings.cpp` (~line 312)
9. Add a new `StrId` for the font to `src/SettingsList.h`
10. Add a new translation string in `lib/I18n/translations/english.yaml`
11. Generate the new i18n file: `python3 scripts/gen_i18n.py lib/I18n/translations lib/I18n/`
12. Verify the binary size is under the 6,553,600 byte limit. If it is over the limit, notify the user and suggest removal of a font.

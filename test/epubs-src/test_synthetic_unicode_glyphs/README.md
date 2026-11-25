# Test: Synthetic Unicode Glyphs

Source files for [`test/epubs/test_synthetic_unicode_glyphs.epub`](../../epubs/test_synthetic_unicode_glyphs.epub).

This fixture covers the synthetic glyph paths added in commit `c4357d06e81e836c14126ff0fbd8adfc99e5da67`:

- `U+2588` FULL BLOCK
- `U+25A0` BLACK SQUARE
- `U+0393` GREEK CAPITAL GAMMA
- `U+03B5` GREEK SMALL EPSILON
- `U+03C9` GREEK SMALL OMEGA
- `U+02BB` MODIFIER LETTER TURNED COMMA, aliased to `U+2018`

Unicode spacing coverage includes the reported nested quotation, thin/hair/en/em
spaces, fractional em spaces, figure/punctuation/mathematical/ideographic spaces,
non-breaking spaces, and invisible spacing controls. Test with built-in fonts and
an SD font lacking these characters; widths should differ without boxes. Section
caches rebuild automatically after installing this fix.

Quick manual verification:

1. Flash or run the build you want to test.
2. Open `test_synthetic_unicode_glyphs.epub`.
3. Check chapter 1 for labeled single-glyph and mixed inline samples.
4. Check chapter 2 for dense block runs near line wraps.
5. Watch logs while opening/rendering chapter 2; synthetic glyphs should not flood `GFX !! Outside range` messages.

Rebuild the `.epub` after editing the source tree:

```sh
cd test/epubs-src/test_synthetic_unicode_glyphs
rm -f ../../epubs/test_synthetic_unicode_glyphs.epub
zip -X0 ../../epubs/test_synthetic_unicode_glyphs.epub mimetype
zip -Xr9D ../../epubs/test_synthetic_unicode_glyphs.epub META-INF OEBPS
```

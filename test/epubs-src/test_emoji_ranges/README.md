# Test: Emoji Ranges

Source files for [`test/epubs/test_emoji_ranges.epub`](../../epubs/test_emoji_ranges.epub).

This fixture mirrors the current extra font intervals from [`lib/EpdFont/scripts/convert-builtin-fonts.sh`](../../../lib/EpdFont/scripts/convert-builtin-fonts.sh):

- `U+2669-U+266F` music symbols and accidentals
- `U+1F600-U+1F64F` emoticons
- `U+1F44B-U+1F44F` hand gestures
- `U+2764` heart
- `U+03BB` Greek lambda
- targeted Cyrillic ranges used by the problem book
- `U+2113` script small l

Quick manual verification:

1. Regenerate fonts and flash the build you want to test.
2. Open `test_emoji_ranges.epub`.
3. Check chapter 2 for the small symbol slices: music, hands, and heart.
4. Check chapter 3 for the full emoticons block.
5. Check chapter 4 for lambda, the targeted Cyrillic subset, and `ℓ`.
6. If a glyph disappears after a font change, the matching `U+XXXX` label makes it easy to map back to the interval.

Rebuild the `.epub` after editing the source tree:

```sh
cd test/epubs-src/test_emoji_ranges
rm -f ../../epubs/test_emoji_ranges.epub
zip -X0 ../../epubs/test_emoji_ranges.epub mimetype
zip -Xr9D ../../epubs/test_emoji_ranges.epub META-INF OEBPS
```

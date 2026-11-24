# Supplemental hyphenation packs

These checked-in binary tries and UTF-8 pattern sources are pinned to
[Typst Hypher d81cc506416bffef2a75ee2dc969d4b41bc36613](https://github.com/typst/hypher/tree/d81cc506416bffef2a75ee2dc969d4b41bc36613)
and correspond byte-for-byte to the CPHY v1 packs in the
[CrossPoint hyphenation asset archive](https://github.com/crosspoint-reader/crosspoint-assets/releases/tag/hyphenation-v1-d81cc50).
The source-to-trie map and SHA-256 digests are in `packs.json`. CrossInk wraps
the pinned trie with the pack's existing language code, 2/2 minima, root
offset and CRC-32; it does not change the trie payload.

The pattern files carry the original authorship and license terms alongside
Hypher's MIT and Apache notices. Belarusian uses the MIT license. Mongolian
is redistributed under its MIT option. The included LPPL
1.3 and 1.3c texts accompany the sources allowing those versions. Serbian uses
the Cyrillic pattern file, Finnish uses the school variant, and Norwegian
maps to `no`.

To regenerate the tries, check out the pinned Hypher revision and run
`rustc --edition 2021 --test -O tests/generate.rs -o hypher-generate`, then
`./hypher-generate --exact generate_code`. Use the exact named source in
`packs.json` for each code. The build uses the pinned files without a
network fetch or Rust toolchain.

Finnish uses the same CPHY bytes in the release ZIP and both download catalogs.
The Dutch sources and trie are maintained separately in
[`assets/hyphenation-v2`](../hyphenation-v2/README.md).

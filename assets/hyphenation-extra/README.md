# Additional SD hyphenation packs

The tries and corresponding UTF-8 pattern sources are from
[Typst Hypher d81cc506416bffef2a75ee2dc969d4b41bc36613](https://github.com/typst/hypher/tree/d81cc506416bffef2a75ee2dc969d4b41bc36613),
also used by the [CrossPoint asset archive](https://github.com/crosspoint-reader/crosspoint-assets/releases/tag/hyphenation-v1-d81cc50).
The Dutch pattern source is from [TeX hyph-utf8 ecf976ab](https://github.com/hyphenation/tex-hyphen/blob/ecf976ab6995acb653d38ab1af0b9b9829ec0c77/hyph-utf8/tex/generic/hyph-utf8/patterns/tex/hyph-nl.tex),
with its full MIT notice. It generates the same Dutch trie byte for byte.
See `packs.json` for the source-to-trie mapping and SHA-256 checksums.

To regenerate the tries, check out that Hypher revision, use these exact
pattern files in its `patterns/` directory, then run its generator from
the checkout:

```sh
rustc --edition 2021 --test -O tests/generate.rs -o hypher-generate
./hypher-generate --exact generate_code
```

`scripts/package_languages.py` uses the pinned, checked-in tries to build
the release ZIP without a network fetch or Rust toolchain. The CPHY v1
wrapper leaves the serialized trie payload unchanged, adjusts its root
offset to account for the replaced header, and adds the language code,
2/2 minima, length and CRC-32. Finnish is also offered over Wi-Fi through
its own catalog, using the same pack bytes. The legacy Wi-Fi catalog and
its nine existing packs are not changed. The other extra packs remain SD-only.

Each pattern source carries the original authorship, license and notices.
The separate Hypher MIT and Apache licenses in `hyphenation-sources/` do
not replace those per-pattern terms. The included LPPL 1.3 text applies to
the Kurmanji source, and LPPL 1.3c is included for the Catalan, Croatian, and
Icelandic sources that allow later versions. Finnish uses the school variant.
The Catalan source retains Hypher’s
modification notice and its link to the original.

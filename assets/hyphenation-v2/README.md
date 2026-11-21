# Dutch hyphenation pattern source

`patterns/hyph-nl.tex` is the unmodified
[TeX hyph-utf8 Dutch pattern source](https://github.com/hyphenation/tex-hyphen/blob/ecf976ab6995acb653d38ab1af0b9b9829ec0c77/hyph-utf8/tex/generic/hyph-utf8/patterns/tex/hyph-nl.tex)
at commit `ecf976ab6995acb653d38ab1af0b9b9829ec0c77`. The Dutch patterns are
Copyright (C) 1996 Piet Tutelaers, licensed under the MIT License (included).
The pattern source SHA-256 is
`d21499bfbee53e4d50e867ef92a41fd28b65725d7c91c980e4f8f02889bf6b2a`.

`tries/nl.bin` was built from that source using
[Typst Hypher](https://github.com/typst/hypher/tree/d81cc506416bffef2a75ee2dc969d4b41bc36613)
at commit `d81cc506416bffef2a75ee2dc969d4b41bc36613`. In that Hypher checkout,
replace `patterns/hyph-nl.tex` with this exact file, then run:

```sh
rustc --edition 2021 --test -O tests/generate.rs -o ../generate
../generate --exact generate_code
```

The resulting `tries/nl.bin` has SHA-256
`1177ef126b31bbb30f080633680fc14cc4cd6f56d1597981c975389b861d978b`.
It matches Hypher's previously generated Dutch trie byte-for-byte. Hypher's
code is available under MIT or Apache 2.0; its licenses do not replace the
separate MIT copyright notice for the Dutch patterns.

CrossInk removes the four-byte big-endian root offset, subtracts those four
bytes from the offset, and writes a CPHY v1 header with code `nl`, 2/2 minima,
adjusted offset, payload size, and CRC-32. The Dutch pack remains an external
asset. The unchanged legacy nine-language source set is in
`assets/hyphenation/`.

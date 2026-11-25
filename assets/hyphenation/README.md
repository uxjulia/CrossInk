# Hyphenation pattern sources

These unmodified sources accompany the nine external language dictionaries.
They come from [Typst Hypher](https://github.com/typst/hypher) at
`d81cc506416bffef2a75ee2dc969d4b41bc36613`, also used by CrossPoint's
`hyphenation-v1-d81cc50` assets. Their compiled payloads match CrossInk's existing
generated tables byte for byte.

Each `patterns/*.tex` file contains its own authorship, copyright and license
terms. Those per-pattern terms apply to the corresponding dictionary. Hypher's
MIT and Apache licenses are included separately; they do not replace the
individual pattern licenses.

CrossInk packages these sources and notices alongside the CPHY v1 files. The
CPHY container and external-provider approach are adapted from CrossPoint Reader
PR #3706 by Uri Tauber, under the CrossPoint Reader MIT license.

When refreshing generated tables, update these matching sources and preserve
their license notices. Packaging uses the checked-in tables, without silently
fetching newer upstream dictionaries.

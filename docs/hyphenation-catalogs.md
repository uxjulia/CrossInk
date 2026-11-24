# Hyphenation download catalog compatibility

Hyphenation packs remain CPHY v1 files. Dutch is an optional external
`hyph-nl.cphyph` pack; it is not embedded in firmware. Online discovery has
separate catalogs on both the stable and RC channels:

| Catalog path under the language-assets prefix | JSON schema | Codes |
| --- | --- | --- |
| `catalogs/{stable,rc}/hyphenation.json` | 1 | de, es, fr, it, pl, pt, ru, sv, uk |
| `catalogs/{stable,rc}/hyphenation-v2.json` | 2 | Same nine, plus nl |
| `catalogs/{stable,rc}/hyphenation-supplement.json` | 3 | be, bg, ca, cs, da, fi, gl, hr, is, ku, la, lt, mn, no, sl, sq, sr, tr |

The schema change only marks the new discovery contract. The pack format
is still 1 and snapshot-relative payload, license and source paths are
unchanged. Old firmware rejects unknown language codes and more than nine
entries, so never add Dutch to the legacy catalog. Leave its existing
pointer bytes and referenced snapshot intact. Current Inky clients continue
to discover the legacy GitHub export until they opt into the new catalog.

The firmware requests the supplemental catalog directly only when a
supplemental language is selected. A missing supplemental catalog does not
fall back, because those languages are not in either older catalog. The
existing nine languages and Dutch still request the extended catalog. If that URL returns
HTTP 404 or 403 (S3 can return 403 for an absent public object), it may
retry the legacy catalog once for an existing language,
never for Dutch. A malformed catalog or any other transport failure does
not trigger fallback. Publish the extended asset snapshot and catalog for
the exact release channel before making its firmware public. S3 publication
prepares all three discovery catalogs for firmware. The standalone GitHub
catalog export for Inky stays unchanged, while the release language ZIP
includes the existing and supplemental packs for offline installation.
The release/v2.0.0 Finnish-only catalog, where applicable, is a separate
compatibility URL and is not rewritten by this publisher. Local staging
does not publish any assets.

For the pattern source and exact trie generation, see
[the Dutch source notice](../assets/hyphenation-v2/README.md) and
[the supplemental source notice](../assets/hyphenation-extra/README.md).

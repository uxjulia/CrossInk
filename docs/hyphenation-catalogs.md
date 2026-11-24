# Hyphenation download catalog compatibility

Hyphenation packs remain CPHY v1 files. Dutch is an optional external
`hyph-nl.cphyph` pack; it is not embedded in firmware. Online discovery has
separate catalogs on both the stable and RC channels:

| Catalog path under the language-assets prefix | JSON schema | Codes |
| --- | --- | --- |
| `catalogs/{stable,rc}/hyphenation.json` | 1 | de, es, fr, it, pl, pt, ru, sv, uk |
| `catalogs/{stable,rc}/hyphenation/fi.json` | 1 | fi |
| `catalogs/{stable,rc}/hyphenation-v2.json` | 2 | Same nine, plus nl |
| `catalogs/{stable,rc}/hyphenation-supplement.json` | 3 | be, bg, ca, cs, da, fi, gl, hr, is, ku, la, lt, mn, no, sl, sq, sr, tr |

The schema change only marks the new discovery contract. The pack format
is still 1 and snapshot-relative payload, license and source paths are
unchanged. Old firmware rejects unknown language codes and more than nine
entries, so never add Dutch to the legacy catalog. Leave its existing
pointer bytes and referenced snapshot intact. Current Inky clients continue
to discover the legacy GitHub export until they opt into the new catalog.

The firmware requests the supplemental catalog when a supplemental language
is selected. Finnish can retry its release compatibility URL if the catalog
returns HTTP 404 or 403. The original nine languages and Dutch request the
extended catalog. The same absence responses allow the original nine languages
to retry the legacy catalog. A malformed catalog or other transport failure
ends the attempt.

Publish the asset snapshots and catalogs for the exact release channel before
making its firmware public. The publisher creates the two schema-1 pointers
only if absent and preserves their existing snapshots. The release language ZIP
includes the existing and supplemental packs for offline installation. Inky
continues to use the separate GitHub catalog export. Local staging does not
publish any assets.

For the pattern source and exact trie generation, see
[the Dutch source notice](../assets/hyphenation-v2/README.md) and
[the supplemental source notice](../assets/hyphenation-extra/README.md).

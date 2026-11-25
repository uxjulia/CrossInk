# Font fixtures

FontAwesome.otf is the unmodified Font Awesome 4.7.0 OpenType/CFF font from
Ubuntu's fonts-font-awesome package. Its font license is SIL OFL 1.1;
see FontAwesome-LICENSE.txt. It is a real OpenType CFF fixture, also copied under a `.ttf` name.

The positive TrueType-outline .otf fixture is materialized by the test from
lib/EpdFont/scalableFonts/bitter_regular.ttf (and the other three styles),
with the same bytes under .otf/.OTF/.TtF names. Those committed static fonts
and their licenses are reused rather than duplicating them. The test checks
actual inspection, registration, resident/streamed loading, and nonempty glyph
pixels through the pinned SDK, not just extension or magic acceptance.
Malformed, CFF2-tagged, variable-tagged and collection inputs are mutations,
not claims of complete real-world font-format coverage.

# Test: Reader Rendering Matrix

This fixture exercises common EPUB text rendering scenarios in one book:

1. Paragraph alignment: left, center, right, justify, and inherited/book CSS alignment.
2. Paragraph spacing and indents: positive first-line indent, zero indent, hanging indent, block margins, and nested indentation.
3. Inline styling: bold, italic, bold italic, underline, strikethrough, nested spans, and mixed styles in one paragraph.
4. Line breaking: long words, soft hyphens, nonbreaking spaces, punctuation attachment, and guide-dot spacing.
5. Page breaking: explicit page-break-before and page-break-after boundaries.
6. Lists, headings, blockquotes, preformatted text, and basic tables.

Build from this directory with the same EPUB packaging pattern used by the other fixtures:

```sh
rm -f ../../epubs/test_reader_rendering_matrix.epub
zip -X0 ../../epubs/test_reader_rendering_matrix.epub mimetype
zip -Xr9D ../../epubs/test_reader_rendering_matrix.epub META-INF OEBPS
```


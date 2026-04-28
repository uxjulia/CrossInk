# Test: Force Paragraph Indents

Source files for [`test/epubs/test_force_paragraph_indents.epub`](../../epubs/test_force_paragraph_indents.epub).

Quick manual verification:

1. Set `Extra Paragraph Spacing` to `ON`.
2. Open chapter 2.
3. Compare `Force Paragraph Indents` set to `OFF` vs `ON`.
4. With `OFF`, the test paragraphs should stay flush left.
5. With `ON`, the first line of the target paragraphs should gain a visible indent.
6. Chapter 3 is the guardrail check: centered, right-aligned, and hanging-indent paragraphs should not be broken.
7. Chapter 4 is the optional embedded-style-off check.

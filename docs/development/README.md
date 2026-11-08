---
title: Development
nav_order: 2
has_children: true
---

# Development Docs

This section contains technical information for building, testing, and understanding CrossInk.
It is written for software developers who may be new to embedded development.

- [Getting Started](./getting-started.md)
- [Architecture Overview](./architecture.md)
- [Testing and Debugging](./testing-debugging.md)
- [Icon Libraries](./icons.md)
- [Font Development and Conversion](./font-development.md)
- [Dictionary Development Guide](../dictionary-development.md)
- [File formats](../file-formats.md)
- [Web server endpoints](../webserver-endpoints.md)
- [Plugin Development Guide](./plugin-development.md)
- [Picco Hardware and Validation](./picco-hardware.md)
- [Local X4 Pro firmware testing](../x4pro-local-testing.md)

If you are new, start with [Getting Started](./getting-started.md).

## Preparing release downloads

**Compile Release** and **Compile Release Candidate** prepare language and
hyphenation downloads on S3 and in `crossink-assets` before creating the firmware
draft. Wait for the entire compile workflow to succeed before publishing the
draft. Failed language uploads fail the compile workflow and prevent draft
creation; publishing the release no longer starts a separate language upload.

Both destinations use the exact firmware source commit and release version.
The release ZIP, editor template and online downloads therefore share the same
language contract. Existing compatible catalogs and immutable snapshots remain
available to older firmware.

To retry asset preparation separately, run **Prepare language download assets**
with the intended release tag and full 40-character source commit. This does
not create or publish firmware. Select that same tag in the workflow's **Use
workflow from** control. The compile workflow creates the tag before preparing
downloads and retains it on failure so retries cannot silently change its
source. The tag is visible before the firmware draft; it has no firmware assets
until the remaining release steps succeed. This also keeps the existing
`language-assets` environment's tag restrictions intact.

Merge the workflow and script updates into the default branch before using this
flow, so GitHub can dispatch the language workflow. After fixing a failed
preparation, rerun the compile workflow to finish creating the draft. An already-created draft from an
earlier run is not removed; confirm the current compile run succeeded before
publishing it.

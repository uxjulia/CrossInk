# Changelog

All notable changes to this project will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),

## [Unreleased]
### Changed


### Added
- New Lyra Carousel theme that displays 3 of your most recent book covers
- Prevent a crash when entering sleep with Page Overlay enabled if the cached EPUB page data is invalid
- Clean up EPUB table rendering by removing synthetic row/cell labels and defaulting table cells to readable left alignment
- Allow simple EPUB tables with full-width note rows so a single `colspan` cell spanning the whole table no longer forces the entire table back to paragraph fallback

### Fixed
- Fix a crash when using `Go to %` in EPUBs by serializing the jump calculation with other reader cache access
- Fix OTA update checks after the streaming release parser merge by keeping variant-aware firmware asset matching

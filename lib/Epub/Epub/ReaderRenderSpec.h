#pragma once
#include <cstdint>

#include "EpubRenderMode.h"
#include "FocusReading.h"

enum class PublisherFonts : uint8_t { Off, Accents, All };

// The resolved text-rendering configuration a reader hands to the layout
// engine. Section-cache validation keys on every field: a section file built
// with a different spec is discarded and rebuilt.
//
// Build one via CrossPointSettings::readerRenderSpec(width, height), which
// fills every field: the settings-derived ones from the store, the viewport
// from the caller. Taking the viewport as arguments is what keeps a spec from
// existing in a half-filled state — the 0 defaults below are a last-resort
// backstop (a 0x0 viewport lays out nothing), not an invitation to omit it.
struct ReaderRenderSpec {
  int fontId = 0;
  float lineCompression = 1.0f;
  bool extraParagraphSpacing = false;
  bool forceParagraphIndents = false;
  uint8_t paragraphAlignment = 0;
  uint16_t viewportWidth = 0;
  uint16_t viewportHeight = 0;
  bool hyphenationEnabled = false;
  bool embeddedStyle = true;
  uint8_t imageRendering = 0;
  bool focusReadingEnabled = false;
  bool guideReadingEnabled = false;
  uint8_t wordSpacing = 0;
  int8_t characterSpacing = 0;
  bool ruled = false;
  PublisherFonts publisherFonts = PublisherFonts::Off;
  uint32_t publisherFontSignature = 0;
  EpubRenderMode renderMode = EpubRenderMode::CrossInkDefault;
};

inline uint32_t readerRenderSpecSignature(const ReaderRenderSpec& spec) {
  uint32_t signature = 2166136261U;
  const auto mix = [&signature](const uint32_t value) {
    signature ^= value;
    signature *= 16777619U;
  };
  mix(static_cast<uint32_t>(spec.fontId));
  mix(static_cast<uint32_t>(spec.lineCompression * 1000.0f));
  mix(spec.extraParagraphSpacing);
  mix(spec.forceParagraphIndents);
  mix(spec.paragraphAlignment);
  mix(spec.viewportWidth);
  mix(spec.viewportHeight);
  mix(spec.hyphenationEnabled);
  mix(spec.embeddedStyle);
  mix(spec.imageRendering);
  mix(spec.focusReadingEnabled);
  if (spec.focusReadingEnabled) mix(FocusReading::LAYOUT_IDENTITY);
  mix(spec.guideReadingEnabled);
  mix(spec.wordSpacing);
  mix(static_cast<uint8_t>(spec.characterSpacing));
  mix(static_cast<uint8_t>(spec.renderMode));
  if (spec.ruled) mix(0x52554C34u);  // Preserve existing signatures for unruled books.
  if (spec.publisherFonts != PublisherFonts::Off) {
    mix(static_cast<uint8_t>(spec.publisherFonts));
    mix(spec.publisherFontSignature);
  }
  return signature == 0 ? 1 : signature;
}

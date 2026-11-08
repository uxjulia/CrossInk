#pragma once

#include <I18nKeys.h>

#include <cstddef>
#include <cstdint>

// Bump RELEASE_NOTES_REVISION whenever the notes table below changes for a new release.
// Everyone whose saved revision is lower (including fresh installs) sees What's New once.
// First published notes revision (v1.6.1 had no notes).
constexpr uint16_t RELEASE_NOTES_REVISION = 1;
// Label shown in the header. Deliberately not CROSSINK_VERSION, so a patch build keeps the notes' label.
constexpr char RELEASE_NOTES_VERSION[] = "2.0.0";
constexpr char RELEASE_NOTES_CHANGELOG_URL[] = "github.com/uxjulia/CrossInk/releases";

constexpr size_t RELEASE_NOTES_MAX_PATH = 5;

struct ReleaseNoteItem {
  StrId title;
  StrId body;           // StrId::_COUNT when the item has no body text
  const StrId* path;    // Menu path shown as a boxed label; nullptr when absent
  uint8_t pathLength;   // Number of StrIds in path
  const char* literal;  // Optional untranslated line (for example a URL)
};

extern const ReleaseNoteItem RELEASE_NOTES[];
extern const size_t RELEASE_NOTES_COUNT;

// True when at least one string in the current notes is missing from the active UI language.
// Translations identical to English are stored as missing, so every note string must read
// differently from English in each language, or that language is always offered Update.
bool releaseNotesNeedLanguageUpdate();
// True when What's New should offer a one-click download of the saved UI language: it is a downloadable
// non-English language, and it is either not the active language (for example after upgrading from firmware
// with built-in translations) or its pack lacks the current notes. updateTried guards against offering it forever.
bool releaseNotesOfferLanguageUpdate(const char* preferredCode, const char* activeCode, uint16_t updateTried);

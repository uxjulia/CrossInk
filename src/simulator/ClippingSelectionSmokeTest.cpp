#if defined(SIMULATOR) && CROSSINK_APP_CAP_TOUCH
#include "ClippingSelectionSmokeTest.h"

#include <Arduino.h>
#include <Logging.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <tuple>

#include "ClippingStore.h"
#include "Epub/ParsedText.h"
#include "Epub/Section.h"
#include "SdCardFontSystem.h"
#include "activities/ActivityManager.h"
#include "activities/reader/DictionaryWordSelectActivity.h"
#include "activities/reader/EpubReaderActivity.h"

extern ActivityManager activityManager;
extern GfxRenderer renderer;
extern MappedInputManager mappedInputManager;

namespace {
void require(bool condition, const char* message) {
  if (condition) return;
  LOG_ERR("SMOKE", "Clipping selection: %s", message);
  std::_Exit(1);
}

void capture(const char* name) {
  const char* directory = std::getenv("CROSSINK_SIMULATOR_SMOKE_STATUS_BAR_CAPTURES");
  if (!directory) return;
  std::filesystem::create_directories(directory);
  const auto path = std::filesystem::path(directory) / (std::string(name) + ".pgm");
  FILE* output = std::fopen(path.c_str(), "wb");
  require(output, "Cannot create screenshot");
  RenderLock lock;
  const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
  std::fprintf(output, "P5\n%d %d\n255\n", width, height);
  for (int y = 0; y < height; ++y)
    for (int x = 0; x < width; ++x) std::fputc(renderer.isPixelBlack(x, y) ? 0 : 255, output);
  std::fclose(output);
}
bool overlaps(const TextSelectionOverlay::Rect a, const TextSelectionOverlay::Rect b) {
  return !a.empty() && !b.empty() && a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}

void verifyOverlayBounds() {
  RenderLock lock;
  const auto orientation = renderer.getOrientation();
  const auto scale = SETTINGS.uiScale;
  for (int rotation = 0; rotation < 4; ++rotation) {
    renderer.setOrientation(static_cast<GfxRenderer::Orientation>(rotation));
    for (int size = 0; size < CrossPointSettings::UI_SCALE_COUNT; ++size) {
      SETTINGS.uiScale = static_cast<CrossPointSettings::UI_SCALE>(size);
      const auto safe = TextSelectionOverlay::safeArea(renderer);
      const auto inside = [&](const TextSelectionOverlay::Rect rect) {
        return rect.empty() ||
               (rect.x >= safe.x && rect.right() <= safe.right() && rect.y >= safe.y && rect.bottom() <= safe.bottom());
      };
      for (const auto point : {TextSelectionOverlay::Rect{safe.x, safe.y, 1, 1},
                               TextSelectionOverlay::Rect{safe.right(), safe.bottom(), 1, 1}}) {
        for (const bool lookup : {false, true}) {
          TextSelectionOverlay::Layout layout;
          TextSelectionOverlay::placeActions(renderer, layout, point, point, lookup);
          require(inside(layout.bar), "Scaled or rotated toolbar escaped usable screen bounds");
          for (const auto& button : layout.actions) {
            require(inside(button), "Scaled or rotated toolbar escaped usable screen bounds");
          }
          require(layout.actions[TextSelectionOverlay::lookupAction].empty() != lookup,
                  "Look Up visibility does not follow dictionary availability");
        }
        for (const bool left : {false, true}) {
          for (const bool isStart : {false, true}) {
            const auto handle = TextSelectionOverlay::handle(safe, point, left, isStart);
            require(inside(handle.stem) && inside(handle.dot) && inside(handle.hit),
                    "Selection handle escaped usable screen bounds");
          }
        }
      }
      // Placement rules: above when there is room, otherwise below; never on the word.
      const int16_t lineHeight = static_cast<int16_t>(renderer.getLineHeight(SETTINGS.getReaderFontId()));
      for (const int16_t wordY : {safe.y, static_cast<int16_t>(safe.y + safe.height / 2)}) {
        const TextSelectionOverlay::Rect word{static_cast<int16_t>(safe.x + safe.width / 3), wordY, 40, lineHeight};
        TextSelectionOverlay::Layout layout;
        layout.start = TextSelectionOverlay::handle(safe, word, true, true);
        layout.end = TextSelectionOverlay::handle(safe, word, false, false);
        TextSelectionOverlay::placeActions(renderer, layout, word, word, true);
        const bool atTop = wordY == safe.y;
        require(layout.showPointer && layout.pointerUp == atTop, "Toolbar did not flip below a top-edge selection");
        require(atTop ? layout.bar.y >= layout.end.dot.bottom() : layout.bar.bottom() <= layout.start.dot.y,
                "Toolbar overlaps the selection or its handles");
        require(!overlaps(layout.start.hit, layout.end.hit), "Start and end handle touch areas overlap");
      }
      const int16_t middleX = static_cast<int16_t>(safe.x + safe.width / 2 - 20);
      for (const int16_t firstX : {static_cast<int16_t>(safe.x + safe.width / 3), middleX}) {
        // Fallback: a selection from the first line to the last leaves no room
        // above or below, but the bar must still leave both handles reachable,
        // including when both handles sit mid-screen.
        const TextSelectionOverlay::Rect first{firstX, safe.y, 40, lineHeight};
        const TextSelectionOverlay::Rect last{middleX, static_cast<int16_t>(safe.bottom() - lineHeight), 40,
                                              lineHeight};
        TextSelectionOverlay::Layout layout;
        layout.start = TextSelectionOverlay::handle(safe, first, true, true);
        layout.end = TextSelectionOverlay::handle(safe, last, false, false);
        TextSelectionOverlay::placeActions(renderer, layout, first, last, true);
        require(inside(layout.bar) && !layout.showPointer, "Fallback toolbar placement escaped the screen");
        for (const auto* handle : {&layout.start, &layout.end}) {
          require(!overlaps(layout.bar, handle->dot) && !overlaps(layout.bar, handle->stem),
                  "Fallback toolbar hides a drag handle");
        }
      }
    }
  }
  SETTINGS.uiScale = scale;
  renderer.setOrientation(orientation);
}
}  // namespace

void ClippingSelectionSmokeTest::verifyHighlightAnchoring(EpubReaderActivity& reader) {
  require(!CLIPPINGS.hasClippings(), "Anchoring test requires an empty clipping store");
  require(reader.section && reader.section->pageCount > 1, "Missing anchoring test section");
  const auto currentPage = static_cast<uint16_t>(reader.section->currentPage);
  const auto pageCount = static_cast<uint16_t>(reader.section->pageCount);
  const auto readerSignature = reader.section->layoutSignature();
  const auto signature = clippingWordLayoutSignature(readerSignature);
  const int fontId = SETTINGS.getReaderFontId();
  const int lineHeight = renderer.getLineHeight(fontId) * 2;
  Page page;
  const std::vector<std::string> words{"best", "in", "the", "world"};
  const std::vector<EpdFontFamily::Style> styles(4, EpdFontFamily::REGULAR);
  std::vector<int16_t> positions;
  positions.reserve(words.size());
  page.elements.reserve(4);
  int x = 0;
  for (const auto& word : words) {
    positions.push_back(x);
    x += renderer.getTextAdvanceX(fontId, word.c_str(), EpdFontFamily::REGULAR) + 12;
  }
  // The same phrase occurs four times; only its second occurrence is clipped.
  for (int row = 0; row < 4; ++row) {
    auto block =
        std::make_shared<TextBlock>(words, positions, styles, std::vector<uint8_t>{}, std::vector<uint16_t>{},
                                    std::vector<uint16_t>{}, std::vector<uint8_t>{}, std::vector<bool>{}, BlockStyle{});
    page.elements.push_back(std::make_unique<PageLine>(std::move(block), 24, 40 + row * lineHeight));
  }
  struct Case {
    const char* name;
    uint32_t signature;
    uint16_t page;
    bool marked;
    const char* text = "best in the world";
    int8_t paginationDelta = 0;
  };
  const uint16_t otherPage = currentPage == 0 ? 1 : 0;
  for (const auto& test :
       {Case{"clip-anchor-exact", signature, currentPage, true},
        Case{"clip-anchor-other-page", signature, otherPage, false},
        Case{"clip-anchor-stale-layout", signature ^ 1U, currentPage, false},
        Case{"clip-anchor-unsigned", 0, currentPage, false},
        Case{"clip-anchor-legacy", readerSignature, currentPage, false},
        Case{"clip-anchor-stale-other-page", signature ^ 1U, otherPage, false},
        Case{"clip-anchor-partial-phrase", signature ^ 1U, currentPage, false, "intro best in the world"},
        Case{"clip-anchor-restored", signature, currentPage, true},
        Case{"clip-anchor-pagination-growth", signature, currentPage, true, "best in the world", 1},
        Case{"clip-anchor-cache-rebuild", signature, currentPage, true, "best in the world", -1}}) {
    require(CLIPPINGS.addClipping(reader.currentSpineIndex, test.page, test.page, pageCount, 4, 7, 4, "", UINT16_MAX,
                                  test.text, UINT16_MAX, test.signature) == ClippingStore::AddResult::Added,
            "Could not save anchoring test range");
    {
      RenderLock lock;
      renderer.clearScreen();
      page.renderText(renderer, fontId, 0, 0, true);
      const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
      std::vector<uint8_t> before(static_cast<size_t>(width) * height);
      for (int y = 0; y < height; ++y)
        for (int px = 0; px < width; ++px) before[y * width + px] = renderer.isPixelBlack(px, y);
      // Model appended pages and a shorter readable prefix after cache deletion.
      reader.section->pageCount = pageCount + test.paginationDelta;
      reader.drawClippingHighlights(page, fontId, 0, 0);
      reader.section->pageCount = pageCount;
      size_t changed = 0;
      const int selectedY = 40 + lineHeight;
      const int bandHeight = (renderer.getLineHeight(fontId) * SETTINGS.lineHeightPercent + 50) / 100;
      for (int y = 0; y < height; ++y) {
        for (int px = 0; px < width; ++px) {
          if (before[y * width + px] == renderer.isPixelBlack(px, y)) continue;
          ++changed;
          require(test.marked && y >= selectedY && y < selectedY + bandHeight,
                  "Highlight escaped its exact saved occurrence");
        }
      }
      require(test.marked ? changed > 100 : changed == 0, "Unexpected anchored marker pixels");
    }
    capture(test.name);
    LOG_INF("SMOKE", "Highlight anchoring passed: %s", test.name);
    require(CLIPPINGS.removeClippingAt(0), "Could not remove anchoring test range");
  }
  reader.requestUpdate();
}

void ClippingSelectionSmokeTest::verifyStableHighlightAnchoring(EpubReaderActivity& reader) {
  require(!CLIPPINGS.hasClippings(), "Stable anchor test requires an empty clipping store");
  require(CLIPPINGS.addClipping(reader.currentSpineIndex, 0, 0, 1, 4, 7, 4, "", UINT16_MAX, "best in the world",
                                UINT16_MAX, 0, 18, 35) == ClippingStore::AddResult::Added,
          "Could not save stable highlight");
  // Exercise the actual saved format before reflowing the passage.
  CLIPPINGS.unload();
  require(CLIPPINGS.loadForBook(reader.epub->getPath(), reader.epub->getTitle(), reader.epub->getAuthor(), "epub"),
          "Could not reload stable highlight");
  const auto oldFamily = SETTINGS.fontFamily;
  const auto oldSize = SETTINGS.readerFontPointSize;
  const auto oldOrientation = renderer.getOrientation();
  for (int variant = 0; variant < 4; ++variant) {
    size_t markedWords = 0;
    {
      RenderLock lock;
      SETTINGS.fontFamily = variant % 2 ? CrossPointSettings::BITTER : CrossPointSettings::LEXENDDECA;
      SETTINGS.readerFontPointSize = variant < 2 ? 12 : 18;
      renderer.setOrientation(variant < 2 ? GfxRenderer::Portrait : GfxRenderer::LandscapeClockwise);
      const int fontId = sdFontSystem.ensureBuiltInReaderFont(renderer);
      require(renderer.getFontMap().contains(fontId), "Reflow font was not loaded");
      ParsedText text(false, false, true, variant == 2, variant == 3);
      for (uint32_t repeat = 0; repeat < 4; ++repeat) {
        uint32_t source = repeat * 18;
        for (const std::string word : {"best", "in", "the", "world"}) {
          text.addWord(word, EpdFontFamily::REGULAR, false, false, false, 0, source);
          source += word.size() + 1;
        }
      }
      Page page;
      page.elements.reserve(16);
      int y = 40;
      require(text.layoutAndExtractLines(renderer, fontId, variant % 2 ? 360 : 220,
                                         [&](std::shared_ptr<TextBlock> line, uint32_t, uint32_t) {
                                           page.elements.push_back(std::make_unique<PageLine>(std::move(line), 24, y));
                                           y += renderer.getLineHeight(fontId);
                                         }),
              "Could not reflow stable highlight fixture");
      renderer.clearScreen();
      page.renderText(renderer, fontId, 0, 0, true);
      const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
      std::vector<uint8_t> before(static_cast<size_t>(width) * height);
      for (int py = 0; py < height; ++py)
        for (int px = 0; px < width; ++px) before[py * width + px] = renderer.isPixelBlack(px, py);
      reader.drawClippingHighlights(page, fontId, 0, 0);
      std::tuple<const std::vector<uint8_t>&, int, int, size_t&> context{before, width, fontId, markedWords};
      page.forEachTextLine(
          [](const PageTextLine& line, void* raw) {
            auto& context = *static_cast<std::tuple<const std::vector<uint8_t>&, int, int, size_t&>*>(raw);
            const auto& before = std::get<0>(context);
            const int width = std::get<1>(context), fontId = std::get<2>(context);
            for (uint16_t i = 0; i < line.block->wordCount(); ++i) {
              const auto source = line.block->wordSourceRange(i);
              const bool selected = source.valid() && source.start < 35 && source.end > 18;
              const int x = line.xPos + line.block->wordXpos(i);
              const int wordWidth = renderer.getTextAdvanceX(fontId, line.block->wordText(i), line.block->wordStyle(i));
              size_t changed = 0;
              for (int py = line.yPos; py < line.yPos + renderer.getLineHeight(fontId); ++py)
                for (int px = x; px < x + wordWidth; ++px)
                  if (before[py * width + px] != renderer.isPixelBlack(px, py)) ++changed;
              require(selected ? changed > 5 : changed == 0,
                      "Stable marker moved to a different occurrence after reflow");
              if (selected) ++std::get<3>(context);
            }
            return true;
          },
          &context);
      require(markedWords == 4, "Reflow lost a word of the anchored passage");
      if (variant == 0) {
        require(CLIPPINGS.removeClippingAt(0), "Could not replace migration fixture");
        const auto currentPage = static_cast<uint16_t>(reader.section->currentPage);
        require(CLIPPINGS.addClipping(reader.currentSpineIndex, currentPage, currentPage, reader.section->pageCount, 4,
                                      7, 4, "", UINT16_MAX, "best in the world", UINT16_MAX,
                                      clippingWordLayoutSignature(reader.section->layoutSignature())) ==
                    ClippingStore::AddResult::Added,
                "Could not save old-coordinate migration fixture");
        reader.drawClippingHighlights(page, fontId, 0, 0);
        require(CLIPPINGS.clippingAt(0)->sourceStart == 18 && CLIPPINGS.clippingAt(0)->sourceEnd == 35,
                "Exact old clipping did not acquire stable source endpoints");
        require(CLIPPINGS.saveToFile(), "Could not persist migrated source endpoints");
      }
    }
    char name[40];
    snprintf(name, sizeof(name), "clip-stable-reflow-%d", variant);
    capture(name);
    LOG_INF("SMOKE", "Stable highlight passed: %s", name);
  }
  SETTINGS.fontFamily = oldFamily;
  SETTINGS.readerFontPointSize = oldSize;
  renderer.setOrientation(oldOrientation);
  require(CLIPPINGS.removeClippingAt(0), "Could not remove stable highlight test range");
  reader.requestUpdate();
}

void ClippingSelectionSmokeTest::verifySavedHighlightSpacing(EpubReaderActivity& reader) {
  const auto oldSpacing = SETTINGS.lineHeightPercent;
  const int fontId = SETTINGS.getReaderFontId();
  const int fontHeight = renderer.getLineHeight(fontId);
  const size_t clippingIndex = CLIPPINGS.clippingCount();
  require(reader.section && reader.section->pageCount > 0, "Missing highlight test section");
  const auto currentPage = static_cast<uint16_t>(reader.section->currentPage);
  require(CLIPPINGS.addClipping(reader.currentSpineIndex, currentPage, currentPage, reader.section->pageCount, 4, 10, 7,
                                "", UINT16_MAX, "Synthetic spacing regression", UINT16_MAX,
                                clippingWordLayoutSignature(reader.section->layoutSignature())) ==
              ClippingStore::AddResult::Added,
          "Could not save highlight test range");
  for (const uint8_t spacing : {70, 100, 200}) {
    for (const bool explicitHeight : {false, true}) {
      size_t erasedPixels = 0;
      {
        RenderLock lock;
        SETTINGS.lineHeightPercent = spacing;
        const int lineHeight = std::max(1, (fontHeight * spacing + 50) / 100);
        Page page;
        page.elements.reserve(4);
        BlockStyle style;
        if (explicitHeight) style.lineHeight = lineHeight;
        const std::vector<std::string> words{"gyp", "spacing", "Ag", "test"};
        const std::vector<EpdFontFamily::Style> styles(4, EpdFontFamily::REGULAR);
        std::vector<int16_t> positions;
        positions.reserve(words.size());
        int x = 0;
        for (const auto& word : words) {
          positions.push_back(x);
          x += renderer.getTextAdvanceX(fontId, word.c_str(), EpdFontFamily::REGULAR) + 12;
        }
        for (int row = 0; row < 4; ++row) {
          auto block =
              std::make_shared<TextBlock>(words, positions, styles, std::vector<uint8_t>{}, std::vector<uint16_t>{},
                                          std::vector<uint16_t>{}, std::vector<uint8_t>{}, std::vector<bool>{}, style);
          page.elements.push_back(std::make_unique<PageLine>(std::move(block), 24, 40 + row * lineHeight));
        }
        renderer.clearScreen();
        page.renderText(renderer, fontId, 0, 0, true);
        const int width = renderer.getScreenWidth(), height = renderer.getScreenHeight();
        // Host-only snapshot: prove that neither selected letters nor adjacent
        // unselected ascenders/descenders are erased by later marker rows.
        std::vector<uint8_t> ink(static_cast<size_t>(width) * height);
        for (int y = 0; y < height; ++y)
          for (int px = 0; px < width; ++px) ink[y * width + px] = renderer.isPixelBlack(px, y);
        reader.drawClippingHighlights(page, fontId, 0, 0);
        size_t markerPixels = 0;
        for (int y = 0; y < height; ++y) {
          for (int px = 0; px < width; ++px) {
            const bool black = renderer.isPixelBlack(px, y);
            if (ink[y * width + px] && !black) ++erasedPixels;
            if (!ink[y * width + px] && black) ++markerPixels;
          }
        }
        require(markerPixels > 100, "Saved highlight was not drawn");
      }
      char name[64];
      std::snprintf(name, sizeof(name), "clip-spacing-%u-%s", spacing, explicitHeight ? "explicit" : "default");
      capture(name);
      LOG_INF("SMOKE", "%s: erased text pixels=%u", name, static_cast<unsigned>(erasedPixels));
      require(erasedPixels == 0, "Saved highlight erased a text pixel");
    }
  }
  require(CLIPPINGS.removeClippingAt(clippingIndex), "Could not remove highlight test range");
  SETTINGS.lineHeightPercent = oldSpacing;
  reader.requestUpdate();
}

bool ClippingSelectionSmokeTest::tick() {
  static int stage = 0;
  static uint32_t lastStep = 0;
  static size_t initialCount = 0;
  static bool pageFailureTestDone = false, blankDwellTestDone = false;
  static uint32_t blankDwellStarted = 0;
  static bool dwellJitterInjected = false;
  static bool dwellEscapeTestDone = false;
  static DictionaryClippingRequest savedCrossPageRequest;
  static DictionaryWordSelectActivity::ReaderPageLoadFn originalPageLoad = nullptr;
  static int x = 0, y = 0, initialIndex = 0;
  if (millis() - lastStep < 250 || RenderLock::peek()) return false;
  lastStep = millis();
  auto* reader = dynamic_cast<EpubReaderActivity*>(activityManager.simulatorCurrentActivity());
  auto* selection = dynamic_cast<DictionaryWordSelectActivity*>(activityManager.simulatorCurrentActivity());
  const auto press = [&](const TextSelectionOverlay::Rect rect) {
    x = rect.x + rect.width / 2;
    y = rect.y + rect.height / 2;
    mappedInputManager.simulatorInjectTouchDown(x, y);
  };
  const auto selectWord = [&] {
    require(selection, "Expected text selector");
    const auto* word = selection->navigator.getSelected();
    require(word, "No selected word");
    initialIndex = selection->navigator.getCurrentFlatIndex();
    x = word->screenX + word->width / 2;
    y = word->screenY + word->heightOr(renderer.getLineHeight(SETTINGS.getReaderFontId())) / 2;
    mappedInputManager.simulatorInjectTouchDown(x, y);
  };
  const auto actionsReady = [&] {
    require(selection && selection->touchSelectionActive_ && !selection->touchDragLookup_, "Missing floating actions");
    require(!selection->controller.isActive(), "Lookup began before choosing an action");
    const auto layout = selection->touchSelectionLayout();
    const auto safe = TextSelectionOverlay::safeArea(renderer);
    for (const auto& button : layout.actions) {
      require(button.empty() || (button.x >= safe.x && button.right() <= safe.right() && button.y >= safe.y &&
                                 button.bottom() <= safe.bottom()),
              "Toolbar escaped usable screen bounds");
    }
    const auto& lookup = layout.actions[TextSelectionOverlay::lookupAction];
    const auto& clip = layout.actions[TextSelectionOverlay::clipAction];
    require(!clip.empty() && (lookup.empty() || lookup.y == clip.y), "Actions are not horizontal");
    require(lookup.empty() != selection->touchLookupEnabled_, "Look Up visibility does not follow the dictionary");
  };
  const auto moveEndpoint = [&](int index) {
    require(selection && selection->touchDragLookup_, "Handle did not start a drag");
    const auto* word = selection->navigator.getWordAt(index);
    require(word, "Missing drag target");
    x = word->screenX + word->width / 2 - selection->touchDragOffsetX_;
    y = word->screenY + word->heightOr(renderer.getLineHeight(SETTINGS.getReaderFontId())) / 2 -
        selection->touchDragOffsetY_;
    mappedInputManager.simulatorInjectTouchMove(x, y);
  };
  LOG_DBG("SMOKE", "Clipping selection stage %d", stage);
  switch (stage++) {
    case 0:
      require(reader, "Expected reader");
      initialCount = CLIPPINGS.clippingCount();
      verifyHighlightAnchoring(*reader);
      verifyStableHighlightAnchoring(*reader);
      reader->openWordSelect(false);
      break;
    case 1:
    case 5:
    case 17:
    case 22:
      selectWord();
      if (stage == 23) Dictionary::setLookupDictPathOverride("/dictionary-smoke/dict");
      break;
    case 2:
    case 6:
    case 9:
    case 12:
    case 15:
    case 18:
    case 20:
    case 23:
    case 25:
      mappedInputManager.simulatorInjectTouchRelease(x, y);
      break;
    case 3:
      actionsReady();
      require(!selection->touchLookupEnabled_, "Missing dictionary lookup enabled");
      require(selection->hasPendingClippingRequest_, "Selection range was lost");
      capture("clip-actions-no-dictionary");
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Back);
      break;
    case 4:
      require(selection && !selection->touchSelectionActive_, "Back did not clear selection");
      require(CLIPPINGS.clippingCount() == initialCount, "Dismissal saved a clipping");
      break;
    case 7:
      actionsReady();
      press(selection->touchSelectionLayout().end.hit);
      break;
    case 8:
      moveEndpoint(initialIndex + 3);
      break;
    case 10:
      actionsReady();
      require(selection->navigator.getLookupSelectionWordCount() == 4, "End handle did not extend the range");
      capture("clip-actions-end-handle");
      press(selection->touchSelectionLayout().start.hit);
      break;
    case 11:
      moveEndpoint(initialIndex - 2);
      break;
    case 13:
      actionsReady();
      require(selection->navigator.getLookupSelectionWordCount() == 6,
              "Start handle did not preserve opposite endpoint");
      capture("clip-actions-both-handles");
      verifyOverlayBounds();
      {
        RenderLock lock;
        auto& first = selection->workingSet_.words[initialIndex - 2];
        auto& last = selection->workingSet_.words[initialIndex + 3];
        const bool firstRtl = first.isRtl, lastRtl = last.isRtl;
        first.isRtl = last.isRtl = true;
        const auto layout = selection->touchSelectionLayout();
        require(layout.start.stem.x >= first.screenX + first.width && layout.end.stem.right() <= last.screenX,
                "RTL handles were not reversed");
        first.isRtl = firstRtl;
        last.isRtl = lastRtl;
        int oldAnchor = -1, oldCursor = -1;
        selection->navigator.getLookupSelectionRange(oldAnchor, oldCursor);
        int wrap = 0;
        while (selection->navigator.getWordAt(wrap + 1) &&
               selection->workingSet_.words[wrap].row == selection->workingSet_.words[wrap + 1].row)
          ++wrap;
        require(selection->navigator.getWordAt(wrap + 1), "Fixture has no row boundary");
        auto& prefix = selection->workingSet_.words[wrap];
        auto& suffix = selection->workingSet_.words[wrap + 1];
        const int oldNext = prefix.continuationIndex, oldPrevious = suffix.continuationOf;
        prefix.continuationIndex = wrap + 1;
        suffix.continuationOf = wrap;
        for (const int fragment : {wrap, wrap + 1}) {
          selection->navigator.setTouchSelection(fragment, fragment);
          int begin = -1, end = -1;
          require(selection->navigator.getExpandedSelectionRange(begin, end) && begin == wrap && end == wrap + 1,
                  "Wrapped selection did not include both fragments");
          const auto handles = selection->touchSelectionLayout();
          const int lineHeight = renderer.getLineHeight(SETTINGS.getReaderFontId());
          require(handles.start.stem.y <= prefix.screenY &&
                      handles.start.stem.bottom() >= prefix.screenY + prefix.heightOr(lineHeight) &&
                      handles.end.stem.y <= suffix.screenY &&
                      handles.end.stem.bottom() >= suffix.screenY + suffix.heightOr(lineHeight),
                  "Wrapped word handles do not match highlighted fragments");
          DictionaryClippingRequest request;
          require(selection->captureCurrentClippingRequest(request) &&
                      request.firstPageWordOrdinal == prefix.pageWordOrdinal &&
                      request.lastPageWordOrdinal == suffix.pageWordOrdinal,
                  "Wrapped word clipping bounds differ from handles");
        }
        prefix.continuationIndex = oldNext;
        suffix.continuationOf = oldPrevious;
        selection->navigator.setTouchSelection(oldAnchor, oldCursor);
      }
      break;
    case 14:
      press(selection->touchSelectionLayout().actions[1]);
      break;
    case 16:
      if (!reader) {
        --stage;
        break;
      }
      require(CLIPPINGS.clippingCount() == initialCount + 1, "Clip was not saved exactly once");
      require(clippingHasSourceRange(*CLIPPINGS.clippingAt(initialCount)), "New clipping lost its source range");
      capture("clip-actions-saved-highlight");
      verifySavedHighlightSpacing(*reader);
      reader->openWordSelect(false);
      stage = 40;
      break;
    case 19:
      actionsReady();
      press(TextSelectionOverlay::Rect{0, 0, 2, 2});
      break;
    case 21:
      require(reader, "Outside tap did not dismiss selection");
      reader->openWordSelect(false);
      break;
    case 24:
      actionsReady();
      require(selection->touchLookupEnabled_, "Installed dictionary lookup disabled");
      capture("clip-actions-lookup");
      press(selection->touchSelectionLayout().actions[0]);
      break;
    case 40: {
      require(selection && selection->hasNextReaderPage_, "Fixture has no next reader page");
      if (!pageFailureTestDone) {
        originalPageLoad = selection->readerPageLoad_;
        selection->readerPageLoad_ = [](void*, int) -> std::unique_ptr<Page> { return nullptr; };
      }
      int last = 0;
      while (selection->navigator.getWordAt(last + 1)) ++last;
      selection->navigator.selectIndex(last - 1);
      selectWord();
      dwellJitterInjected = false;
      if (pageFailureTestDone) stage = 70;
      break;
    }
    case 70:
      mappedInputManager.simulatorInjectTouchRelease(x, y);
      break;
    case 71:
      actionsReady();
      press(selection->touchSelectionLayout().end.hit);
      stage = 41;
      break;
    case 41:
      if (!blankDwellTestDone) {
        const auto* word = selection->navigator.getWordAt(initialIndex + 1);
        x = word->screenX + word->width / 2;
        y = renderer.getScreenHeight() - 1;
        require(y > word->screenY + word->heightOr(renderer.getLineHeight(SETTINGS.getReaderFontId())) + 8,
                "Fixture has no blank space below its last word");
        mappedInputManager.simulatorInjectTouchMove(x, y);
        blankDwellStarted = millis();
        stage = 60;
        break;
      }
      moveEndpoint(initialIndex + 1);
      break;
    case 60:
      require(selection && selection->activePageOffset_ == 0 && selection->touchPageEndIndex_ == -1,
              "Blank-space drag armed page continuation");
      if (millis() - blankDwellStarted < 1250) {
        --stage;
        break;
      }
      blankDwellTestDone = true;
      moveEndpoint(initialIndex + 1);
      stage = 42;
      break;
    case 42:
      require(selection, "Lost selector while paging");
      if (!dwellJitterInjected) {
        require(selection->touchPageEndIndex_ == initialIndex + 1, "Final word did not arm page continuation");
        const auto* word = selection->navigator.getSelected();
        // A real capacitive contact can drift into the preceding inter-word
        // gap while the user holds the final word. Stay inside the dwell slop.
        x = word->screenX - 7 - selection->touchDragOffsetX_;
        mappedInputManager.simulatorInjectTouchMove(x, y);
        dwellJitterInjected = true;
        if (pageFailureTestDone && !dwellEscapeTestDone) {
          stage = 72;
          break;
        }
        --stage;
        break;
      }
      require(selection->activePageOffset_ == 1 || selection->navigator.getCurrentFlatIndex() == initialIndex + 1,
              "Small final-word jitter cancelled the held endpoint");
      if (!pageFailureTestDone) {
        if (selection->pendingTouchPhrase_.empty()) {
          --stage;
          break;
        }
        mappedInputManager.simulatorInjectTouchRelease(x, y);
        stage = 50;
        break;
      }
      if (selection->activePageOffset_ == 0) {
        --stage;
        break;
      }
      require(selection->navigator.getCurrentFlatIndex() == 0, "Paging did not start at the first word");
      break;
    case 72:
      require(selection->navigator.getCurrentFlatIndex() == initialIndex + 1,
              "Handle dwell did not tolerate final-word jitter");
      moveEndpoint(initialIndex);
      break;
    case 73:
      require(selection->navigator.getCurrentFlatIndex() == initialIndex && selection->touchPageEndIndex_ == -1,
              "Dragging away from the final word did not cancel page continuation");
      dwellEscapeTestDone = true;
      dwellJitterInjected = false;
      moveEndpoint(initialIndex + 1);
      stage = 42;
      break;
    case 43:
      require(selection->navigator.getCurrentFlatIndex() == 0, "Stationary finger selected the whole next page");
      mappedInputManager.simulatorInjectTouchRelease(x, y);
      break;
    case 44:
      actionsReady();
      require(selection->hasCrossPageSelection_, "Paging lost the first endpoint");
      require(selection->pendingClippingRequest_.firstPageOffset == 0 &&
                  selection->pendingClippingRequest_.lastPageOffset == 1,
              "Clipping request lost its page boundary");
      require(selection->touchSelectionLayout().start.empty(), "Off-page start handle was drawn");
      press(selection->touchSelectionLayout().end.hit);
      break;
    case 45:
      moveEndpoint(2);
      break;
    case 46:
      mappedInputManager.simulatorInjectTouchRelease(x, y);
      break;
    case 47:
      actionsReady();
      require(selection->navigator.getLookupSelectionWordCount() == 3, "Next-page end handle did not update");
      capture("clip-actions-next-page");
      savedCrossPageRequest = selection->pendingClippingRequest_;
      press(selection->touchSelectionLayout().actions[1]);
      break;
    case 48:
      mappedInputManager.simulatorInjectTouchRelease(x, y);
      break;
    case 49: {
      if (!reader) {
        --stage;
        break;
      }
      require(CLIPPINGS.clippingCount() == initialCount + 2, "Cross-page clipping was not saved exactly once");
      const auto* clipping = CLIPPINGS.clippingAt(initialCount + 1);
      require(clipping && clipping->endPage == clipping->startPage + 1 &&
                  clipping->startWordIndex == savedCrossPageRequest.firstPageWordOrdinal &&
                  clipping->endWordIndex == savedCrossPageRequest.lastPageWordOrdinal,
              "Saved cross-page clipping lost its endpoints");
      require(clippingHasSourceRange(*clipping), "Cross-page clipping lost its source range");
      std::string text;
      require(CLIPPINGS.readClippingText(*clipping, text) && !text.empty(), "Saved cross-page clipping has no text");
      capture("clip-actions-cross-page-saved");
      reader->openWordSelect(false);
      stage = 17;
      break;
    }
    case 50:
      actionsReady();
      press(selection->touchSelectionLayout().start.hit);
      break;
    case 51:
      moveEndpoint(initialIndex - 2);
      break;
    case 52:
      mappedInputManager.simulatorInjectTouchRelease(x, y);
      break;
    case 53:
      actionsReady();
      require(selection->pendingTouchPhrase_.empty(), "Handle edit retained stale fallback lookup text");
      require(selection->navigator.getLookupSelectionWordCount() == 4, "Fallback range did not follow handle edit");
      selection->readerPageLoad_ = originalPageLoad;
      pageFailureTestDone = true;
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Back);
      break;
    case 54:
      mappedInputManager.simulatorInjectRelease(MappedInputManager::Button::Back);
      break;
    case 55:
      require(reader, "Fallback dismissal did not return to reader");
      reader->openWordSelect(false);
      stage = 40;
      break;
    case 26:
      require((selection && selection->controller.isActive()) ||
                  activityManager.isCurrentActivityNamed("DictionaryDefinition"),
              "Lookup action did not start dictionary flow");
      require(CLIPPINGS.clippingCount() == initialCount + 2, "Other actions unexpectedly saved a clipping");
      return true;
  }
  return false;
}
#endif

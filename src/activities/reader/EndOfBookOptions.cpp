#include "EndOfBookOptions.h"

#include <FsHelpers.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>
#include <iterator>

#include "CrossPointSettings.h"
#include "ReaderUtils.h"
#include "components/UITheme.h"
#include "components/UIThemeTokens.h"
#include "components/UiAppHelpers.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"
#include "util/NextBookFinder.h"

namespace fui = freeink::ui;

namespace {
constexpr fui::ActionId ACTION_ROW = 1;

// Display name without the file extension, mirroring the file browser rows
std::string displayName(const std::string& filename) {
  const auto pos = filename.rfind('.');
  return filename.substr(0, pos);
}
}  // namespace

EndOfBookOptions::EndOfBookOptions(GfxRenderer& renderer)
    : renderer(renderer), uiTarget(makeUiTarget(renderer)), app(uiTarget, uiTarget.deviceContext()) {}

void EndOfBookOptions::loadOnce(const std::string& currentBookPath) {
  if (isLoaded.load(std::memory_order_acquire)) {
    return;
  }
  folder = FsHelpers::extractFolderPath(currentBookPath);
  names = NextBookFinder::findNextBooks(currentBookPath, MAX_SUGGESTIONS);
  selector.store(0, std::memory_order_relaxed);
  if (!names.empty()) {
    rowLabels.reserve(names.size() + 1);
    std::transform(names.begin(), names.end(), std::back_inserter(rowLabels), displayName);
    rowLabels.emplace_back(tr(STR_HOME));
    rowCount = static_cast<uint16_t>(rowLabels.size());
    for (uint16_t index = 0; index < rowCount; ++index) {
      rowItems[index].label = rowLabels[index].c_str();
      rowItems[index].actionValue = static_cast<int16_t>(index);
    }

    // One-time app setup on the render task, before the first render/route.
    applySharedUiTheme(app, uiTarget);
    app.on(ACTION_ROW, &EndOfBookOptions::onRowEvent, this);
    app.setScreen(&EndOfBookOptions::listScreen, this);
  }
  // Release-publish so the main task, which gates all access on isLoaded, never
  // observes a partially built list
  isLoaded.store(true, std::memory_order_release);
}

bool EndOfBookOptions::loaded() const { return isLoaded.load(std::memory_order_acquire); }

bool EndOfBookOptions::menuActive() const { return loaded() && !names.empty(); }

std::string EndOfBookOptions::fullPath(const size_t index) const {
  if (index >= names.size()) {
    return {};
  }
  return folder == "/" ? "/" + names[index] : folder + "/" + names[index];
}

void EndOfBookOptions::onRowEvent(const fui::ActionEvent& event, void* user) {
  auto* self = static_cast<EndOfBookOptions*>(user);
  if (event.value < 0 || event.value > static_cast<int16_t>(self->names.size())) return;
  self->selector.store(event.value, std::memory_order_relaxed);
  // The tapped row leaves this screen (open book or home); a lingering flash
  // would gray an unrelated element on the next render.
  self->app.clearTapFlash();
}

EndOfBookOptions::Action EndOfBookOptions::handleMenuInput(const MappedInputManager& input, std::string* openPath) {
  // Touch goes through the FreeInkApp: render() registered the row hit rects;
  // route the snapshot and consume the dispatched row from the returned event.
  if (uiReady.load(std::memory_order_acquire)) {
    const fui::InputSnapshot snap = touchSnapshotFrom(input);
    if (snap.touchPressed || snap.touchReleased) {
      const auto event = app.route(snap);
      if (event.action == ACTION_ROW && event.value >= 0 && event.value <= static_cast<int16_t>(names.size())) {
        const auto tappedRow = static_cast<size_t>(event.value);
        if (tappedRow < names.size()) {
          if (openPath) {
            *openPath = fullPath(tappedRow);
          }
          return Action::OpenBook;
        }
        return Action::GoHome;  // "Home" row tapped
      }
      if (app.invalidated()) {
        return Action::Redraw;
      }
    }
  }

  const int selectedIndex = selector.load(std::memory_order_relaxed);
  if (input.wasReleased(MappedInputManager::Button::Confirm)) {
    if (selectedIndex < static_cast<int>(names.size())) {
      if (openPath) {
        *openPath = fullPath(selectedIndex);
      }
      return Action::OpenBook;
    }
    return Action::GoHome;  // "Home" entry selected
  }

  // Short-press Back returns to the last page; a long press falls through to the
  // reader's own handler (file browser). Home is reached through the list's Home entry.
  if (input.wasReleased(MappedInputManager::Button::Back) && input.getHeldTime() < ReaderUtils::GO_HOME_MS) {
    return Action::LastPage;
  }

  // Suggestions are a menu: use spatial directions rather than reader page-turn actions.
  const int itemCount = static_cast<int>(names.size()) + 1;  // + "Home" entry
  if (input.wasReleased(MappedInputManager::Button::Up) || input.wasReleased(MappedInputManager::Button::Left)) {
    selector.store(ButtonNavigator::previousIndex(selectedIndex, itemCount),
                   std::memory_order_relaxed);  // wraps to the bottom
    return Action::Redraw;
  }
  if (input.wasReleased(MappedInputManager::Button::Down) || input.wasReleased(MappedInputManager::Button::Right)) {
    selector.store(ButtonNavigator::nextIndex(selectedIndex, itemCount),
                   std::memory_order_relaxed);  // wraps to the top
    return Action::Redraw;
  }
  return Action::None;
}

void EndOfBookOptions::listScreen(UiApp::ScreenType& screen, void* user) {
  static_cast<EndOfBookOptions*>(user)->buildListScreen(screen);
}

void EndOfBookOptions::buildListScreen(UiApp::ScreenType& screen) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  // Same layout math as render(): the list band starts under the title/subtitle it
  // draws, and stops above the button hints (the safe-area bottom edge).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int titleY = safe.y + safe.height / 8;
  const int subtitleY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;
  const int listTop = subtitleY + renderer.getLineHeight(UI_10_FONT_ID) + metrics.verticalSpacing * 2;
  setUiContentMargin(
      screen, renderer,
      fui::Insets{static_cast<int16_t>(listTop),
                  static_cast<int16_t>(renderer.getScreenWidth() - (safe.x + safe.width)),
                  static_cast<int16_t>(renderer.getScreenHeight() - (safe.y + safe.height) + metrics.verticalSpacing),
                  static_cast<int16_t>(safe.x)});

  fui::ListProps props;
  props.items = rowItems.data();
  props.count = rowCount;
  props.selectedIndex = static_cast<int16_t>(selector.load(std::memory_order_relaxed));
  props.action = ACTION_ROW;
  props.inputMask = fui::InputTouch;  // physical buttons stay in handleMenuInput()
  screen.list(props);
}

void EndOfBookOptions::render(GfxRenderer& renderer, const MappedInputManager& input) {
  const auto& metrics = UITheme::getInstance().getMetrics();

  if (!menuActive()) {
    // No suggestions: the historical plain end screen. 3/8 of the screen height matches
    // the previous fixed position on the 480x800 panel and scales to other resolutions.
    renderer.drawCenteredText(UI_12_FONT_ID, renderer.getScreenHeight() * 3 / 8, tr(STR_END_OF_BOOK), true,
                              EpdFontFamily::BOLD);
    return;
  }

  // Suggestion menu: title, list (+ Home entry) and button hints. The hints are drawn at
  // the physical front buttons, which is a logical side/top edge in the rotated
  // orientations — lay out inside the safe area so nothing hides behind them. Vertical
  // positions derive from the safe-area height and font line heights so other panel
  // resolutions scale (review request on #2532).
  const Rect safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  const int titleY = safe.y + safe.height / 8;
  const int subtitleY = titleY + renderer.getLineHeight(UI_12_FONT_ID) + metrics.verticalSpacing;

  UITheme::drawCenteredText(renderer, safe, UI_12_FONT_ID, titleY, tr(STR_END_OF_BOOK), true, EpdFontFamily::BOLD);
  UITheme::drawCenteredText(renderer, safe, UI_10_FONT_ID, subtitleY, tr(STR_EOB_CONTINUE_WITH));

  // The list renders through the FreeInkApp so its rows register touch hit rects. The
  // orientation can have changed since construction (reader menu rotate), so re-derive
  // the device context before laying out.
  uiReady.store(false, std::memory_order_release);
  renderUiApp(app, uiTarget);
  uiReady.store(true, std::memory_order_release);

  const auto labels =
      input.mapLabels(input.withBackArrow(tr(STR_BACK)), tr(STR_OPEN), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
  GUI.drawButtonHints(renderer, labels.btn1, labels.btn2, labels.btn3, labels.btn4);
}

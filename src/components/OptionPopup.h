#pragma once
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

#include "GfxRenderer.h"
#include "MappedInputManager.h"
#include "components/OptionLabels.h"
#include "components/TouchActionButtons.h"
#include "components/UIScale.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/ButtonNavigator.h"

class OptionPopup {
 public:
#ifdef SIMULATOR
  int simulatorSelectedIndex() const { return selectedIndex; }
  Rect simulatorOptionRect(const int index) const {
    const int visibleIndex = index - layout.firstOptionIndex;
    if (visibleIndex < 0 || visibleIndex >= static_cast<int>(layout.options.size())) return Rect{};
    return layout.options[visibleIndex];
  }
  bool simulatorOptionDisabled(const int index) const { return isDisabled(index); }
  const char* simulatorOptionLabel(const int index) const { return labels()[index]; }
#endif

  struct Note {
    constexpr Note(const char* label = nullptr, const char* body = nullptr, const char* secondLabel = nullptr,
                   const char* secondBody = nullptr)
        : boldLabel(label), body(body), secondBoldLabel(secondLabel), secondBody(secondBody) {}

    const char* boldLabel;
    const char* body;
    const char* secondBoldLabel;
    const char* secondBody;

    bool visible() const { return boldLabel && body; }
  };

  void show(StrId titleId, const StrId* optionIds, int optionCount, int currentIndex, std::function<void(int)> onSelect,
            Note note = Note()) {
    title = I18N.get(titleId);
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = I18N.get(optionIds[i]);
    }
    onSelectCallback = std::move(onSelect);
    popupNote = note;
    prepareStandardShow();
    activate(currentIndex);
  }

  void show(const char* titleStr, const char* const* options, int optionCount, int currentIndex,
            std::function<void(int)> onSelect, Note note = Note()) {
    title = titleStr;
    ownedStrings.resize(optionCount);
    for (int i = 0; i < optionCount; i++) {
      ownedStrings[i] = options[i];
    }
    onSelectCallback = std::move(onSelect);
    popupNote = note;
    prepareStandardShow();
    activate(currentIndex);
  }

  void show(StrId titleId, const std::vector<std::string>& options, int currentIndex, std::function<void(int)> onSelect,
            Note note = Note()) {
    title = I18N.get(titleId);
    ownedStrings = options;
    onSelectCallback = std::move(onSelect);
    popupNote = note;
    prepareStandardShow();
    activate(currentIndex);
  }

  void showConfirmed(StrId titleId, const std::vector<std::string>& options, int currentIndex,
                     std::function<void(int)> onActivate, std::function<void()> onSave,
                     std::function<void()> onCancel) {
    borrowedLabels = {};
    title = I18N.get(titleId);
    ownedStrings = options;
    onSelectCallback = std::move(onActivate);
    onSaveCallback = std::move(onSave);
    onCancelCallback = std::move(onCancel);
    primaryOptionIndex = -1;
    popupNote = Note();
    confirmationMode = true;
    dividerAfterOption = -1;
    selectionArrow = false;
    activate(currentIndex);
  }

  void showBorrowed(StrId titleId, OptionLabels labels, int currentIndex, std::function<void(int)> onSelect,
                    Note note = Note()) {
    clear();
    title = I18N.get(titleId);
    onSelectCallback = std::move(onSelect);
    popupNote = note;
    prepareStandardShow();
    borrowedLabels = labels;
    activate(currentIndex);
  }

  // Notes borrow their text, which must outlive the popup. Allocate the small
  // per-option list once when opening, then reuse it while moving the highlight.
  void setOptionNotes(std::vector<Note> notes) {
    optionNotes = std::move(notes);
    layoutValid = false;
  }

  void setCancelCallback(std::function<void()> onCancel) { onCancelCallback = std::move(onCancel); }

  // Disabled rows stay visible for context but cannot receive touch or button
  // selection. The caller supplies one flag per option after show().
  void setDisabledOptions(std::vector<bool> disabled) {
    disabledOptions = std::move(disabled);
    if (disabledOptions.size() != ownedStrings.size()) disabledOptions.assign(ownedStrings.size(), false);
    selectedIndex = firstEnabledIndex(selectedIndex, 1);
    firstOptionIndex = -1;
    layoutValid = false;
  }

  // Confirmation-style option lists can mark one option as the primary action
  // without changing the appearance of ordinary option selectors.
  void setPrimaryOptionIndex(const int index) {
    primaryOptionIndex = index;
    layoutValid = false;
  }

  void setDividerAfterOption(const int index) { dividerAfterOption = index; }
  void setSelectionArrow(const bool enabled) { selectionArrow = enabled; }

  // Informational rows keep their normal appearance without dismissing the popup.
  void setInertOptionIndex(const int index) { inertOptionIndex = index; }

  void show(const char* titleStr, const std::vector<std::string>& options, int currentIndex,
            std::function<void(int)> onSelect, Note note = Note()) {
    title = titleStr;
    ownedStrings = options;
    onSelectCallback = std::move(onSelect);
    popupNote = note;
    prepareStandardShow();
    activate(currentIndex);
  }

  // Dismiss on the press edge and suppress its matching release, so an
  // activity revealed beneath a popup cannot receive the same tap.
  void setDismissOnOutsideTouchDown(bool enabled) { dismissOnOutsideTouchDown = enabled; }

  // Actions that repaint synchronously can suppress the redundant update queued
  // after their selection callback returns.
  void skipPostSelectionUpdate() { skipPostSelectionUpdate_ = true; }

  // Retire a completed popup before starting memory-intensive work. Call only
  // after its selection callback returns, while holding the render lock.
  void clear() {
    active = false;
    borrowedLabels = {};
    std::vector<std::string>().swap(ownedStrings);
    std::vector<bool>().swap(disabledOptions);
    std::vector<Note>().swap(optionNotes);
    onSelectCallback = nullptr;
    onSaveCallback = nullptr;
    onCancelCallback = nullptr;
    popupNote = Note();
    layout = Layout{};
    layoutValid = false;
  }

  bool handleInput(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (!active) return false;

    const int count = static_cast<int>(labels().size());
    if (count <= 0) {
      active = false;
      return true;
    }
    int tx = 0;
    int ty = 0;
    if (input.wasScreenTouchDown(tx, ty)) {
      touchDownOptionIndex = -1;
      touchDownTarget = TouchTarget::None;
      const auto& hitLayout = getLayout(input.getRenderer());
      for (int i = 0; i < static_cast<int>(hitLayout.options.size()); i++) {
        if (contains(hitLayout.options[i], tx, ty)) {
          const int optionIndex = hitLayout.firstOptionIndex + i;
          if (isDisabled(optionIndex) || optionIndex == inertOptionIndex) break;
          touchDownOptionIndex = optionIndex;
          touchDownTarget = TouchTarget::Option;
          break;
        }
      }
      if (confirmationMode && contains(hitLayout.cancel, tx, ty)) {
        touchDownTarget = TouchTarget::Cancel;
        return true;
      }
      if (confirmationMode && contains(hitLayout.save, tx, ty)) {
        touchDownTarget = TouchTarget::Save;
        return true;
      }
      if (dismissOnOutsideTouchDown && !contains(hitLayout.dialog, tx, ty)) {
        input.suppressCurrentTouchContact();
        cancel(input, requestUpdate, false);
        return true;
      }
      if (confirmationMode && !contains(hitLayout.dialog, tx, ty)) {
        touchDownTarget = TouchTarget::Outside;
      }
      return true;
    }

    if (input.wasScreenTapped(tx, ty)) {
      const auto& hitLayout = getLayout(input.getRenderer());
      if (touchDownTarget == TouchTarget::Cancel && contains(hitLayout.cancel, tx, ty)) {
        cancel(input, requestUpdate, false);
        return true;
      }
      if (touchDownTarget == TouchTarget::Save && contains(hitLayout.save, tx, ty)) {
        confirm(input, requestUpdate, false);
        return true;
      }
      if (touchDownTarget == TouchTarget::Outside && !contains(hitLayout.dialog, tx, ty)) {
        cancel(input, requestUpdate, false);
        return true;
      }
      if (touchDownTarget == TouchTarget::Option && touchDownOptionIndex >= 0) {
        selectedIndex = touchDownOptionIndex;
        touchDownOptionIndex = -1;
        selectTouchOption(input, requestUpdate);
        return true;
      }
      for (int i = 0; i < static_cast<int>(hitLayout.options.size()); i++) {
        if (contains(hitLayout.options[i], tx, ty)) {
          const int optionIndex = hitLayout.firstOptionIndex + i;
          if (!isDisabled(optionIndex) && optionIndex != inertOptionIndex) {
            selectedIndex = optionIndex;
            selectTouchOption(input, requestUpdate);
          }
          return true;
        }
      }
      // Taps on the dialog chrome (title, padding) keep the popup open; taps outside dismiss it
      if (contains(hitLayout.dialog, tx, ty)) return true;
      cancel(input, requestUpdate, false);
      return true;
    }

    const auto swipe = input.wasSwipe();
    if (swipe == MappedInputManager::SwipeDir::Up || swipe == MappedInputManager::SwipeDir::Down) {
      const auto& hitLayout = getLayout(input.getRenderer());
      const int visibleCount = static_cast<int>(hitLayout.options.size());
      if (visibleCount < count) {
        const int delta = swipe == MappedInputManager::SwipeDir::Up ? visibleCount : -visibleCount;
        const int next = std::clamp(hitLayout.firstOptionIndex + delta, 0, count - visibleCount);
        if (next != firstOptionIndex) {
          firstOptionIndex = next;
          layoutValid = false;
          requestUpdate();
        }
      }
      touchDownOptionIndex = -1;
      touchDownTarget = TouchTarget::None;
      return true;
    }

    const auto& buttonLayout = getLayout(input.getRenderer());
    const int visibleCount = static_cast<int>(buttonLayout.options.size());
    const auto movePrevious = [&](const bool page) {
      if (confirmationMode && footerFocused) {
        footerFocused = false;
        selectedIndex = count - 1;
      } else if (confirmationMode && selectedIndex == 0) {
        footerFocused = true;
      } else {
        const int next = page ? ButtonNavigator::previousPageIndex(selectedIndex, count, visibleCount)
                              : ButtonNavigator::previousIndex(selectedIndex, count);
        selectedIndex = firstEnabledIndex(next, -1);
      }
      followSelection(visibleCount, count);
      layoutValid = false;
      requestUpdate();
    };
    const auto moveNext = [&](const bool page) {
      if (confirmationMode && footerFocused) {
        footerFocused = false;
        selectedIndex = 0;
      } else if (confirmationMode && selectedIndex == count - 1) {
        footerFocused = true;
      } else {
        const int next = page ? ButtonNavigator::nextPageIndex(selectedIndex, count, visibleCount)
                              : ButtonNavigator::nextIndex(selectedIndex, count);
        selectedIndex = firstEnabledIndex(next, 1);
      }
      followSelection(visibleCount, count);
      layoutValid = false;
      requestUpdate();
    };
    buttonNavigator.onPreviousRelease([&movePrevious] { movePrevious(false); });
    buttonNavigator.onNextRelease([&moveNext] { moveNext(false); });
    buttonNavigator.onPreviousContinuous([&movePrevious] { movePrevious(true); });
    buttonNavigator.onNextContinuous([&moveNext] { moveNext(true); });

    if (input.wasPressed(MappedInputManager::Button::Confirm)) {
      if (confirmationMode && !footerFocused) {
        activateSelection(input, requestUpdate, true);
      } else if (confirmationMode) {
        confirm(input, requestUpdate, true);
      } else {
        save(input, requestUpdate, true);
      }
      return true;
    } else if (input.wasReleased(MappedInputManager::Button::Back)) {
      // Consume the release that closes the popup so a reader does not also
      // treat it as its Back-to-Home action on the following frame.
      cancel(input, requestUpdate, false);
      return true;
    }
    return true;
  }

  bool processRender(GfxRenderer& renderer, const MappedInputManager& input) const {
    if (!active) return false;
    const auto popupLabels = input.mapLabels(
        confirmationMode ? MappedInputManager::Label(tr(STR_CANCEL)) : input.withBackArrow(tr(STR_BACK)),
        confirmationMode && footerFocused ? tr(STR_SAVE) : tr(STR_SELECT), tr(STR_DIR_UP), tr(STR_DIR_DOWN));
    GUI.drawButtonHints(renderer, popupLabels.btn1, popupLabels.btn2, popupLabels.btn3, popupLabels.btn4, true);
    render(renderer);
    renderer.displayBuffer();
    return true;
  }

  void render(const GfxRenderer& renderer) const {
    if (!active) return;
    const auto& renderLayout = getLayout(renderer);
    const auto& note = selectedNote();
    GUI.drawOptionPopup(renderer, title.c_str(), labels(), selectedIndex, confirmationMode, tr(STR_CANCEL),
                        tr(STR_SAVE), footerFocused, primaryOptionIndex, note.boldLabel, note.body,
                        renderLayout.firstOptionIndex, note.secondBoldLabel, note.secondBody);
    const int visibleIndex = dividerAfterOption - renderLayout.firstOptionIndex;
    if (visibleIndex >= 0 && visibleIndex + 1 < static_cast<int>(renderLayout.options.size())) {
      const auto& row = renderLayout.options[visibleIndex];
      const auto& next = renderLayout.options[visibleIndex + 1];
      const int y = (row.y + row.height + next.y) / 2;
      const int inset = UITheme::getInstance().getMetrics().optionPopupInnerPadding;
      renderer.drawLine(renderLayout.dialog.x + inset, y, renderLayout.dialog.x + renderLayout.dialog.width - inset, y);
    }
    const int selectedVisibleIndex = selectedIndex - renderLayout.firstOptionIndex;
    if (selectionArrow && selectedVisibleIndex >= 0 &&
        selectedVisibleIndex < static_cast<int>(renderLayout.options.size())) {
      const auto& row = renderLayout.options[selectedVisibleIndex];
      const int x = row.x + 8;
      const int y = row.y + row.height / 2;
      const int xs[] = {x, x, x + 9};
      const int ys[] = {y - 6, y + 6, y};
      renderer.fillPolygon(xs, ys, 3, UITheme::getInstance().getMetrics().optionPopupSelectionLight);
    }
  }

  bool isActive() const { return active; }

  void dismiss(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (active) cancel(input, requestUpdate, false);
  }

 private:
  struct Layout {
    Rect dialog{0, 0, 0, 0};
    std::vector<Rect> options;
    Rect cancel{0, 0, 0, 0};
    Rect save{0, 0, 0, 0};
    TouchActionButtons::Layout footer;
    int firstOptionIndex = 0;
  };

  enum class TouchTarget : uint8_t { None, Option, Cancel, Save, Outside };

  // Text measurement is expensive and wasScreenTouchDown() is level-triggered, so the
  // layout is computed once per show() and cached rather than rebuilt every loop().
  const Layout& getLayout(const GfxRenderer& renderer) const {
    if (layoutValid) return layout;

    const auto& metrics = UITheme::getInstance().getMetrics();
    const auto safe = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
    const int pageWidth = safe.width;
    const int pageHeight = safe.height;
    const int optionFontId = uiScaleSpec().bodyFontId;
    const bool touch = gpio.hasTouch();
    const EpdFontFamily::Style optionStyle =
        metrics.optionPopupOptionFontBold ? EpdFontFamily::BOLD : EpdFontFamily::REGULAR;

    const bool touchActionStyle = touch && primaryOptionIndex >= 0 && labels().size() == 2;
    const int itemSpacing = touchActionStyle ? TouchActionButtons::kDefaultGap : metrics.optionPopupItemSpacing;
    const int innerPadding = metrics.optionPopupInnerPadding;
    const int selectionHPadding = metrics.optionPopupSelectionHPadding;
    const int selectionVPadding = metrics.optionPopupSelectionVPadding;

    const int optionLineHeight = renderer.getLineHeight(optionFontId);
    const int titleLineHeight = renderer.getLineHeight(UI_12_FONT_ID);
    const int noteLineHeight = renderer.getLineHeight(UI_10_FONT_ID);
    const auto& note = selectedNote();
    const int noteHeight = note.visible() ? noteLineHeight * 2 + metrics.optionPopupTitleGap : 0;
    const int rowHeight =
        touchActionStyle ? TouchActionButtons::kDefaultHeight : optionLineHeight + selectionVPadding * 2;

    int maxTextWidth = renderer.getTextWidth(UI_12_FONT_ID, title.c_str(), EpdFontFamily::BOLD);
    const auto options = labels();
    for (size_t i = 0; i < options.size(); ++i) {
      const char* opt = options[i];
      const auto style = primaryOptionIndex == static_cast<int>(i) ? EpdFontFamily::BOLD : optionStyle;
      const int width = renderer.getTextWidth(optionFontId, opt, style);
      if (width > maxTextWidth) maxTextWidth = width;
    }
    const auto measureNote = [&](const Note& candidate) {
      if (!candidate.visible()) return;
      const auto measureLine = [&](const char* label, const char* body) {
        if (!label || !body) return;
        const int width = renderer.getTextWidth(UI_10_FONT_ID, label, EpdFontFamily::BOLD) +
                          renderer.getSpaceWidth(UI_10_FONT_ID) + renderer.getTextWidth(UI_10_FONT_ID, body);
        maxTextWidth = std::max(maxTextWidth, width);
      };
      measureLine(candidate.boldLabel, candidate.body);
      measureLine(candidate.secondBoldLabel, candidate.secondBody);
    };
    measureNote(note);

    const int optionCount = static_cast<int>(labels().size());
    constexpr int footerHeight = 56;
    const int footerSpace = confirmationMode ? footerHeight : 0;
    const int maxDialogH = std::max(
        rowHeight + titleLineHeight + metrics.optionPopupTitleGap + noteHeight + innerPadding * 2 + footerSpace,
        pageHeight - metrics.optionPopupDialogSideMargin * 2);
    const int dialogW = std::min((maxTextWidth + innerPadding * 2 + selectionHPadding * 2 + metrics.scrollBarWidth +
                                  metrics.scrollBarRightOffset + selectionHPadding) *
                                     12 / 10,
                                 pageWidth - metrics.optionPopupDialogSideMargin * 2);
    const int titleContentWidth = std::max(1, dialogW - innerPadding * 2);
    const int maxTitleLines = std::max(
        1, (maxDialogH - innerPadding * 2 - metrics.optionPopupTitleGap - noteHeight - rowHeight - footerSpace) /
               titleLineHeight);
    const auto titleLines =
        renderer.wrappedText(UI_12_FONT_ID, title.c_str(), titleContentWidth, maxTitleLines, EpdFontFamily::BOLD);
    const int titleHeight = static_cast<int>(titleLines.size()) * titleLineHeight;
    const int maxListHeight = std::max(rowHeight, maxDialogH - innerPadding * 2 - titleHeight -
                                                      metrics.optionPopupTitleGap - noteHeight - footerSpace);
    const int rowStep = rowHeight + itemSpacing;
    const int visibleCount = std::max(1, std::min(optionCount, (maxListHeight + itemSpacing) / rowStep));
    const int safeSelectedIndex = std::clamp(selectedIndex, 0, optionCount - 1);
    const int centeredStart = std::clamp(safeSelectedIndex - visibleCount / 2, 0, optionCount - visibleCount);
    const int visibleStart =
        firstOptionIndex < 0 ? centeredStart : std::clamp(firstOptionIndex, 0, optionCount - visibleCount);
    const int listHeight = rowHeight * visibleCount + itemSpacing * (visibleCount - 1);
    const bool hasHiddenOptions = visibleCount < optionCount;
    const int scrollBarGutter =
        hasHiddenOptions ? metrics.scrollBarWidth + metrics.scrollBarRightOffset + selectionHPadding : 0;
    const int contentHeight = titleHeight + metrics.optionPopupTitleGap + noteHeight + listHeight;
    const int dialogH = contentHeight + innerPadding * 2 + footerSpace;
    const int dialogX = safe.x + (pageWidth - dialogW) / 2;
    const int dialogY = safe.y + (pageHeight - dialogH) / 2;
    const int itemRectX = dialogX + innerPadding;
    const int itemRectW = std::max(1, dialogW - innerPadding * 2 - scrollBarGutter);
    const int firstItemY = dialogY + innerPadding + titleHeight + metrics.optionPopupTitleGap + noteHeight;

    layout.dialog = Rect{dialogX, dialogY, dialogW, dialogH};
    layout.firstOptionIndex = visibleStart;
    firstOptionIndex = visibleStart;
    layout.footer = TouchActionButtons::Layout();
    if (confirmationMode) {
      const int footerY = dialogY + dialogH - footerSpace;
      const bool showCancelButton = gpio.hasTouch();
      layout.cancel = showCancelButton ? Rect{dialogX, footerY, dialogW / 2, footerHeight} : Rect();
      layout.save = showCancelButton ? Rect{dialogX + dialogW / 2, footerY, dialogW - dialogW / 2, footerHeight}
                                     : Rect{dialogX, footerY, dialogW, footerHeight};
    } else {
      layout.cancel = Rect();
      layout.save = Rect();
    }
    layout.options.clear();
    layout.options.reserve(visibleCount);
    if (touchActionStyle && visibleCount == 2) {
      const auto actions =
          TouchActionButtons::vertical(Rect{itemRectX, firstItemY, itemRectW, listHeight}, 2, rowHeight, itemSpacing);
      const int primaryOffset = primaryOptionIndex - visibleStart;
      const int secondaryOffset = primaryOffset == 0 ? 1 : 0;
      layout.options.resize(2);
      layout.options[primaryOffset] = actions.buttons[0];
      layout.options[secondaryOffset] = actions.buttons[1];
    } else {
      for (int i = 0; i < visibleCount; i++) {
        layout.options.push_back(Rect{itemRectX, firstItemY + i * (rowHeight + itemSpacing), itemRectW, rowHeight});
      }
    }
    layoutValid = true;
    return layout;
  }

  static bool contains(const Rect& rect, const int x, const int y) {
    return x >= rect.x && x < rect.x + rect.width && y >= rect.y && y < rect.y + rect.height;
  }

  bool active = false;
  bool dismissOnOutsideTouchDown = false;
  bool confirmationMode = false;
  bool footerFocused = false;
  std::string title;
  std::vector<std::string> ownedStrings;
  OptionLabels borrowedLabels;
  OptionLabels labels() const {
    return borrowedLabels.read ? borrowedLabels : OptionLabels(ownedStrings, disabledOptions);
  }
  std::vector<bool> disabledOptions;
  int selectedIndex = 0;
  mutable int firstOptionIndex = -1;
  int touchDownOptionIndex = -1;
  TouchTarget touchDownTarget = TouchTarget::None;
  std::function<void(int)> onSelectCallback;
  std::function<void()> onSaveCallback;
  std::function<void()> onCancelCallback;
  Note popupNote;
  std::vector<Note> optionNotes;
  bool skipPostSelectionUpdate_ = false;
  int primaryOptionIndex = -1;
  int inertOptionIndex = -1;
  int dividerAfterOption = -1;
  bool selectionArrow = false;
  ButtonNavigator buttonNavigator;
  mutable Layout layout;
  mutable bool layoutValid = false;

  const Note& selectedNote() const {
    return selectedIndex >= 0 && selectedIndex < static_cast<int>(optionNotes.size()) ? optionNotes[selectedIndex]
                                                                                      : popupNote;
  }

  void activate(int currentIndex) {
    inertOptionIndex = -1;
    optionNotes.clear();
    layoutValid = false;
    firstOptionIndex = -1;
    touchDownOptionIndex = -1;
    touchDownTarget = TouchTarget::None;
    if (!borrowedLabels.read) disabledOptions.assign(ownedStrings.size(), false);
    footerFocused = false;
    skipPostSelectionUpdate_ = false;
    if (labels().empty()) {
      active = false;
      onSelectCallback = nullptr;
      selectedIndex = 0;
      return;
    }

    const int count = static_cast<int>(labels().size());
    if (currentIndex < 0) {
      selectedIndex = 0;
    } else if (currentIndex >= count) {
      selectedIndex = count - 1;
    } else {
      selectedIndex = currentIndex;
    }
    active = true;
  }

  void prepareStandardShow() {
    borrowedLabels = {};
    confirmationMode = false;
    footerFocused = false;
    onSaveCallback = nullptr;
    onCancelCallback = nullptr;
    primaryOptionIndex = -1;
    dividerAfterOption = -1;
    selectionArrow = false;
  }

  void activateSelection(MappedInputManager& input, const std::function<void()>& requestUpdate,
                         const bool suppressRelease) {
    if (selectedIndex == inertOptionIndex) return;
    active = false;
    suppressSelectionRelease(input, suppressRelease);
    if (onSelectCallback) onSelectCallback(selectedIndex);
    requestUpdate();
  }

  void confirm(MappedInputManager& input, const std::function<void()>& requestUpdate, const bool suppressRelease) {
    active = false;
    suppressSelectionRelease(input, suppressRelease);
    if (onSaveCallback) onSaveCallback();
    requestUpdate();
  }

  void save(MappedInputManager& input, const std::function<void()>& requestUpdate, const bool suppressRelease) {
    if (isDisabled(selectedIndex) || selectedIndex == inertOptionIndex) return;
    active = false;
    suppressSelectionRelease(input, suppressRelease);
    if (onSelectCallback) onSelectCallback(selectedIndex);
    const bool skipUpdate = skipPostSelectionUpdate_;
    skipPostSelectionUpdate_ = false;
    if (!skipUpdate) requestUpdate();
  }

  void selectTouchOption(MappedInputManager& input, const std::function<void()>& requestUpdate) {
    if (confirmationMode) {
      activateSelection(input, requestUpdate, false);
    } else {
      save(input, requestUpdate, false);
    }
  }

  static void suppressSelectionRelease(MappedInputManager& input, const bool suppressRelease) {
    if (!suppressRelease) return;

    input.suppressNextConfirmRelease();
    // Some boards expose Power as Confirm directly. Consume its matching Power
    // release too, otherwise it can immediately re-run the shortcut that opened
    // this popup after the selection callback closes it.
    if (input.isPressed(MappedInputManager::Button::Power)) {
      input.suppressNextPowerRelease();
    }
  }

  void cancel(MappedInputManager& input, const std::function<void()>& requestUpdate, const bool suppressRelease) {
    active = false;
    if (suppressRelease) input.suppressNextBackRelease();
    if (onCancelCallback) onCancelCallback();
    requestUpdate();
  }

  bool isDisabled(const int index) const {
    return index >= 0 && index < static_cast<int>(labels().size()) && labels().isDisabled(index);
  }

  int firstEnabledIndex(const int start, const int direction) const {
    if (labels().empty()) return 0;
    const int count = static_cast<int>(labels().size());
    int index = std::clamp(start, 0, count - 1);
    for (int attempts = 0; attempts < count; ++attempts) {
      if (!isDisabled(index)) return index;
      index = (index + (direction < 0 ? count - 1 : 1)) % count;
    }
    return std::clamp(start, 0, count - 1);
  }

  void followSelection(const int visibleCount, const int count) {
    const int maxTop = std::max(0, count - visibleCount);
    int top = std::clamp(firstOptionIndex, 0, maxTop);
    if (selectedIndex < top) {
      top = selectedIndex;
    } else if (selectedIndex >= top + visibleCount) {
      top = selectedIndex - visibleCount + 1;
    }
    firstOptionIndex = std::clamp(top, 0, maxTop);
  }
};

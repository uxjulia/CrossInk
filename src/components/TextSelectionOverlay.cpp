#include "TextSelectionOverlay.h"
#if CROSSINK_APP_CAP_TOUCH
#include <FreeInkUIGfxRenderer.h>
#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <I18n.h>

#include <algorithm>

#include "UIScale.h"
#include "UITheme.h"
#include "icons/selectionIcons.h"

namespace TextSelectionOverlay {
namespace fui = freeink::ui;
namespace {
// Device pixels. Keep the visible handle compact while leaving room for a fingertip.
constexpr int kStemWidth = 3;
constexpr int kDotSize = 16;
constexpr int kHandleHitSize = 72;
constexpr int kBarMinHeight = 56;
constexpr int kBarPaddingY = 10;
constexpr int kSegmentMinWidth = 116;
constexpr int kSegmentPaddingX = 16;
constexpr int kIconSize = 24;
constexpr int kIconGap = 8;
constexpr int kBorderWidth = 2;
constexpr int kRadius = 4;
constexpr int kDividerInset = 10;
constexpr int kPointerHalfWidth = 12;
constexpr int kPointerHeight = 12;
// Inner white triangle inset; keeps the pointer outline about as thick as the border.
constexpr int kPointerInsetX = 4;
constexpr int kPointerInsetTip = 5;
constexpr int kSelectionGap = 6;

const char* label(int index) {
  static constexpr StrId labels[] = {StrId::STR_LOOKUP_SHORT, StrId::STR_CLIP};
  return I18n::getInstance().get(labels[index]);
}

const freeink::Icon& icon(int index) {
  return index == lookupAction ? icon_selection_lookup_24 : icon_selection_clip_24;
}

Rect makeRect(int x, int y, int width, int height) {
  return {static_cast<int16_t>(x), static_cast<int16_t>(y), static_cast<int16_t>(std::max(0, width)),
          static_cast<int16_t>(std::max(0, height))};
}

Rect clipTo(const Rect rect, const Rect bounds) {
  const int x = std::max(rect.x, bounds.x), y = std::max(rect.y, bounds.y);
  return makeRect(x, y, std::min(rect.right(), bounds.right()) - x, std::min(rect.bottom(), bounds.bottom()) - y);
}

int centerX(const Rect rect) { return rect.x + rect.width / 2; }

bool overlaps(const Rect a, const Rect b) {
  return !a.empty() && !b.empty() && a.x < b.right() && b.x < a.right() && a.y < b.bottom() && b.y < a.bottom();
}
}  // namespace

Rect safeArea(const GfxRenderer& renderer) {
  const auto area = UITheme::getInstance().getScreenSafeArea(renderer, true, false);
  int top, right, bottom, left;
  renderer.getOrientedViewableTRBL(&top, &right, &bottom, &left);
  const int x = std::max(area.x, left), y = std::max(area.y, top);
  return {static_cast<int16_t>(x), static_cast<int16_t>(y),
          static_cast<int16_t>(std::max(0, std::min(area.x + area.width, renderer.getScreenWidth() - right) - x)),
          static_cast<int16_t>(std::max(0, std::min(area.y + area.height, renderer.getScreenHeight() - bottom) - y))};
}

Handle handle(const Rect safe, const Rect word, const bool onLeft, const bool isStart) {
  Handle result;
  if (word.empty() || safe.width < kDotSize || safe.height < kDotSize) return result;
  const int stemX = std::clamp(onLeft ? word.x - kStemWidth : static_cast<int>(word.right()), static_cast<int>(safe.x),
                               safe.right() - kStemWidth);
  const int stemTop = std::clamp(word.y - 2, static_cast<int>(safe.y), safe.bottom() - 1);
  const int stemBottom = std::clamp(word.bottom() + 2, stemTop + 1, static_cast<int>(safe.bottom()));
  result.stem = makeRect(stemX, stemTop, kStemWidth, stemBottom - stemTop);

  const int middleX = stemX + kStemWidth / 2;
  const int dotX = std::clamp(middleX - kDotSize / 2, static_cast<int>(safe.x), safe.right() - kDotSize);
  const int dotY =
      std::clamp(isStart ? stemTop - kDotSize + 1 : stemBottom - 1, static_cast<int>(safe.y), safe.bottom() - kDotSize);
  result.dot = makeRect(dotX, dotY, kDotSize, kDotSize);

  const int dotMiddle = dotY + kDotSize / 2;
  const int hitTop = std::min(stemTop, dotMiddle - kHandleHitSize / 2);
  const int hitBottom = std::max(stemBottom, dotMiddle + kHandleHitSize / 2);
  result.hit = clipTo(makeRect(middleX - kHandleHitSize / 2, hitTop, kHandleHitSize, hitBottom - hitTop), safe);
  return result;
}

const Handle* handleAt(const Layout& layout, const int x, const int y) {
  const bool start = layout.start.hit.contains(x, y);
  const bool end = layout.end.hit.contains(x, y);
  if (!start) return end ? &layout.end : nullptr;
  if (!end) return &layout.start;
  // Overlapping fingertip targets are common on a short word. Pick the closest ink.
  const auto distance = [x, y](const Rect rect) {
    const int dx = x - std::clamp(x, static_cast<int>(rect.x), rect.right() - 1);
    const int dy = y - std::clamp(y, static_cast<int>(rect.y), rect.bottom() - 1);
    return dx * dx + dy * dy;
  };
  const auto score = [&](const Handle& handle) { return std::min(distance(handle.dot), distance(handle.stem)); };
  return score(layout.start) < score(layout.end) ? &layout.start : &layout.end;
}

void placeActions(const GfxRenderer& renderer, Layout& layout, Rect firstWord, Rect lastWord,
                  const bool lookupAvailable) {
  for (auto& action : layout.actions) action = {};
  layout.bar = {};
  layout.showPointer = false;
  layout.showIcons = true;
  const auto safe = safeArea(renderer);
  if (safe.empty()) return;
  if (firstWord.empty()) firstWord = lastWord;
  if (lastWord.empty()) lastWord = firstWord;

  const int font = uiScaleSpec().bodyFontId;
  const bool visible[actionCount] = {lookupAvailable, true};
  int widths[actionCount] = {};
  int total = 0;
  for (int i = 0; i < actionCount; ++i) {
    if (!visible[i]) continue;
    const int labelWidth = renderer.getTextWidth(font, label(i), EpdFontFamily::BOLD);
    widths[i] = std::max(kSegmentMinWidth, kSegmentPaddingX * 2 + kIconSize + kIconGap + labelWidth);
    total += widths[i];
  }
  // Long translations shrink the segments evenly and wrap to two lines.
  int lines = 1;
  if (total > safe.width) {
    for (auto& width : widths) width = width * safe.width / total;
    total = safe.width;
    lines = 2;
    // The SDK centers icon + label on one line, so a wrapped label drops its icon.
    layout.showIcons = false;
  }
  const int barWidth = total;
  const int barHeight =
      std::min<int>(safe.height, std::max(kBarMinHeight, renderer.getLineHeight(font) * lines + kBarPaddingY * 2));

  // Visual extent of the selection, including the handle dots.
  int selectionTop = firstWord.empty() ? safe.y : firstWord.y;
  int selectionBottom = lastWord.empty() ? safe.y : lastWord.bottom();
  if (!layout.start.empty()) selectionTop = std::min<int>(selectionTop, layout.start.dot.y);
  if (!layout.end.empty()) selectionBottom = std::max<int>(selectionBottom, layout.end.dot.bottom());

  const int reach = kSelectionGap + kPointerHeight;
  const int aboveY = selectionTop - reach - barHeight;
  const int belowY = selectionBottom + reach;
  const auto clampX = [&](const int x) { return std::clamp(x, static_cast<int>(safe.x), safe.right() - barWidth); };
  int barY = 0;
  int anchorX = firstWord.empty() ? centerX(safe) : centerX(firstWord);
  int barX = clampX(anchorX - barWidth / 2);
  if (aboveY >= safe.y) {
    barY = aboveY;
    layout.showPointer = !firstWord.empty();
    layout.pointerUp = false;
  } else if (belowY + barHeight <= safe.bottom()) {
    barY = belowY;
    anchorX = lastWord.empty() ? anchorX : centerX(lastWord);
    barX = clampX(anchorX - barWidth / 2);
    layout.showPointer = !lastWord.empty();
    layout.pointerUp = true;
  } else {
    // The selection fills the page: the bar has to cover some of it. Prefer the
    // edge that covers less, but never hide a drag handle, which would turn a
    // handle press into a toolbar tap.
    const auto covered = [&](const int y) {
      return std::max(0, std::min(y + barHeight, selectionBottom) - std::max(y, selectionTop));
    };
    const auto hidesHandle = [&](const int x, const int y) {
      const auto bar = makeRect(x, y, barWidth, barHeight);
      for (const auto* handle : {&layout.start, &layout.end}) {
        if (overlaps(bar, handle->dot) || overlaps(bar, handle->stem)) return true;
      }
      return false;
    };
    const auto clampY = [&](const int y) { return std::clamp(y, static_cast<int>(safe.y), safe.bottom() - barHeight); };
    // Screen edges first; then just inside the first or last selected line,
    // for when both handles sit mid-screen and nothing beside them fits.
    int rows[] = {safe.y, safe.bottom() - barHeight, clampY(layout.start.stem.bottom() + kSelectionGap),
                  clampY(layout.end.stem.y - kSelectionGap - barHeight)};
    if (covered(rows[1]) < covered(rows[0])) std::swap(rows[0], rows[1]);
    barY = rows[0];
    bool placed = false;
    for (const int y : rows) {
      const int candidates[] = {barX,
                                clampX(layout.start.hit.right()),
                                clampX(layout.start.hit.x - barWidth),
                                clampX(layout.end.hit.right()),
                                clampX(layout.end.hit.x - barWidth),
                                clampX(layout.start.dot.right() + kSelectionGap),
                                clampX(layout.start.dot.x - kSelectionGap - barWidth),
                                clampX(layout.end.dot.right() + kSelectionGap),
                                clampX(layout.end.dot.x - kSelectionGap - barWidth)};
      for (const int x : candidates) {
        if (hidesHandle(x, y)) continue;
        barX = x;
        barY = y;
        placed = true;
        break;
      }
      if (placed) break;
    }
  }

  layout.bar = makeRect(barX, barY, barWidth, barHeight);
  const int pointerMargin = kRadius + kBorderWidth + kPointerHalfWidth;
  layout.pointerX = static_cast<int16_t>(
      barWidth > pointerMargin * 2 ? std::clamp(anchorX, barX + pointerMargin, barX + barWidth - pointerMargin)
                                   : barX + barWidth / 2);
  int x = barX;
  for (int i = 0; i < actionCount; ++i) {
    if (!visible[i]) continue;
    layout.actions[i] = makeRect(x, barY, widths[i], barHeight);
    x += widths[i];
  }
}

int actionAt(const Layout& layout, const int x, const int y) {
  for (int i = 0; i < actionCount; ++i) {
    if (layout.actions[i].contains(x, y)) return i;
  }
  return -1;
}

void draw(const GfxRenderer& renderer, const Layout& layout, const bool showActions) {
  const auto spec = uiScaleSpec();
  // Fixed-capacity SDK frame: no retained widget tree or framebuffer allocation.
  fui::GfxRendererFrame<actionCount> ui(renderer, spec.smallFontId, spec.bodyFontId, spec.titleFontId);
  const auto black = fui::Paint::solid(fui::Color::Black);
  const auto white = fui::Paint::solid(fui::Color::White);
  for (const auto* handle : {&layout.start, &layout.end}) {
    if (handle->empty()) continue;
    ui.target.fill(handle->stem, black);
    ui.target.fill(handle->dot, black, kDotSize / 2);
  }
  if (!showActions || layout.bar.empty()) return;

  const auto& bar = layout.bar;
  ui.target.fill(bar, white, kRadius);
  ui.target.stroke(bar, black, kBorderWidth, kRadius);
  if (layout.showPointer) {
    // The outline triangle starts inside the border; the white inner triangle
    // then erases that stretch of border so the outline runs unbroken.
    const int direction = layout.pointerUp ? -1 : 1;
    const int baseY = (layout.pointerUp ? bar.y : bar.bottom()) - direction * kBorderWidth;
    const int tipY = (layout.pointerUp ? bar.y : bar.bottom()) + direction * kPointerHeight;
    const int px = layout.pointerX;
    const auto point = [](const int x, const int y) {
      return fui::Point{static_cast<int16_t>(x), static_cast<int16_t>(y)};
    };
    ui.target.triangle(point(px - kPointerHalfWidth, baseY), point(px + kPointerHalfWidth, baseY), point(px, tipY),
                       black);
    ui.target.triangle(point(px - kPointerHalfWidth + kPointerInsetX, baseY),
                       point(px + kPointerHalfWidth - kPointerInsetX, baseY),
                       point(px, tipY - direction * kPointerInsetTip), white);
  }

  bool first = true;
  for (int i = 0; i < actionCount; ++i) {
    const auto& rect = layout.actions[i];
    if (rect.empty()) continue;
    if (!first) {
      ui.target.fill(
          makeRect(rect.x - kBorderWidth / 2, rect.y + kDividerInset, kBorderWidth, rect.height - kDividerInset * 2),
          black);
    }
    first = false;
    fui::ButtonProps props;
    props.label = label(i);
    if (layout.showIcons) props.icon = fui::bitmapFromIcon(icon(i));
    props.gap = kIconGap;
    props.text.font = fui::GfxRendererTarget::FONT_BODY;
    props.text.bold = true;
    props.text.maxLines = 2;
    props.styles = fui::plainStyles(black);
    props.padding = {kBorderWidth, kSegmentPaddingX / 2, kBorderWidth, kSegmentPaddingX / 2};
    // Use the same exact bounds for painting and hit testing, as upstream does.
    props.minTouchSize = 0;
    fui::button(ui.frame, rect, props);
  }
}
}  // namespace TextSelectionOverlay
#endif

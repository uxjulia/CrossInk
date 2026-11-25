#include "CatalogScreens.h"

#include <FreeInkUIIcon.h>
#include <GfxRenderer.h>
#include <HalGPIO.h>
#include <I18n.h>

#include <algorithm>
#include <numeric>

#include "TouchHeaderBackButton.h"
#include "UITheme.h"

namespace fui = freeink::ui;

void catalogScreenHeader(UiAppHost<24, 6>::Screen& screen, GfxRenderer& renderer, const MappedInputManager& input,
                         freeink::ui::GfxRendererTarget& target, const char* title, const fui::BitmapRef& trailingIcon,
                         const fui::ActionId trailingAction) {
  const auto& metrics = UITheme::getInstance().getMetrics();
  screen.takeBottom(static_cast<int16_t>(metrics.buttonHintsHeight));
  if (input.hasTouchHardware()) {
    const Rect headerRect = TouchHeaderBackButton::headerRect(renderer, input);
    const auto layout = TouchHeaderBackButton::layout(headerRect);
    const bool search = trailingAction != fui::NO_ACTION;
    TouchHeaderBackButton::draw(renderer, target, headerRect, title, false,
                                search ? static_cast<int>(layout.iconRect.width + 8) : 0);
    screen.takeTop(static_cast<int16_t>(headerRect.height));
    if (search) {
      fui::ButtonProps button;
      button.action = trailingAction;
      button.styles = fui::plainStyles(fui::Paint::solid(fui::Color::Black));
      button.minTouchSize = screen.theme().minTouchSize;
      const fui::Rect rect{static_cast<int16_t>(headerRect.x + headerRect.width - layout.iconRect.width),
                           static_cast<int16_t>(layout.iconRect.y), static_cast<int16_t>(layout.iconRect.width),
                           static_cast<int16_t>(layout.iconRect.height)};
      screen.button(button, rect);
      const int16_t x = rect.x + (rect.width - TouchHeaderBackButton::ICON_SIZE) / 2;
      const int16_t y = layout.iconRect.y + TouchHeaderBackButton::TITLE_VERTICAL_OFFSET +
                        (rect.height - TouchHeaderBackButton::ICON_SIZE) / 2;
      screen.target().bitmap(fui::Rect{x, y, TouchHeaderBackButton::ICON_SIZE, TouchHeaderBackButton::ICON_SIZE},
                             trailingIcon, fui::BitmapMode::Center, fui::Paint::solid(fui::Color::Black));
    }
  } else {
    fui::HeaderProps header;
    header.title = title;
    header.borderEdges = fui::EdgeBottom;
    header.trailingIcon = trailingIcon;
    header.trailingAction = trailingAction;
    screen.header(header);
  }
  screen.spacer(static_cast<int16_t>(metrics.verticalSpacing));
}

void catalogCenteredBlock(UiAppHost<24, 6>::Screen& screen, const std::initializer_list<CatalogLine> lines) {
  const int count = static_cast<int>(lines.size());
  if (count == 0) return;
  // Long lines (sign-in hints, server errors) wrap instead of clipping.
  fui::TextStyle centered = screen.theme().bodyText;
  centered.align = fui::TextAlign::Center;
  centered.maxLines = 4;
  const int16_t gap = screen.theme().spaceMd;
  const int16_t pad = static_cast<int16_t>(UITheme::getInstance().getMetrics().contentSidePadding);
  const int16_t width = static_cast<int16_t>(screen.body().width - 2 * pad);
  const auto styleOf = [&](const CatalogLine& line) {
    fui::TextStyle style = centered;
    style.bold = line.bold;
    return style;
  };
  const auto heightOf = [&](const CatalogLine& line) {
    const int16_t h = fui::measureWrappedText(screen.target(), line.text ? line.text : "", styleOf(line), width).height;
    return std::max(h, screen.target().lineHeight(centered.font));
  };
  const int blockH = std::accumulate(lines.begin(), lines.end(), gap * (count - 1),
                                     [&](const int sum, const CatalogLine& line) { return sum + heightOf(line); });
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));
  int i = 0;
  for (const CatalogLine& line : lines) {
    const fui::Rect rect = screen.takeTop(heightOf(line), ++i < count ? gap : 0).inset(fui::Insets{0, pad, 0, pad});
    screen.target().text(rect, line.text ? line.text : "", styleOf(line));
  }
}

void catalogDownloadScreen(UiAppHost<24, 6>::Screen& screen, const char* status, const size_t progress,
                           const size_t total, const fui::ActionId cancelAction) {
  // Centered block: status line, item title, progress, optional cancel.
  const auto& theme = screen.theme();
  fui::TextStyle centered = theme.bodyText;
  centered.align = fui::TextAlign::Center;
  const int16_t lh = screen.target().lineHeight(centered.font);
  const int16_t gap = theme.spaceMd;
  const bool showBytes = total == 0;
  const int16_t progressH = showBytes ? lh : 16;
  const bool withCancel = cancelAction != fui::NO_ACTION;
  const int16_t btnH = withCancel ? theme.rowHeight : 0;
  // The item title gets two padded lines: long book titles wrap once and then
  // truncate, instead of running edge-to-edge in a single clipped line.
  fui::TextStyle title = centered;
  title.maxLines = 2;
  const int16_t titleH = static_cast<int16_t>(lh * 2);
  const int16_t pad = static_cast<int16_t>(UITheme::getInstance().getMetrics().contentSidePadding);
  const int16_t blockH = static_cast<int16_t>(lh + titleH + progressH + btnH + gap * (withCancel ? 3 : 2));
  const fui::Rect body = screen.body();
  if (body.height > blockH) screen.spacer(static_cast<int16_t>((body.height - blockH) / 2));

  screen.target().text(screen.takeTop(lh, gap), tr(STR_DOWNLOADING), centered);
  screen.target().text(screen.takeTop(titleH, gap).inset(fui::Insets{0, pad, 0, pad}), status, title);

  const fui::Rect progressArea = screen.takeTop(progressH, gap).inset(fui::Insets{0, 50, 0, 50});
  if (!showBytes) {
    fui::ProgressBarProps progressProps;
    progressProps.value = static_cast<int32_t>(progress);
    progressProps.max = static_cast<int32_t>(total);
    progressProps.border = fui::Paint::solid(fui::Color::Black);
    progressProps.borderWidth = 1;
    fui::progressBar(screen.frame(), progressArea, progressProps);
  } else {
    char bytes[24];
    if (progress >= 1024 * 1024) {
      snprintf(bytes, sizeof(bytes), "%u.%u MB", static_cast<unsigned>(progress >> 20),
               static_cast<unsigned>((progress >> 10) % 1024 * 10 / 1024));
    } else {
      snprintf(bytes, sizeof(bytes), "%u KB", static_cast<unsigned>(progress >> 10));
    }
    screen.target().text(progressArea, bytes, centered);
  }

  if (withCancel) {
    const fui::Rect btnArea = screen.takeTop(btnH);
    const int16_t btnW = static_cast<int16_t>(btnArea.width / 3);
    fui::ButtonProps cancel;
    cancel.label = tr(STR_CANCEL);
    cancel.action = cancelAction;
    screen.button(cancel,
                  fui::Rect{static_cast<int16_t>(btnArea.x + (btnArea.width - btnW) / 2), btnArea.y, btnW, btnH});
  }
}

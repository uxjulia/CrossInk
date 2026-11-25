#include "DetailsActivity.h"

#include <I18n.h>

#include <algorithm>

#include "components/TouchHeaderBackButton.h"
#include "components/UITheme.h"
#include "fontIds.h"
#include "util/HtmlToPlainText.h"

DetailsActivity::DetailsActivity(GfxRenderer& renderer, MappedInputManager& input, DetailsSpec spec)
    : Activity("Details", renderer, input), spec(std::move(spec)) {}

void DetailsActivity::onEnter() {
  Activity::onEnter();
  ActivityResult result;
  result.isCancelled = true;
  setResult(std::move(result));
  if (spec.descriptionHtml) spec.description = htmlToPlainText(spec.description, ' ');
  text.reserve(spec.title.size() + spec.author.size() + spec.source.size() + spec.metadata.size() +
               spec.description.size() + 10);
  for (const auto* value : {&spec.title, &spec.author, &spec.source, &spec.metadata, &spec.description}) {
    if (!value->empty()) {
      text += *value;
      text += "\n\n";
    }
  }
  // Release metadata copies before wrapping. At most an 8 KiB response plus metadata is retained.
  spec.title.clear();
  spec.author.clear();
  spec.source.clear();
  spec.metadata.clear();
  std::string().swap(spec.description);
  starts.reserve(16);
  starts.push_back(0);
  layout();
  requestUpdate();
}

void DetailsActivity::layout() {
  width = renderer.getScreenWidth();
  height = renderer.getScreenHeight();
  orientation = renderer.getOrientation();
  auto& theme = UITheme::getInstance();
  const auto& metrics = theme.getMetrics();
  const Rect safe = theme.getScreenSafeArea(renderer, !mappedInput.hasTouch(), false);
  header = TouchHeaderBackButton::headerRect(renderer, mappedInput);
  actions = TouchActionButtons::vertical(
      Rect{safe.x + metrics.contentSidePadding,
           safe.y + safe.height - metrics.verticalSpacing - TouchActionButtons::kDefaultHeight,
           std::max(1, safe.width - 2 * metrics.contentSidePadding), TouchActionButtons::kDefaultHeight},
      1);
  body = Rect{actions.container.x, header.y + header.height + metrics.verticalSpacing, actions.container.width,
              std::max(1, actions.container.y - header.y - header.height - 2 * metrics.verticalSpacing)};
  lineHeight = std::max(1, renderer.getLineHeight(UI_10_FONT_ID));
  const int count = std::max(1, body.height / lineHeight);
  lines.clear();
  lines.reserve(count);
  size_t cursor = starts[page];
  // Reused short scratch bounds temporary memory, including unbroken URLs.
  std::string line;
  line.reserve(256);
  while (cursor < text.size() && static_cast<int>(lines.size()) < count) {
    const size_t begin = cursor;
    size_t lastSpace = std::string::npos;
    line.clear();
    while (cursor < text.size() && text[cursor] != '\n') {
      size_t end = cursor + 1;
      while (end < text.size() && (static_cast<unsigned char>(text[end]) & 0xc0) == 0x80) ++end;
      line.append(text, cursor, end - cursor);
      if ((line.size() > 256 || renderer.getTextWidth(UI_10_FONT_ID, line.c_str()) > body.width) && cursor > begin) {
        if (lastSpace != std::string::npos) {
          cursor = lastSpace + 1;
          line.assign(text, begin, lastSpace - begin);
        } else
          line.resize(cursor - begin);
        break;
      }
      if (text[cursor] == ' ') lastSpace = cursor;
      cursor = end;
    }
    if (cursor < text.size() && text[cursor] == '\n') ++cursor;
    lines.push_back(line);
  }
  nextOffset = cursor;
}

void DetailsActivity::turn(int direction) {
  RenderLock lock;
  if (direction < 0) {
    if (!page) return;
    --page;
  } else {
    if (nextOffset >= text.size()) return;
    if (starts.size() == page + 1) starts.push_back(nextOffset);
    ++page;
  }
  layout();
  requestUpdate();
}
void DetailsActivity::confirm() {
  setResult(ActivityResult{});
  finish();
}
void DetailsActivity::loop() {
  if (width != renderer.getScreenWidth() || height != renderer.getScreenHeight() ||
      orientation != renderer.getOrientation()) {
    RenderLock lock;
    page = 0;
    starts.resize(1);
    layout();
    requestUpdate();
  }
  if (mappedInput.wasReleased(MappedInputManager::Button::Back) ||
      TouchHeaderBackButton::wasTapped(mappedInput, header)) {
    finish();
    return;
  }
  int x, y;
  if (mappedInput.wasReleased(MappedInputManager::Button::Confirm) ||
      (mappedInput.wasScreenTapped(x, y) && TouchActionButtons::indexAt(actions, x, y) == 0)) {
    confirm();
    return;
  }
  const auto swipe = mappedInput.wasSwipe();
  if (swipe == MappedInputManager::SwipeDir::Up)
    turn(1);
  else if (swipe == MappedInputManager::SwipeDir::Down)
    turn(-1);
  navigator.onNextRelease([this] { turn(1); });
  navigator.onPreviousRelease([this] { turn(-1); });
}
void DetailsActivity::render(RenderLock&&) {
  renderer.clearScreen();
  TouchHeaderBackButton::draw(renderer, header, spec.header.c_str(), false);
  for (size_t i = 0; i < lines.size(); ++i)
    renderer.drawText(UI_10_FONT_ID, body.x, body.y + i * lineHeight, lines[i].c_str());
  const char* labels[] = {spec.actionLabel.c_str()};
  TouchActionButtons::draw(renderer, actions, labels, 0, -1, UI_10_FONT_ID);
  const auto hints =
      mappedInput.mapLabels(tr(STR_BACK), spec.actionLabel.c_str(), tr(STR_PREV_PAGE), tr(STR_NEXT_PAGE));
  GUI.drawButtonHints(renderer, hints.btn1, hints.btn2, hints.btn3, hints.btn4);
  renderer.displayBuffer();
}

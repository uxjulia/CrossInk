#include "ClipTextBuilder.h"

#include <Logging.h>

#include <algorithm>
#include <cctype>

namespace ClipTextBuilder {
namespace {

bool hasEmSpace(const std::string& word) {
  return word.size() >= 3 && static_cast<unsigned char>(word[0]) == 0xE2 &&
         static_cast<unsigned char>(word[1]) == 0x80 && static_cast<unsigned char>(word[2]) == 0x83;
}

bool isUtf8SpaceAt(const std::string& text, const size_t index, size_t& advance) {
  const auto c = static_cast<unsigned char>(text[index]);
  if (c == 0xC2 && index + 1 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0xA0) {
    advance = 2;
    return true;
  }
  if (c == 0xE2 && index + 2 < text.size() && static_cast<unsigned char>(text[index + 1]) == 0x80) {
    const auto c2 = static_cast<unsigned char>(text[index + 2]);
    if (c2 == 0x83 || c2 == 0xAF) {
      advance = 3;
      return true;
    }
  }
  return false;
}

std::string cleanWordText(const std::string& word) {
  std::string out;
  out.reserve(word.size());
  for (size_t i = 0; i < word.size();) {
    size_t advance = 0;
    if (isUtf8SpaceAt(word, i, advance)) {
      if (!out.empty() && out.back() != ' ') {
        out += ' ';
      }
      i += advance;
      continue;
    }
    const char c = word[i++];
    if (c == '\r' || c == '\n' || c == '\t') {
      if (!out.empty() && out.back() != ' ') {
        out += ' ';
      }
      continue;
    }
    out += c;
  }

  while (!out.empty() && out.front() == ' ') {
    out.erase(out.begin());
  }
  while (!out.empty() && out.back() == ' ') {
    out.pop_back();
  }
  return out;
}

std::string stripTrailingInsertedHyphen(std::string word, const bool insertedHyphen) {
  if (insertedHyphen && !word.empty() && word.back() == '-') {
    word.pop_back();
  }
  return word;
}

}  // namespace

ClippingResult build(const std::vector<WordRef>& words, const int from, const int to, const int total,
                     const int startPageInSection, const int sectionPageCount) {
  std::string text;
  text.reserve(256);

  uint16_t startPageWordIndex = 0;
  uint16_t endPageWordIndex = 0;
  for (int i = 0; i <= to; ++i) {
    if (i < from && words[i].pageIdx == words[from].pageIdx) {
      startPageWordIndex++;
    }
    if (i < to && words[i].pageIdx == words[to].pageIdx) {
      endPageWordIndex++;
    }
  }

  constexpr int ANCHOR_WORDS = 4;
  std::string startAnchor;
  int anchorCount = 0;

  for (int i = from; i <= to; ++i) {
    const auto wordText = stripTrailingInsertedHyphen(cleanWordText(words[i].text), words[i].endsWithInsertedHyphen);
    if (wordText.empty()) {
      continue;
    }
    const bool yGap =
        i > from && words[i].pageIdx == words[i - 1].pageIdx && words[i].y > words[i - 1].y + words[i - 1].h;
    const bool paragraphStart = i > from && (hasEmSpace(words[i].text) || words[i].paragraphStart || yGap);

    if (i > from && !text.empty() && !paragraphStart) {
      const auto prevClean = cleanWordText(words[i - 1].text);
      if (!prevClean.empty() && prevClean.back() == '-' && !std::isspace(static_cast<unsigned char>(wordText[0])) &&
          !std::ispunct(static_cast<unsigned char>(wordText[0]))) {
        text += wordText;
        continue;
      }
    }

    if (paragraphStart) {
      text += '\n';
    } else if (!text.empty()) {
      const bool attached = words[i].pageIdx == words[i - 1].pageIdx && words[i].y == words[i - 1].y &&
                            words[i].x <= words[i - 1].x + words[i - 1].w + 2;
      if (!attached) {
        text += ' ';
      }
    }
    text += wordText;

    if (anchorCount < ANCHOR_WORDS) {
      if (!startAnchor.empty()) startAnchor += ' ';
      startAnchor += wordText;
      anchorCount++;
    }
  }

  std::string endAnchor;
  anchorCount = 0;
  for (int i = to; i >= from && anchorCount < ANCHOR_WORDS; --i) {
    const auto wordText = stripTrailingInsertedHyphen(cleanWordText(words[i].text), words[i].endsWithInsertedHyphen);
    endAnchor = endAnchor.empty() ? wordText : wordText + ' ' + endAnchor;
    anchorCount++;
  }

  constexpr int CONTEXT_WORDS = 3;
  std::string beforeStart;
  for (int i = from - 1; i >= 0 && (from - i) <= CONTEXT_WORDS; --i) {
    const auto stripped = stripTrailingInsertedHyphen(cleanWordText(words[i].text), words[i].endsWithInsertedHyphen);
    if (stripped.find_first_not_of(' ') == std::string::npos) continue;
    beforeStart = beforeStart.empty() ? stripped : stripped + ' ' + beforeStart;
  }
  std::string afterEnd;
  for (int i = to + 1; i < total && (i - to) <= CONTEXT_WORDS; ++i) {
    const auto stripped = stripTrailingInsertedHyphen(cleanWordText(words[i].text), words[i].endsWithInsertedHyphen);
    if (stripped.find_first_not_of(' ') == std::string::npos) continue;
    afterEnd = afterEnd.empty() ? stripped : afterEnd + ' ' + stripped;
  }

  std::string midText;
  constexpr int MID_WORDS = 4;
  int midStart = (from + to) / 2 - (MID_WORDS / 2);
  int midEnd = midStart + MID_WORDS - 1;
  if (midStart < from) midStart = from;
  if (midEnd > to) midEnd = to;
  for (int i = midStart; i <= midEnd; ++i) {
    const auto wordText = stripTrailingInsertedHyphen(cleanWordText(words[i].text), words[i].endsWithInsertedHyphen);
    if (!midText.empty()) midText += ' ';
    midText += wordText;
  }

  LOG_DBG("CLIP", "Built clipping: words=%d start=\"%.24s\" end=\"%.24s\"", to - from + 1, startAnchor.c_str(),
          endAnchor.c_str());

  return ClippingResult{std::move(text),
                        from,
                        to,
                        static_cast<uint16_t>(startPageInSection + words[from].pageIdx),
                        static_cast<uint16_t>(startPageInSection + words[to].pageIdx),
                        static_cast<uint16_t>(std::max(1, sectionPageCount)),
                        startPageWordIndex,
                        endPageWordIndex,
                        UINT16_MAX,
                        std::move(startAnchor),
                        std::move(endAnchor),
                        std::move(beforeStart),
                        std::move(afterEnd),
                        std::move(midText),
                        static_cast<uint16_t>(to - from + 1)};
}

}  // namespace ClipTextBuilder

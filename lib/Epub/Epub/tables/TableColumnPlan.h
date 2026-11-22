#pragma once

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace TableColumnLayout {

// The optional intrinsic-width probe is intentionally as bounded as CrossPoint's
// simple table path. Richer/longer markup keeps the existing row layout.
constexpr unsigned PROBE_COLUMNS = 4;
struct Measures {
  enum class ProbeStatus : uint8_t { Unsupported, Accepted, ScanLimit, WordLimit, ReadFailure };
  std::array<uint16_t, PROBE_COLUMNS> minimum{};
  std::array<uint16_t, PROBE_COLUMNS> preferred{};
  uint8_t count = 0;
  uint16_t bytesScanned = 0;
  uint16_t wordsScanned = 0;
  ProbeStatus status = ProbeStatus::Unsupported;
};
struct Plan {
  std::array<uint16_t, PROBE_COLUMNS> ends{};
  uint8_t count = 0;
};

inline Plan proportionalPlan(const Measures& measure, const uint16_t tableWidth, const uint8_t padding) {
  const unsigned columns = measure.count;
  const unsigned floor = 2u * padding + 20;
  if (columns < 2 || columns > PROBE_COLUMNS || tableWidth < floor * columns || tableWidth > 2047) return {};
  std::array<unsigned, PROBE_COLUMNS> minimum{}, preferred{};
  unsigned minTotal = 0, prefTotal = 0;
  for (unsigned i = 0; i < columns; ++i) {
    minimum[i] = std::max<unsigned>(floor, measure.minimum[i] + 2u * padding);
    preferred[i] = std::max<unsigned>(minimum[i], measure.preferred[i] + 2u * padding);
    minTotal += minimum[i];
    prefTotal += preferred[i];
  }

  // Floors are reserved before sharing any extra width so a squeeze cannot
  // produce a clipped cell or push the final edge outside the viewport.
  Plan plan;
  plan.count = measure.count;
  unsigned assigned = 0;
  for (unsigned i = 0; i < columns; ++i) {
    unsigned width;
    if (prefTotal <= tableWidth) {
      width = preferred[i] + (tableWidth - prefTotal) * preferred[i] / prefTotal;
    } else if (minTotal <= tableWidth) {
      width = minimum[i] + (tableWidth - minTotal) * (preferred[i] - minimum[i]) / (prefTotal - minTotal);
    } else {
      width = floor + (tableWidth - floor * columns) * (minimum[i] - floor) / (minTotal - floor * columns);
    }
    assigned += width;
    plan.ends[i] = i + 1 == columns ? tableWidth : static_cast<uint16_t>(assigned);
  }
  return plan;
}

// Read optional lookahead from the existing seekable chapter handle. No DOM,
// per-word allocation or glyph preparation. The main XML parser is authoritative;
// complex tags can only disable this measurement, never change parsed content.
template <typename Read, typename Width>
Measures measureSimpleTable(Read read, Width width, const uint16_t widthLimit, const uint16_t space) {
  constexpr unsigned MAX_SCAN_BYTES = 32 * 1024;
  constexpr unsigned MAX_MEASURED_WORDS = 384;
  Measures whole, row;
  // A 256-byte read batch cuts SD read calls by about 2.7x at the same 32 KiB
  // lookahead cap. The additional 160 bytes are bounded stack scratch on the
  // parser task; tag and word state remain separately bounded across batches.
  char buffer[256], tag[48], word[40];
  unsigned tagLen = 0, wordLen = 0, line = 0, measuredWords = 0;
  unsigned consumed = 0;
  bool inTag = false, inTable = false, inRow = false, inCell = false, inEntity = false;
  bool bold = false, spanning = false, skipCell = false;
  char quote = 0;

  const auto reject = [&](const Measures::ProbeStatus status = Measures::ProbeStatus::Unsupported) {
    Measures result;
    result.bytesScanned = static_cast<uint16_t>(consumed);
    result.wordsScanned = static_cast<uint16_t>(measuredWords);
    result.status = status;
    return result;
  };

  const auto whitespace = [](const char c) { return c == ' ' || c == '\n' || c == '\r' || c == '\t'; };
  const auto finishWord = [&] {
    if (wordLen == 0 || !inCell || skipCell || row.count == 0) return;
    if (++measuredWords > MAX_MEASURED_WORDS) return;
    word[wordLen] = '\0';
    const auto w = static_cast<unsigned>(std::clamp(width(word, bold), 0, static_cast<int>(widthLimit)));
    row.minimum[row.count - 1] = static_cast<uint16_t>(std::max<unsigned>(row.minimum[row.count - 1], w));
    line = std::min<unsigned>(widthLimit, line + (line > 0 ? space : 0) + w);
    wordLen = 0;
  };
  const auto finishLine = [&] {
    finishWord();
    if (inCell && !skipCell && row.count > 0) {
      row.preferred[row.count - 1] = static_cast<uint16_t>(std::max<unsigned>(row.preferred[row.count - 1], line));
    }
    line = 0;
    wordLen = 0;
    inEntity = false;
  };

  for (; consumed < MAX_SCAN_BYTES;) {
    const unsigned requested = std::min<unsigned>(sizeof(buffer), MAX_SCAN_BYTES - consumed);
    const int count = read(buffer, requested);
    if (count < 0 || count > static_cast<int>(requested)) return reject(Measures::ProbeStatus::ReadFailure);
    if (count == 0) return reject();
    consumed += count;
    for (int i = 0; i < count; ++i) {
      const char c = buffer[i];
      if (!inTag) {
        if (c == '<') {
          inTag = true;
          tagLen = 0;
        } else if (inCell && !skipCell) {
          if (inEntity) {
            inEntity = c != ';';
          } else if (whitespace(c)) {
            finishWord();
            if (measuredWords > MAX_MEASURED_WORDS) return reject(Measures::ProbeStatus::WordLimit);
          } else {
            if (wordLen + 1 >= sizeof(word)) return reject();  // do not split a UTF-8 word during the probe
            inEntity = c == '&';
            word[wordLen++] = inEntity ? 'n' : c;  // approximate the width of an entity, as upstream does
          }
        }
        continue;
      }
      if (quote) {
        if (c == quote) quote = 0;
      } else if (c == '\'' || c == '"') {
        quote = c;
      } else if (c == '>') {
        inTag = false;
        const std::string_view text(tag, tagLen);
        const bool close = text.starts_with('/');
        const auto name = text.substr(close ? 1 : 0, text.find_first_of(" \r\n\t/", close ? 1 : 0) - (close ? 1 : 0));
        if (name == "table") {
          if (close) {
            if (!inTable || inRow || inCell || whole.count < 2) return reject();
            whole.status = Measures::ProbeStatus::Accepted;
            whole.bytesScanned = static_cast<uint16_t>(consumed);
            whole.wordsScanned = static_cast<uint16_t>(measuredWords);
            return whole;
          }
          if (inTable) return reject();  // let the real parser handle a nested table
          inTable = true;
        } else if (name == "tr") {
          if (!inTable || inCell || (close != inRow)) return reject();
          inRow = !close;
          if (!close) {
            row = {};
            spanning = false;
          } else if (!spanning) {
            if (row.count < 2 || (whole.count != 0 && row.count != whole.count)) return reject();
            whole.count = row.count;
            for (unsigned k = 0; k < row.count; ++k) {
              whole.minimum[k] = std::max(whole.minimum[k], row.minimum[k]);
              whole.preferred[k] = std::max(whole.preferred[k], row.preferred[k]);
            }
          }
        } else if (name == "td" || name == "th") {
          if (!inRow || (close != inCell)) return reject();
          if (close) finishLine();
          inCell = !close;
          if (!close) {
            if (row.count == PROBE_COLUMNS) return reject();
            ++row.count;
            skipCell = text.find("colspan") != text.npos || text.find("rowspan") != text.npos;
            spanning |= skipCell;
            bold = name == "th";
          }
        } else if (name == "br") {
          finishLine();
        } else if (name == "p" || name == "div" || name == "li") {
          finishWord();
        } else if (name.starts_with("!") || name.starts_with("?")) {
          return reject();  // comments/CDATA are not scanned approximately
        }
        if (measuredWords > MAX_MEASURED_WORDS) return reject(Measures::ProbeStatus::WordLimit);
        continue;
      }
      if (tagLen == sizeof(tag)) return reject();
      tag[tagLen++] = c;
    }
  }
  return reject(Measures::ProbeStatus::ScanLimit);
}
}  // namespace TableColumnLayout

#pragma once

#include <ArduinoJson.h>

#include "ReaderStatusBarConfig.h"

inline bool readDisplayStatusBarJson(const JsonVariantConst source, DisplayStatusBarConfig& config,
                                     const bool clockAvailable) {
  const auto slots = source.as<JsonArrayConst>();
  if (slots.size() != config.slots.size()) return false;
  DisplayStatusBarConfig parsed = config;  // Slots only; keep the separately stored battery style.
  for (unsigned i = 0; i < parsed.slots.size(); ++i) {
    if (!slots[i].is<int>() || !validDisplayStatusBarItemValue(slots[i].as<int>(), clockAvailable)) return false;
    parsed.slots[i] = static_cast<ReaderStatusBarItem>(slots[i].as<int>());
  }
  config = parsed;
  return true;
}

inline void writeReaderStatusBarJson(JsonObject target, const ReaderStatusBarConfig& config) {
  JsonArray slots = target["slots"].to<JsonArray>();
  for (const auto item : config.slots) slots.add(static_cast<uint8_t>(item));
  target["percentageFormat"] = config.percentageFormat;
  target["progressBar"] = config.progressBar;
  target["thickness"] = config.progressBarThickness;
  target["hidden"] = config.hidden;
  target["battery"] = static_cast<uint8_t>(config.batteryStyle);
  target["centered"] = config.centered;
  target["separator"] = static_cast<uint8_t>(config.separator);
}

// Missing means the document predates battery styles; callers decide the fallback.
inline bool readReaderStatusBarBatteryStyle(const JsonVariantConst value, ReaderStatusBarBatteryStyle& style) {
  if (value.isUnbound()) return true;
  const int raw = value.as<int>();
  if (!value.is<int>() || !validReaderStatusBarChoice(raw, static_cast<int>(ReaderStatusBarBatteryStyle::Count))) {
    return false;
  }
  style = static_cast<ReaderStatusBarBatteryStyle>(raw);
  return true;
}

// Missing means the document predates separators; the default stays in place.
inline bool readReaderStatusBarSeparator(const JsonVariantConst value, ReaderStatusBarSeparator& separator) {
  if (value.isUnbound()) return true;
  const int raw = value.as<int>();
  if (!value.is<int>() || !validReaderStatusBarChoice(raw, static_cast<int>(ReaderStatusBarSeparator::Count))) {
    return false;
  }
  separator = static_cast<ReaderStatusBarSeparator>(raw);
  return true;
}

inline bool readReaderStatusBarJson(const JsonVariantConst source, ReaderStatusBarConfig& config,
                                    const bool clockAvailable, const int percentageFormatCount,
                                    const int progressModeCount, const int thicknessCount) {
  const JsonArrayConst slots = source["slots"].as<JsonArrayConst>();
  if (slots.size() != ReaderStatusBarConfig::SLOT_COUNT) return false;
  ReaderStatusBarConfig parsed;
  for (unsigned i = 0; i < ReaderStatusBarConfig::SLOT_COUNT; ++i) {
    const int item = slots[i].as<int>();
    if (!slots[i].is<int>() || !validReaderStatusBarItemValue(item, clockAvailable)) return false;
    parsed.slots[i] = static_cast<ReaderStatusBarItem>(item);
  }
  const int percentageFormat = source["percentageFormat"].as<int>();
  const int progressMode = source["progressBar"].as<int>();
  const int thickness = source["thickness"].as<int>();
  if (!source["percentageFormat"].is<int>() || !validReaderStatusBarChoice(percentageFormat, percentageFormatCount) ||
      !source["progressBar"].is<int>() || !validReaderStatusBarChoice(progressMode, progressModeCount) ||
      !source["thickness"].is<int>() || !validReaderStatusBarChoice(thickness, thicknessCount)) {
    return false;
  }
  if (!source["hidden"].isUnbound() && !source["hidden"].is<bool>()) return false;
  if (!readReaderStatusBarBatteryStyle(source["battery"], parsed.batteryStyle)) return false;
  if (!source["centered"].isUnbound() && !source["centered"].is<bool>()) return false;
  if (!readReaderStatusBarSeparator(source["separator"], parsed.separator)) return false;
  parsed.hidden = source["hidden"] | false;
  parsed.centered = source["centered"] | false;
  parsed.percentageFormat = percentageFormat;
  parsed.progressBar = progressMode;
  parsed.progressBarThickness = thickness;
  config = parsed;
  return true;
}

inline bool readReaderStatusBarsPayload(const JsonVariantConst source, ReaderStatusBarsPayload& payload,
                                        const bool clockAvailable, const int percentageFormatCount,
                                        const int progressModeCount, const int thicknessCount, const int xtcModeCount) {
  ReaderStatusBarsPayload parsed;
  const int xtcMode = source["xtcMode"].as<int>();
  if (!readReaderStatusBarJson(source["top"], parsed.top, clockAvailable, percentageFormatCount, progressModeCount,
                               thicknessCount) ||
      !readReaderStatusBarJson(source["bottom"], parsed.bottom, clockAvailable, percentageFormatCount,
                               progressModeCount, thicknessCount) ||
      !source["xtcMode"].is<int>() || !validReaderStatusBarChoice(xtcMode, xtcModeCount)) {
    return false;
  }
  parsed.xtcMode = static_cast<uint8_t>(xtcMode);
  payload = parsed;
  return true;
}

// Repairs a damaged saved bar one field at a time, retaining valid choices.
// Returns true when the caller must resave the corrected document.
inline bool repairReaderStatusBarJson(const JsonVariantConst source, ReaderStatusBarConfig& config,
                                      const bool clockAvailable, const int percentageFormatCount,
                                      const int progressModeCount, const int thicknessCount) {
  if (readReaderStatusBarJson(source, config, clockAvailable, percentageFormatCount, progressModeCount,
                              thicknessCount)) {
    return false;
  }
  const JsonArrayConst slots = source["slots"].as<JsonArrayConst>();
  // The previous layout had two left slots, one center slot, and three right slots.
  constexpr unsigned LEGACY_SLOT_COUNT = ReaderStatusBarConfig::SLOT_COUNT - 1;
  if (slots.size() != LEGACY_SLOT_COUNT && slots.size() != ReaderStatusBarConfig::SLOT_COUNT) return true;
  ReaderStatusBarConfig repaired;
  for (unsigned i = 0; i < slots.size(); ++i) {
    const int item = slots[i].as<int>();
    if (slots[i].is<int>() && validReaderStatusBarItemValue(item, clockAvailable)) {
      const unsigned target = slots.size() == LEGACY_SLOT_COUNT && i >= 2 ? i + 1 : i;
      repaired.slots[target] = static_cast<ReaderStatusBarItem>(item);
    }
  }
  const auto choice = [](const JsonVariantConst value, const int count, const uint8_t fallback) {
    const int raw = value.as<int>();
    return value.is<int>() && validReaderStatusBarChoice(raw, count) ? static_cast<uint8_t>(raw) : fallback;
  };
  repaired.percentageFormat = choice(source["percentageFormat"], percentageFormatCount, repaired.percentageFormat);
  repaired.progressBar = choice(source["progressBar"], progressModeCount, repaired.progressBar);
  repaired.progressBarThickness = choice(source["thickness"], thicknessCount, repaired.progressBarThickness);
  repaired.hidden = source["hidden"].is<bool>() && source["hidden"].as<bool>();
  if (!readReaderStatusBarBatteryStyle(source["battery"], repaired.batteryStyle)) {
    repaired.batteryStyle = ReaderStatusBarBatteryStyle::IconAndPercent;
  }
  repaired.centered = source["centered"].is<bool>() && source["centered"].as<bool>();
  if (!readReaderStatusBarSeparator(source["separator"], repaired.separator)) {
    repaired.separator = ReaderStatusBarSeparator::Bullet;
  }
  config = repaired;
  return true;
}

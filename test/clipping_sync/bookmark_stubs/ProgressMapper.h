#pragma once
#include <Epub.h>

#include <cstdint>
#include <memory>
enum class PositionCoordinateSpace { SourceDocument, CurrentDocument };
struct CrossPointPosition {
  int spineIndex, pageNumber, totalPages;
  uint16_t paragraphIndex = 0;
  bool hasParagraphIndex = false;
  bool valid = true;
};
struct KOReaderPosition {
  std::string xpath;
  float percentage;
  bool valid = true;
};
struct ProgressMapper {
  static KOReaderPosition toKOReader(const std::shared_ptr<Epub>&, const CrossPointPosition& p,
                                     PositionCoordinateSpace) {
    return {"/test/path[" + std::to_string(p.spineIndex) + "]", float(p.pageNumber) / (p.totalPages - 1)};
  }
  static CrossPointPosition toCrossPoint(const std::shared_ptr<Epub>&, const KOReaderPosition&, int, int,
                                         PositionCoordinateSpace, int fixedSpineResolution = 0) {
    if (fixedSpineResolution <= 1) return {3, 0, 0};
    return {3, (fixedSpineResolution - 1) * 2 / 5, fixedSpineResolution, 42, true};
  }
};

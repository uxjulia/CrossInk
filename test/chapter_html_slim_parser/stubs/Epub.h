#pragma once

#include <GfxRenderer.h>

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

class Epub {
 public:
  bool optimizerImageAvailable = false;
  uint16_t optimizerImageWidth = 0;
  uint16_t optimizerImageHeight = 0;
  mutable size_t streamReadCount = 0;
  mutable size_t extractCount = 0;
  mutable bool extractHadLoan = false;
  bool requireLoan = false;
  bool streamFails = false;
  bool extractSucceeds = false;
  std::vector<uint8_t> probeBytes;

  template <typename Output>
  bool readItemContentsToStream(const std::string&, Output& output, size_t, bool = false) const {
    ++streamReadCount;
    if (requireLoan && !GfxRenderer::loanActive) return false;
    output.write(probeBytes.data(), probeBytes.size());
    return !streamFails;
  }
  bool extractItemToFile(const std::string&, const std::string&) const {
    ++extractCount;
    extractHadLoan = GfxRenderer::loanActive;
    return extractSucceeds;
  }
  bool getOptimizerImageDimensions(const std::string&, uint16_t& width, uint16_t& height) const {
    if (!optimizerImageAvailable) return false;
    width = optimizerImageWidth;
    height = optimizerImageHeight;
    return true;
  }
};

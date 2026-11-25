#pragma once
class GfxRenderer {
 public:
  bool available = true;
  int clears = 0;
  bool hasOldNotice = true;
  void clearScreen() {
    ++clears;
    hasOldNotice = false;
  }
  bool hasFrameBuffer() const { return available; }
};

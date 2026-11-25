#pragma once
#include <string>

#include "ReaderSession.h"

// Reader callers serialize these small fields with RenderLock. Rendering only
// records scalars; the main loop and exit path perform SD writes.
class PluginReaderEvents {
 public:
  void noteTurn(bool forward) { session.noteTurn(forward, true); }
  void rendered(int progressBasisPoints);
  // Main-loop polling must never wait for a page render.
  void pollWhenIdle(const std::string& book);
  // Callers of poll/flush/exit hold RenderLock (including sleep preparation).
  void poll(const std::string& book);
  void flush(const std::string& book);
  void exit(const std::string& book, int percent);

 private:
  ReaderSession session;
  bool renderedPage = false;
  bool opened = false;
};

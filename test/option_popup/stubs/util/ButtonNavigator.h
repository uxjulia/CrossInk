#pragma once

#include "MappedInputManager.h"

class ButtonNavigator {
 public:
  template <typename Callback>
  void onPreviousRelease(Callback&& callback) {
    if (previousRelease) {
      previousRelease = false;
      callback();
    }
  }
  template <typename Callback>
  void onNextRelease(Callback&& callback) {
    if (nextRelease) {
      nextRelease = false;
      callback();
    }
  }
  template <typename Callback>
  void onPreviousContinuous(Callback&&) {}
  template <typename Callback>
  void onNextContinuous(Callback&&) {}
  static int previousPageIndex(int index, int total, int) { return previousIndex(index, total); }
  static int nextPageIndex(int index, int total, int) { return nextIndex(index, total); }
  static int previousIndex(int index, int total) { return total > 0 ? (index + total - 1) % total : 0; }
  static int nextIndex(int index, int total) { return total > 0 ? (index + 1) % total : 0; }
  static void injectNextRelease() { nextRelease = true; }

 private:
  inline static bool previousRelease = false;
  inline static bool nextRelease = false;
};

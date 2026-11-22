#pragma once

// Tracks the one source rebuild allowed after repeated failures reading a
// committed section page. A staged incremental page is not a durable repair;
// keep bypassing the old cache until a full or partial replacement is committed.
class SectionCacheRepairState {
 public:
  enum class State : unsigned char { Idle, Staging, Failed };

  bool shouldBypassCache(const int spineIndex) const { return state_ == State::Staging && spineIndex_ == spineIndex; }
  bool hasFailed(const int spineIndex) const { return state_ == State::Failed && spineIndex_ == spineIndex; }
  bool isStaging(const int spineIndex) const { return state_ == State::Staging && spineIndex_ == spineIndex; }

  void begin(const int spineIndex) {
    spineIndex_ = spineIndex;
    state_ = State::Staging;
  }

  void markFailed(const int spineIndex) {
    if (isStaging(spineIndex)) state_ = State::Failed;
  }

  void markCommitted(const int spineIndex) {
    if (spineIndex_ == spineIndex) reset();
  }

  void resetForSpine(const int spineIndex) {
    if (spineIndex_ != -1 && spineIndex_ != spineIndex) reset();
  }

  void reset() {
    spineIndex_ = -1;
    state_ = State::Idle;
  }

 private:
  int spineIndex_ = -1;
  State state_ = State::Idle;
};

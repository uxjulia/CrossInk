#pragma once

struct TestSettings {
  bool track = true;
  bool shouldTrackReadingStats() const { return track; }
};
inline TestSettings SETTINGS;

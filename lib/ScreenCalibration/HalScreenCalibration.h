#pragma once

#include "PanelLayout.h"
#include "ScreenInsets.h"

// Device-local calibration deliberately stays outside SD settings/backups.
// Failure to load leaves the original bezel allowances in place.
namespace HalScreenCalibration {
ScreenInsets defaults();
PanelLayout panelLayout();
ScreenInsets load();
bool save(const ScreenInsets& insets);
}  // namespace HalScreenCalibration

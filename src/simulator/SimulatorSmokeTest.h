#pragma once

#include <cstdint>

#ifdef SIMULATOR
void runSimulatorSmokeTestTick();
void prepareSimulatorSmokeState();
// True when a smoke scenario captured this network reboot request instead of letting the simulator relaunch.
bool simulatorInterceptNetworkRestart(uint32_t target, uint32_t payload);
#endif

#pragma once

// Releases the ESP32-S3 USB-OTG PHY and reconnects the hardware USB
// Serial/JTAG peripheral before a normal application restart. No-op on targets
// other than the USB Drive-capable X4 Pro, X4 Classic, and Picco.
void handoffUsbOtgToSerialJtag();

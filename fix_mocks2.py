import os
import re

mock_dir = "lib/simulator_mock/src"

# Fix WString.h
wstring_path = os.path.join(mock_dir, "WString.h")
with open(wstring_path, "r") as f:
    wstring = f.read()
wstring = wstring.replace("String(const std::string& s) : std::string(s) {}", "explicit String(const std::string& s) : std::string(s) {}")
with open(wstring_path, "w") as f:
    f.write(wstring)

# Fix Arduino.h for random()
arduino_path = os.path.join(mock_dir, "Arduino.h")
with open(arduino_path, "r") as f:
    arduino = f.read()
if "inline long random(long max)" not in arduino:
    arduino += "\ninline long random(long max) { return std::rand() % max; }\n"
with open(arduino_path, "w") as f:
    f.write(arduino)

# Fix FreeRTOS for TaskHandle_t
freertos_path = os.path.join(mock_dir, "freertos/FreeRTOS.h")
with open(freertos_path, "r") as f:
    freertos = f.read()
if "TaskHandle_t" not in freertos:
    freertos += "\ntypedef void* TaskHandle_t;\n"
with open(freertos_path, "w") as f:
    f.write(freertos)

# Create Dummy NetworkUdp.h
udp_path = os.path.join(mock_dir, "NetworkUdp.h")
with open(udp_path, "w") as f:
    f.write("#pragma once\n")

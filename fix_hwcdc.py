import os
import re

mock_dir = "lib/simulator_mock/src"

arduino_path = os.path.join(mock_dir, "Arduino.h")
with open(arduino_path, "r") as f:
    ard = f.read()

# remove the macros properly using regex
ard = re.sub(r'#ifndef max\n#define max\(a,b\) \(\(a\)>\(b\)\?\(a\):\(b\)\)\n#endif', '', ard)
ard = re.sub(r'#ifndef min\n#define min\(a,b\) \(\(a\)<\(b\)\?\(a\):\(b\)\)\n#endif', '', ard)

with open(arduino_path, "w") as f:
    f.write(ard)


hs_path = os.path.join(mock_dir, "HardwareSerial.h")
with open(hs_path, "r") as f:
    hs = f.read()

if "int read() override {" not in hs:
    hs = hs.replace("int available() override { return 0; }", "int available() override { return 0; }\\n    int read() override { return -1; }\\n    int peek() override { return -1; }")
    with open(hs_path, "w") as f:
        f.write(hs)

print("Fixed Arduino and HardwareSerial")

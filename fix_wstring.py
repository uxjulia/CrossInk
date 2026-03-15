import os

mock_dir = "lib/simulator_mock/src"

wstring_code = """#pragma once
#include <string>
#include <cstdint>

class String {
public:
    std::string s;
    String() {}
    String(const char* str) : s(str ? str : "") {}
    String(const std::string& str) : s(str) {}
    
    String& operator=(const char* str) { s = str ? str : ""; return *this; }
    String& operator=(const std::string& str) { s = str; return *this; }
    String& operator+=(const char* str) { s += str; return *this; }
    String& operator+=(char c) { s += c; return *this; }
    
    bool startsWith(const String& prefix) const { return s.find(prefix.s) == 0; }
    bool startsWith(const char* prefix) const { return s.find(prefix) == 0; }
    String substring(unsigned int from, unsigned int to = (unsigned int)-1) const {
        if (from >= s.length()) return String();
        return String(s.substr(from, to == (unsigned int)-1 ? std::string::npos : to - from));
    }
    void trim() {
        size_t first = s.find_first_not_of(" \\t\\n\\r");
        if(first == std::string::npos) { s.clear(); return; }
        size_t last = s.find_last_not_of(" \\t\\n\\r");
        s = s.substr(first, (last-first+1));
    }
    bool isEmpty() const { return s.empty(); }
    size_t length() const { return s.length(); }
    const char* c_str() const { return s.c_str(); }
    
    bool operator==(const char* other) const { return s == (other ? other : ""); }
    bool operator!=(const char* other) const { return !(*this == other); }
    bool operator==(const String& other) const { return s == other.s; }
    bool operator!=(const String& other) const { return s != other.s; }
    
    size_t write(uint8_t c) { s.push_back(c); return 1; }
    size_t write(const uint8_t *buffer, size_t size) { s.append((const char*)buffer, size); return size; }
    size_t write(const char *buffer, size_t size) { s.append(buffer, size); return size; }
};
"""

with open(f"{mock_dir}/WString.h", "w") as f:
    f.write(wstring_code)

storage_path = f"{mock_dir}/HalStorage.h"
with open(storage_path, "r") as f:
    storage = f.read()

if '#include "WString.h"' not in storage:
    storage = storage.replace("#include <string>", "#include <string>\\n#include \"WString.h\"")
    with open(storage_path, "w") as f:
        f.write(storage)

print("Fixed WString and HalStorage")

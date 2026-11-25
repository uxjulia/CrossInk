#pragma once

#include <cstdint>
#include <string>

// First 16 hex digits of SHA256(decimal timestamp + exact UTF-8 text).
bool clippingSyncIdentity(uint32_t timestamp, const std::string& text, char (&id)[17]);

std::string newClippingSyncId();

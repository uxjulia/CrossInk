#pragma once
#include <string>

// A bundle is assembled beside its destination, retaining plugin configuration
// already in that folder. The small journal permits recovery after power loss.
namespace PluginBundle {
using Cancel = bool (*)(void*);
std::string stagingPath(const std::string& destination);
bool recoverPending();
bool prepare(const std::string& destination, Cancel cancel, void* context);
bool publish(const std::string& destination);
void discard(const std::string& destination);
}  // namespace PluginBundle

#include "PluginBundle.h"

#include <DownloadFileSwap.h>
#include <FsHelpers.h>
#include <HalStorage.h>
#include <Logging.h>
#include <Memory.h>

#include "PluginEvents.h"
#include "network/ProtectedPaths.h"

namespace PluginBundle {
namespace {
constexpr char JOURNAL[] = "/.crosspoint/plugin-install.txt";
constexpr size_t COPY_BUFFER = 1024;
constexpr unsigned MAX_FILES = 128;
constexpr unsigned MAX_DEPTH = 8;

std::string sibling(const std::string& path, const char* suffix) {
  const auto slash = path.rfind('/');
  return path.substr(0, slash + 1) + "." + path.substr(slash + 1) + suffix;
}
std::string backupPath(const std::string& path) { return sibling(path, ".plugin-old"); }
bool removeTree(const std::string& path) { return !Storage.exists(path.c_str()) || Storage.removeDir(path.c_str()); }

// One reusable heap buffer avoids a large buffer in every recursion frame.
bool copyTree(const std::string& from, const std::string& to, uint8_t* buffer, unsigned depth, unsigned& entries,
              Cancel cancel, void* context) {
  if (depth > MAX_DEPTH || !Storage.mkdir(to.c_str())) return false;
  HalFile source = Storage.open(from.c_str());
  if (!source || !source.isDirectory()) {
    source.close();
    return false;
  }
  bool ok = true;
  for (HalFile file = source.openNextFile(); file; file = source.openNextFile()) {
    char name[128];
    const size_t length = file.getName(name, sizeof(name));
    if (++entries > MAX_FILES || length == 0 || length >= sizeof(name) - 1 || (cancel && cancel(context))) {
      file.close();
      ok = false;
      break;
    }
    const std::string child = from + "/" + name;
    const std::string output = to + "/" + name;
    if (file.isDirectory()) {
      file.close();
      ok = copyTree(child, output, buffer, depth + 1, entries, cancel, context);
    } else {
      HalFile dest;
      ok = Storage.openFileForWrite("PLUG", output, dest);
      const size_t fileBytes = file.size();
      size_t copied = 0;
      while (ok && copied < fileBytes) {
        if (cancel && cancel(context)) {
          ok = false;
          break;
        }
        const auto read = file.read(buffer, std::min(COPY_BUFFER, fileBytes - copied));
        if (read <= 0 || dest.write(buffer, read) != static_cast<size_t>(read)) {
          ok = false;
          break;
        }
        copied += read;
      }
      const bool sourceClosed = file.close();
      const bool destClosed = dest.close();
      ok = ok && sourceClosed && destClosed;
    }
    if (!ok) break;
  }
  ok = !FsHelpers::directoryIterationFailed(source) && ok;
  return source.close() && ok;
}
}  // namespace

std::string stagingPath(const std::string& destination) { return sibling(destination, ".plugin-part"); }

bool recoverPending() {
  if (!DownloadFileSwap::recover(JOURNAL)) return false;
  if (!Storage.exists(JOURNAL)) return true;
  std::string destination;
  if (!Storage.readFileToString("PLUG", JOURNAL, 240, destination) || !protectedpaths::isPluginPath(destination)) {
    LOG_ERR("PLUG", "Invalid bundle recovery journal");
    return false;
  }
  const auto backup = backupPath(destination);
  if (Storage.exists(backup.c_str())) {
    const bool ok =
        Storage.exists(destination.c_str()) ? removeTree(backup) : Storage.rename(backup.c_str(), destination.c_str());
    if (!ok) {
      LOG_ERR("PLUG", "Bundle recovery failed: %s", destination.c_str());
      return false;
    }
  }
  if (!removeTree(stagingPath(destination))) return false;
  return Storage.remove(JOURNAL);
}

bool prepare(const std::string& destination, Cancel cancel, void* context) {
  if (!protectedpaths::isPluginPath(destination) || !recoverPending()) return false;
  const auto stage = stagingPath(destination);
  if (!removeTree(stage) || Storage.exists(backupPath(destination).c_str())) return false;
  if (!Storage.exists(destination.c_str())) return Storage.mkdir(stage.c_str());
  auto buffer = makeUniqueNoThrow<uint8_t[]>(COPY_BUFFER);
  if (!buffer) {
    LOG_ERR("PLUG", "OOM: bundle copy buffer");
    return false;
  }
  unsigned entries = 0;
  if (copyTree(destination, stage, buffer.get(), 0, entries, cancel, context)) return true;
  LOG_ERR("PLUG", "Could not stage existing bundle: %s", destination.c_str());
  removeTree(stage);
  return false;
}

bool publish(const std::string& destination) {
  if (!protectedpaths::isPluginPath(destination)) return false;
  const std::string partial = std::string(JOURNAL) + ".part";
  HalFile journal;
  if (!Storage.openFileForWrite("PLUG", partial, journal)) return false;
  const bool written = journal.write(destination.data(), destination.size()) == destination.size();
  const bool closed = journal.close();
  if (!written || !closed || !DownloadFileSwap::publish(JOURNAL)) return false;
  if (Storage.exists(destination.c_str()) && !Storage.rename(destination.c_str(), backupPath(destination).c_str())) {
    recoverPending();
    return false;
  }
  if (!Storage.rename(stagingPath(destination).c_str(), destination.c_str())) {
    LOG_ERR("PLUG", "Bundle publish failed: %s", destination.c_str());
    recoverPending();
    return false;
  }
  if (!recoverPending()) LOG_ERR("PLUG", "Bundle installed; backup cleanup deferred");
  pluginevents::refreshSubscriptions();
  return true;
}
void discard(const std::string& destination) { removeTree(stagingPath(destination)); }
}  // namespace PluginBundle

#include "LookupHistory.h"

#include <HalStorage.h>
#include <Logging.h>
#include <OwnedFileSwap.h>
#include <common/FsApiConstants.h>

#include <algorithm>
#include <array>
#include <mutex>

namespace {
std::mutex historyMutex;
}

std::string LookupHistory::filePath(const std::string& cachePath) { return cachePath + "/dictionary_history.txt"; }

static LookupHistory::Status parseStatusCode(char code) {
  switch (code) {
    case 'D':
      return LookupHistory::Status::Direct;
    case 'T':
      return LookupHistory::Status::Stem;
    case 'Y':
      return LookupHistory::Status::AltForm;
    case 'S':
      return LookupHistory::Status::Suggestion;
    default:
      return LookupHistory::Status::NotFound;
  }
}

static LookupHistory::Entry parseLine(const char* lineBuf, int lineLen) {
  LookupHistory::Entry entry;
  int separator = -1;
  for (int i = lineLen - 1; i >= 0; i--) {
    if (lineBuf[i] == '|') {
      separator = i;
      break;
    }
  }
  if (separator >= 0 && separator + 1 < lineLen) {
    entry.word = std::string(lineBuf, separator);
    entry.status = parseStatusCode(lineBuf[separator + 1]);
  } else {
    entry.word = std::string(lineBuf, lineLen);
  }
  return entry;
}

std::vector<LookupHistory::Entry> LookupHistory::readRecent(const std::string& path) {
  std::vector<Entry> entries;
  entries.reserve(MAX_VISIBLE_ENTRIES);

  HalFile file;
  if (!Storage.openFileForRead("LH", path.c_str(), file)) return entries;
  const size_t fileLength = file.fileSize();

  char lineBuf[256];
  int lineLen = 0;
  size_t bytesRead = 0;
  auto retainLine = [&]() {
    if (lineLen == 0) return;
    Entry entry = parseLine(lineBuf, lineLen);
    if (!entry.word.empty()) {
      if (entries.size() == MAX_VISIBLE_ENTRIES) entries.erase(entries.begin());
      entries.push_back(std::move(entry));
    }
    lineLen = 0;
  };

  while (bytesRead < fileLength) {
    const int byte = file.read();
    if (byte < 0) break;
    ++bytesRead;
    if (byte == '\n' || byte == '\r') {
      retainLine();
    } else if (lineLen < static_cast<int>(sizeof(lineBuf)) - 1) {
      lineBuf[lineLen++] = static_cast<char>(byte);
    }
  }
  const bool completeRead = bytesRead == fileLength;
  // A reset during append can leave a partial last line. It is not an entry.
  if (completeRead && bytesRead > 0 && file.seek(fileLength - 1)) {
    const int finalByte = file.read();
    if (finalByte == '\n' || finalByte == '\r') retainLine();
  }
  const bool closed = file.close();
  if (!completeRead || !closed) {
    LOG_ERR("LH", "Failed to read complete history: %s", path.c_str());
    return {};
  }
  return entries;
}

// A failed append can leave bytes after the last complete record. Remove only
// that tail through the reserved replacement path before another append.
static bool discardInterruptedTail(const std::string& path) {
  HalFile source;
  if (!Storage.openFileForRead("LH", path.c_str(), source)) return !Storage.exists(path.c_str());
  const size_t length = source.fileSize();
  if (length == 0) return source.close();

  if (!source.seek(length - 1)) {
    source.close();
    return false;
  }
  const int last = source.read();
  if (last < 0) {
    source.close();
    return false;
  }
  if (last == '\n' || last == '\r') return source.close();

  if (!source.seek(0)) {
    source.close();
    return false;
  }
  std::array<uint8_t, 128> buffer{};
  size_t consumed = 0;
  size_t completeLength = 0;
  while (consumed < length) {
    const size_t chunkLength = std::min(buffer.size(), length - consumed);
    const int count = source.read(buffer.data(), chunkLength);
    if (count != static_cast<int>(chunkLength)) {
      source.close();
      return false;
    }
    for (int i = 0; i < count; ++i) {
      if (buffer[i] == '\n' || buffer[i] == '\r') completeLength = consumed + static_cast<size_t>(i) + 1;
    }
    consumed += chunkLength;
  }
  if (!source.close()) return false;
  if (completeLength == 0) return Storage.remove(path.c_str());

  HalFile input;
  if (!Storage.openFileForRead("LH", path.c_str(), input)) return false;
  const std::string tempPath = path + ".tmp";
  HalFile output;
  if (!Storage.openFileForWrite("LH", tempPath.c_str(), output)) {
    input.close();
    return false;
  }
  bool copied = input.seek(0);
  consumed = 0;
  while (copied && consumed < completeLength) {
    const size_t chunkLength = std::min(buffer.size(), completeLength - consumed);
    const int count = input.read(buffer.data(), chunkLength);
    if (count != static_cast<int>(chunkLength) || output.write(buffer.data(), chunkLength) != chunkLength) {
      copied = false;
      break;
    }
    consumed += chunkLength;
  }
  const bool inputClosed = input.close();
  const bool synced = copied && output.sync();
  const bool outputClosed = output.close();
  if (!copied || !inputClosed || !synced || !outputClosed || !OwnedFileSwap::publish(path, tempPath)) {
    Storage.remove(tempPath.c_str());
    return false;
  }
  return true;
}

bool LookupHistory::addWord(const std::string& cachePath, const std::string& word, Status status) {
  if (word.empty()) return false;
  std::lock_guard<std::mutex> lock(historyMutex);

  const std::string path = filePath(cachePath);
  if (!OwnedFileSwap::recover(path) || !discardInterruptedTail(path)) return false;
  HalFile file = Storage.open(path.c_str(), O_WRONLY | O_CREAT | O_AT_END);
  if (!file) {
    LOG_ERR("LH", "Failed to open for append: %s", path.c_str());
    return false;
  }

  const char suffix[] = {'|', static_cast<char>(status), '\n'};
  const bool ok =
      file.write(word.c_str(), word.size()) == word.size() && file.write(suffix, sizeof(suffix)) == sizeof(suffix);
  const bool synced = ok && file.sync();
  const bool closed = file.close();
  if (!ok || !synced || !closed) LOG_ERR("LH", "Failed to append history: %s", path.c_str());
  return ok && synced && closed;
}

void LookupHistory::addWordIf(const std::string& cachePath, const std::string& word, Status status, bool enabled) {
  if (!enabled || word.empty() || cachePath.empty()) return;
  addWord(cachePath, word, status);
}

std::vector<LookupHistory::Entry> LookupHistory::load(const std::string& cachePath) {
  std::lock_guard<std::mutex> lock(historyMutex);
  const auto path = filePath(cachePath);
  if (!OwnedFileSwap::recover(path)) return {};
  auto entries = readRecent(path);
  std::reverse(entries.begin(), entries.end());
  return entries;
}

bool LookupHistory::removeRecentAt(const std::string& cachePath, int indexFromNewest) {
  if (indexFromNewest < 0) return false;
  std::lock_guard<std::mutex> lock(historyMutex);
  const std::string path = filePath(cachePath);
  if (!OwnedFileSwap::recover(path) || !discardInterruptedTail(path)) return false;
  HalFile source;
  if (!Storage.openFileForRead("LH", path.c_str(), source)) return false;

  int lineCount = 0;
  bool inLine = false;
  const size_t length = source.fileSize();
  for (size_t i = 0; i < length; ++i) {
    const int byte = source.read();
    if (byte < 0) {
      source.close();
      return false;
    }
    if (byte == '\n') {
      lineCount++;
      inLine = false;
    } else if (byte != '\r') {
      inLine = true;
    }
  }
  if (inLine) lineCount++;
  const int skipLine = lineCount - 1 - indexFromNewest;
  if (skipLine < 0) {
    source.close();
    return false;
  }

  if (!source.seek(0)) {
    source.close();
    return false;
  }
  const std::string tempPath = path + ".tmp";
  HalFile destination;
  if (!Storage.openFileForWrite("LH", tempPath.c_str(), destination)) {
    source.close();
    return false;
  }

  int currentLine = 0;
  bool ok = true;
  for (size_t i = 0; i < length; ++i) {
    const int byte = source.read();
    if (byte < 0) {
      ok = false;
      break;
    }
    if (currentLine != skipLine && destination.write(static_cast<uint8_t>(byte)) != 1) {
      ok = false;
      break;
    }
    if (byte == '\n') currentLine++;
  }
  const bool readClosed = source.close();
  const bool saved = destination.sync();
  const bool closed = destination.close();

  if (!ok || !readClosed || !saved || !closed || !OwnedFileSwap::publish(path, tempPath)) {
    LOG_ERR("LH", "Failed to remove history entry: %s", path.c_str());
    Storage.remove(tempPath.c_str());
    return false;
  }
  return true;
}

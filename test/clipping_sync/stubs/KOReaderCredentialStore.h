#pragma once
#include <string>
enum class SyncServerSupport { UNSUPPORTED, SUPPORTED };
enum class KOReaderServerType { CROSSPOINT, KOSYNC, OTHER };
enum class DocumentMatchMethod { FILENAME, FILE_CONTENT };
struct TestCredentials {
  bool bookmarks = false;
  KOReaderServerType type = KOReaderServerType::CROSSPOINT;
  bool getSyncBookmarks() const { return bookmarks; }
  bool hasCredentials() const { return !user.empty(); }
  KOReaderServerType getServerType() const { return type; }
  DocumentMatchMethod getMatchMethod() const { return DocumentMatchMethod::FILENAME; }
  std::string server = "https://sync.test", user = "reader";
  const std::string& getBaseUrl() const { return server; }
  const std::string& getUsername() const { return user; }
  const std::string& getMd5Password() const { return user; }
  const std::string& getCustomHeaders() const { return user; }
  SyncServerSupport support = SyncServerSupport::UNSUPPORTED;
  SyncServerSupport getServerSupport() const { return support; }
  void setServerSupport(SyncServerSupport value) { support = value; }
};
inline TestCredentials testCredentials;
#define KOREADER_STORE testCredentials

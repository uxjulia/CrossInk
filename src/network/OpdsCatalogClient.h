#pragma once
#include "HttpDownloader.h"

// Per-browser volatile negotiation state. Reuses only the catalog's configured
// Basic credentials; unsupported OAuth/SSO flows never contact their endpoints.
class OpdsCatalogClient {
 public:
  HttpDownloader::DownloadError get(const std::string& url, const HttpDownloader::DataCallback& body,
                                    const std::string& username, const std::string& password,
                                    HttpDownloader::DownloadOptions options,
                                    HttpRequestDiagnostics* diagnostics = nullptr);
  void reset() { basic = false; }

 private:
  bool basic = false;
};

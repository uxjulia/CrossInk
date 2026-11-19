#include "OpdsCatalogClient.h"

#include <Logging.h>
#include <Memory.h>
#include <OpdsAuthChallenge.h>

#include "HttpRedirectPolicy.h"

HttpDownloader::DownloadError OpdsCatalogClient::get(const std::string& url, const HttpDownloader::DataCallback& body,
                                                     const std::string& username, const std::string& password,
                                                     HttpDownloader::DownloadOptions options,
                                                     HttpRequestDiagnostics* diagnostics) {
  HttpRequestDiagnostics local;
  if (!diagnostics) diagnostics = &local;
  // Approximately 2.2 KiB of bounded SAX scratch, off the small network task
  // stack, only while probing. No body copy, token store, or additional feed array.
  auto challenge = !basic && !username.empty() ? makeUniqueNoThrow<OpdsAuthChallenge>() : nullptr;
  if (!basic && !username.empty() && !challenge) LOG_ERR("OPDS", "Cannot allocate authentication parser");
  if (challenge)
    options.authChallenge = [&challenge](const uint8_t* data, size_t size) { return challenge->write(data, size); };
  const std::string empty;
  auto result = HttpDownloader::streamUrl(url, body, nullptr, basic ? username : empty, basic ? password : empty,
                                          options, diagnostics);
  if (result != HttpDownloader::UNAUTHORIZED || diagnostics->status != 401 || basic || !challenge ||
      !diagnostics->authorizationOriginMatched ||
      !(diagnostics->basicChallenge || (diagnostics->challengeComplete && challenge->offersBasic())))
    return result;
  // Exactly one retry. Only the original credential origin can authorize it;
  // redirects still use HttpDownloader's per-hop credential/downgrade policy.
  options.authChallenge = nullptr;
  challenge.reset();
  result = HttpDownloader::streamUrl(url, body, nullptr, username, password, options, diagnostics);
  basic = result == HttpDownloader::OK;
  return result;
}
